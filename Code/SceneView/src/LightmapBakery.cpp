#include "Sim/SceneView/LightmapBakery.h"

#include "Sim/Assets/LightmapRaster.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/Raycast/Query.h"
#include "Sim/Reference/PathTracer.h"
#include "Sim/Reference/Shading.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace sim::view {
namespace {

/// Geometri sebuah objek beserta petaknya, dikumpulkan sebelum panggangan
/// berangkat.
struct BakeItem {
    uint64_t owner = 0;
    Mat4 transform{1.0f};
    std::shared_ptr<const assets::MeshData> mesh;
    uint32_t chart = 0;
};

uint64_t HashInto(uint64_t hash, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

/// Mengumpulkan objek yang punya geometri **dan** UV lightmap.
///
/// **Yang tanpa UV lightmap dilewati, bukan dipaksa masuk.** Mesh yang belum
/// pernah di-unwrap punya `lightmapUv` nol di seluruh vertex, dan
/// merasterisasinya menumpuk seluruh permukaannya di satu texel — yang terbaca
/// sebagai lightmap yang bekerja, bukan sebagai mesh yang terlewat.
std::vector<BakeItem> CollectItems(std::span<const PickItem> items,
                                   assets::MeshGeometryCache* cache,
                                   const LightmapBakery::Settings& settings,
                                   std::vector<assets::LightmapChart>& outCharts,
                                   uint32_t& outMissingUv, uint32_t& outLoading) {
    std::vector<BakeItem> collected;
    outMissingUv = 0;
    outLoading = 0;
    if (cache == nullptr) {
        return collected;
    }

    for (const PickItem& item : items) {
        if (item.meshKey.empty()) {
            continue;
        }
        const std::string key(item.meshKey);
        const assets::MeshGeometryRef ref =
            item.sourcePath.empty() ? cache->Find(key)
                                    : cache->Request(std::filesystem::path(item.sourcePath));
        if (ref.state == assets::MeshGeometryState::Pending) {
            ++outLoading;
            continue;
        }
        if (ref.state != assets::MeshGeometryState::Ready || ref.data == nullptr) {
            continue;
        }
        if (!item.lightmapEligible) {
            // **Bukan dihitung sebagai "tanpa UV".** Yang tidak berhak atas
            // petak memang tidak boleh punya, dan menghitungnya di sana membuat
            // peringatan menyebut cacat yang tidak ada.
            continue;
        }
        if (!ref.data->hasLightmapUv) {
            ++outMissingUv;
            continue;
        }

        BakeItem baked;
        baked.owner = static_cast<uint64_t>(item.entity);
        baked.transform = item.worldMatrix;
        baked.mesh = ref.data;
        baked.chart = static_cast<uint32_t>(outCharts.size());

        assets::LightmapChart chart;
        // **Penimpaan per objek menang atas setelan level.** Nol berarti ikut
        // levelnya — sebuah angka, bukan sebuah bendera, karena kerapatan nol
        // tidak punya arti lain yang masuk akal.
        const float density = item.lightmapTexelDensity > 0.0f ? item.lightmapTexelDensity
                                                               : settings.texelsPerMeter;
        chart.side = assets::LightmapChartSide(
            assets::MeshWorldArea(*ref.data, item.worldMatrix), density, settings.minChartSide,
            settings.maxChartSide);
        outCharts.push_back(chart);
        collected.push_back(std::move(baked));
    }
    return collected;
}

}  // namespace

struct LightmapBakery::Job {
    std::atomic<bool> ready{false};
    std::atomic<uint32_t> done{0};
    /// Berapa objek yang belum selesai. **Yang terakhir menurunkannya ke nol
    /// yang menulis artefaknya dan menandai selesai** — bukan `WaitIdle`, yang
    /// dipanggil dari dalam sebuah tugas akan menunggu dirinya sendiri.
    std::atomic<uint32_t> remaining{0};
    uint32_t total = 1;
    bool fromCache = false;
    std::shared_ptr<render::Lightmap> lightmap;
    std::mutex statusMutex;
    std::string status;
    /// Geometrinya dipegang tugasnya sendiri, dengan alasan yang sama seperti
    /// `ProbeBakery::Job::picks`: level yang ditutup di tengah panggangan tidak
    /// boleh membebaskan segitiga yang sedang ditembaki.
    std::shared_ptr<PickScene> picks;
    std::vector<BakeItem> items;

