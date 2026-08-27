#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Render/Atmosphere.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <utility>
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

/// Langit prosedural: gradien cakrawala→zenit dengan cakram matahari.
///
/// **Ada supaya IBL punya sumber tanpa satu berkas pun.** Peta lingkungan
/// sungguhan datang bersama importir tekstur HDR; sampai itu ada, lingkungan
/// yang dijawab secara analitis lebih baik daripada tidak ada lingkungan sama
/// sekali — dan ia sekaligus lingkungan yang jawabannya bisa diperiksa tangan
/// di test.
class GradientSky final : public IEnvironmentSampler {
public:
    Vec3 zenith{0.18f, 0.32f, 0.62f};
    Vec3 horizon{0.62f, 0.66f, 0.72f};
    Vec3 ground{0.14f, 0.13f, 0.12f};
    /// Arah **ke** matahari.
    Vec3 sunDirection{-0.4f, 0.8f, 0.45f};
    Vec3 sunRadiance{14.0f, 12.6f, 10.5f};
    /// Kosinus jari-jari sudut cakram matahari. Bawaannya kira-kira 3°, jauh
    /// lebih lebar daripada matahari sungguhan — cakram setengah derajat hanya
    /// akan tertangkap beberapa sampel saja, dan hasilnya prefilter yang
    /// berbintik alih-alih sorotan yang mulus.
    float sunCos = 0.9986f;

    Vec3 Sample(const Vec3& direction) const override;
};

/// Langit atmosferik sebagai sumber lingkungan.
///
/// **Inilah yang menyinari tingkat `Baked` + `Sky`** (B1 di docs/PLAN-IBL.md).
/// Sebelum ia ada, cahaya tak-langsung adalah konstanta 0,25 yang tidak berasal
/// dari langit mana pun: menukar seluruh langit tidak menggerakkan pencahayaan
/// permukaan satu tingkat pun. Yang di sini menghitung radiansi langit yang
/// sama yang tergambar, dari parameter yang sama, di CPU.
///
/// **Cakram mataharinya sengaja TIDAK ikut.** Sebuah adegan yang punya langit
/// hampir selalu juga punya lampu directional yang mewakili mataharinya, dan
/// lampu itu sudah mengantarkan cahaya langsungnya. Menyertakan cakramnya di
/// sini membuat matahari terhitung dua kali — cacat yang persis sama dengan
/// yang keputusan 1 cegah untuk berkas HDR, muncul dalam bentuk prosedural.
/// Yang dipanggang di sini karena itu murni cahaya yang **dihamburkan** udara.
///
/// **Hamburan tunggal saja**, sama dengan `IntegrateAerialPerspective` dan
/// dengan alasan yang sama: suku multiscattering-nya ada di GPU lewat LUT yang
/// dibangun pass tersendiri. Untuk iradiansi panggang selisihnya paling terasa
/// pada langit yang sangat tebal, dan itu bukan keadaan yang sedang ditala
/// siapa pun.
///
/// **Arah di bawah cakrawala mengembalikan udara yang dilewatinya saja; tanahnya
/// hitam.** Itu mengikuti `AtmosphereParameters::groundAlbedo` yang bawaannya
/// nol, dan konsekuensinya jujur: permukaan yang menghadap ke bawah tidak
/// menerima pantulan tanah. Pantulan tanah adalah transport cahaya, yaitu
/// pekerjaan GI — dan tingkat panggang memang tidak punya itu. Batas ini ditulis
/// di risiko rencananya, bukan ditemukan sebagai "kenapa bawah objek saya
/// gelap".
class AtmosphereSky final : public IEnvironmentSampler {
public:
    AtmosphereParameters atmosphere;

    /// Arah **ke** matahari, ruang dunia, ternormalisasi. Cerminan arah lampu
    /// directional adegan — itulah yang membuat iradiansi ikut bergeser saat
    /// Time-of-Day menggerakkan matahari.
    Vec3 sunDirection{0.0f, 1.0f, 0.0f};

    /// Ketinggian kamera di atas permukaan, kilometer. Cerminan
    /// `scene::SkyComponent::cameraHeightKm`, dan ia yang menentukan warna
    /// cakrawala serta setebal apa udara yang dilihat.
    float cameraHeightKm = 0.5f;

