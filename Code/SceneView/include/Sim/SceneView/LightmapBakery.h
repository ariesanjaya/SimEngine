#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Assets/LightmapAtlas.h"
#include "Sim/Render/Lightmap.h"
#include "Sim/SceneView/PickScene.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace sim {
class TaskPool;
}

namespace sim::view {

/// Memanggang lightmap untuk permukaan statis (S5 di docs/PLAN-STATIC-GI.md).
///
/// **Kedudukannya sama dengan `ProbeBakery`:** pengondisi aset, bukan bagian
/// jalur gambar. Ia menelusuri sinar dengan path tracer acuan, dan itu menuntut
/// Embree beserta geometri CPU; renderer hanya menerima atlasnya yang sudah
/// jadi.
///
/// Ada hanya bila `Sim::Reference` ada — modul itu dilewati ketika slangc tidak
/// ditemukan. Yang memakainya wajib memeriksa null.
class LightmapBakery {
public:
    struct Settings {
        /// Texel per **meter**, bukan per meter persegi. Setelan pengarang;
        /// lihat keputusan 8.
        float texelsPerMeter = 4.0f;
        uint32_t maxAtlasSide = 4096;
        /// Sisi terkecil dan terbesar sebuah petak. Yang terkecil menjaga benda
        /// kecil tetap punya texel; yang terbesar menjaga satu benda tidak
        /// menelan seluruh atlas.
        uint32_t minChartSide = 4;
        uint32_t maxChartSide = 512;
        /// Sinar per texel. **Yang menentukan derau, dan derau di sini
        /// terpanggang** — ia tidak hilang saat menggambar.
        uint32_t samplesPerTexel = 128;
        /// Berapa kali tepi chart diperluas ke texel kosong di sekitarnya.
        uint32_t dilateRadius = 2;

        Vec3 sunIrradiance{0.0f};
        Vec3 sunDirection{0.0f, -1.0f, 0.0f};
        float albedo = 0.5f;

        std::filesystem::path cacheDir;
        /// Sidik jari langit dari pemanggil, dengan alasan yang sama seperti
        /// `ProbeBakery::skyKey`: langitnya sebuah `std::function` yang tidak
        /// bisa di-hash, dan kunci yang tidak memuatnya membaca panggangan
        /// matahari sore sebagai panggangan matahari pagi.
        uint64_t skyKey = 0;
    };

    /// Rencana atlas untuk sekumpulan objek, **tanpa memanggang apa pun**.
    ///
    /// Ini yang dipanggil panel: kriteria terima S5 menuntut ukuran atlasnya
    /// diumumkan sebelum Bake ditekan, dan sebuah angka yang baru diketahui
    /// sesudah menunggu bukan pengumuman.
    static assets::LightmapAtlasLayout PlanAtlas(std::span<const PickItem> items,
                                                 assets::MeshGeometryCache* cache,
                                                 const Settings& settings);

    explicit LightmapBakery(TaskPool* tasks = nullptr);
    ~LightmapBakery();

    LightmapBakery(const LightmapBakery&) = delete;
    LightmapBakery& operator=(const LightmapBakery&) = delete;

    void SetCache(assets::MeshGeometryCache* cache) { cache_ = cache; }

    /// Memulai panggangan. **Dipanggil di main thread**, dengan aturan yang sama
    /// seperti `ProbeBakery::Bake`: geometrinya disalin sebelum tugasnya
    /// berangkat, dan langitnya disalin ke dalamnya.
    bool Bake(std::span<const PickItem> items, std::function<Vec3(const Vec3&)> sky,
              const Settings& settings);

    bool Running() const;
    float Progress() const;
    std::shared_ptr<const render::Lightmap> Take();
    std::string Status() const;

private:
    struct Job;

    TaskPool* tasks_ = nullptr;
    assets::MeshGeometryCache* cache_ = nullptr;
    std::shared_ptr<Job> job_;
};

}  // namespace sim::view
