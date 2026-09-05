#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Render/Atmosphere.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
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
    /// **Ia membeli kecepatan per cuplikan dengan sekitar 460 ms di muka**, jadi
    /// ia menguntungkan hanya di atas titik impas beberapa ribu cuplikan.
    /// Terukur (Debug): proyeksi SH 1024 cuplikan memakan 418 ms tanpa tabel dan
    /// **779 ms dengan** — di bawah titik impas, dan memanggilnya di sana justru
    /// memperlambat. Membangun mip 0 sebuah cubemap 64², yaitu 24.576 cuplikan,
    /// memakan 7164 ms tanpa tabel dan **2016 ms dengan**. Mip 0 bawaannya
    /// sekarang 256², yaitu enam belas kali cuplikan itu — jauh di atas titik
    /// impas, dan yang melewatkan panggilan ini membayarnya penuh.
    ///
    /// Aturannya karena itu: panggil untuk yang mencuplik puluhan ribu kali,
    /// lewati untuk yang mencuplik seribu.
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
    /// Pengali radiansi. Cerminan `scene::SkyComponent::hdriIntensity`, dan
    /// terpisah dari pengali langit atmosferik dengan alasan yang tertulis di
    /// komponennya: berkas HDR sudah berisi radiansi, jadi rentang bergunanya di
    /// sekitar satu.
    ///
    /// **Rotasi sengaja tidak ada di sini.** Ia memutar arah cuplikan, bukan
    /// lingkungannya — keputusan 4 di docs/PLAN-IBL.md — jadi ia diterapkan saat
    /// membaca hasil panggangan, bukan saat memanggangnya. Menaruhnya di sini
    /// akan menjadikan setiap geseran slider sebuah panggangan baru.
    float intensity = 1.0f;

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

// --- Ekstraksi matahari dari peta lingkungan ---------------------------------

/// Pengaturan pencarian matahari di dalam sebuah peta.
struct SunExtractionSettings {
    /// Jari-jari sudut maksimum kawasannya, radian. Bawaannya sekitar 6°: jauh
    /// lebih lebar daripada matahari sungguhan yang 0,27°, dan itu disengaja —
    /// peta HDR hampir selalu mengaburkan cakramnya, dan kawasan yang terlalu
    /// sempit meninggalkan halonya di dalam lingkungan sebagai sumber kedua.
    float maxAngularRadius = 0.1f;
    /// Ambang luminansi relatif terhadap texel paling terang. Yang di bawahnya
    /// bukan bagian mataharinya melainkan langit di sekitarnya.
    float relativeThreshold = 0.15f;
    /// Peta yang texel paling terangnya tidak jauh lebih terang daripada
    /// rata-ratanya tidak punya matahari — ia mendung. Nol berarti terima apa
    /// pun.
    float minPeakOverMean = 8.0f;
};

/// Matahari yang ditemukan di dalam sebuah peta lingkungan, dan dikeluarkan
/// darinya.
struct ExtractedSun {
    bool found = false;
    /// Arah **ke** matahari, ruang dunia, ternormalisasi.
    Vec3 direction{0.0f, 1.0f, 0.0f};
    /// Iradiansi pada permukaan yang menghadap tegak lurus ke arahnya, yaitu
    /// ∫(L − langit di sekitarnya) dω atas kawasannya.
    ///
    /// **Selisihnya, bukan seluruh radiansinya**, dan itu yang membuat
    /// kriterianya bisa dipenuhi tepat: yang dikeluarkan dari peta adalah
    /// kelebihan di atas langit di sekitarnya, jadi yang dibawa lampunya harus
    /// kelebihan yang sama. Membawa seluruh radiansinya berarti langit di balik
    /// mataharinya ikut terhitung dua kali.
    Vec3 irradiance{0.0f};
    /// Sudut ruang kawasannya, steradian. Dilaporkan supaya "seberapa besar
    /// cakram yang ditemukan" bisa dinilai alih-alih ditebak.
    float solidAngle = 0.0f;
};

