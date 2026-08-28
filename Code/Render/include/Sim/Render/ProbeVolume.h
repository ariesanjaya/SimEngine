#pragma once

#include "Sim/Render/Ibl.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sim::render {

/// Kisi iradiansi yang dipanggang, disimpan sebagai brick (S1 di
/// docs/PLAN-STATIC-GI.md).
///
/// **Brick sejak awal, walaupun S1 belum membuang satu pun.** Indireksinya tidak
/// bisa ditambahkan belakangan dengan murah: pencarian di shader, format
/// artefak, dan cara editor melaporkan ukurannya semuanya bergantung padanya.
/// Yang dibuat S1 adalah tempatnya; yang mengosongkan brick yang jauh dari
/// permukaan adalah S2, karena ia satu-satunya yang sudah menelusuri geometri
/// dan karena itu sudah tahu di mana permukaannya.
///
/// **Kisinya beraturan dan berjangkar adegan, bukan kamera.** Clipmap SDF di
/// mesin ini berkaskade dan mengikuti kamera, dan seluruh rancangannya —
/// pengalamatan toroidal, pengancingan titik asal — ada karena kameranya
/// bergerak. Panggangan tidak punya kamera.
struct ProbeVolumeLayout {
    /// Posisi dunia probe (0,0,0).
    Vec3 origin{0.0f};
    /// Jarak antar-probe, meter. Setelan pengarang; lihat keputusan 8.
    float spacing = 1.0f;
    /// Jumlah probe tiap sumbu.
    glm::uvec3 counts{0u};

    /// Probe per sumbu di dalam satu brick.
    ///
    /// **Empat, dan itu bukan angka bulat yang kebetulan.** Satu brick lalu
    /// memuat 64 probe — cukup besar untuk membuat tabel indeksnya kecil, cukup
    /// kecil untuk membuat brick yang dibuang S2 benar-benar berarti. Brick 8³
    /// menyimpan 512 probe, dan sebuah dinding tipis di tengahnya membuat
    /// seluruhnya tetap dialokasikan.
    static constexpr uint32_t kBrickSize = 4;

    glm::uvec3 BrickCounts() const {
        return glm::uvec3((counts.x + kBrickSize - 1) / kBrickSize,
                          (counts.y + kBrickSize - 1) / kBrickSize,
                          (counts.z + kBrickSize - 1) / kBrickSize);
    }
    uint32_t BrickCount() const {
        const glm::uvec3 bricks = BrickCounts();
        return bricks.x * bricks.y * bricks.z;
    }
    /// Jumlah probe kalau setiap brick ada. **Bukan jumlah probe yang tersimpan**
    /// — itu `ProbeVolume::probes.size()`, dan keduanya berbeda begitu S2
    /// membuang brick.
    uint32_t FullProbeCount() const {
        return BrickCount() * kBrickSize * kBrickSize * kBrickSize;
    }
    bool IsValid() const {
        return counts.x > 0 && counts.y > 0 && counts.z > 0 && spacing > 0.0f;
    }

    /// Posisi dunia sebuah probe.
    Vec3 ProbePosition(const glm::uvec3& probe) const {
        return origin + Vec3(probe) * spacing;
    }
};

/// Brick yang tidak dialokasikan.
inline constexpr uint32_t kEmptyBrick = 0xFFFFFFFFu;

/// Kisi beserta isinya.
struct ProbeVolume {
    ProbeVolumeLayout layout;
    /// Satu entri per brick, menurut urutan x-cepat: slot brick di dalam
    /// `probes`, atau `kEmptyBrick`.
    std::vector<uint32_t> brickSlots;
    /// SH tiap probe, brick demi brick. Satu brick menempati
    /// `kBrickSize³` entri berurutan, urutan x-cepat di dalamnya.
    std::vector<Sh9> probes;

