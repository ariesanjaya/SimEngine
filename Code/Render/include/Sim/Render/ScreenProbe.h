#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>

namespace sim::render {

// --- Pemetaan oktahedral -----------------------------------------------------

/// Arah satuan → kotak [0,1]².
///
/// **Oktahedral, bukan kubus atau bola.** Pemetaan kubus menuntut enam sisi dan
/// karena itu enam kali pemilihan sisi di setiap pembacaan; pemetaan bola
/// (latitude/longitude) memusatkan hampir seluruh texel-nya di kutub. Oktahedral
/// memetakan seluruh bola ke satu kotak dengan luasan yang hampir seragam — dan
/// keseragaman itulah yang membuat 16 texel per probe cukup untuk mewakili
/// seluruh arah.
Vec2 OctEncode(const Vec3& direction);

/// Kotak [0,1]² → arah satuan. Kebalikan `OctEncode`.
Vec3 OctDecode(const Vec2& uv);

// --- Kisi probe --------------------------------------------------------------

struct ProbeGridSettings {
    /// Piksel per sisi ubin. Satu probe per ubin.
    uint32_t tileSize = 16;
    /// Akar jumlah ray per probe per frame. 4 berarti 4×4 = 16 arah.
    uint32_t raysPerAxis = 4;
    /// Banyaknya frame yang diakumulasi.
    ///
    /// **Lima, bukan enam belas, dan angkanya datang dari pengukuran.** Kriteria
    /// selesai M5 menuntut GI merespons lampu dinyalakan-matikan di bawah 200 ms
    /// — dua belas frame pada 60 Hz. `FramesToRespond` menunjukkan jendela 16
    /// butuh 36 frame (600 ms) dan jendela 5 butuh 11 (183 ms). Rencana GI
    /// menyebut 8–16, dan angka itu ternyata tidak pernah bisa memenuhi
    /// kriterianya sendiri; yang membuat jendela sependek ini bisa ditoleransi
    /// adalah penyaring à-trous dan penjepitan riwayat.
    uint32_t accumulationFrames = 5;
    /// Pergeseran titik asal sinar sepanjang normal permukaan, dalam voxel
    /// kaskade SDF terhalus.
    ///
    /// **Tanpa ini setiap sinar mengenai permukaan tempat ia berangkat.** Titik
    /// asal probe direkonstruksi dari depth buffer, jadi ia duduk tepat di
    /// permukaan — dan sphere tracing yang mulai di permukaan berhenti pada
    /// langkah pertama, karena jarak di sana nol sedangkan ambang berhentinya
    /// setengah voxel. Arah sinarnya tidak berpengaruh sama sekali: yang
    /// menunjuk lurus menjauh pun mengenai, pada jarak nol.
    ///
    /// Gejalanya bukan galat melainkan gambar yang tampak masuk akal — sinar
    /// yang "mengenai" dirinya sendiri lalu dibuang karena cache belum
    /// mengenalnya, dan probe yang seluruh sinarnya dibuang mengembalikan hitam.
    /// Inilah sebab dinding Cornell box tetap gelap sesudah `traceRange`
    /// diperbaiki: keduanya berbagi akar yang sama, tapi yang ini tidak
    /// bergantung pada jangkauan trace sama sekali.
    ///
    /// Satu voxel, bukan setengah: ambangnya sendiri setengah voxel, jadi
    /// setengah voxel adalah tepat di batasnya — dan sampel trilinear di dekat
    /// permukaan meleset cukup untuk melewatinya.
    ///
    /// **Voxel kaskade setempat, bukan voxel kaskade terhalus.** Ambangnya
    /// mengikuti kaskade tempat titiknya berada, dan kaskade terkasar bervoxel
    /// enam belas kali lebih besar; sebuah bias yang dihitung sekali dari
    /// kaskade nol karena itu terlalu kecil di mana-mana kecuali di dekat
    /// kamera.
    float normalBiasVoxels = 1.0f;
    /// Panjang urutan jitter sebelum ia berulang.
    ///
    /// **Terpisah dari jendela akumulasi, dan pemisahannya lahir dari sebuah
    /// kesalahan.** Keduanya sempat satu angka, dan itu tampak masuk akal —
    /// sampai jendelanya dipendekkan dari 16 ke 5 demi kriteria respons M5, dan
    /// urutan jitternya ikut runtuh menjadi lima pola yang berulang selamanya.
    /// Uji tungku yang menangkapnya: iradiansi meleset 10% dari πL karena arah
    /// yang tersedia tidak lagi menutupi bola dengan merata. Keduanya menjawab
    /// pertanyaan yang berbeda — yang satu seberapa cepat GI merespons
    /// perubahan, yang lain berapa banyak pola sampel berbeda yang ada sebelum
    /// berulang — dan menyatukannya berarti tidak bisa menyetel salah satunya
    /// tanpa merusak yang lain.
    uint32_t jitterPeriod = 64;
};

/// Kisi probe di atas layar: satu probe per ubin `tileSize`×`tileSize` piksel.
class ProbeGrid {
public:
    void Configure(uint32_t viewportWidth, uint32_t viewportHeight,
                   const ProbeGridSettings& settings);

