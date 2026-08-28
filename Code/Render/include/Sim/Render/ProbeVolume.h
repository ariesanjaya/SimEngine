#pragma once

#include "Sim/Render/Ibl.h"

#include <cstdint>
#include <filesystem>
#include <span>
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

// --- pengalamatan kisi ------------------------------------------------------
//
// **Dipublikkan supaya bisa diadu dengan `Shaders/probe_grid.slang`.** Yang
// membaca kisi saat menggambar adalah shader; yang menulisnya adalah berkas ini.
// Dua pengalamatan yang berselisih tidak menghasilkan galat apa pun — hanya
// cahaya tak-langsung yang bergeser beberapa meter, yang terbaca sebagai
// transport yang salah alih-alih sebagai indeks yang salah. Ujinya menjalankan
// versi slang-nya lewat `slangc -target cpp` dan membandingkan keduanya.

/// Indeks linear brick yang memuat sebuah probe, urutan x-cepat.
uint32_t ProbeBrickIndex(const ProbeVolumeLayout& layout, const glm::uvec3& probe);

/// Indeks probe di dalam `probes` bila brick-nya menempati `slot`.
uint32_t ProbeSlotOffset(uint32_t slot, const glm::uvec3& probe);

/// Sel kisi yang memuat sebuah titik dunia, beserta pecahannya. Yang di luar
/// kisi dijepit ke tepinya.
void ProbeCell(const ProbeVolumeLayout& layout, const Vec3& position, glm::uvec3& outBase,
               Vec3& outFraction);

/// Koordinat probe sebuah sudut sel, dijepit ke dalam kisi.
glm::uvec3 ProbeCorner(const ProbeVolumeLayout& layout, const glm::uvec3& base, uint32_t corner);

/// Bobot trilinear sebuah sudut sel.
float ProbeCornerWeight(uint32_t corner, const Vec3& fraction);

/// Kisi beserta isinya.
struct ProbeVolume {
    ProbeVolumeLayout layout;
    /// Satu entri per brick, menurut urutan x-cepat: slot brick di dalam
    /// `probes`, atau `kEmptyBrick`.
    std::vector<uint32_t> brickSlots;
    /// SH tiap probe, brick demi brick. Satu brick menempati
    /// `kBrickSize³` entri berurutan, urutan x-cepat di dalamnya.
    std::vector<Sh9> probes;

    /// Peta kedalaman oktahedral tiap probe (S3): dua float per texel — rata-rata
    /// jarak, lalu rata-rata kuadratnya. Sejajar dengan `probes`, dan kosong
    /// berarti kisi ini dipanggang sebelum S3 ada.
    ///
    /// **Ini yang membedakan probe di depan dinding dari probe di dalamnya.**
    /// Yang di dalam dipanggang tepat nol dan bocor ke permukaan di dekatnya;
    /// diukur pada `bench.simlevel`, 246 dari 21.312 probe pada jarak 1 m.
    std::vector<float> depth;

    /// Texel peta kedalaman per sumbu. Harus sama dengan `kProbeDepthSize` di
    /// `Shaders/probe_grid.slang`; ujinya mengadu keduanya.
    static constexpr uint32_t kDepthSize = 8;
    /// Float per probe di dalam `depth`.
    static constexpr uint32_t kDepthFloats = kDepthSize * kDepthSize * 2;

    bool IsValid() const {
        return layout.IsValid() && brickSlots.size() == layout.BrickCount() &&
               (depth.empty() || depth.size() == probes.size() * kDepthFloats);
    }

    /// True bila kisi ini membawa visibilitas arah (S3).
    bool HasVisibility() const { return !depth.empty(); }
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

/// Kotak dunia yang ditempati sesuatu — satu instance mesh, biasanya.
struct ProbeOccupancy {
    Vec3 minimum{0.0f};
    Vec3 maximum{0.0f};
};

/// Tabel slot brick: brick yang dekat salah satu kotak mendapat slot berurutan,
/// sisanya `kEmptyBrick` (S2, keputusan 9 di docs/PLAN-STATIC-GI.md).
///
/// **Kisi beraturan membayar probe untuk ruang kosong dan untuk ruang di dalam
/// benda pejal.** Pada adegan besar itulah yang menghabiskan anggarannya, dan
/// pertumbuhannya kubik: memperhalus dua kali lipat membayar delapan kali.
/// Dengan brick yang dibuang, biayanya mengikuti luas permukaan alih-alih
/// volume.
///
/// **`margin` bukan hiasan.** Sebuah titik di permukaan membaca delapan sudut
/// selnya, dan sudut yang paling jauh berada satu jarak-antar-probe darinya —
/// jadi brick yang hanya "menyentuh" geometri tidak cukup. Margin yang terlalu
/// kecil menghasilkan permukaan yang membaca sudut yang tidak ada, dan
/// normalisasi bobot menyelamatkan kecerahannya tetapi bukan arahnya.
///
/// Kotak kosong berarti seluruh brick dialokasikan — jawaban yang benar untuk
/// pemanggil yang belum tahu di mana geometrinya.
std::vector<uint32_t> AssignProbeBricks(const ProbeVolumeLayout& layout,
                                        std::span<const ProbeOccupancy> volumes, float margin);

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

// --- visibilitas arah (S3) --------------------------------------------------
//
// **Cerminan `Shaders/probe_grid.slang`**, dan keduanya diadu oleh
// `SimProbeGridCpuTests` lewat sumber yang sama yang dikompilasi dua kali.

/// Arah unit → koordinat oktahedral di [0,1]².
Vec2 ProbeOctEncode(const Vec3& direction);

/// Koordinat oktahedral → arah unit.
Vec3 ProbeOctDecode(const Vec2& uv);

/// Indeks texel peta kedalaman untuk sebuah arah, di dalam peta satu probe.
uint32_t ProbeDepthTexel(const Vec3& direction);

/// Bobot visibilitas sebuah probe untuk titik berjarak `distance` darinya.
///
/// Mengembalikan satu ketika titiknya lebih dekat daripada geometri terdekat
/// pada arah itu, dan turun ketika ia berada di baliknya. Probe di dalam benda
/// pejal punya `mean` mendekati nol ke segala arah, jadi bobotnya jatuh ke nol
/// tanpa satu pun cabang khusus tentang "di dalam".
float ProbeVisibilityWeight(float distance, float mean, float meanSquare);

/// Geseran titik teduh sepanjang normalnya sebelum visibilitasnya ditanyakan,
/// meter. **Harus sama dengan `kProbeVisibilityBias` di
/// `Shaders/probe_grid.slang`**; ujinya mengadu keduanya.
inline constexpr float kProbeVisibilityBias = 0.15f;

/// Iradiansi pada sebuah posisi dunia, dengan bobot visibilitas ikut
/// diperhitungkan bila kisinya membawanya (S3).
///
/// **Terpisah dari `SampleProbeVolume`, bukan menggantikannya**, karena keduanya
/// menjawab pertanyaan yang berbeda: yang lama "apa isi kisi di sini", yang ini
/// "apa yang diterima permukaan di sini". Yang kedua menuntut tahu di mana
/// permukaannya, dan itu argumen yang tidak dipunyai yang pertama.
///
/// `normal` menggeser titik yang ditanyakan ke peta kedalaman sejauh
/// `kProbeVisibilityBias`; nol berarti tanpa geseran — jalur yang benar untuk
/// pemanggil yang memang tidak punya permukaan, misalnya sebuah probe yang
/// menanyai tetangganya.
Sh9 SampleProbeVolumeAt(const ProbeVolume& volume, const Vec3& position,
                        const Vec3& normal = Vec3(0.0f));

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
