#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Core/VolumeGrid.h"
#include "Sim/Render/Bloom.h"
#include "Sim/Render/TraceBackend.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace sim::render {

/// Handle buram ke tekstur GPU yang bisa digambar UI.
///
/// Nilainya sengaja uint64_t dan bukan pointer Vulkan: sejak Dear ImGui 1.92,
/// ImTextureID adalah ImU64, jadi handle ini bisa dilempar langsung ke
/// ImGui::Image() tanpa modul Editor perlu tahu apa pun soal Vulkan.
using TextureHandle = uint64_t;
inline constexpr TextureHandle kInvalidTexture = 0;

/// Material yang pipeline-nya sudah dibangun renderer.
///
/// Nol berarti "ruas ini digambar jalur mundur `box.frag`" — bukan sebuah galat.
/// Itu keadaan setiap ruas yang materialnya belum dikompilasi, gagal
/// dikompilasi, atau memang tidak punya material sama sekali.
using MaterialHandle = uint32_t;
inline constexpr MaterialHandle kInvalidMaterial = 0;

/// Handle buram ke geometri mesh yang sudah ada di GPU.
///
/// **Nol berarti kubus satuan, bukan berarti kesalahan.** Aset mesh yang belum
/// ditetapkan, yang gagal dimuat, dan yang sedang menunggu semuanya menggambar
/// kubus — sama seperti sebelum importir ada. Yang tidak boleh terjadi adalah
/// entity yang punya MeshRenderer tapi tidak menggambar apa pun: yang hilang
/// dari viewport tidak bisa diklik, tidak bisa dipilih, dan karena itu tidak
/// bisa diperbaiki.
using MeshHandle = uint64_t;
inline constexpr MeshHandle kUnitCubeMesh = 0;

/// Geometri yang siap dipakai beserta batas ruang lokalnya.
///
/// Batasnya ikut mengalir balik karena **yang digambar dan yang bisa diklik
/// harus memakai kotak yang sama.** Editor menghitung picking dari batas ini,
/// renderer menghitung culling dan bayangan dari batas yang sama; dua sumber
/// berarti objek yang tampak tapi tak bisa dipilih, dan penyebabnya sulit
/// dilacak.
struct MeshAsset {
    MeshHandle handle = kUnitCubeMesh;
    Vec3 boundsMin{-0.5f};
    Vec3 boundsMax{0.5f};
    /// False bila berkasnya gagal dimuat. Handle-nya lalu kubus satuan.
    bool loaded = false;
    /// Berapa ruas bermaterial yang dimiliki mesh ini. Selalu >= 1 untuk mesh
    /// yang berhasil dimuat — inilah jumlah slot material yang berarti untuk
    /// entity yang memakainya.
    uint32_t partCount = 1;
    /// Jumlah bone rangka yang mengulit mesh ini, atau nol bila tidak ber-skin.
    ///
    /// **Sebuah angka, bukan rangkanya.** Header publik Render sengaja bebas
    /// dari tipe modul lain — itu seam #1 di docs/ARCHITECTURE.md — dan yang
    /// dibutuhkan pemanggil untuk mengirim pose hanyalah panjang paletnya. Nama
    /// bone, hierarki, dan bind pose datang dari sisi aset, dari berkas yang
    /// sama.
    uint32_t boneCount = 0;
    /// Segitiga dan vertex mesh ini, atau nol bila tidak diketahui.
    ///
    /// **Dilaporkan dari sini, bukan dihitung importir.** Menghitungnya saat
    /// impor menuntut mengurai berkasnya — 118 ms untuk satu FBX — dan
    /// pemindaian pertama sebuah project berjalan **sinkron**, jadi seratus mesh
    /// berarti sepuluh detik sebelum jendela pertama muncul. Yang mengunggahnya
    /// ke GPU sudah memegang angkanya; menyerahkannya di sini berarti nol
    /// penguraian tambahan, selamanya.
    ///
    /// Di akhir struct, bukan di tengahnya: beberapa tempat menyusunnya dengan
    /// kurung kurawal berurutan, dan menyisipkan medan di tengah memindahkan
    /// `boneCount` ke tempat yang salah tanpa satu pun galat kompilasi.
    uint32_t triangleCount = 0;
    uint32_t vertexCount = 0;
};