    /// **Dipegang sekali di sini, bukan disalin ke tiap tugas.** `trace.sky`
    /// memegang `AtmosphereSky` beserta tabel transmitansinya, 256×64 Vec3 =
    /// 192 KB; menangkapnya by value pada 179 tugas yang dikirim sekaligus
    /// berarti ~34 MB salinan identik hidup bersamaan sebelum tugas pertama
    /// jalan, dan ia naik linier dengan jumlah objek. Keduanya hanya-baca
    /// selama panggangan, jadi satu salinan yang dipegang `job` cukup —
    /// pola yang sama dengan `picks`.
    assets::LightmapAtlasLayout layout;
    reference::TraceSettings trace;
    reference::Surface surface;
    uint32_t samples = 1;
    uint32_t dilate = 0;
};

LightmapBakery::LightmapBakery(TaskPool* tasks) : tasks_(tasks) {}
LightmapBakery::~LightmapBakery() = default;

assets::LightmapAtlasLayout LightmapBakery::PlanAtlas(std::span<const PickItem> items,
                                                      assets::MeshGeometryCache* cache,
                                                      const Settings& settings) {
    std::vector<assets::LightmapChart> charts;
    uint32_t missingUv = 0;
    uint32_t loading = 0;
    CollectItems(items, cache, settings, charts, missingUv, loading);
    return assets::PackLightmapAtlas(std::move(charts), settings.maxAtlasSide);
}

bool LightmapBakery::Running() const {
    return job_ != nullptr && !job_->ready.load(std::memory_order_acquire);
}

float LightmapBakery::Progress() const {
    if (job_ == nullptr) {
        return 0.0f;
    }
    if (job_->ready.load(std::memory_order_acquire)) {
        return 1.0f;
    }
    return static_cast<float>(job_->done.load(std::memory_order_relaxed)) /
           static_cast<float>(std::max(job_->total, 1u));
}

bool LightmapBakery::LoadedFromCache() const {
    return job_ != nullptr && job_->fromCache;
}

std::string LightmapBakery::Status() const {
    if (job_ == nullptr) {
        return {};
    }
    std::lock_guard<std::mutex> lock(job_->statusMutex);
    return job_->status;
}

std::shared_ptr<const render::Lightmap> LightmapBakery::Take() {
    if (job_ == nullptr || !job_->ready.load(std::memory_order_acquire)) {
        return nullptr;
    }
    std::shared_ptr<render::Lightmap> lightmap = std::move(job_->lightmap);
    job_->lightmap.reset();
    return lightmap;
}

bool LightmapBakery::Bake(std::span<const PickItem> items, std::function<Vec3(const Vec3&)> sky,
                          const Settings& settings) {
    if (Running()) {
        return false;
    }

    std::vector<assets::LightmapChart> charts;
    uint32_t missingUv = 0;
    uint32_t loading = 0;
    std::vector<BakeItem> collected = CollectItems(items, cache_, settings, charts, missingUv,
                                                   loading);
    if (loading > 0) {
        // **Yang masih dimuat ditunggu, bukan dilewati** — pelajaran yang sama
        // dari panggangan probe: geometri yang siap seketika membuat panggangan
        // berangkat sebelum yang lain selesai diurai, dan yang terlewat tidak
        // menghasilkan satu pun galat.
        SIM_INFO("Bake", "waiting for {} of {} objects to finish loading before baking lightmaps",
                 loading, items.size());
        return false;
    }
    if (collected.empty()) {
        SIM_WARN("Bake", "no object has lightmap UVs; nothing to bake ({} of {} were skipped)",
                 missingUv, items.size());
        return false;
    }
    if (missingUv > 0) {
        // Disebutkan angkanya: yang tanpa UV lightmap digambar tanpa lightmap,
        // dan yang melihatnya akan mengira lightmap-nya rusak.
        SIM_WARN("Bake", "{} of {} objects have no lightmap UVs and will not be lit by the atlas",
                 missingUv, items.size());
    }

    const assets::LightmapAtlasLayout layout =
        assets::PackLightmapAtlas(std::move(charts), settings.maxAtlasSide);
    if (!layout.IsValid()) {
        SIM_WARN("Bake", "lightmap atlas could not be laid out");
        return false;
    }
    if (layout.dropped > 0) {
        SIM_WARN("Bake", "{} objects did not fit the {}x{} lightmap atlas; raise the limit or "
                         "lower the texel density",
                 layout.dropped, layout.width, layout.height);
    }

    // Kunci: langit, matahari, albedo, cuplikan, dan geometri beserta letaknya.
    // Yang tidak masuk terbaca kembali sebagai panggangan yang benar.
    uint64_t key = HashInto(1469598103934665603ull, &settings.skyKey, sizeof(settings.skyKey));
    key = HashInto(key, &settings.sunIrradiance.x, sizeof(float) * 3);
    key = HashInto(key, &settings.sunDirection.x, sizeof(float) * 3);
    key = HashInto(key, &settings.albedo, sizeof(settings.albedo));
    key = HashInto(key, &settings.samplesPerTexel, sizeof(settings.samplesPerTexel));
    key = HashInto(key, &settings.texelsPerMeter, sizeof(settings.texelsPerMeter));
    key = HashInto(key, &layout.width, sizeof(layout.width));
    for (const BakeItem& item : collected) {
        key = HashInto(key, &item.owner, sizeof(item.owner));
        key = HashInto(key, &item.transform[0][0], sizeof(float) * 16);
    }

    auto job = std::make_shared<Job>();
    const std::filesystem::path file =
        settings.cacheDir.empty()
            ? std::filesystem::path{}
            : render::LightmapCachePath(settings.cacheDir, render::LightmapCacheKey(key));

    if (!file.empty()) {
        auto cached = std::make_shared<render::Lightmap>();
        std::string cacheError;
        if (render::ReadLightmap(file, *cached, cacheError) && cached->IsValid() &&
            cached->width == layout.width) {
            job->lightmap = std::move(cached);
            job->fromCache = true;
            job->status = "lightmap loaded from " + file.filename().string();
            job->ready.store(true, std::memory_order_release);
            job_ = std::move(job);
            return true;
        }
    }

    // Geometrinya disalin sekarang, di main thread — `Sync` menyentuh cache
    // geometri, yang bukan milik thread mana pun.
    job->picks = std::make_shared<PickScene>(cache_);
    job->picks->Sync(items);
    if (job->picks->ReadyCount() == 0) {
        SIM_WARN("Bake", "no mesh geometry is ready; a lightmap bake now would see an empty world");
        return false;
    }
    job->items = std::move(collected);

    uint32_t texels = 0;
    for (const BakeItem& item : job->items) {
        const assets::LightmapChart& chart = layout.charts[item.chart];
        if (chart.placed) {
            texels += chart.side * chart.side;
        }
    }
    job->total = std::max(texels, 1u);

    const float albedo = settings.albedo;
    job->layout = layout;
    job->samples = std::max(settings.samplesPerTexel, 1u);
    job->dilate = settings.dilateRadius;

    auto lightmap = std::make_shared<render::Lightmap>();
    lightmap->width = layout.width;
    lightmap->height = layout.height;
    lightmap->texels.assign(static_cast<std::size_t>(layout.width) * layout.height, Vec3(0.0f));

    // **Penempatannya dihitung di sini, di main thread.** Tugas per objek lalu
    // hanya menulis texel — dan texel tiap objek berada di petaknya sendiri,
    // yang menurut definisi tidak beririsan dengan petak objek lain. Tanpa ini
    // setiap tugas harus menambah `placements`, dan `push_back` bersamaan dari
    // banyak thread adalah kerusakan memori, bukan urutan yang berubah-ubah.
    std::vector<uint32_t> baking;
    for (uint32_t i = 0; i < job->items.size(); ++i) {
        const assets::LightmapChart& chart = layout.charts[job->items[i].chart];
        if (!chart.placed) {
            continue;
        }
        render::LightmapPlacement placement;
        placement.owner = job->items[i].owner;
        const auto width = static_cast<float>(layout.width);
        const auto height = static_cast<float>(layout.height);
        placement.scaleOffset = Vec4(static_cast<float>(chart.side) / width,
                                     static_cast<float>(chart.side) / height,
                                     static_cast<float>(chart.x) / width,
                                     static_cast<float>(chart.y) / height);
        lightmap->placements.push_back(placement);
        baking.push_back(i);
    }
    job->lightmap = std::move(lightmap);
    job->remaining.store(static_cast<uint32_t>(std::max<std::size_t>(baking.size(), 1)),
                         std::memory_order_relaxed);

    job->trace.sky = [sky = std::move(sky)](const Vec3& direction) { return sky(direction); };
    job->trace.sunIrradiance = settings.sunIrradiance;
    job->trace.sunDirection = settings.sunDirection;

    job->surface.baseColor = Vec3(albedo);
    job->surface.baseMetalness = 0.0f;
    job->surface.specularWeight = 0.0f;
    job->surface.specularRoughness = 1.0f;

    /// Menyelesaikan panggangan: menulis artefaknya dan menandai selesai.
    const auto finish = [job, file]() {
        std::string message = "lightmap baked";
        if (!file.empty()) {
            std::string error;
            if (!render::WriteLightmap(file, *job->lightmap, error)) {
                message = "lightmap baked, but the artefact could not be written: " + error;
            } else {
                message = "lightmap baked to " + file.filename().string();
            }
        }
        {
            std::lock_guard<std::mutex> lock(job->statusMutex);
            job->status = std::move(message);
        }
        job->ready.store(true, std::memory_order_release);
    };

    if (baking.empty()) {
        job_ = job;
        finish();
        return true;
    }

    // **Status awal dan `job_` dipasang sebelum satu pun tugas dikirim.** Tugas
    // yang selesai lebih dulu menulis status akhirnya, dan status awal yang
    // dipasang sesudahnya akan menimpanya — panggangan yang sudah selesai lalu
    // terbaca sebagai masih berjalan.
    job_ = job;
    {
        std::lock_guard<std::mutex> lock(job->statusMutex);
        job->status = "baking " + std::to_string(job->total) + " lightmap texels…";
    }

    // **Satu tugas per objek, bukan satu tugas untuk seluruhnya.** Sebelum ini
    // panggangan memakai satu inti: 179 objek pada 2 texel/m memakan 5 detik,
    // dan 4 texel/m — empat kali texelnya — belum selesai dalam lima menit.
    //
    // Yang membuatnya aman: tiap tugas menulis ke petaknya sendiri, dan
    // `raycast::Raycast` menerima scene-nya sebagai `const` tanpa state yang
    // berubah, jadi penelusuran bersamaan membaca pohon yang sama tanpa
    // menyentuhnya.
    for (const uint32_t index : baking) {
        auto bakeOne = [job, index, finish]() {
            const BakeItem& item = job->items[index];
            const assets::LightmapAtlasLayout& layout = job->layout;
            const assets::LightmapChart& chart = layout.charts[item.chart];
            const uint32_t samples = job->samples;
            const uint32_t dilate = job->dilate;
            const reference::TraceSettings& trace = job->trace;

            const reference::SurfaceResolver resolve =
                [&surface = job->surface](const raycast::RayHit& hit, const Vec3& origin,
                                          const Vec3& direction) -> reference::SurfaceHit {
                reference::SurfaceHit out;
                out.position = origin + direction * hit.distance;
                out.normal = hit.normal;
                out.surface = surface;
                return out;
            };

            // **Dirasterisasi di ruang dunia**, bukan di ruang lokal lalu
            // ditransformasi: normal yang ditransformasi lewat matriks berskala
            // tidak lagi tegak lurus permukaannya, dan iradiansi pada normal
            // yang miring adalah iradiansi permukaan yang lain.
            assets::MeshData world = *item.mesh;
            const Mat3 normalMatrix = glm::transpose(glm::inverse(Mat3(item.transform)));
            for (assets::MeshVertex& vertex : world.vertices) {
                vertex.position = Vec3(item.transform * Vec4(vertex.position, 1.0f));
                vertex.normal = glm::normalize(normalMatrix * vertex.normal);
            }

            const assets::LightmapRaster raster =
                assets::RasteriseLightmap(world, chart.side, chart.side);
            if (!raster.IsValid()) {
                // **Texel-nya tetap dihitung selesai.** `job->total` sudah
                // memuatnya, dan kemajuan yang tidak pernah mencapai seratus
                // persen terbaca sebagai panggangan yang menggantung — bukan
                // sebagai satu objek yang tidak bisa dirasterisasi.
                job->done.fetch_add(chart.side * chart.side, std::memory_order_relaxed);
            } else {
                std::vector<Vec3> values(raster.texels.size(), Vec3(0.0f));
                for (std::size_t i = 0; i < raster.texels.size(); ++i) {
                    const assets::LightmapTexel& texel = raster.texels[i];
                    if (texel.covered) {
                        values[i] = reference::TraceSurfaceIrradiance(
                            job->picks->Scene(), resolve, reference::LightList{}, texel.position,
                            texel.normal, samples, trace);
                    }
                    job->done.fetch_add(1, std::memory_order_relaxed);
                }
                // Diperluas **sebelum** disalin ke atlas: memperluas di dalam
                // atlas akan menarik warna dari petak tetangga, yaitu objek lain.
                assets::DilateLightmap(values, raster, dilate);

                for (uint32_t y = 0; y < chart.side; ++y) {
                    for (uint32_t x = 0; x < chart.side; ++x) {
                        const std::size_t at =
                            static_cast<std::size_t>(chart.y + y) * layout.width + chart.x + x;
                        job->lightmap->texels[at] =
                            values[static_cast<std::size_t>(y) * chart.side + x];
                    }
                }

                // **Selokannya diisi replika texel tepi petak ini sendiri.**
                // Atlasnya disampel `VK_FILTER_LINEAR`, jadi cuplikan di tepi
                // petak menjangkau setengah texel ke luar isinya. Tanpa cincin
                // ini yang dijangkaunya petak sebelahnya — objek lain — dan
                // diukur pada atlas `bench` sebelum cincin ini ada, cuplikan
                // tepi meleset rata-rata 30% dari rerata iradiansi adegan.
                //
                // Cincin sebuah petak tidak beririsan dengan petak mana pun:
                // pemaketnya memesan `side + 2 * padding` untuk tiap petak.
                // Karena itu ia tetap boleh ditulis dari tugas per objek.
                for (uint32_t ring = 0; ring < layout.padding; ++ring) {
                    const auto clampToChart = [&chart](int32_t v) {
                        return static_cast<uint32_t>(
                            std::clamp<int32_t>(v, 0, static_cast<int32_t>(chart.side) - 1));
                    };
                    const auto atlasAt = [&](int32_t x, int32_t y) -> std::size_t {
                        return static_cast<std::size_t>(static_cast<int32_t>(chart.y) + y) *
                                   layout.width +
                               static_cast<std::size_t>(static_cast<int32_t>(chart.x) + x);
                    };
                    const auto content = [&](int32_t x, int32_t y) {
                        return values[static_cast<std::size_t>(clampToChart(y)) * chart.side +
                                      clampToChart(x)];
                    };
                    const auto low = -static_cast<int32_t>(ring) - 1;
                    const auto high = static_cast<int32_t>(chart.side) + static_cast<int32_t>(ring);
                    for (int32_t x = low; x <= high; ++x) {
                        job->lightmap->texels[atlasAt(x, low)] = content(x, low);
                        job->lightmap->texels[atlasAt(x, high)] = content(x, high);
                    }
                    for (int32_t y = low + 1; y < high; ++y) {
                        job->lightmap->texels[atlasAt(low, y)] = content(low, y);
                        job->lightmap->texels[atlasAt(high, y)] = content(high, y);
                    }
                }
            }

            if (job->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                finish();
            }
        };
        if (tasks_ != nullptr) {
            tasks_->Submit(std::move(bakeOne));
        } else {
            bakeOne();
        }
    }

    return true;
}

}  // namespace sim::view
