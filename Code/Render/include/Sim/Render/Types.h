#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Render/Bloom.h"
#include "Sim/Render/TraceBackend.h"

#include <cstdint>
#include <span>

namespace sim::render {

/// Handle buram ke tekstur GPU yang bisa digambar UI.
///
/// Nilainya sengaja uint64_t dan bukan pointer Vulkan: sejak Dear ImGui 1.92,
/// ImTextureID adalah ImU64, jadi handle ini bisa dilempar langsung ke
/// ImGui::Image() tanpa modul Editor perlu tahu apa pun soal Vulkan.
using TextureHandle = uint64_t;
inline constexpr TextureHandle kInvalidTexture = 0;

enum class DrawMode : uint8_t {
    Lit,
    Unlit,
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

/// Pengaturan sekali-gambar untuk sebuah viewport.
struct ViewportDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    Camera camera;
    DrawMode mode = DrawMode::Lit;
    Vec4 clearColor{0.13f, 0.14f, 0.16f, 1.0f};
    bool showGrid = true;
    float gridCellSize = 1.0f;   ///< meter per petak kecil
    float gridFadeDistance = 120.0f;

    /// Arah **dari permukaan ke matahari**, tidak harus ternormalisasi.
    ///
    /// Di sini, bukan di `ViewportScene`: ia pengaturan tampilan viewport
    /// selama scene belum benar-benar punya lampu directional. Begitu komponen
    /// lampu dibaca renderer, medan ini yang menjadi nilai mundurnya.
    Vec3 sunDirection{-0.4f, 0.8f, 0.45f};
    /// Radiance matahari — warna dikali intensitas — saat scene tidak punya
    /// lampu directional. Angkanya sama dengan matahari yang disemai editor,
    /// jadi scene tanpa matahari tampak seperti scene dengan matahari bawaan.
    Vec3 sunRadiance{3.0f};
    bool castShadows = true;

    /// Langit atmosferik. Saat mati, latar viewport tetap `clearColor`.
    ///
    /// **Menyala secara bawaan tidak otomatis benar**: langit menggantikan warna
    /// latar yang selama ini disetel pemakai, dan editor yang mengganti latar
    /// sendiri tanpa diminta sulit dibedakan dari editor yang rusak. Sakelarnya
    /// karena itu ada, dan bawaannya mati sampai matahari benar-benar disetir
    /// Time-of-Day.
    bool skyEnabled = false;
    /// Pengali radiansi langit. Atmosfer Bruneton menghasilkan angka dalam
    /// satuannya sendiri; sampai lampu memakai satuan fotometrik, angka ini yang
    /// menjembatani keduanya.
    float skyIntensity = 20.0f;
    /// Ketinggian kamera di atas permukaan laut, kilometer.
    float cameraHeightKm = 0.5f;

    /// Pengaturan post-process: eksposur, operator nada, dan yang menyusul di
    /// atasnya.
    PostProcessSettings post;

    /// Pengaturan global illumination. Mengalir dari editor ke renderer;
    /// backend yang **akhirnya** dipakai mengalir balik lewat
    /// `IViewportRenderer::GiBackend()` — permintaan dan hasil sengaja dua hal
    /// yang berbeda, karena permintaan bisa tidak terpenuhi.
    GiSettings gi;
};

/// Satu objek yang bisa digambar, sudah dalam ruang dunia.
///
/// `boundsMin/boundsMax` adalah AABB dalam ruang lokal objek. Sampai mesh
/// sungguhan bisa dimuat (E5/E8) nilainya kubus satuan, dan StubRenderer
/// menggambar wireframe kotak itu. Renderer E8 akan memakai field yang sama
/// untuk frustum culling dan mengabaikan bagian wireframe-nya.
struct MeshInstance {
    Mat4 transform{1.0f};
    Vec3 boundsMin{-0.5f, -0.5f, -0.5f};
    Vec3 boundsMax{0.5f, 0.5f, 0.5f};
    Vec4 color{0.72f, 0.74f, 0.78f, 1.0f};
    bool selected = false;
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
    std::span<const LineSegment> lines;
    /// Lampu punctual. Directional boleh ada di sini juga — renderer memakai
    /// yang pertama sebagai matahari dan mengabaikan sisanya, karena cascade
    /// bayangan hanya ada satu himpunan.
    std::span<const LightInstance> lights;
};

}  // namespace sim::render