/// Saringan tampilan viewport.
///
/// **Lima, dan setiap satunya benar-benar digambar.** Enum ini berdiri sejak E1
/// dengan nama-nama yang menggambarkan rencana, bukan jalur yang ada — dan
/// sampai belum lama ini tidak ada satu pun perender yang membacanya: tombol
/// "Lit/Wireframe" di viewport mengubah medan ini dan tidak mengubah apa pun di
/// layar.
///
/// **Tidak satu pun dari keempatnya menuntut pipeline dibangun ulang, dan itu
/// syarat, bukan kebetulan.** Pipeline pass forward dibuat satu per material
/// oleh kompiler graph material; sebuah mode yang menjadi varian pipeline
/// berarti setiap material di project dikompilasi ulang saat sakelarnya
/// ditekan. Semuanya karena itu masuk lewat jalan lain: rangka kawat lewat pass
/// tambahan sesudah forward, dan `Unlit` maupun `Clay` lewat satu angka di blok
/// uniform per-frame yang dibaca setiap shader fragmen.
enum class DrawMode : uint8_t {
    /// Material apa adanya. Bentuk viewport sejak E8, dan bawaannya.
    Material,
    /// Material tanpa satu pun cahaya: albedo datar beserta emissive-nya.
    ///
    /// **Cabang yang sama dengan adegan tanpa lampu**, diminta alih-alih
    /// disimpulkan — lihat `unlitView()` di `Shaders/cluster_common.slang`.
    /// Itulah yang membuat ruas bermaterial dan ruas jalur-mundur sepakat di
    /// mode ini tanpa dua definisi "tanpa cahaya" yang harus dijaga sinkron.
    ///
    /// Yang dijawabnya: warna mana yang benar-benar milik material, dan mana
    /// yang datang dari cahaya. Tekstur yang terlalu gelap dan lampu yang
    /// terlalu redup menghasilkan gambar yang sama, dan mode ini yang
    /// memisahkan keduanya. Uji alfa tetap berlaku, jadi siluet benda tidak
    /// berubah.
    Unlit,
    /// Cahaya tanpa material: seluruh permukaan menjadi satu abu-abu netral yang
    /// matte, dan penyinaran, bayangan, serta GI berjalan utuh di atasnya.
    ///
    /// **Kebalikan `Unlit`, dan dipakai untuk pertanyaan yang kebalikan pula:**
    /// bukan warna mana yang milik material, melainkan bayangan ini jatuh di
    /// mana. Peta albedo adalah persis hal yang membuat pertanyaan itu tidak
    /// bisa dijawab — mata tidak memisahkan bercak gelap sebuah tekstur dari
    /// bercak gelap sebuah bayangan, jadi kaskade yang beresolusi kurang, acne,
    /// dan bayangan yang lepas dari kakinya semuanya tenggelam di dalam
    /// permukaan yang ramai.
    ///
    /// Yang dibuang bukan hanya warna dasar. Logam memantulkan lingkungannya
    /// alih-alih menaunginya, sorotan spekular terbaca seperti cahaya yang
    /// jatuh di tempat yang bukan tempatnya, dan peta normal melekukkan cahaya
    /// pada permukaan yang sebenarnya datar. Uji alfa tetap berlaku, dengan
    /// alasan yang sama seperti di `Unlit`: bayangan yang diperiksa harus jatuh
    /// dari siluet yang sungguhan.
    Clay,
    /// Material, lalu rangka kawat di atasnya.
    ///
    /// Inilah mode yang dipakai saat memeriksa topologi sambil tetap melihat
    /// permukaannya: rangka kawat sendirian menyembunyikan justru hal yang
    /// sedang dinilai — bahwa sebuah lipatan halus ternyata datang dari
    /// segitiga yang panjang sekali.
    MaterialWireframe,
    /// Hanya rangka kawat.
    ///
    /// **Garis yang tersembunyi tetap tersembunyi.** Prepass tetap berjalan
    /// walaupun tidak ada permukaan yang digambar, dan kedalaman yang
    /// ditulisnya itulah yang membuat rusuk di sisi jauh sebuah benda tidak
    /// ikut terlihat. Tanpa itu, sebuah kotak menjadi dua belas garis yang
    /// saling menembus dan bentuknya tidak lagi terbaca.
    Wireframe,
};

enum class ExposureMode : uint8_t {
    /// Diukur dari adegan tiap frame, lalu diadaptasi terhadap waktu.
    Automatic,
    /// EV100 yang disetel tangan. Dibutuhkan justru saat menyetel material dan
    /// lampu: eksposur yang bergerak sendiri membuat setiap perubahan
    /// kecerahan dilawan oleh pengukurnya, dan yang terlihat adalah lampu yang
    /// "tidak berpengaruh apa-apa".
    Manual,
};

/// Pengaturan rantai post-process.
///
/// **Menggantikan `ViewportDesc::exposure` yang berdiri sejak E8.3.** Angka itu
/// satu pengali yang dipakai kedua jalur cahaya supaya radiance sungguhan tidak
/// terpotong putih pada target 8-bit — sebuah penambal untuk tidak adanya
/// operator nada. Sekarang operatornya ada, jadi lampu kembali memakai radiance
/// sungguhannya dan yang memetakannya ke layar adalah pass tersendiri.
struct PostProcessSettings {
    /// Saat mati, isi HDR disalin apa adanya ke target tampilan — dijepit, tanpa
    /// operator nada dan tanpa encode sRGB. Dipakai membandingkan.
    bool enabled = true;

    ExposureMode exposureMode = ExposureMode::Automatic;

    /// EV100 saat manual.
    ///
    /// **Nol, bukan 13.** EV100 sungguhan untuk siang hari ada di sekitar 13-15,
    /// dan itu memang nilai pertama yang saya tulis — hasilnya viewport hitam
    /// pekat tanpa satu pun galat. Sebabnya: lampu di engine ini belum dalam
    /// satuan fotometrik. Matahari bawaan beradiansi 3,0, bukan sepuluh ribu
    /// cd/m², jadi EV100 yang dihitung dari angka-angka itu berada di sekitar
    /// nol. Rentang yang berguna di sini kira-kira -8 sampai +8, dan label
    /// "EV100" baru berarti harfiah begitu lampu memakai satuan sungguhan.
    float manualEv100 = 0.0f;

    /// Kompensasi dalam stop, berlaku pada kedua mode. Positif berarti lebih
    /// terang.
    float exposureCompensation = 0.0f;