    const ProbeGridSettings& Settings() const { return settings_; }
    glm::uvec2 Counts() const { return counts_; }
    uint32_t ProbeCount() const { return counts_.x * counts_.y; }
    uint32_t RaysPerProbe() const { return settings_.raysPerAxis * settings_.raysPerAxis; }

    /// Piksel tempat probe (`x`, `y`) mengambil posisinya dari depth buffer.
    ///
    /// **Dijepit ke dalam viewport.** Ubin terakhir sebuah layar yang lebarnya
    /// bukan kelipatan ubin hanya terisi sebagian, dan pusat ubin itu jatuh di
    /// luar layar — probe yang mengambil depth dari sana mengambilnya dari
    /// piksel yang tidak pernah digambar.
    Vec2 ProbePixel(uint32_t x, uint32_t y) const;

    /// Koordinat ubin pecahan sebuah piksel. Bagian bulatnya adalah probe kiri
    /// atas dari empat yang menjadi tetangganya, pecahannya bobot bilinearnya.
    Vec2 TileCoordinate(const Vec2& pixel) const;

private:
    ProbeGridSettings settings_;
    glm::uvec2 viewport_{0};
    glm::uvec2 counts_{0};
};

/// Arah ray ke-`ray` sebuah probe pada frame ke-`frame`.
///
/// **Seluruh bola, bukan setengahnya.** Sebuah probe melayani seluruh ubin
/// 16×16 piksel, dan piksel di dalam satu ubin bisa punya normal yang sangat
/// berbeda — di tepi geometri, bahkan berlawanan. Probe yang hanya menelusuri
/// setengah bola di sekitar satu normal tidak punya apa pun untuk diberikan
/// kepada piksel yang normalnya menghadap ke arah lain.
///
/// **Arahnya berjitter tiap frame, dan jitternya berbeda tiap probe.** Enam
/// belas arah tetap menghasilkan enam belas berkas cahaya yang sama di setiap
/// frame: bukan derau melainkan pola, dan pola tidak hilang oleh akumulasi
/// berapa pun lamanya. Jitter yang sama di seluruh probe sama buruknya — polanya
/// lalu terlihat sebagai kisi ubin.
///
/// Urutannya **deterministik**: sama dengan alasan sampel IBL deterministik —
/// gambar yang berbeda tiap kali dijalankan tidak bisa dibandingkan dengan
/// gambar acuan, dan test yang hasilnya berubah tiap kali dijalankan bukan test.
Vec3 ProbeRayDirection(uint32_t ray, uint32_t frame, uint32_t probeIndex,
                       const ProbeGridSettings& settings);

// --- Integrasi ---------------------------------------------------------------

/// Satu sampel radiance sebuah probe.
struct ProbeRay {
    Vec3 direction{0.0f, 1.0f, 0.0f};
    Vec3 radiance{0.0f};
};

/// Iradiansi pada sebuah normal dari sekumpulan sampel arah seragam.
///
/// Penaksir Monte Carlo untuk ∫ L(ω)·cos θ dω atas setengah bola, dengan sampel
/// yang tersebar seragam di **seluruh** bola: 4π/N × Σ L·maks(0, n·ω). Faktor
/// 4π-nya luas bola, bukan 2π: yang disampel seluruh bola, dan separuhnya yang
/// di belakang permukaan menyumbang nol lewat `maks`.
Vec3 IntegrateIrradiance(const ProbeRay* rays, uint32_t count, const Vec3& normal);

/// Radiansi sebuah probe sebagai SH orde satu, satu vektor koefisien per kanal.
///
/// **SH, bukan enam belas texel oktahedral yang disimpan apa adanya.** Yang
/// dibutuhkan piksel adalah iradiansi — integral radiansi terhadap cosinus — dan
/// integral itu memangkas seluruh frekuensi tinggi. Menyimpan arahnya utuh
/// berarti setiap piksel membaca 16 texel dari masing-masing empat probe
/// tetangganya lalu menjumlahkannya kembali menjadi satu angka yang hanya punya
/// empat derajat kebebasan. Orde satu, bukan dua: dengan 16 ray per frame,
/// koefisien orde dua lebih banyak berisi derau daripada isyarat.
///
/// Tata letaknya **satu vektor empat per kanal warna**, bukan empat vektor tiga
/// per koefisien: itu bentuk yang muat persis di tiga lampiran RGBA16F, dan
/// lampiran adalah satuan yang bisa ditulis satu pass di GPU.
struct ProbeSh {
    Vec4 r{0.0f};
    Vec4 g{0.0f};
    Vec4 b{0.0f};
};

/// Memproyeksikan sampel arah seragam ke SH orde satu.
ProbeSh ProjectProbeSh(const ProbeRay* rays, uint32_t count);

/// Iradiansi pada sebuah normal. Konvolusi cosinus-nya sudah termasuk, dengan
/// faktor yang sama dengan `EvaluateIrradiance` di `Ibl.h`: π untuk orde nol,
/// 2π/3 untuk orde satu.
Vec3 EvaluateProbeSh(const ProbeSh& sh, const Vec3& normal);

/// Campuran linear dua probe. Dipakai interpolasi ke piksel.
ProbeSh BlendProbeSh(const ProbeSh& sh, float weight);
ProbeSh AddProbeSh(const ProbeSh& a, const ProbeSh& b);

/// Rata-rata berjalan atas `maxFrames` frame terakhir.
///
/// **Jumlah sampel dibatasi, bukan dibiarkan tumbuh.** Rata-rata sejati atas
/// seluruh riwayat berhenti merespons perubahan setelah beberapa detik — dan
/// yang berubah bukan hanya lampu melainkan juga geometri yang bergerak.
/// Membatasinya membuat bobot frame terbaru tidak pernah turun di bawah
/// 1/maxFrames, jadi responsnya punya batas atas yang bisa disebut.
Vec3 AccumulateProbe(const Vec3& history, const Vec3& current, uint32_t frames,
                     uint32_t maxFrames);

// --- Interpolasi ke piksel ---------------------------------------------------

/// Keadaan permukaan tempat sebuah probe duduk.
struct ProbeSurface {
    Vec3 position{0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    /// False bila probe-nya jatuh di langit: tidak ada permukaan untuk dipijak,
    /// dan iradiansinya tidak berarti apa-apa.
    bool valid = false;
};

struct ProbeFilterSettings {
    /// Selisih jarak sepanjang normal yang masih dianggap permukaan yang sama,
    /// meter. Di atasnya bobotnya nol.
    float planeDistance = 0.1f;
    /// Kesamaan normal minimum. Di bawahnya bobotnya nol.
    float normalCosine = 0.5f;
};

/// Bobot sebuah probe untuk sebuah piksel.
///
/// **Jaraknya diukur tegak lurus bidang piksel, bukan sebagai jarak lurus.**
/// Dua titik di sebuah lantai yang sama bisa berjarak beberapa meter dan tetap
/// merupakan permukaan yang sama; dua titik yang berjarak sepuluh sentimeter
/// tapi terpisah oleh sebuah dinding bukan. Yang membedakan keduanya jarak ke
/// bidangnya, bukan jarak antar-titiknya — dan memakai jarak lurus membuat
/// cahaya merembes menembus sudut ruangan.
float ProbeWeight(const ProbeSurface& probe, const Vec3& pixelPosition,
                  const Vec3& pixelNormal, const ProbeFilterSettings& settings);

}  // namespace sim::render
