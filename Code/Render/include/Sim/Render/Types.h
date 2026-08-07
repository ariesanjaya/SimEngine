#pragma once

#include "Sim/Core/Math.h"

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

    /// Pengali eksposur, dipakai seragam oleh matahari dan lampu punctual.
    ///
    /// **Berdiri sebagai pengganti tone mapping sampai E8.8.** Target warna
    /// viewport 8-bit dan tidak ada satu pun operator nada di antaranya, jadi
    /// radiance sungguhan — matahari bawaan bernilai 3,0 — akan terpotong putih.
    /// Yang salah bukan angkanya melainkan tidak adanya yang memetakannya.
    ///
    /// Sebuah parameter, bukan konstanta di dalam shader, karena alasan yang
    /// sama dengan `sourceRadius`: angka yang tersembunyi tidak bisa disetel
    /// siapa pun dan akan dikira bagian dari model. Bawaannya dipilih supaya
    /// matahari bawaan menghasilkan tepat 0,75 — nilai yang dulu ditulis
    /// langsung di `box.frag` — sehingga adegan yang ada tidak berubah rupa.
    ///
    /// **Berlaku untuk kedua jalur cahaya.** Matahari dan lampu punctual pada
    /// skala yang berbeda adalah persis jenis ketidakcocokan yang paling sulit
    /// dilacak: setiap lampu terlihat masuk akal sendiri-sendiri.
    float exposure = 0.25f;
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