    /// Tetapan waktu adaptasi, detik. **Dua, bukan satu:** satu tetapan waktu
    /// memaksa memilih antara kedipan saat berbalik menghadap matahari dan
    /// keterlambatan saat masuk ke lorong, dan keduanya terlihat.
    float adaptationBrightenSeconds = 0.4f;
    float adaptationDarkenSeconds = 1.2f;

    BloomSettings bloom;
};

/// Apa yang menyinari tingkat pencahayaan panggang. Cerminan
/// `scene::EnvironmentSource`, disalin dengan alasan yang sama seperti
/// `SkySource`: `Sim::Scene` tidak boleh melihat `Sim::Render`.
enum class EnvironmentSource : uint8_t {
    Sky,
    File,
};

/// Dari mana langit datang.
enum class SkySource : uint8_t {
    /// Atmosfer Bruneton-Hillaire: dihitung dari fisika, berubah terhadap
    /// matahari, dan memberi awan volumetrik udara untuk ditinggali.
    Atmosphere,
    /// Peta lingkungan HDR equirectangular. **Satu cuplikan tekstur** alih-alih
    /// empat pass LUT ditambah raymarch — dan awannya sudah ada di dalam
    /// gambarnya. Yang tidak bisa dilakukannya adalah berubah terhadap waktu:
    /// matahari di HDRI diam di tempatnya, jadi Time-of-Day tidak lagi
    /// menggerakkan langit walaupun ia tetap menggerakkan bayangan.
    HdrMap,
};

/// Awan volumetrik.
///
/// **Mati secara bawaan, dan alasannya biaya.** Langit dan kabut berharga
/// sepersepuluh milidetik; raymarch awan berharga puluhan kali itu, dan ia
/// dibayar pada setiap frame di setiap viewport. Sakelar yang menyala sendiri
/// membuat seseorang membayar harga itu tanpa pernah memintanya.
struct CloudSettings {
    bool enabled = false;

    /// Alas dan puncak lapisan, kilometer di atas permukaan laut.
    float bottomKm = 1.5f;
    float topKm = 4.0f;

    /// Cakupan langit, 0..1. **Mengurangi kerapatan, bukan mengalikannya** —
    /// pengurangan memakan tepi awan, yang memang cara langit menjadi cerah;
    /// perkalian menipiskan semuanya merata sehingga langit cerah tampak seperti
    /// langit mendung yang tembus pandang.
    float coverage = 0.45f;
    /// Pengali kerapatan sesudah pembentukan.
    float density = 1.0f;

    /// Sisi ubin derau, kilometer. Ini yang menentukan besar awannya.
    float tileKm = 12.0f;
    /// Pengali skala volume rincian terhadap volume bentuk.
    float detailScale = 7.0f;
    /// Kekuatan kikisan rincian di tepi.
    float detailStrength = 0.35f;

    /// Cuplikan sepanjang pandangan, dan cuplikan menuju matahari. Keduanya
    /// mengalikan biaya pass ini secara langsung.
    int viewSamples = 48;
    int lightSamples = 6;

    /// Serapan menuju matahari dan menembus awan.
    float sunAbsorption = 0.85f;
    float cloudAbsorption = 1.05f;
    /// Lantai kegelapan bagian dalam awan, pengganti murah hamburan berganda.
    /// Nol menghasilkan awan sehitam batu.
    float darknessThreshold = 0.18f;

    /// Kecepatan angin, km per detik. Menggeser ubin derau secara mendatar.
    float windSpeed = 0.01f;
};

/// Kamera editor. Rotasi disimpan sebagai quaternion supaya tidak ada gimbal
/// lock saat kamera fly diarahkan lurus ke atas/bawah.
struct Camera {
    Vec3 position{0.0f, 3.0f, 8.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    float fovYRadians = 60.0f * kDegToRad;
    float nearZ = 0.05f;
    float farZ = 2000.0f;
    bool orthographic = false;
    float orthoHeight = 10.0f;

    Vec3 Forward() const { return rotation * Vec3(0.0f, 0.0f, -1.0f); }
    Vec3 Right() const { return rotation * Vec3(1.0f, 0.0f, 0.0f); }
    Vec3 Up() const { return rotation * Vec3(0.0f, 1.0f, 0.0f); }

    Mat4 View() const { return LookAt(position, position + Forward(), Up()); }

    Mat4 Projection(float aspect) const {
        if (orthographic) {
            const float halfHeight = orthoHeight * 0.5f;
            const float halfWidth = halfHeight * aspect;
            Mat4 proj = glm::ortho(-halfWidth, halfWidth, -halfHeight, halfHeight, nearZ, farZ);
            proj[1][1] *= -1.0f;
            return proj;
        }
        return Perspective(fovYRadians, aspect, nearZ, farZ);
    }
};

/// Sebuah volume `.vdb` yang digambar di viewport.
///
/// **Gridnya sudah dimuat, bukan sebuah jalur berkas.** Yang membacanya dari
/// disk adalah pemanggil lewat `Sim::Volume`; renderer tidak pernah menautkan
/// OpenVDB, dengan aturan yang sama yang menjaga OpenImageIO di luar jalur
/// runtime. Nullptr berarti tidak ada volume yang digambar.
struct ViewportVolume {
    const VolumeGrid* grid = nullptr;

