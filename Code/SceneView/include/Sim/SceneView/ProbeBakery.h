#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Render/ProbeVolume.h"
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

/// Memanggang transport cahaya statis ke dalam kisi probe (S2 di
/// docs/PLAN-STATIC-GI.md).
///
/// **Pengondisi aset, bukan bagian jalur gambar** — kedudukan yang sama dengan
/// `assets::MeshSdfBakery`. Ia menelusuri sinar dengan path tracer acuan, dan
/// itu menuntut Embree beserta geometri CPU; renderer hanya menerima kisinya
/// yang sudah jadi.
///
/// **Ada hanya bila `Sim::Reference` ada.** Modul itu dilewati ketika slangc
/// tidak ditemukan, karena model shading-nya dibangkitkan darinya. Yang
/// memakainya wajib memeriksa — pola yang sama dengan runtime Lua di
/// `EditorContext`.
class ProbeBakery {
public:
    struct Settings {
        render::ProbeVolumeLayout layout;
        /// Arah yang dicuplik tiap probe. **Yang menentukan derau, dan
        /// derau di sini tidak hilang saat menggambar:** ia terpanggang.
        uint32_t samplesPerProbe = 128;
        /// Iradiansi matahari pada bidang tegak lurus — dipakai NEE saja,
        /// sehingga yang terpanggang pantulannya dan bukan mataharinya sendiri.
        Vec3 sunIrradiance{0.0f};
        /// Arah rambat cahayanya.
        Vec3 sunDirection{0.0f, -1.0f, 0.0f};
        /// Albedo yang diandaikan untuk seluruh permukaan.
        ///
        /// **Sebuah tebakan, dan disebut tebakan** — angka dan alasan yang sama
        /// dengan `kBounceAlbedo` di jalur clipmap SDF: 0,5 adalah albedo
        /// rata-rata bahan bangunan. Albedo per-material menuntut menjalankan
        /// graph material di CPU untuk tiap segitiga, dan itu pekerjaan
        /// tersendiri; sampai ada, yang meleset kecerahan pantulannya, bukan
        /// keberadaannya.
        float albedo = 0.5f;
        /// Tempat artefak `.simprobe` ditulis. Kosong berarti tidak ditulis.
        std::filesystem::path cacheDir;
        /// Ikut ke dalam kunci artefak: dua lingkungan berbeda tidak boleh
        /// berbagi berkas.
        uint64_t environmentKey = 0;
    };

    explicit ProbeBakery(TaskPool* tasks = nullptr);
    ~ProbeBakery();

    ProbeBakery(const ProbeBakery&) = delete;
    ProbeBakery& operator=(const ProbeBakery&) = delete;

    void SetCache(assets::MeshGeometryCache* cache) { picks_.SetCache(cache); }

    /// Memulai panggangan. **Dipanggil di main thread**, dan geometrinya sudah
    /// harus tersalin ke `PickScene` sebelum tugasnya berangkat: `Sync`
    /// menyentuh cache geometri, yang bukan milik thread mana pun.
    ///
    /// `sky` disalin ke dalam tugasnya, jadi ia harus memiliki apa yang
    /// dicuplikinya — sebuah lambda yang menangkap pointer ke langit adegan akan
    /// menggantung begitu levelnya ditutup di tengah panggangan.
    ///
    /// Mengembalikan false bila tidak ada geometri, kisinya tidak sah, atau
    /// sebuah panggangan masih berjalan.
    bool Bake(std::span<const PickItem> items, std::function<Vec3(const Vec3&)> sky,
              const Settings& settings);

    bool Running() const;
    /// 0..1. Angka yang ditampilkan panel, dan alasannya bukan hiasan:
    /// panggangan yang memakan puluhan detik tanpa angka terbaca sebagai editor
    /// yang menggantung.
    float Progress() const;

    /// Hasil yang sudah selesai, sekali. Null bila belum ada.
    std::shared_ptr<const render::ProbeVolume> Take();

    /// Pesan dari panggangan terakhir, ditampilkan panel apa adanya.
    std::string Status() const;

private:
    struct Job;

    TaskPool* tasks_ = nullptr;
    PickScene picks_;
    std::shared_ptr<Job> job_;
};

}  // namespace sim::view
