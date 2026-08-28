#include "Sim/SceneView/ProbeBakery.h"

#include "Sim/Core/Log.h"
#include "Sim/Core/TaskPool.h"
#include "Sim/Raycast/Query.h"
#include "Sim/Reference/PathTracer.h"
#include "Sim/Reference/Shading.h"

#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace sim::view {

/// Keadaan satu panggangan, dipegang `shared_ptr`.
///
/// **Tugasnya tidak pernah menangkap `this`**, aturan yang sama dengan bakery
/// tekstur dan SDF: bakery-nya boleh dihancurkan saat level ditutup, sementara
/// worker masih menelusuri sinar. Yang ditangkap kotak hasil ini, dan ia hidup
/// selama masih ada yang memegangnya.
struct ProbeBakery::Job {
    std::atomic<bool> ready{false};
    std::atomic<uint32_t> done{0};
    uint32_t total = 1;
    std::shared_ptr<render::ProbeVolume> volume;
    std::mutex statusMutex;
    std::string status;
    /// **Geometrinya dipegang di sini, bukan di bakery.** Tugasnya menelusuri
    /// sinar ke dalam `PickScene` ini selama puluhan detik; kalau yang
    /// memilikinya bakery, sebuah level yang ditutup di tengah panggangan
    /// membebaskan BVH yang sedang ditembaki. Yang keluar bukan galat melainkan
    /// pembacaan memori bebas. Di sini ia hidup selama masih ada yang memegang
    /// `shared_ptr`-nya — yaitu tugasnya sendiri.
    std::shared_ptr<PickScene> picks;
};

ProbeBakery::ProbeBakery(TaskPool* tasks) : tasks_(tasks) {}
ProbeBakery::~ProbeBakery() = default;

bool ProbeBakery::Running() const {
    return job_ != nullptr && !job_->ready.load(std::memory_order_acquire);
}

float ProbeBakery::Progress() const {
    if (job_ == nullptr) {
        return 0.0f;
    }
    if (job_->ready.load(std::memory_order_acquire)) {
        return 1.0f;
    }
    return static_cast<float>(job_->done.load(std::memory_order_relaxed)) /
           static_cast<float>(std::max(job_->total, 1u));
}

std::string ProbeBakery::Status() const {
    if (job_ == nullptr) {
        return {};
    }
    std::lock_guard<std::mutex> lock(job_->statusMutex);
    return job_->status;
}

std::shared_ptr<const render::ProbeVolume> ProbeBakery::Take() {
    if (job_ == nullptr || !job_->ready.load(std::memory_order_acquire)) {
        return nullptr;
    }
    std::shared_ptr<render::ProbeVolume> volume = std::move(job_->volume);
    job_->volume.reset();
    return volume;
}