    /// Naik setiap kali isi `grid` berubah.
    ///
    /// **Pointer saja tidak cukup**: memuat volume kedua ke alamat yang baru
    /// saja dibebaskan volume pertama menghasilkan pointer yang sama dengan isi
    /// yang berbeda, dan yang tergambar adalah asap yang lama. Unggahannya
    /// berharga puluhan megabyte, jadi ia hanya terjadi ketika angka ini
    /// berubah.
    uint64_t revision = 0;

    /// Di mana titik asal grid berada di ruang dunia.
    ///
    /// **Tanpa rotasi.** Kotaknya sejajar sumbu — itu bentuk yang dibawa berkas
    /// VDB sendiri, dan yang membuat raymarch bisa memakai satu uji slab.
    /// Memutarnya menuntut ray dipindahkan ke ruang lokal lebih dulu, dan itu
    /// belum ada.
    Vec3 position{0.0f};
    /// Pengali ukuran. Kerapatannya tidak ikut diskala: volume yang dibesarkan
    /// dua kali punya jalur dua kali lebih panjang, jadi ia memang lebih pekat.
    float scale = 1.0f;

    /// Kepunahan per satuan kerapatan per meter.
    float extinction = 1.0f;
    /// Berapa bagian dari yang punah kembali sebagai cahaya.
    Vec3 scatterAlbedo{0.8f};
    /// Cahaya yang datang, sudah termasuk fase dan bayangannya.
    ///
    /// **Tetapan, bukan hasil hitungan.** Bayangan sendiri di dalam asap
    /// menuntut raymarch kedua ke arah matahari; sampai itu ada, ini yang
    /// dipakai — dan asap yang dihasilkannya rata, bukan berlekuk.
    Vec3 incomingLight{1.0f};

    /// Panjang satu langkah, meter.
    float stepSize = 0.05f;
    uint32_t maxSteps = 256;
    /// Transmitansi di mana penjejakan berhenti.
    float minTransmittance = 0.003f;
};

/// Pengaturan sekali-gambar untuk sebuah viewport.
struct ViewportDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    Camera camera;

    /// Langkah waktu yang dipaksakan, detik. Negatif berarti "ukur sendiri".
    ///
    /// **Bawaannya negatif, dan itu yang membuat medan ini bukan beban.**
    /// Perender mengukur waktunya sendiri justru supaya tidak ada pemanggil yang
    /// perlu tahu bahwa eksposur beradaptasi dan awan bergerak — lihat
    /// catatannya di `VulkanRenderer::Render`. Yang berubah di sini hanyalah
    /// bahwa satu pemanggil boleh berkata "pakai angka ini", dan hanya satu yang
    /// melakukannya: mode ukur.
    ///
    /// Alasannya bukan kerapian melainkan syarat. Sebuah frame yang waktunya
    /// datang dari jam dinding tidak pernah sama dua kali, jadi dua gambar dari
    /// binary yang identik pun berbeda — dan setiap milestone optimisasi
    /// mengklaim "lebih cepat dengan gambar yang sama". Separuh klaim itu tidak
    /// bisa diperiksa selama perbandingannya tenggelam dalam derau yang
    /// besarnya tidak diketahui.
    float fixedDeltaSeconds = -1.0f;
    DrawMode mode = DrawMode::Material;
    Vec4 clearColor{0.13f, 0.14f, 0.16f, 1.0f};
    bool showGrid = true;
    float gridCellSize = 1.0f;   ///< meter per petak kecil
    float gridFadeDistance = 120.0f;

    /// Bayangan matahari boleh digambar sama sekali.
    ///
    /// **Arah dan radiansi mataharinya tidak ada di sini lagi.** Keduanya dulu
    /// medan `ViewportDesc` dengan alasan yang tertulis di tempatnya: "pengaturan
    /// tampilan viewport selama scene belum benar-benar punya lampu
    /// directional". Syarat itu sudah lama terpenuhi, dan yang tersisa adalah
    /// matahari yang menyala tanpa dimiliki entity mana pun — sehingga menghapus
    /// lampu terakhir sebuah level tidak menggelapkan apa pun, dan tidak ada
    /// tempat untuk mematikannya. Sekarang keduanya datang dari
    /// `ViewportScene::lights`, dan adegan tanpa lampu memang gelap.
    bool castShadows = true;

    /// Langit atmosferik. Saat mati, latar viewport tetap `clearColor`.
    ///
    /// **Bawaannya menyala sejak matahari disetir Time-of-Day**, dan itu syarat
    /// yang memang sudah dipenuhi. Sebelumnya ia mati karena langit menggantikan
    /// warna latar yang disetel pemakai — editor yang mengganti latar sendiri
    /// tanpa diminta sulit dibedakan dari editor yang rusak. Yang membalik
    /// timbangannya: langit yang mati secara bawaan adalah langit yang tidak
    /// pernah dilihat siapa pun, dan cacat pada sesuatu yang tidak pernah
    /// terlihat adalah cacat yang tidak pernah ditemukan. Sakelarnya tetap ada.
    bool skyEnabled = true;
    /// Pengali radiansi langit. Atmosfer Bruneton menghasilkan angka dalam
    /// satuannya sendiri; sampai lampu memakai satuan fotometrik, angka ini yang
    /// menjembatani keduanya.
    float skyIntensity = 20.0f;
    /// Ketinggian kamera di atas permukaan laut, kilometer.
    float cameraHeightKm = 0.5f;

