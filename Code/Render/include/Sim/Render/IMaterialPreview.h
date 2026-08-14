#pragma once

#include "Sim/Render/Types.h"

#include <cstdint>
#include <span>
#include <string>

namespace sim::render {

/// Bentuk yang dipakai preview material.
enum class PreviewShape : uint8_t {
    Sphere,
    Cube,
    Plane,
};

const char* ToString(PreviewShape shape);

/// Shader sebuah material, sudah dalam bentuk SPIR-V.
///
/// **Ini tetap bebas Vulkan.** SPIR-V hanyalah larik `uint32_t`; yang membuat
/// seam bocor bukan formatnya melainkan tipe seperti `VkShaderModule`. Panel
/// material menyerahkan hasil `ShaderCache` apa adanya dan tidak pernah melihat
/// satu pun simbol Vulkan.
struct MaterialPreviewShaders {
    std::span<const uint32_t> vertexSpirv;
    std::span<const uint32_t> fragmentSpirv;
    /// Ukuran blok uniform material, dari `MaterialParameterBlock::Bytes()`.
    uint32_t parameterBytes = 0;
    /// Banyaknya slot tekstur yang dideklarasikan material.
    uint32_t textureCount = 0;
    /// Menyalakan konstanta spesialisasi `kAlphaTest`.
    ///
    /// Ada di sini, bukan di `MaterialPreviewDesc`, karena ia menentukan
    /// pipeline: mengubahnya berarti membangun ulang, dan sesuatu yang dibangun
    /// ulang tidak boleh duduk di struct yang berganti tiap frame.
    bool alphaTest = false;
};

/// Pengaturan sekali-gambar untuk preview.
struct MaterialPreviewDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    PreviewShape shape = PreviewShape::Sphere;

    /// Orbit kamera mengelilingi objek yang diam, radian.
    ///
    /// Objeknya tidak pernah diputar. Arah cahaya diberikan terpisah dan tidak
    /// ikut kamera, jadi mengorbit memperlihatkan permukaan yang sama dari sudut
    /// pandang lain sementara cahayanya tetap di tempatnya — itulah cara melihat
    /// bagaimana sebuah material berubah rupa terhadap sudut pandang, yang untuk
    /// material anisotropik dan ber-coat justru bagian yang paling penting.
    float cameraYaw = 0.0f;
    float cameraPitch = 0.0f;
    /// Jarak kamera dari pusat objek.
    float distance = 3.0f;
    /// Arah **dari permukaan ke cahaya**, tidak harus ternormalisasi.
    Vec3 lightDirection{0.4f, 0.7f, 0.6f};
    Vec3 lightRadiance{3.0f, 3.0f, 3.0f};
    Vec4 clearColor{0.09f, 0.09f, 0.11f, 1.0f};
    float alphaCutoff = 0.5f;
    float time = 0.0f;
};

/// Batas antara panel Material Editor dan rendering.
///
/// **Seam kedua, bukan instance kedua `IViewportRenderer`.** Catatan E7.1
/// menyebut jalan keluarnya "membuat instance KEDUA lewat pabrik yang sudah
/// ada", dan itu memang menyelesaikan tabrakan target render — tapi tidak
/// menyelesaikan gunanya. `ViewportScene` tidak punya tempat untuk sebuah
/// material: ia daftar `MeshInstance` berwarna solid. Instance kedua
/// `IViewportRenderer` karena itu akan menggambar bola abu-abu, yaitu persis
/// kekosongan yang membuat preview ditunda sejak awal.
///
/// Yang dibutuhkan preview memang bukan "satu scene lagi" melainkan satu mesh,
/// satu material, satu cahaya. Antarmuka ini menyebutkan tepat itu, dan tetap
/// bebas Vulkan seperti seam pertama.
///
/// **Materialnya diserahkan sebagai SPIR-V, bukan sebagai graph.** Preview yang
/// mengompilasi sendiri akan menjadi implementasi kedua dari jalur graph →
/// shader, dan seluruh E7.1 dipegang oleh satu alasan: model shading hanya boleh
/// punya satu implementasi. Yang dilihat preview adalah shader yang sama persis
/// dengan yang nanti dipakai renderer.
class IMaterialPreview {
public:
    virtual ~IMaterialPreview() = default;

    /// Membangun pipeline dari shader sebuah material. Mengembalikan false bila
    /// gagal; alasannya di `LastError()`.
    ///
    /// Mahal — membangun modul shader, layout descriptor, dan pipeline. Panel
    /// hanya boleh memanggilnya saat materialnya benar-benar berubah.
    virtual bool SetMaterial(const MaterialPreviewShaders& shaders) = 0;

    /// Melepas material yang sedang dipakai. Preview kembali menggambar latar
    /// kosong, bukan menggambar dengan pipeline yang sudah dibuang.
    virtual void ClearMaterial() = 0;
    virtual bool HasMaterial() const = 0;

    /// Memasang tekstur tiap slot material, urutannya sama dengan deklarasi
    /// materialnya.
    ///
    /// **Berkas `.ktx2` yang sudah di-bake, bukan berkas sumber** — aturan yang
    /// sama dengan `IViewportRenderer::AcquireTexture`, dan alasan yang sama:
    /// yang mendekode gambar adalah baker, bukan yang menggambar. Jalur yang
    /// kosong berarti slot itu memakai putih 1×1, yaitu nilai satuan perkalian.
    ///
    /// Sebelum ini ada, **setiap** slot memakai putih — pratinjau menyatakan
    /// jumlah teksturnya dan tidak pernah menerima satu pun gambar, jadi
    /// material bertekstur tampak persis seperti material polos.
    virtual void SetTextures(std::span<const std::string> ktx2Paths) { (void)ktx2Paths; }

    /// Menulis blok uniform material. Murah — dipanggil setiap kali slider
    /// digeser, tanpa membangun ulang apa pun.
    virtual void SetParameters(std::span<const uint8_t> block) = 0;

    virtual void Render(const MaterialPreviewDesc& desc) = 0;

    virtual TextureHandle ColorTarget() const = 0;
    virtual Vec2 ColorTargetUvMax() const = 0;

    /// Kegagalan terakhir, untuk ditampilkan panel. Kosong berarti tidak ada.
    virtual const char* LastError() const = 0;

    virtual const char* Name() const = 0;
};

}  // namespace sim::render