    /// Pengali radiansi. Cerminan `scene::SkyComponent::intensity` (Sky Gain),
    /// supaya yang memanggang dan yang menggambar memakai angka yang sama.
    float intensity = 20.0f;

    /// Langkah integrasi sepanjang sinar pandang.
    ///
    /// **32, bukan 512 seperti `Transmittance`.** Yang dipanggang di sini
    /// diproyeksikan ke sembilan koefisien SH, dan proyeksi itu adalah integral
    /// atas seluruh bola: derau per-sinar yang tersisa saling meniadakan jauh
    /// sebelum ia terlihat pada koefisiennya. Menaikkannya menambah waktu bake
    /// secara linear untuk ketelitian yang tidak sampai ke keluaran.
    uint32_t stepCount = 32;
    /// Langkah untuk transmitansi menuju matahari di tiap titik sampel. Hanya
    /// dipakai bila `Prepare()` belum dipanggil.
    uint32_t sunStepCount = 16;

    /// Menghitung tabel transmitansi di muka.
    ///
    /// **Panggil ini sebelum memanggang, dan biayanya kembali berlipat ganda.**
    /// Transmitansi menuju matahari adalah gelung terdalam — sekali per sampel
    /// per langkah — dan menghitungnya sebagai integral bersarang membuat
    /// proyeksi SH 4096 sampel memakan 1150 ms (Debug). Dengan tabel ini 40 ms.
    /// Angkanya terukur, dan selisih itulah yang memisahkan "panggang ulang tiap
    /// matahari bergeser" dari editor yang tersendat tiap Time-of-Day digeser.
    ///
    /// **Melewatinya tetap benar, hanya lambat.** Tanpa tabel, `Sample`
    /// mengintegrasikan langsung dengan `sunStepCount` langkah — jawaban yang
    /// sama dalam toleransi ujinya. Itu disengaja: sebuah pencuplik yang salah
    /// diam-diam kalau sebuah panggilan persiapan terlupa adalah pencuplik yang
    /// tidak bisa dipercaya, sedangkan yang sekadar lebih lambat bisa.
    ///
    /// Harus dipanggil ulang setiap `atmosphere` berubah. **Matahari yang
    /// bergeser tidak menuntutnya** — tabelnya hanya bergantung pada udaranya,
    /// bukan pada arah datangnya cahaya, dan itu justru yang membuat panggang
    /// ulang saat Time-of-Day berjalan menjadi murah.
    void Prepare();

    /// Memasang tabel yang sudah dibangun di tempat lain. Dipakai uji yang
    /// menala jumlah langkahnya, dan pemanggang yang membangunnya sekali untuk
    /// beberapa langit sekaligus.
    void SetTransmittanceLut(TransmittanceLut lut) { transmittance_ = std::move(lut); }

    Vec3 Sample(const Vec3& direction) const override;

private:
    TransmittanceLut transmittance_;
};

// --- Peta lingkungan equirectangular -----------------------------------------

/// Arah dunia → uv equirectangular (latitude-longitude), keduanya 0..1.
///
/// **Pemetaan yang tidak membalik tidak menghasilkan galat apa pun**, hanya
/// langit yang isinya benar di tempat yang salah — matahari di HDRI muncul di
/// arah yang berbeda dari matahari yang menerangi adegan, dan yang terlihat
/// adalah bayangan yang "arahnya aneh" alih-alih peta yang terpasang terbalik.
/// Karena itu keduanya ada, dan keduanya diuji saling membalik.
Vec2 DirectionToEquirectUv(const Vec3& direction);
Vec3 EquirectUvToDirection(const Vec2& uv);

/// Peta lingkungan HDR equirectangular, dibaca per arah.
///
/// **Ini yang ditunggu `GradientSky`.** Catatan di atas kelas itu menyebut
/// "peta lingkungan sungguhan datang bersama importir tekstur HDR"; ini
/// importirnya, dan karena ia mengimplementasikan antarmuka yang sama, seluruh
/// rantai IBL — SH iradiansi, prefilter spekular — langsung menerimanya tanpa
/// satu baris pun berubah.
class EquirectEnvironment final : public IEnvironmentSampler {
public:
    uint32_t width = 0;
    uint32_t height = 0;
    /// RGB linear, baris demi baris, tiga float per texel.
    std::vector<float> pixels;