    /// Sumber langitnya. HDRI mematikan aerial perspective dan awan volumetrik
    /// dengan sendirinya: keduanya milik model atmosfer prosedural, dan
    /// menumpuknya di atas foto langit berarti menghitung udara yang sama dua
    /// kali dengan dua warna yang berlainan.
    SkySource skySource = SkySource::Atmosphere;
    /// Jalur berkas `.hdr` saat `skySource` adalah `HdrMap`. Hanya sah selama
    /// panggilan `Render` — renderer menyalin apa yang perlu disimpannya.
    std::string_view hdriPath;
    /// Volume `.vdb` yang digambar di viewport ini. `grid` nullptr berarti tidak
    /// ada — dan pass-nya tidak ikut didaftarkan sama sekali.
    ViewportVolume volume;

    /// Putaran mendatar peta HDR, radian. Yang dipakai menyelaraskan matahari di
    /// dalam petanya dengan matahari yang menyinari adegan.
    float hdriRotation = 0.0f;
    /// Apa yang **menyinari** tingkat panggang: langit yang tergambar, atau
    /// berkas HDR yang disebut `hdriPath`.
    ///
    /// **Terpisah dari `skySource`, dan keduanya memang bisa berbeda.** Yang
    /// satu menjawab "langit yang mana yang tergambar", yang lain "adegan ini
    /// disinari apa" — sebuah level boleh menggambar langit atmosferik sambil
    /// disinari sebuah foto, dan sebaliknya. Cerminan
    /// `scene::WorldSettings::environment`; pemetaannya sekali, di
    /// `Sim::SceneView`.
    EnvironmentSource environment = EnvironmentSource::Sky;

    /// Keluarkan matahari dari berkas lingkungan sebelum dipanggang (B4).
    /// Cerminan `scene::WorldSettings::extractSun`.
    bool extractSunFromEnvironment = false;

    /// Jarak antar-probe iradiansi, meter (S1). Cerminan
    /// `scene::WorldSettings::probeSpacing`.
    ///
    /// **Nol berarti tidak ada kisi**, dan itu keadaan yang sah: tingkat `None`
    /// dan `RealTime` tidak memanggang kisi, dan begitu juga jalur yang
    /// memanggil renderer tanpa `World` sama sekali — uji, thumbnail, pratinjau
    /// material. Permukaan lalu memakai SH panggang yang berlaku sama di seluruh
    /// adegan, persis seperti sebelum S1 ada.
    float probeSpacing = 0.0f;

    /// Isi kisi probe dengan papan catur atas koordinat probe, bukan dengan
    /// langit (S1, jalur ukur).
    ///
    /// **Kontrol positif, dan itu gunanya ia ada.** Tanpa transport, tiap probe
    /// memuat SH yang sama — sehingga gambar lewat kisi identik dengan gambar
    /// lewat SH panggang *baik ketika kisinya benar maupun ketika ia tidak
    /// pernah dibaca satu piksel pun*. Angka nol yang sama menjawab dua keadaan
    /// yang berlawanan, dan itu membuatnya bukan bukti apa-apa sendirian.
    ///
    /// Papan catur memutus kesamaan itu: isinya bergantung pada koordinat probe,
    /// jadi gambarnya wajib bergaris dengan periode dua kali jarak antar-probe.
    /// Garis yang tidak muncul berarti kisinya mati; garis yang periodenya lain
    /// atau patah tiap empat sel berarti indeks brick-nya salah.
    bool probeDebugPattern = false;

    /// Pengali radiansi peta HDR.
    ///
    /// **Terpisah dari `skyIntensity`, dan itu bukan duplikasi.** Atmosfer
    /// Bruneton menghasilkan angka dalam satuannya sendiri, jadi pengalinya
    /// berguna di sekitar 20; berkas HDR sudah berisi radiansi, jadi pengalinya
    /// berguna di sekitar 1. Satu angka untuk keduanya berarti berpindah sumber
    /// diam-diam mengalikan langit dua puluh kali — yang tidak terlihat sebagai
    /// pengali yang salah melainkan sebagai eksposur otomatis yang "menggelapkan
    /// segalanya".
    float hdriIntensity = 1.0f;

    /// Aerial perspective: udara yang berada **di antara** kamera dan permukaan.
    ///
    /// Terpisah dari `skyEnabled` karena keduanya bisa dinilai sendiri-sendiri —
    /// dan karena yang ini punya biaya yang bisa dibedakan di tabel pass.
    bool aerialPerspective = true;
    /// Pengali kerapatan aerosol Mie. Satu berarti udara seperti yang dimodelkan
    /// Bruneton.
    ///
    /// **Yang dikalikan aerosol, bukan seluruh atmosfer.** Aerosol adalah apa
    /// yang membuat udara berkabut; mengalikan Rayleigh ikut akan membirukan
    /// udara tanpa mengaburkannya — kebalikan dari yang diminta orang saat
    /// meminta kabut lebih tebal.
    float aerialHaze = 1.0f;

    /// Awan volumetrik.
    CloudSettings clouds;

    /// Pengaturan post-process: eksposur, operator nada, dan yang menyusul di
    /// atasnya.
    PostProcessSettings post;

    /// Pengaturan global illumination. Mengalir dari editor ke renderer;
    /// backend yang **akhirnya** dipakai mengalir balik lewat
    /// `IViewportRenderer::GiBackend()` — permintaan dan hasil sengaja dua hal
    /// yang berbeda, karena permintaan bisa tidak terpenuhi.
    GiSettings gi;

