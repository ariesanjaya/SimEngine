#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Render/ScreenProbe.h"

#include <cstdint>
#include <span>

namespace sim::render {

// --- Reproyeksi --------------------------------------------------------------

/// Hasil mencari sebuah titik dunia di kisi probe frame sebelumnya.
struct ProbeReprojection {
    /// False bila titiknya tidak ada di layar frame sebelumnya. Riwayatnya lalu
    /// tidak ada — bukan nol, dan bukan pula boleh dipakai.
    bool onScreen = false;
    /// Koordinat ubin pecahan di kisi frame sebelumnya.
    Vec2 tile{0.0f};
};

/// Mencari sebuah titik dunia di kisi probe frame sebelumnya.
///
/// **Inilah yang membuat riwayat bertahan saat kamera bergerak.** Sampai M4,
/// riwayat probe dibuang seluruhnya begitu kamera berpindah: riwayatnya terikat
/// ke piksel, dan piksel yang sama menunjuk permukaan yang berbeda. Reproyeksi
/// mengikat riwayat ke **dunia**, bukan ke piksel — titik yang sama dicari di
/// tempatnya berada frame lalu.
ProbeReprojection ReprojectProbe(const Vec3& worldPosition, const Mat4& previousViewProj,
                                 const glm::uvec2& viewport, uint32_t tileSize);

// --- Penyaring A-trous -------------------------------------------------------

struct AtrousSettings {
    /// Selisih jarak tegak lurus bidang yang masih dianggap permukaan yang sama,
    /// meter.
    float planeDistance = 0.15f;
    /// Kesamaan normal minimum.
    float normalCosine = 0.7f;
};

/// Bobot kernel B-spline 5×5 pada offset `(dx, dy)`.
///
/// **B-spline, bukan kotak.** Kernel kotak menyebarkan tepi menjadi tangga yang
/// lebarnya persis lebar kernel, dan tangga itu justru terlihat lebih buruk
/// daripada derau yang dihilangkannya. B-spline meluruh mulus dan jumlah seluruh
/// bobotnya satu, jadi bidang yang rata tetap rata.
float AtrousKernel(int dx, int dy);

/// Satu lintasan à-trous atas medan iradiansi.
///
/// **À-trous, bukan Gaussian bertingkat.** Lintasan ke-`n` melompat 2ⁿ piksel
/// dengan kernel yang sama, jadi dua lintasan menjangkau 20 piksel dengan 50
/// pengambilan — sementara satu Gaussian selebar itu menuntut ratusan. Yang
/// membuatnya sah adalah bobot bilateralnya: lompatan besar tidak mencampur
/// permukaan yang berbeda, karena bobot yang melintasi permukaan bernilai nol.
///
/// `stride` adalah lompatannya: 1 untuk lintasan pertama, 2 untuk yang kedua.
void AtrousPass(std::span<const Vec3> input, std::span<const Vec3> positions,
                std::span<const Vec3> normals, std::span<const uint8_t> valid,
                const glm::uvec2& size, uint32_t stride, const AtrousSettings& settings,
                std::span<Vec3> output);

// --- Respons temporal --------------------------------------------------------

/// Berapa frame sampai akumulasi mencapai `fraction` dari perubahan mendadak.
///
/// **Kriteria selesai M5 adalah angka, dan ini yang mengubahnya menjadi angka.**
/// Rencana menuntut GI merespons lampu dinyalakan-matikan di bawah 200 ms; pada
/// 60 Hz itu dua belas frame. Rata-rata berjalan dengan jendela `maxFrames`
/// meluruh secara eksponensial, jadi waktu responsnya kira-kira `maxFrames ×
/// ln(1/(1−fraction))` — dan itu berarti jendela yang panjang tidak akan pernah
/// memenuhi kriterianya berapa pun bagusnya penyaring spasialnya.
uint32_t FramesToRespond(uint32_t maxFrames, float fraction);

/// Menjepit riwayat ke sekitar rerata tetangga sebelum dicampur.
///
/// **Ini yang membuat respons cepat dan derau rendah bisa hidup bersamaan.**
/// Jendela pendek merespons cepat tapi berderau; jendela panjang sebaliknya.
/// Penjepitan memutus pertukaran itu: selama sampel barunya sejalan dengan
/// tetangganya, riwayat panjang dipertahankan dan deraunya hilang; begitu
/// adegannya benar-benar berubah, riwayat yang sudah tidak sejalan dijepit
/// masuk ke rentang baru dalam satu frame.
///
/// `mean` dan `deviation` dari tetangga spasial sampel sekarang. `scale`
/// mengatur seberapa longgar jepitannya — terlalu ketat menghapus riwayat yang
/// masih sah dan mengembalikan deraunya, terlalu longgar tidak menjepit apa pun.
Vec3 ClampHistory(const Vec3& history, const Vec3& mean, const Vec3& deviation, float scale);

}  // namespace sim::render