    bool IsValid() const {
        return width > 0 && height > 0 &&
               pixels.size() >= static_cast<std::size_t>(width) * height * 3;
    }

    /// Cuplikan bilinear. **Membungkus di U dan menjepit di V**, dan keduanya
    /// harus berbeda: U adalah lingkaran penuh, jadi menjepitnya meninggalkan
    /// jahitan tegak selebar satu texel yang membelah langit; V berakhir di
    /// kutub, jadi membungkusnya mengambil warna dari kutub seberang.
    Vec3 SampleUv(const Vec2& uv) const;

    Vec3 Sample(const Vec3& direction) const override;
};

/// Memuat peta lingkungan equirectangular berjangkauan dinamis lebar.
///
/// **Formatnya ditentukan backend `Sim::ImageIO` yang aktif, bukan oleh fungsi
/// ini.** Radiance `.hdr` selalu bisa; `.exr` ikut bisa ketika backend EXR
/// terbangun. Berkas yang formatnya tidak didukung ditolak dengan pesan yang
/// menyebut backend yang dibutuhkannya — bukan dengan "format tidak dikenal",
/// yang mengirim orang memeriksa berkasnya.
///
/// Mengembalikan peta kosong bila berkasnya tidak ada, tidak bisa didekode, atau
/// bukan peta RGB; pemanggil memeriksa `IsValid()`.
EquirectEnvironment LoadHdrEquirect(const std::filesystem::path& path);

/// Enam muka cubemap, urutan Vulkan: +X, −X, +Y, −Y, +Z, −Z.
inline constexpr int kCubeFaceCount = 6;

/// Arah dunia untuk sebuah texel muka cubemap.
///
/// `u` dan `v` dalam 0..1 dan sudah di tengah texel. Konvensinya konvensi
/// cubemap Vulkan/D3D, yang sumbu V-nya menghadap ke bawah — memakai konvensi
/// OpenGL di sini menghasilkan lingkungan yang terbalik atas-bawah pada dua
/// mukanya saja, yang jauh lebih membingungkan daripada terbalik seluruhnya.
Vec3 CubeFaceDirection(int face, float u, float v);

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

/// Pembagian energi lingkungan sebuah permukaan menjadi tiga suku.
///
/// **Split-sum satu-pantulan kehilangan energi, dan kehilangannya besar.**
/// GGX yang hanya menghitung satu pantulan mikrofaset membuang cahaya yang di
/// permukaan nyata memantul lagi di lereng sebelah. Terukur dengan LUT DFG
/// engine ini sendiri: logam putih kehilangan 1,9% energinya pada kekasaran 0,2,
/// 27,6% pada 0,6, dan **63,9% pada 1,0**. Itu bukan kesalahan kecil yang bisa
/// diabaikan — itu logam kasar yang tampak abu-abu kotor alih-alih putih, dan
/// tidak ada penyetelan material yang bisa memperbaikinya karena energinya
/// memang hilang sebelum sampai ke penyetelan.
///
/// Kompensasinya mengikuti Fdez-Agüera: energi yang hilang dari pantulan pertama
/// dikembalikan sebagai pantulan lanjutan yang tersebar merata. Sifat yang
/// membuatnya sah — dan yang diuji — adalah bahwa ketiga suku ini berjumlah
/// **tepat satu** untuk albedo satu, pada kekasaran dan metalness berapa pun.
struct EnergyTerms {
    /// Spekular pantulan-tunggal. Dikalikan radiansi spekular terprafilter.
    Vec3 singleScatter{0.0f};
    /// Spekular pantulan-lanjutan. Dikalikan iradiansi, karena pantulan
    /// berikutnya sudah kehilangan arahnya.
    Vec3 multiScatter{0.0f};
    /// Sisa yang boleh dipakai difus, sebelum dikalikan albedo. Nol untuk logam
    /// putih — dan itu bukan aturan yang ditulis terpisah melainkan akibat
    /// langsung dari rumusnya.
    Vec3 diffuse{0.0f};
};

/// Membagi energi lingkungan dari suku DFG dan reflektansi tegak lurus.
EnergyTerms SplitEnergy(const DfgTerms& dfg, const Vec3& f0);

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
