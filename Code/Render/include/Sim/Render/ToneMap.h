#pragma once

#include "Sim/Core/Math.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sim::render {

// --- Operator nada -----------------------------------------------------------

/// Kurva ACES (pendekatan Stephen Hill: matriks masuk, kecocokan RRT+ODT,
/// matriks keluar).
///
/// **Bukan Reinhard, dan bukan pemotongan.** Yang membedakan keduanya bukan
/// keindahan melainkan apa yang terjadi pada sorotan berwarna: pemotongan per
/// kanal menggeser rona ke arah kanal yang belum jenuh — matahari jingga
/// terpotong menjadi kuning, lalu menjadi putih, dan pergeserannya terjadi
/// justru di piksel paling terang yang paling diperhatikan. ACES memetakan
/// ketiga kanal lewat matriks bersama, jadi rona bertahan sampai ke putih.
Vec3 AcesToneMap(const Vec3& color);

/// Kurva ACES untuk satu kanal, tanpa matriksnya. Dipakai menguji bentuk
/// kurvanya sendiri.
float AcesCurve(float value);

/// Nilai masukan yang tepat menjadi 1,0 pada kurva ACES, ~25,7.
///
/// **Bukan 1,0, dan itu maksudnya.** Operator nada yang memetakan 1,0 ke putih
/// tidak menyisakan tempat untuk sorotan; seluruh gunanya adalah bahwa ada
/// rentang di atas putih layar yang masih bisa dibedakan.
float AcesWhitePoint();

// --- Eksposur ----------------------------------------------------------------

/// EV100 dari luminansi rata-rata adegan, dengan konstanta kalibrasi
/// cahaya-pantul K = 12,5 dan ISO 100 — angka yang dipakai pengukur cahaya
/// sungguhan.
float Ev100FromLuminance(float averageLuminance);

/// Pengali eksposur dari EV100 (formulasi Lagarde–de Rousiers berbasis
/// kejenuhan).
float ExposureFromEv100(float ev100);

/// Pengali eksposur langsung dari luminansi rata-rata, ditambah kompensasi
/// dalam stop.
///
/// **Sifat yang menentukan benar-tidaknya: tidak bergantung skala.** Adegan
/// seragam berluminansi berapa pun, sesudah diukur dan dikalikan, selalu
/// menjadi 1/9,6 ≈ 0,104. Itulah arti "eksposur otomatis bekerja" — bukan
/// bahwa gambarnya enak dilihat, melainkan bahwa menaikkan seluruh lampu
/// sepuluh kali lipat tidak mengubah apa pun di layar. Sebuah eksposur yang
/// tidak punya sifat ini akan terlihat benar pada satu adegan dan salah pada
/// semua adegan lain, tanpa satu pun galat.
float AutoExposure(float averageLuminance, float compensationStops = 0.0f);

/// Luminansi menurut Rec. 709.
float Luminance(const Vec3& color);

// --- Pengukuran luminansi ----------------------------------------------------

/// Rata-rata log-luminansi lewat rantai penurunan 2×2 — bentuk yang sama persis
/// dengan rantai mip yang dijalankan GPU.
///
/// **Rata-rata geometrik, bukan aritmetik.** Rata-rata aritmetik luminansi
/// didominasi beberapa piksel paling terang: satu sorotan spekular bisa
/// menggelapkan seluruh adegan, dan yang terlihat bukan sorotan yang terlalu
/// terang melainkan segalanya yang terlalu gelap — cacat yang sumbernya berada
/// di tempat lain daripada tampaknya.
///
/// **Rantainya berangkat dari petak berukuran pangkat dua, bukan dari ukuran
/// viewport.** Penurunan 2×2 atas ukuran ganjil harus membuang satu baris atau
/// menimbang tiga texel, dan keduanya memasukkan bias yang bergantung ukuran
/// jendela — eksposur yang berubah sedikit saat pemisah dock digeser adalah
/// cacat yang tidak akan pernah dicurigai orang. Lintasan pertama menyalin
/// viewport seukuran berapa pun ke petak tetap ini dengan penyaringan bilinear;
/// sesudah itu setiap tingkat membagi dua dengan tepat.
class LogLuminanceReducer {
public:
    /// Sisi petak awal. 256×256 memberi sembilan tingkat penurunan dan sudah
    /// jauh lebih halus daripada yang bisa dibedakan sebuah pengukur cahaya.
    static constexpr uint32_t kSeedSize = 256;

    /// Lantai luminansi. **Wajib, dan bukan kehati-hatian berlebihan:**
    /// log2(0) adalah negatif tak hingga, dan satu piksel hitam sudah cukup
    /// membuat seluruh rata-rata menjadi NaN — yang muncul sebagai layar hitam
    /// atau putih penuh, bukan sebagai galat.
    static constexpr float kMinLuminance = 1e-4f;

    /// Banyaknya tingkat dari `kSeedSize` sampai 1×1.
    static uint32_t LevelCount();

    /// Menurunkan satu tingkat dengan rata-rata 2×2 tepat.
    static std::vector<float> ReduceLevel(const std::vector<float>& source, uint32_t size);

    /// Log-luminansi sebuah warna, sudah berlantai.
    static float LogLuminanceOf(const Vec3& color);

    /// Menyalin gambar seukuran berapa pun ke petak awal, dengan merata-ratakan
    /// piksel yang jatuh di tiap texel.
    static std::vector<float> Seed(const std::vector<Vec3>& pixels, uint32_t width,
                                   uint32_t height);

    /// Menjalankan seluruh rantai sampai satu texel, lalu mengembalikannya ke
    /// luminansi.
    static float Reduce(const std::vector<Vec3>& pixels, uint32_t width, uint32_t height);
};

// --- Adaptasi ----------------------------------------------------------------

/// Eksposur yang mengejar sasarannya dalam waktu, bukan melompat ke sana.
///
/// **Dua tetapan waktu, bukan satu.** Mata menyesuaikan diri ke terang jauh
/// lebih cepat daripada ke gelap, dan lebih penting lagi: satu tetapan waktu
/// memaksa memilih antara kedipan saat berbalik menghadap matahari dan
/// keterlambatan saat masuk ke lorong. Keduanya cacat yang berbeda dan
/// keduanya terlihat.
class ExposureAdaptation {
public:
    /// Tetapan waktu dalam detik.
    ExposureAdaptation(float brightenSeconds = 0.4f, float darkenSeconds = 1.2f)
        : brighten_(brightenSeconds), darken_(darkenSeconds) {}

    /// Menyetel nilai sekarang tanpa transisi. Dipakai pada frame pertama:
    /// memulai dari nol berarti seluruh adegan menyala dari gelap gulita setiap
    /// kali viewport dibuka.
    void Reset(float exposure) { current_ = exposure; ready_ = true; }

    bool Ready() const { return ready_; }
    float Current() const { return current_; }

    /// Memajukan satu langkah waktu menuju `target`, lalu mengembalikannya.
    float Update(float target, float deltaSeconds);

private:
    float brighten_;
    float darken_;
    float current_ = 1.0f;
    bool ready_ = false;
};

}  // namespace sim::render