    bool IsValid() const {
        return layout.IsValid() && brickSlots.size() == layout.BrickCount();
    }
    /// Berapa brick yang benar-benar dialokasikan.
    uint32_t AllocatedBrickCount() const;
    /// Byte yang benar-benar ditempati probe-nya di dalam artefak `.simprobe`.
    ///
    /// **Angka yang ditulis, bukan angka yang dicita-citakan.** Bentuk pertama
    /// melaporkan RGB float16 — 54 byte per probe — sedangkan artefaknya menulis
    /// `Sh9` apa adanya, 108 byte, dan buffer GPU-nya menempati 144. Tiga angka
    /// untuk satu hal, dan yang ditampilkan panel adalah satu-satunya yang tidak
    /// pernah dibayar siapa pun.
    uint64_t StoredBytes() const;

    /// Byte yang ditempati kisi ini di dalam buffer GPU: sembilan `Vec4` per
    /// probe. Lebih besar daripada artefaknya karena std430 menaikkan tiap
    /// anggota larik ke 16 byte — dan itu angka yang dibayar tiap frame, jadi
    /// itu pula yang paling berarti bagi yang menyetel jaraknya.
    uint64_t GpuBytes() const;
};

/// Menyusun kisi yang menutupi sebuah kotak batas.
///
/// Batasnya dilebarkan ke kelipatan `spacing` supaya seluruh isinya berada di
/// dalam kisi, bukan di tepinya: sebuah permukaan tepat di batas akan
/// mengambil sampel di luar kisi dan mendapat nol.
ProbeVolumeLayout MakeProbeLayout(const Vec3& boundsMin, const Vec3& boundsMax, float spacing);

/// Mengisi seluruh kisi dari sebuah lingkungan. **Tanpa transport sama sekali.**
///
/// Setiap probe menerima iradiansi lingkungan yang sama, karena tanpa geometri
/// tidak ada yang bisa membedakan satu titik dari titik lain. Itu bukan
/// penyederhanaan sementara yang menutupi sesuatu — itu jawaban yang benar untuk
/// tingkat yang belum punya transport, dan itulah yang membuat S1 bisa diadu
/// dengan tingkat panggang seri B: keduanya harus menghasilkan gambar yang sama.
///
/// S2 mengganti isi fungsi ini dengan penelusuran per-probe.
ProbeVolume BakeProbeVolumeFromEnvironment(const ProbeVolumeLayout& layout,
                                           const IEnvironmentSampler& environment,
                                           uint32_t sampleCount = 1024);

/// Iradiansi pada sebuah posisi dunia, interpolasi trilinear atas kisinya.
///
/// **Cerminan pembacaan di shader**, dan keduanya harus bergerak bersama: yang
/// di sini dipakai uji dan path tracer acuan, yang di sana dipakai menggambar,
/// dan dua interpolasi yang berselisih menghasilkan gambar acuan yang tidak bisa
/// dipakai menilai apa pun.
///
/// Posisi di luar kisi dijepit ke tepinya. Mengembalikan nol bila volumenya
/// tidak sah atau brick yang menaunginya tidak dialokasikan.
Sh9 SampleProbeVolume(const ProbeVolume& volume, const Vec3& position);

// --- artefak masak ----------------------------------------------------------

/// Kunci artefak: bentuk kisinya, lingkungan yang mengisinya, dan versi
/// pemanggangnya. Pola yang sama dengan `IblCacheKey`.
uint64_t ProbeVolumeCacheKey(const ProbeVolumeLayout& layout, uint64_t environmentKey);

std::filesystem::path ProbeVolumeCachePath(const std::filesystem::path& cacheDir, uint64_t key);

/// Menulis artefaknya. **Atomik** — berkas sementara lalu dipindahkan, dengan
/// alasan yang sama seperti `WriteIblCache`: proses yang mati di tengah tulis
/// meninggalkan berkas terpotong yang jalan berikutnya temukan sebagai cache
/// yang sah.
bool WriteProbeVolume(const std::filesystem::path& file, const ProbeVolume& volume,
                      std::string& error);

/// Membacanya. Gagal membaca bukan galat — yang benar lalu memanggang ulang.
bool ReadProbeVolume(const std::filesystem::path& file, ProbeVolume& out, std::string& error);

}  // namespace sim::render