    /// Debug view yang menggantikan seluruh gambar dengan keluaran satu pass
    /// compute (G3).
    ///
    /// **Sebuah pemeriksaan jalur, bukan sebuah tampilan diagnostik atas
    /// adegan.** Yang dijawabnya cuma satu pertanyaan — apakah compute berjalan
    /// di mesin ini — dan pertanyaan itu tidak punya jawaban lain yang murah:
    /// pass compute yang gagal dibuat, shader yang ditolak driver, dan barrier
    /// yang salah semuanya terlihat sebagai fitur yang "tidak muncul", pada
    /// mesin yang bukan mesin tempat fiturnya ditulis.
    bool computeGradient = false;

    /// Penetapan lampu ke cluster dijalankan di GPU (G4).
    ///
    /// **Sakelarnya ada karena jalur CPU-nya masih ada, dan jalur CPU-nya masih
    /// ada karena ia pembanding.** Dua implementasi dari aturan yang sama —
    /// satu diuji tanpa GPU, satu dijalankan GPU — adalah cara termurah
    /// menemukan yang mana yang salah ketika lampu mulai hilang di sudut
    /// tertentu. Yang mahal bukan memelihara keduanya melainkan mencari selisih
    /// di antara satu implementasi dan harapan.
    bool gpuClusters = true;

    /// Komposit clipmap SDF dijalankan di GPU (G4).
    ///
    /// Sakelarnya ada karena alasan yang sama dengan `gpuClusters`, dan di sini
    /// alasan itu lebih kuat: satu-satunya pembaca kaskade SDF adalah
    /// penelusuran GI, jadi jalur yang salah tidak terlihat sebagai gambar yang
    /// salah melainkan sebagai cahaya pantul yang sedikit berbeda — dan tidak
    /// ada yang bisa menilai "sedikit berbeda" dengan mata. Yang menilainya
    /// perbandingan isi voxel antara kedua jalur.
    bool gpuSdf = true;

    /// Frustum culling dan pembangkitan perintah gambar dijalankan di GPU (G6).
    ///
    /// Sakelarnya ada karena alasan yang sama dengan `gpuClusters`, ditambah
    /// satu yang khusus: jalur CPU adalah satu-satunya yang bisa dipakai
    /// perangkat tanpa `multiDrawIndirect`, jadi ia bukan sekadar pembanding
    /// melainkan jalur yang benar-benar dipakai orang. Yang berhenti dijalankan
    /// siapa pun adalah yang diam-diam rusak.
    bool gpuCull = true;

    /// Occlusion culling terhadap piramida depth frame ini (G6).
    ///
    /// **Tepat, dan tetap mati secara bawaan — sekarang karena angkanya.** Ia
    /// menghasilkan gambar yang sama persis byte demi byte dengan jalur tanpa
    /// occlusion pada kedua adegan uji, dan menurunkan segitiga yang diserahkan
    /// pass forward dari 2,79 menjadi 2,65 juta pada adegan padat. Yang tidak
    /// ikut turun adalah waktunya: dua pass yang ditambahkannya berharga 0,059
    /// ms sementara yang dihemat pass forward sekitar 0,03 ms. Ia baru menang
    /// pada adegan yang benar-benar berlapis; sampai ada yang seperti itu di
    /// repo ini, ia sebuah sakelar.
    ///
    /// Menyalakannya menuntut `gpuCull`.
    bool gpuOcclusion = false;

    /// Menuliskan angka antara uji occlusion tiap permukaan ke buffer yang bisa
    /// dibaca CPU (G6).
    ///
    /// **Alat diagnostik, dan ia ada karena menebak sudah kehabisan giliran.**
    /// Tanpa ini satu-satunya cara memeriksa uji occlusion adalah
    /// membandingkan gambar — yang hanya bisa menjawab "ada yang salah", tidak
    /// pernah "yang mana".
    bool cullDebug = false;
    /// Nomor permukaan tertinggi yang boleh diuji occlusion, eksklusif.
    ///
    /// Alat bisect: menaikkannya setengah-setengah menemukan permukaan mana
    /// yang hilang dari gambar saat occlusion menyala. Bawaannya "semua".
    uint32_t cullLimit = 0xffffffffu;
    /// Nomor permukaan terendah yang digambar. Pasangan `cullLimit`.
    uint32_t cullFirst = 0;

    /// Pass compute yang tidak menghalangi apa pun dijalankan di antrean
    /// terpisah, ditumpangkan di atas pekerjaan grafis (G7).
    ///
    /// **Sakelarnya wajib ada dan wajib dipakai membandingkan.** Balapan
    /// lintas-antrean muncul sebagai kedipan di satu GPU dan tidak pernah di GPU
    /// lain, dan validation layer tidak melihatnya; satu-satunya cara memeriksa
    /// jalur ini adalah menjalankan adegan yang sama dengan sakelar ini mati dan
    /// membandingkan gambarnya byte demi byte.
    ///
    /// Menuntut keluarga antrean compute yang terpisah **dan** timeline
    /// semaphore. Tanpa keduanya ia diam-diam kembali ke satu antrean — bukan
    /// gagal, karena jalur satu-antrean memang jalur yang benar di sana.
    ///
    /// **Mati secara bawaan karena angkanya, bukan karena ia salah.** Gambarnya
    /// sama persis byte demi byte di kedua adegan uji, GI mati maupun menyala.
    /// Yang menahannya: frame adegan padat turun 7,50 menjadi 7,21 ms — 3,9% —
    /// karena `sdf-fill` sendiri berjalan 36% lebih lambat di antrean compute,
    /// dan yang didapat dari menjalankan keduanya bersamaan sebagian besar
    /// dibayar kembali di sana. Angkanya lengkap di G7 pada
    /// docs/PLAN-GPU-OPTIM.md.
    bool asyncCompute = false;
};