/// Menemukan kawasan paling terang sebuah peta dan **mengeluarkannya**.
///
/// **Berkas HDR sudah berisi mataharinya.** Kalau level juga punya lampu
/// directional, ada dua — dan tidak ada satu pun galat yang menyebutkannya,
/// hanya adegan yang bayangannya dua kali lebih tegas daripada yang diharapkan
/// pengarangnya. Fungsi ini memisahkan keduanya: petanya kehilangan
/// mataharinya, dan lampunya mendapat mataharinya.
///
/// Texel kawasan itu diganti dengan rata-rata langit tepat di sekitarnya, bukan
/// dengan nol: yang dikeluarkan matahari, bukan lubang. Karena itu yang dibawa
/// lampunya adalah **selisih** terhadap langit itu, dan penjumlahan keduanya
/// mengembalikan iradiansi peta utuh — itulah kriteria terima B4 di
/// docs/PLAN-IBL.md, dan ia diuji.
ExtractedSun ExtractSun(EquirectEnvironment& map, const SunExtractionSettings& settings = {});

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

/// Cubemap yang sudah jadi, dibaca per arah.
///
/// **Ada karena prefilter tidak boleh mencuplik pencuplik analitik.** Menyaring
/// satu texel prefilter menuntut ratusan cuplikan lingkungan, dan untuk langit
/// atmosferik satu cuplikan adalah satu ray march: menyaring 130.560 texel
/// dengan 256 sampel berarti 33 juta ray march. Dari cubemap, ia pencarian
/// tekstur — dan mip 0 memang sudah berisi lingkungan yang sama, dicuplik
/// sekali per texel.
///
/// **Bukan pengganti pencuplik aslinya, melainkan cuplikan sekalinya.** Yang
/// hilang ketelitian di bawah satu texel muka; yang didapat prefilter yang biaya
/// pembangunannya tidak lagi berlipat dengan biaya lingkungannya.
class CubemapEnvironment final : public IEnvironmentSampler {
public:
    uint32_t size = 0;
    /// Enam muka berurutan, tiap muka `size * size` texel, empat float per texel
    /// — tata letak mip 0 yang ditulis `BakeIbl`.
    const float* texels = nullptr;

    bool IsValid() const { return size > 0 && texels != nullptr; }

    /// Cuplikan bilinear di dalam satu muka, tanpa memadu antar-muka.
    ///
    /// **Jahitan antar-muka tidak dipadu**, dan itu batas yang diterima: yang
    /// membacanya prefilter, yang setiap texelnya sudah merata-ratakan puluhan
    /// arah — sebuah jahitan selebar setengah texel pada mip 0 tidak bertahan
    /// melewati perataan itu. Memadunya menuntut penelusuran tetangga per muka,
    /// yaitu tabel yang harus benar di dua puluh empat tepi.
    Vec3 Sample(const Vec3& direction) const override;
};

/// Enam muka cubemap, urutan Vulkan: +X, −X, +Y, −Y, +Z, −Z.
inline constexpr int kCubeFaceCount = 6;

/// Arah dunia untuk sebuah texel muka cubemap.
///
/// `u` dan `v` dalam 0..1 dan sudah di tengah texel. Konvensinya konvensi
/// cubemap Vulkan/D3D, yang sumbu V-nya menghadap ke bawah — memakai konvensi
/// OpenGL di sini menghasilkan lingkungan yang terbalik atas-bawah pada dua
/// mukanya saja, yang jauh lebih membingungkan daripada terbalik seluruhnya.
Vec3 CubeFaceDirection(int face, float u, float v);

/// Kebalikannya: arah dunia → muka beserta uv-nya, keduanya 0..1.
///
/// **Keduanya harus saling membalik dengan tepat.** Pemetaan yang meleset tidak
/// menghasilkan galat apa pun, hanya lingkungan yang isinya benar di muka yang
/// salah — dan itu terlihat sebagai pantulan yang "arahnya aneh", bukan sebagai
/// kesalahan. Aturan yang sama sudah dipegang pasangan equirect di atas.
void DirectionToCubeFace(const Vec3& direction, int& face, float& u, float& v);

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

// --- Panggangan IBL, sisi CPU ------------------------------------------------
//
// **Di header publik karena ia matematika, bukan sumber daya GPU.**
// Menghitung texel prefilter tidak menyentuh satu pun objek Vulkan, dan yang
// tidak menyentuhnya harus bisa diuji tanpa perangkat grafis. Yang membuat
// teksturnya — `BakedIbl`, `UploadIbl` — tinggal di header privat modul,
// bersama tipe RHI yang disebutnya.