namespace {

uint64_t HashInto(uint64_t hash, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

/// Sidik jari segala sesuatu yang mengubah jawaban panggangan **selain**
/// langitnya, yang datang dari pemanggil.
///
/// **Yang tidak masuk ke sini akan terbaca kembali sebagai panggangan yang
/// benar.** Sebuah artefak yang kuncinya cuma bentuk kisi akan dipakai ulang
/// setelah mataharinya digeser, setelah albedonya diubah, dan setelah setengah
/// adegannya dipindahkan — dan ketiganya menghasilkan berkas yang sah berisi
/// cahaya yang salah. Itu kegagalan yang paling mahal untuk ditemukan, karena
/// tidak ada satu pun yang tampak rusak.
uint64_t BakeInputKey(std::span<const PickItem> items, const ProbeBakery::Settings& settings) {
    uint64_t hash = HashInto(1469598103934665603ull, &settings.skyKey, sizeof(settings.skyKey));
    hash = HashInto(hash, &settings.sunIrradiance.x, sizeof(float) * 3);
    hash = HashInto(hash, &settings.sunDirection.x, sizeof(float) * 3);
    hash = HashInto(hash, &settings.albedo, sizeof(settings.albedo));
    hash = HashInto(hash, &settings.samplesPerProbe, sizeof(settings.samplesPerProbe));
    for (const PickItem& item : items) {
        hash = HashInto(hash, item.meshKey.data(), item.meshKey.size());
        hash = HashInto(hash, &item.worldMatrix[0][0], sizeof(float) * 16);
    }
    return hash;
}

}  // namespace

bool ProbeBakery::Bake(std::span<const PickItem> items, std::function<Vec3(const Vec3&)> sky,
                       const Settings& settings) {
    if (Running()) {
        return false;
    }
    if (!settings.layout.IsValid()) {
        SIM_WARN("Bake", "probe grid is not valid; nothing to bake");
        return false;
    }

    const render::ProbeVolumeLayout layout = settings.layout;
    const uint64_t key = render::ProbeVolumeCacheKey(layout, BakeInputKey(items, settings));
    const std::filesystem::path file =
        settings.cacheDir.empty() ? std::filesystem::path{}
                                  : render::ProbeVolumeCachePath(settings.cacheDir, key);

    auto job = std::make_shared<Job>();

    // **Cache dibaca sebelum satu sinar pun ditembakkan.** Sebuah panggangan
    // adalah puluhan detik sampai menit; artefak yang ditulis tapi tidak pernah
    // dibaca bukan cache melainkan berkas yang menumpuk.
    if (!file.empty()) {
        auto cached = std::make_shared<render::ProbeVolume>();
        std::string error;
        if (render::ReadProbeVolume(file, *cached, error) && cached->IsValid() &&
            cached->layout.counts == layout.counts) {
            job->volume = std::move(cached);
            job->status = "probes loaded from " + file.filename().string();
            job->ready.store(true, std::memory_order_release);
            job_ = std::move(job);
            return true;
        }
    }

    // **Geometrinya disalin sekarang, di main thread.** `Sync` menyentuh cache
    // geometri, yang bukan milik thread mana pun; memanggilnya dari worker
    // berarti dua thread mengurai FBX yang sama ke dalam peta yang sama.
    job->picks = std::make_shared<PickScene>(cache_);
    job->picks->Sync(items);
    if (job->picks->ReadyCount() == 0) {
        SIM_WARN("Bake",
                 "no mesh geometry is ready out of {} objects; a bake now would see an empty "
                 "world and light every point as if it were outdoors",
                 items.size());
        return false;
    }
    // **Berapa yang tidak ikut disebutkan, bukan didiamkan.** Yang tidak punya
    // geometri CPU tidak menghalangi apa pun — kubus bawaan, whitebox yang
    // belum diadopsi, dan terrain semuanya begitu — dan panggangan yang
    // melewatkannya menghasilkan ruangan yang tetap disinari langit tanpa satu
    // pun galat. Angka ini yang membuat keadaan itu bisa dilihat.
    if (job->picks->PendingCount() > 0) {
        SIM_WARN("Bake", "{} of {} objects have no CPU geometry and will not occlude the bake",
                 job->picks->PendingCount(), items.size());
    }

    // Brick yang jauh dari permukaan dibuang sebelum satu sinar pun ditembakkan
    // (keputusan 9): yang tidak dialokasikan tidak dipanggang, dan itu bukan
    // penghematan memori melainkan penghematan waktu panggangnya.
    std::vector<render::ProbeOccupancy> occupancy;
    occupancy.reserve(items.size());
    for (const PickItem& item : items) {
        if (item.worldMinimum == item.worldMaximum) {
            // Kotak yang belum diisi: jangan menebak bentuknya. Yang paling
            // aman adalah tidak membuang apa pun untuk benda ini, dan cara
            // mengatakannya adalah kotak sebesar kisi.
            occupancy.push_back(render::ProbeOccupancy{
                layout.origin, layout.origin + Vec3(layout.counts) * layout.spacing});
            continue;
        }
        occupancy.push_back(render::ProbeOccupancy{item.worldMinimum, item.worldMaximum});
    }

    std::vector<uint32_t> slots =
        render::AssignProbeBricks(layout, occupancy, layout.spacing * 2.0f);

    uint32_t allocated = 0;
    for (const uint32_t slot : slots) {
        if (slot != render::kEmptyBrick) {
            ++allocated;
        }
    }
    constexpr uint32_t kProbesPerBrick = render::ProbeVolumeLayout::kBrickSize *
                                         render::ProbeVolumeLayout::kBrickSize *
                                         render::ProbeVolumeLayout::kBrickSize;
    job->total = std::max(allocated * kProbesPerBrick, 1u);

    const float albedo = settings.albedo;
    const uint32_t samples = std::max(settings.samplesPerProbe, 1u);
    const Vec3 sunIrradiance = settings.sunIrradiance;
    const Vec3 sunDirection = settings.sunDirection;

    auto bake = [job, layout, slots = std::move(slots), allocated, albedo, samples, sunIrradiance,
                 sunDirection, sky = std::move(sky), file]() mutable {
        reference::TraceSettings trace;
        trace.sky = [sky](const Vec3& direction) { return sky(direction); };
        trace.sunIrradiance = sunIrradiance;
        trace.sunDirection = sunDirection;

        // Satu albedo untuk seluruh adegan; alasannya di `Settings::albedo`.
        reference::Surface surface;
        surface.baseColor = Vec3(albedo);
        surface.baseMetalness = 0.0f;
        surface.specularWeight = 0.0f;
        surface.specularRoughness = 1.0f;
        const reference::SurfaceResolver resolve =
            [surface](const raycast::RayHit& hit, const Vec3& origin,
                      const Vec3& direction) -> reference::SurfaceHit {
            reference::SurfaceHit out;
            out.position = origin + direction * hit.distance;
            out.normal = hit.normal;
            out.surface = surface;
            return out;
        };

        auto volume = std::make_shared<render::ProbeVolume>();
        volume->layout = layout;
        volume->brickSlots = std::move(slots);
        volume->probes.assign(static_cast<std::size_t>(allocated) * kProbesPerBrick,
                              render::Sh9{});

        const glm::uvec3 bricks = layout.BrickCounts();
        constexpr uint32_t kSide = render::ProbeVolumeLayout::kBrickSize;
        for (uint32_t brick = 0; brick < volume->brickSlots.size(); ++brick) {
            const uint32_t slot = volume->brickSlots[brick];
            if (slot == render::kEmptyBrick) {
                continue;
            }
            const glm::uvec3 brickAt(brick % bricks.x, (brick / bricks.x) % bricks.y,
                                     brick / (bricks.x * bricks.y));
            for (uint32_t local = 0; local < kProbesPerBrick; ++local) {
                const glm::uvec3 inside(local % kSide, (local / kSide) % kSide,
                                        local / (kSide * kSide));
                const glm::uvec3 coordinate = brickAt * kSide + inside;
                const Vec3 position = layout.ProbePosition(coordinate);
                const std::array<Vec3, 9> coefficients = reference::TraceProbeIrradiance(
                    job->picks->Scene(), resolve, reference::LightList{}, position, samples,
                    trace);
                volume->probes[static_cast<std::size_t>(slot) * kProbesPerBrick + local]
                    .coefficients = coefficients;
                job->done.fetch_add(1, std::memory_order_relaxed);
            }
        }

        std::string message = "probes baked";
        if (!file.empty()) {
            std::string error;
            if (!render::WriteProbeVolume(file, *volume, error)) {
                // Gagal menulis bukan gagal memanggang: kisinya sudah ada di
                // memori dan tetap dipakai. Yang hilang cuma kesempatan
                // melewatkan panggangan berikutnya.
                message = "probes baked, but the artefact could not be written: " + error;
            } else {
                message = "probes baked to " + file.filename().string();
            }
        }
        {
            std::lock_guard<std::mutex> lock(job->statusMutex);
            job->status = std::move(message);
        }
        job->volume = std::move(volume);
        job->ready.store(true, std::memory_order_release);
    };

    job_ = job;
    {
        std::lock_guard<std::mutex> lock(job->statusMutex);
        job->status = "baking " + std::to_string(job->total) + " probes…";
    }
    if (tasks_ != nullptr) {
        tasks_->Submit(std::move(bake));
    } else {
        bake();
    }
    return true;
}

}  // namespace sim::view