/// Satu objek yang bisa digambar, sudah dalam ruang dunia.
///
/// `boundsMin/boundsMax` adalah AABB dalam ruang lokal objek. Sampai mesh
/// sungguhan bisa dimuat (E5/E8) nilainya kubus satuan, dan StubRenderer
/// menggambar wireframe kotak itu. Renderer E8 akan memakai field yang sama
/// untuk frustum culling dan mengabaikan bagian wireframe-nya.
struct MeshInstance {
    Mat4 transform{1.0f};
    /// Geometrinya. `kUnitCubeMesh` berarti kubus satuan yang diskalakan ke
    /// `boundsMin/boundsMax` — perilaku yang berlaku sebelum importir mesh ada,
    /// dan yang tetap berlaku untuk entity tanpa aset mesh.
    ///
    /// **Untuk mesh sungguhan, `transform` dipakai apa adanya.** Vertexnya sudah
    /// berada di ruang lokal yang sama dengan batasnya, jadi memetakannya ulang
    /// lewat kotak batas — seperti yang harus dilakukan kubus — akan menskalakan
    /// mesh dua kali.
    MeshHandle mesh = kUnitCubeMesh;
    Vec3 boundsMin{-0.5f, -0.5f, -0.5f};
    Vec3 boundsMax{0.5f, 0.5f, 0.5f};

    /// Ruas milik instance ini di dalam `ViewportScene::skinMatrices`.
    ///
    /// `skinCount` nol berarti instance ini digambar tanpa kulit — jalur yang
    /// berlaku untuk seluruh mesh statis, dan juga untuk mesh ber-rig yang tidak
    /// dipasok pose. Yang tergambar lalu adalah vertexnya apa adanya, yaitu
    /// bind pose-nya.
    ///
    /// **Ruas ke dalam satu larik bersama, bukan span per instance.** Seluruh
    /// pose satu frame diunggah sekali sebagai satu buffer; satu span per
    /// instance berarti satu penyalinan per karakter per frame, dan renderer
    /// yang menyusun ulang larik yang sudah bersebelahan di memori pemanggilnya.
    /// Petak instance ini di dalam atlas lightmap: xy skala, zw geseran (S5).
    ///
    /// **Skala nol berarti tidak ber-lightmap**, dan itu keadaan yang sah —
    /// bukan bawaan yang belum diisi: objek dinamis memang tidak punya petak,
    /// dan yang membacanya jatuh ke kisi probe. Nol dipilih sebagai penanda
    /// karena petak yang sah selalu punya luas.
    Vec4 lightmapScaleOffset{0.0f};

    uint32_t skinFirst = 0;
    uint32_t skinCount = 0;

    /// Ruas milik instance ini di dalam `ViewportScene::partColors`.
    ///
    /// Warna ber-alpha nol berarti "slot ini tidak ditetapkan" — renderer lalu
    /// memakai material yang tertulis di berkas mesh untuk ruas itu, dan `color`
    /// di bawah bila berkasnya pun tidak menyebut apa-apa.
    uint32_t partFirst = 0;
    uint32_t partCount = 0;

    Vec4 color{0.72f, 0.74f, 0.78f, 1.0f};
    bool selected = false;

    /// Jarak terjauh dari kamera tempat instance ini masih dirasterisasi, meter.
    /// **Nol berarti tak terbatas**, dan itu bawaannya.
    ///
    /// **Rasterisasi saja, bukan pencahayaan.** Clipmap SDF membaca
    /// `ViewportScene::meshes` langsung, bukan daftar yang sudah disaring, jadi
    /// objek yang berhenti digambar tetap menyumbang cahaya tak-langsung dan
    /// tetap menghalangi sinar GI. Kalau tidak, berjalan menjauh dari sebuah
    /// dinding akan menyalakan ruangan di baliknya.
    ///
    /// **Bayangannya ikut berhenti**, sama seperti di Unreal: yang tidak
    /// digambar tidak menjatuhkan bayangan. Itu yang membuat setelan ini
    /// menyentuh pass yang paling mahal — bayangan 47% frame GPU pada adegan
    /// padat — dan sekaligus yang membuatnya salah dipakai pada benda besar.
    float maxDrawDistance = 0.0f;

    /// Ikut pass bayangan. Yang tidak menjatuhkan bayangan tetap digambar
    /// seperti biasa — ia hanya tidak muncul di peta bayangan.
    bool castShadows = true;
    /// Menerima bayangan dari yang lain. Dimatikan biasanya untuk permukaan
    /// yang bayangannya sudah dipanggang, atau untuk latar yang tidak boleh
    /// tergelapkan apa pun.
    bool receiveShadows = true;
};

enum class LightKind : uint8_t {
    Directional,
    Point,
    Spot,
};

