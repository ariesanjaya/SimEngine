#pragma once

#include "Sim/Core/Math.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sim::render {

/// Sumber radiance lingkungan, dibaca per arah.
///
/// Antarmuka, bukan cubemap konkret. Pembakaran IBL adalah pekerjaan sekali per
/// environment, jadi biaya panggilan virtual tidak berarti apa-apa di sini —
/// sementara yang didapat besar: seluruh matematikanya bisa diuji terhadap
/// lingkungan yang jawabannya diketahui secara analitis, tanpa satu berkas
/// gambar pun.
class IEnvironmentSampler {
public:
    virtual ~IEnvironmentSampler() = default;
    /// `direction` ternormalisasi, ruang dunia. Mengembalikan radiance linear.
    virtual Vec3 Sample(const Vec3& direction) const = 0;
};

// --- LUT DFG -----------------------------------------------------------------

/// Dua suku integral BRDF split-sum: `F0 * scale + bias`.
///
/// **Disimpan sebagai skala dan bias, bukan sebagai satu nilai untuk sebuah
/// F0.** Bentuk ini yang membuat satu LUT melayani setiap material: F0 keluar
/// dari integralnya sebagai faktor linear, jadi ia bisa dikalikan belakangan.
/// Menyimpan hasil jadi untuk F0 tertentu berarti satu LUT per material — dan
/// LUT-nya sendiri berukuran sama dengan tekstur.
struct DfgTerms {
    float scale = 0.0f;
    float bias = 0.0f;
};

/// Mengintegralkan suku DFG untuk sebuah sudut pandang dan kekasaran.
///
/// **Sampelnya deterministik, bukan acak.** Urutan Hammersley memberi hasil yang
/// sama persis di setiap mesin dan setiap kali dijalankan. LUT yang berbeda
/// antar-jalan membuat perbandingan gambar tidak bisa dipakai sebagai test, dan
/// membuat cache apa pun yang menyimpannya tidak pernah sah — sama alasannya
/// dengan penabur vegetasi di E7.4 yang menolak RNG pustaka standar.
DfgTerms IntegrateDfg(float nDotV, float roughness, uint32_t sampleCount = 1024);

/// LUT DFG dua dimensi: sumbu X kosinus sudut pandang, sumbu Y kekasaran.
struct DfgLut {
    uint32_t size = 0;
    /// Baris demi baris, dua float per texel.
    std::vector<float> data;

    DfgTerms At(uint32_t x, uint32_t y) const {
        const size_t at = (static_cast<size_t>(y) * size + x) * 2;
        return DfgTerms{data[at], data[at + 1]};
    }
    /// Pembacaan bilinear dengan koordinat 0..1, mencerminkan sampler GPU.
    DfgTerms Sample(float nDotV, float roughness) const;
};

/// Membakar LUT DFG. Ukuran 64 sudah cukup — fungsinya mulus, dan kesalahan
/// interpolasinya jauh di bawah kesalahan pendekatan split-sum itu sendiri.
DfgLut BakeDfgLut(uint32_t size = 64, uint32_t sampleCount = 1024);

// --- Irradiance sebagai harmonik bola ----------------------------------------

/// Sembilan koefisien RGB, orde dua.
///
/// **Sembilan angka, bukan cubemap irradiance.** Kernel lobe kosinus meredam
/// pita di atas orde dua sampai di bawah satu persen, jadi cubemap irradiance
/// membayar memori dan satu pengambilan tekstur untuk ketelitian yang tidak
/// terlihat. Sembilan koefisien muat di uniform buffer dan dievaluasi dengan
/// belasan operasi.
struct Sh9 {
    std::array<Vec3, 9> coefficients{};
};

/// Memproyeksikan lingkungan ke SH orde dua.
///
/// `sampleCount` menentukan kerapatan sampel bola. Sampelnya merata di bola —
/// bukan merata di sudut bola — supaya kutub tidak mendapat bobot berlebih.
Sh9 ProjectIrradiance(const IEnvironmentSampler& environment, uint32_t sampleCount = 16384);

/// Irradiance pada sebuah normal, yaitu E — bukan radiance.
///
/// Pemakainya masih harus membagi dengan pi untuk mendapatkan radiance difus.
/// Perbedaannya sebesar faktor pi, yaitu jenis kesalahan yang terlihat sebagai
/// "material terlalu terang" dan biasanya ditutupi dengan mengecilkan intensitas
/// lampu sampai tidak ada lagi yang cocok.
Vec3 EvaluateIrradiance(const Sh9& sh, const Vec3& normal);

// --- Prefilter spekular ------------------------------------------------------

/// Kekasaran yang diwakili sebuah level mip peta prefilter.
///
/// Linear terhadap kekasaran, bukan terhadap alpha. Kekasaranlah yang
/// diinterpolasi shader di antara dua mip, dan pemetaan yang tidak linear
/// terhadapnya membuat perubahan kekasaran terasa melompat di tengah rentang.
float RoughnessForMip(uint32_t mip, uint32_t mipCount);

/// Menyaring lingkungan untuk sebuah arah pantul dan kekasaran.
///
/// **Mengandaikan N = V = R.** Itu pendekatan yang dipakai semua orang sejak
/// Karis 2013, dan harganya jelas: pantulan yang seharusnya memanjang pada sudut
/// serong menjadi bundar. Menyimpan variasi terhadap sudut pandang menuntut
/// dimensi ketiga pada petanya — memori yang belum ada yang menuntutnya.
Vec3 PrefilterSpecular(const IEnvironmentSampler& environment, const Vec3& reflection,
                       float roughness, uint32_t sampleCount = 256);

// --- Sampling GGX ------------------------------------------------------------

/// Titik ke-i dari urutan Hammersley berukuran `count`.
Vec2 Hammersley(uint32_t index, uint32_t count);

/// Sampel setengah-vektor GGX untuk sebuah titik urutan, di sekitar `normal`.
Vec3 ImportanceSampleGgx(const Vec2& xi, const Vec3& normal, float roughness);

}  // namespace sim::render
