#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <vector>

namespace sim::render {

/// Satu tingkat rantai bloom.
struct BloomLevel {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<Vec3> pixels;

    const Vec3& At(uint32_t x, uint32_t y) const {
        return pixels[static_cast<std::size_t>(y) * width + x];
    }
    Vec3& At(uint32_t x, uint32_t y) {
        return pixels[static_cast<std::size_t>(y) * width + x];
    }
};

/// Pengaturan bloom.
struct BloomSettings {
    bool enabled = true;
    /// Ambang luminansi. Di bawahnya tidak ikut berpendar.
    float threshold = 1.0f;
    /// Lebar lutut peralihan, dalam luminansi. **Bukan nol:** ambang yang tajam
    /// membuat permukaan yang luminansinya melintasi ambang berkedip antara
    /// berpendar dan tidak — dan permukaan seperti itu adalah justru yang bergerak
    /// pelan di layar, misalnya sorotan pada benda yang berputar.
    float knee = 0.5f;
    /// Seberapa jauh pendarannya menyebar antar-tingkat, 0..1.
    float scatter = 0.7f;
    /// Kekuatan pendaran yang **ditambahkan** ke adegan.
    ///
    /// Ditambahkan, bukan dipadu — dan itu keharusan yang mengikuti dari
    /// ambangnya. Rantai ini berambang: semua yang di bawah ambang bernilai nol
    /// di dalamnya. Memadukan adegan dengan nol berarti **menggelapkan setiap
    /// piksel yang tidak berpendar**, yang terlihat sebagai seluruh adegan yang
    /// meredup begitu bloom dinyalakan — bukan sebagai pendaran yang salah.
    /// Paduan hanya sah untuk rantai tanpa ambang.
    float strength = 0.05f;
};

/// Rantai bloom: penurunan berjenjang lalu penaikan kembali.
///
/// **Penaikannya memadu, bukan menambahkan.** Bloom yang menambahkan tiap
/// tingkat menaikkan energi seluruh gambar — dan energi yang naik akan diukur
/// oleh eksposur otomatis, yang lalu menurunkan eksposur, yang menurunkan bloom.
/// Keduanya saling mengejar dan yang terlihat adalah kecerahan yang bergoyang
/// pelan tanpa sebab. Dengan paduan, gambar konstan lewat rantai ini tanpa
/// berubah sama sekali — dan itulah uji yang menentukan.
class BloomChain {
public:
    /// Banyaknya tingkat sampai sisi terpendek mencapai `minSize`.
    static uint32_t LevelsFor(uint32_t width, uint32_t height, uint32_t minSize = 8);

    /// Ambang berlutut lembut. Mengembalikan pengali 0..1 untuk sebuah luminansi.
    static float SoftThreshold(float luminance, float threshold, float knee);

    /// Bobot 13 cuplikan penurunan (Jimenez). Lima kelompok: satu di tengah
    /// berbobot 0,5 dan empat di sudut masing-masing 0,125.
    ///
    /// **Jumlahnya harus tepat satu.** Jumlah yang meleset sedikit saja akan
    /// mengalikan gambar pada setiap tingkat, dan pada tujuh tingkat selisih satu
    /// persen menjadi tujuh persen — sebuah pergeseran kecerahan yang tampak
    /// seperti bloom yang "terlalu kuat" alih-alih seperti tapis yang salah.
    static float DownsampleWeightSum();

    /// Menurunkan satu tingkat dengan tapis 13 cuplikan.
    /// `karis` menyalakan pembobotan 1/(1+luma) per kelompok — dipakai hanya pada
    /// penurunan pertama, tempat satu piksel sangat terang bisa mendominasi.
    static BloomLevel Downsample(const BloomLevel& source, bool karis);

    /// Menaikkan `source` ke ukuran `target` dengan tapis tenda 3×3, lalu
    /// memadunya ke `target` sebanyak `scatter`.
    static void UpsampleInto(BloomLevel& target, const BloomLevel& source, float scatter);

    /// Rantai penuh: ambang, turun, naik. Mengembalikan tingkat nol.
    static BloomLevel Build(const BloomLevel& scene, const BloomSettings& settings);

    /// Campuran akhir: `scene + bloom * strength`.
    static BloomLevel Composite(const BloomLevel& scene, const BloomLevel& bloom, float strength);
};

}  // namespace sim::render