struct IblBakeSettings {
    /// Sisi muka cubemap prefilter pada mip 0.
    ///
    /// **Ini resolusi pantulan cermin, bukan sekadar ukuran cache.** Mip 0
    /// mewakili kekasaran nol, dan setiap krom, logam poles, serta permukaan
    /// glossy memantulkannya apa adanya — pada 64 texel per muka yang terlihat
    /// adalah lingkungan 64 piksel yang diperbesar, dan yang mengeluhkannya
    /// menyebutnya "pantulan kurang detail" tanpa satu pun petunjuk ke angka
    /// ini. 256 memindahkan biayanya ke tempat yang sanggup membayarnya: mip 0
    /// tetap di CPU, sisanya ke `IblPrefilter`.
    uint32_t cubeSize = 256;
    /// Banyaknya mip. Mip terakhir mewakili kekasaran 1.
    uint32_t mipCount = 5;
    uint32_t dfgSize = 64;
    /// Sampel GGX per texel prefilter, di luar mip 0 yang cuma satu pengambilan.
    ///
    /// Naik bersama `cubeSize` karena yang membayarnya berganti: 256 sampel atas
    /// 130.560 texel adalah 33 juta cuplikan, yaitu belasan detik di CPU dan
    /// beberapa milidetik di compute. Yang tetap memakai jalur CPU — test, dan
    /// pemanggang tanpa perangkat grafis — menurunkannya sendiri.
    uint32_t prefilterSamples = 256;
    uint32_t dfgSamples = 256;
    /// Cuplikan bola untuk proyeksi SH. **Nol berarti lewati**, dan itu dipakai
    /// pemanggang yang sudah punya SH-nya sendiri dari panggangan yang lebih
    /// sering: menghitungnya lagi di sini hanya menghasilkan angka yang sama.
    uint32_t irradianceSamples = 8192;

    /// Mip pertama yang diserahkan ke pemanggang GPU.
    ///
    /// **Nol berarti tidak ada** — seluruh rantai disaring CPU, dan itulah
    /// jalur acuan: ia tidak menyentuh satu pun objek Vulkan, jadi ia bisa
    /// diuji tanpa perangkat grafis dan tetap menjadi pembanding kebenaran
    /// jalur compute.
    ///
    /// **Tidak pernah boleh nol-tapi-bukan-nol, yaitu tidak pernah boleh 0
    /// sebagai "mulai dari mip 0".** Mip 0 mencuplik `IEnvironmentSampler`
    /// langsung — langit atmosferik yang satu cuplikannya satu ray march, atau
    /// sebuah HDR yang baru didekode ke memori host — dan tidak satu pun dari
    /// keduanya ada di GPU saat panggangan berjalan. Yang menyerahkannya ke
    /// compute karena itu menyetel `1`: mip 0 di CPU, integral GGX di atasnya
    /// di GPU.
    ///
    /// Texel mip yang diserahkan tetap dialokasikan dan tetap nol di
    /// `IblBakeCpu::cubeTexels` — unggahannya butuh rantai utuh, dan nol yang
    /// terlihat sebagai pantulan hitam jauh lebih mudah dikenali sebagai
    /// dispatch yang gagal daripada memori yang belum ditulis siapa pun.
    uint32_t firstGpuMip = 0;
};

/// Panggangan yang belum menyentuh GPU sama sekali.
///
/// **Dipisah karena yang mahal dan yang harus di main thread bukan bagian yang
/// sama.** Menghitung texel prefilter memakan detik dan tidak menyentuh satu
/// pun objek Vulkan; membuat tekstur dan menyalinnya menyentuh device dan
/// karena itu harus di thread yang memilikinya. Yang menggabungkan keduanya
/// memaksa memilih antara membekukan editor dan menyentuh device dari worker.
///
/// **LUT DFG sengaja tidak di sini.** Ia tidak bergantung pada lingkungan sama
/// sekali — hanya pada BRDF-nya — jadi memanggangnya ulang setiap langit
/// berubah adalah 914 ms (Debug) yang dibuang untuk menghasilkan byte yang sama
/// persis. Yang memanggangnya memanggilnya sekali, sendiri.
struct IblBakeCpu {
    Sh9 irradiance;
    uint32_t cubeSize = 0;
    uint32_t mipCount = 0;
    /// Mip pertama yang **belum** terisi dan menunggu pemanggang GPU. Nol
    /// berarti seluruh rantai sudah disaring di sini.
    ///
    /// Ikut ke dalam artefak masak, dan memang harus: sebuah `.simibl` yang
    /// dimuat kembali membawa mip yang sama kosongnya, dan yang memuatnya
    /// harus tahu bahwa ia masih berutang satu dispatch.
    uint32_t firstGpuMip = 0;
    /// RGBA32F, seluruh mip berurutan, tiap mip enam muka berurutan — tata letak
    /// yang diminta `TextureCube::Create`.
    std::vector<float> cubeTexels;

