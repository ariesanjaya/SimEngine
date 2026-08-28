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

bool ProbeBakery::Bake(std::span<const PickItem> items, std::function<Vec3(const Vec3&)> sky,
                       const Settings& settings) {
    if (Running()) {
        return false;
    }
    if (!settings.layout.IsValid()) {
        SIM_WARN("Bake", "probe grid is not valid; nothing to bake");
        return false;
    }

    // **Geometrinya disalin sekarang, di main thread.** `Sync` menyentuh cache
    // geometri, yang bukan milik thread mana pun; memanggilnya dari worker
    // berarti dua thread mengurai FBX yang sama ke dalam peta yang sama.
    picks_.Sync(items);
    if (picks_.ReadyCount() == 0) {
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
    if (picks_.PendingCount() > 0) {
        SIM_WARN("Bake",
                 "{} of {} objects have no CPU geometry and will not occlude the bake",
                 picks_.PendingCount(), items.size());
    }

    // Brick yang jauh dari permukaan dibuang sebelum satu sinar pun ditembakkan
    // (keputusan 9): yang tidak dialokasikan tidak dipanggang, dan itu bukan
    // penghematan memori melainkan penghematan waktu panggangnya.
    std::vector<render::ProbeOccupancy> occupancy;
    occupancy.reserve(items.size());
    for (const PickItem& item : items) {
        // Kotak dunia dari geometrinya sendiri belum tersedia di sini tanpa
        // memuat mesh-nya, jadi yang dipakai kotak yang sama yang dilihat ray
        // scene: titik asal matriksnya, dilebarkan oleh skalanya. Yang terlalu
        // lebar cuma menyimpan brick yang tidak perlu; yang terlalu sempit
        // membuang brick yang dipakai — jadi lebar yang menang.
        const Vec3 origin(item.worldMatrix[3]);
        const float scale = std::max({glm::length(Vec3(item.worldMatrix[0])),
                                      glm::length(Vec3(item.worldMatrix[1])),
                                      glm::length(Vec3(item.worldMatrix[2]))});
        occupancy.push_back(render::ProbeOccupancy{origin - Vec3(scale), origin + Vec3(scale)});
    }

    auto job = std::make_shared<Job>();
    const render::ProbeVolumeLayout layout = settings.layout;
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

    // Yang ikut ke dalam tugas: `RayScene` milik bakery ini — dipegang lewat
    // referensi, dan itu sah karena `TaskPool::Stop` dijalankan sebelum
    // pemiliknya dihancurkan (lihat catatannya di `TaskPool::Stop`).
    const raycast::RayScene& scene = picks_.Scene();
    const float albedo = settings.albedo;
    const uint32_t samples = std::max(settings.samplesPerProbe, 1u);
    const Vec3 sunIrradiance = settings.sunIrradiance;
    const Vec3 sunDirection = settings.sunDirection;
    const std::filesystem::path cacheDir = settings.cacheDir;
    const uint64_t environmentKey = settings.environmentKey;

    auto bake = [job, &scene, layout, slots = std::move(slots), allocated, albedo, samples,
                 sunIrradiance, sunDirection, sky = std::move(sky), cacheDir,
                 environmentKey]() mutable {
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
        volume->probes.assign(static_cast<std::size_t>(allocated) * kProbesPerBrick, render::Sh9{});

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
                    scene, resolve, reference::LightList{}, position, samples, trace);
                volume->probes[static_cast<std::size_t>(slot) * kProbesPerBrick + local]
                    .coefficients = coefficients;
                job->done.fetch_add(1, std::memory_order_relaxed);
            }
        }

        std::string message = "probes baked";
        if (!cacheDir.empty()) {
            const uint64_t key = render::ProbeVolumeCacheKey(layout, environmentKey);
            const std::filesystem::path file = render::ProbeVolumeCachePath(cacheDir, key);
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