/// Sebuah lampu dalam ruang dunia.
///
/// Bentuknya sengaja berbeda dari `scene::LightComponent`: yang di sini sudah
/// dalam ruang dunia dan sudut kerucutnya sudah menjadi kosinus. Renderer tidak
/// boleh mengenal tipe komponen — itu seam #1 di docs/ARCHITECTURE.md — dan
/// menyalinnya apa adanya berarti renderer ikut memutuskan bagaimana rotasi
/// entity menjadi arah pancar.
struct LightInstance {
    LightKind kind = LightKind::Point;
    Vec3 position{0.0f};
    /// Untuk directional dan spot: arah **dari permukaan ke cahaya** pada
    /// directional, dan arah pancar (dari lampu ke luar) pada spot. Keduanya
    /// dibedakan karena keduanya memang menjawab pertanyaan yang berbeda, dan
    /// menyatukannya berarti satu tanda yang harus diingat di setiap pemakaian.
    Vec3 direction{0.0f, -1.0f, 0.0f};
    Vec3 color{1.0f};
    float intensity = 1.0f;
    float range = 10.0f;
    /// Jari-jari sumber, meter. Membatasi peredupan kuadrat terbalik dari dekat:
    /// cahaya tidak pernah lebih dekat daripada permukaan lampunya sendiri.
    float sourceRadius = 0.01f;
    float cosInner = 0.9f;
    float cosOuter = 0.8f;
    /// Hanya berarti untuk directional sampai atlas bayangan point/spot ada.
    bool castShadows = true;
};

/// Garis dalam ruang dunia. Dipakai grid bantu, sumbu, dan penanda hubungan
/// parent-child di outliner.
struct LineSegment {
    Vec3 a{0.0f};
    Vec3 b{0.0f};
    Vec4 color{1.0f};
    /// Digambar menembus geometri, tanpa uji kedalaman.
    ///
    /// **Untuk garis yang menjelaskan sesuatu yang tidak terlihat.** Rangka
    /// kawat collider adalah contohnya: bentuk tabrakan hampir selalu berada di
    /// dalam mesh yang menggambarkannya, jadi garis yang diuji kedalaman
    /// tertutup persis oleh benda yang ukurannya sedang diperiksa orang.
    ///
    /// Bawaannya false: garis yang menggambarkan sesuatu yang **ada** di dunia —
    /// batas volume asap, sumbu, kisi bantu — harus tertutup benda di depannya,
    /// karena kalau tidak ia terbaca melayang di depan segalanya.
    bool throughGeometry = false;
};

/// Isi yang harus digambar frame ini.
///
/// Diisi ulang tiap frame dan hanya menunjuk ke penyimpanan milik pemanggil —
/// renderer tidak boleh menyimpan span ini melewati Render().
///
/// Tidak ada daftar ikon di sini, dan itu disengaja. Penanda lampu, kamera, dan
/// node kosong adalah alat bantu editor, bukan isi scene: menaruhnya di sini
/// akan memaksa renderer mengenali tipe komponen untuk memilih gambar yang
/// tepat. Panel Viewport menggambarnya sendiri sebagai overlay 2D.
struct ViewportScene {
    std::span<const MeshInstance> meshes;
    /// Palet kulit seluruh instance ber-skin frame ini, bersambung.
    ///
    /// Isinya **matriks kulit — global × invers bind** — bukan transform bone,
    /// yaitu persis yang dihasilkan `animation::Pose::ComputeSkinning`.
    /// Mengirim transform global saja berarti setiap vertex harus dikembalikan
    /// ke ruang bind di dalam vertex shader: satu inversi matriks per vertex per
    /// frame untuk hasil yang tidak berubah selama rangkanya tidak berubah.
    ///
    /// Renderer tidak mengenal rangka mana pun; yang dibacanya hanya ruas yang
    /// ditunjuk tiap `MeshInstance`. Ruas yang melewati ujung larik ini
    /// menggambar instance-nya sebagai mesh statis, bukan sebagai kulit yang
    /// membaca memori orang lain.
    std::span<const Mat4> skinMatrices;
    /// Warna dasar per ruas seluruh instance frame ini, bersambung. Ditunjuk
    /// tiap `MeshInstance` lewat `partFirst`/`partCount`, sama seperti palet
    /// kulit — dan karena alasan yang sama: satu span per instance berarti satu
    /// penyalinan per entity per frame.
    std::span<const Vec4> partColors;
    /// Tekstur albedo per ruas, **sejajar dengan `partColors`**. Nol berarti
    /// tidak ada, dan ruas itu digambar dengan warnanya saja.
    ///
    /// Sejajar dan bukan digabung menjadi satu struct: `partColors` sudah
    /// menjadi kunci pengelompokan draw, dan menambah medan ke dalamnya membuat
    /// dua ruas yang warnanya sama tetapi teksturnya sama pula gagal
    /// dikelompokkan hanya karena strukturnya membesar.
    std::span<const TextureHandle> partTextures;
    /// Material per ruas, **sejajar indeks** dengan `partColors`. Kosong atau
    /// lebih pendek berarti ruas sesudahnya memakai jalur mundur.
    std::span<const MaterialHandle> partMaterials;
    std::span<const LineSegment> lines;
    /// Lampu punctual. Directional boleh ada di sini juga — renderer memakai
    /// yang pertama sebagai matahari dan mengabaikan sisanya, karena cascade
    /// bayangan hanya ada satu himpunan.
    std::span<const LightInstance> lights;
};

}  // namespace sim::render