    bool IsValid() const { return cubeSize > 0 && mipCount > 0 && !cubeTexels.empty(); }
};

/// Menghitung iradiansi SH dan peta prefilter. Aman dipanggil dari thread mana
/// pun: ia tidak menyentuh `rhi::Device`.
IblBakeCpu BakeIblCpu(const IEnvironmentSampler& environment, const IblBakeSettings& settings);


/// Artefak masak sebuah lingkungan panggang: `.simibl` (B3).
///
/// **Ada supaya membuka level pra-GI tidak memanggang apa pun.** Memanggang
/// sebuah HDR 4096×2048 menuntut mendekodenya — ratusan milidetik dan seratus
/// megabyte — lalu 393.216 cuplikan untuk mip 0. Semuanya menghasilkan 8,0 MiB
/// yang sama persis setiap kali, untuk berkas yang tidak berubah. Menyimpannya
/// sekali dan memuatnya kemudian adalah selisih antara level yang terbuka
/// seketika dan level yang terbuka sedetik kemudian, setiap kali.
///
/// **Mip yang diserahkan GPU ikut tersimpan sebagai nol**, dan itu bukan
/// pemborosan yang terlewat: dispatch yang mengisinya 49 ms, sementara
/// menyimpannya berarti artefak yang isinya bergantung pada perangkat yang
/// memanggangnya. `firstGpuMip` di header-nya yang memberi tahu pemuat bahwa ia
/// masih berutang dispatch itu.
///
/// **Di folder cache pengguna, bukan di sebelah berkas sumbernya.** Rencananya
/// menulis "di sebelah `.meta` berkasnya", dan itu tidak diikuti dengan sengaja:
/// berkas HDR bawaan tinggal di `Resources/` yang read-only, dan menulis ke
/// folder aset milik orang lain sebagai efek samping dari sekadar *membaca*
/// adalah persis cacat yang importir FBX di mesin ini sudah menolak sekali
/// (`IMP_FBX_EXTRACT_EMBEDDED_DATA` dimatikan dengan alasan yang sama). Konvensi
/// yang diikuti karena itu konvensi `MeshSdfCache`: nama berkasnya hash, isinya
/// bisa dibuang kapan saja, dan tidak ada satu pun folder pengguna yang
/// ketambahan berkas yang tidak diminta.
///
/// Kuncinya isi berkas sumbernya, pengaturan panggangannya, pengalinya, dan
/// versi pemanggangnya. Berkas yang berubah menghasilkan kunci lain, dan artefak
/// lamanya tinggal sebagai sampah yang tidak pernah dibaca — sama seperti cache
/// SDF mesh.
uint64_t IblCacheKey(const std::filesystem::path& source, const IblBakeSettings& settings,
                     float intensity);

std::filesystem::path IblCachePath(const std::filesystem::path& cacheDir, uint64_t key);

/// Menulis artefaknya. Direktorinya dibuat bila belum ada.
bool WriteIblCache(const std::filesystem::path& file, const IblBakeCpu& baked,
                   std::string& error);

/// Membaca artefaknya. Mengembalikan false — tanpa menyentuh `out` — bila
/// berkasnya tidak ada, versinya lain, atau isinya tidak sepanjang yang
/// dijanjikan headernya.
///
/// **Gagal membaca bukan galat.** Artefak masak boleh hilang, boleh usang, dan
/// boleh ditulis versi lain; yang benar lalu memanggang ulang, bukan menolak
/// membuka level.
bool ReadIblCache(const std::filesystem::path& file, IblBakeCpu& out, std::string& error);


// --- Sampling GGX ------------------------------------------------------------

/// Titik ke-i dari urutan Hammersley berukuran `count`.
Vec2 Hammersley(uint32_t index, uint32_t count);

/// Sampel setengah-vektor GGX untuk sebuah titik urutan, di sekitar `normal`.
Vec3 ImportanceSampleGgx(const Vec2& xi, const Vec3& normal, float roughness);

}  // namespace sim::render
