#pragma once

#include "Sim/RHI/Buffer.h"
#include "Sim/RHI/Pipeline.h"
#include "Sim/RHI/Texture3D.h"
#include "Sim/Render/SdfVolume.h"
#include "Sim/Render/Types.h"

#include <array>
#include <filesystem>
#include <span>
#include <vector>

namespace sim::render {

/// Kaskade SDF di GPU, beserta pembaruannya dari sisi CPU.
///
/// **Geometrinya masih kotak, dan itu batas yang disengaja sejak M1.** Rencana
/// GI menyebut bake per-mesh menjadi brick sparse; selama geometrinya kotak,
/// medan jaraknya punya bentuk analitik yang tepat — jadi yang diuji di sini
/// benar-benar clipmap dan sphere tracing-nya, bukan ketelitian sebuah baker
/// yang belum ada.
///
/// **Sejak G4 kompositnya punya dua jalur.** Yang di GPU mengevaluasi medan
/// jaraknya satu voxel per thread dan menulis langsung ke tekstur kaskade —
/// tanpa larik byte di CPU, tanpa staging buffer, tanpa pengemasan per wilayah.
/// Yang di CPU tetap ada sebagai jalur mundur, dan sebagai pembanding: satu-
/// satunya cara memeriksa jalur GPU adalah membandingkan isi voxelnya byte demi
/// byte dengan hasil jalur CPU, karena satu-satunya pembaca kaskade ini adalah
/// penelusuran GI — dan GI tidak deterministik.
///
/// Jalur GPU menuntut `VK_FORMAT_R8_UNORM` bisa dipakai sebagai storage image.
/// Itu bukan jaminan spesifikasi, jadi ia ditanyakan saat pembuatan dan jalur
/// CPU yang dipakai bila jawabannya tidak.
class SdfClipmapResource {
public:
    SdfClipmapResource() = default;
    ~SdfClipmapResource() { Destroy(); }

    SdfClipmapResource(const SdfClipmapResource&) = delete;
    SdfClipmapResource& operator=(const SdfClipmapResource&) = delete;

    bool Create(rhi::Device& device, const SdfClipmapSettings& settings);
    void Destroy();

    bool IsValid() const { return textures_[0].IsValid(); }
    const SdfVolume& Volume() const { return volume_; }

    /// Menggeser kaskade mengikuti kamera lalu mengemas wilayah yang basi ke
    /// `staging`.
    ///
    /// **Tidak menyentuh queue.** Salinannya direkam `RecordUploads` ke command
    /// buffer frame. Bentuk sebelumnya memanggil `UploadRegion` per wilayah, dan
    /// tiap panggilan itu menyubmit sendiri lalu menunggu queue idle — belasan
    /// kali per frame. Yang terukur di panel Statistics: 3,98 ms untuk 16 ribu
    /// voxel, hampir seluruhnya menunggu, bukan menghitung.
    ///
    /// `staging` harus milik slot frame yang sedang direkam: isinya masih dibaca
    /// GPU sampai submit slot itu selesai. Mengembalikan banyaknya voxel yang
    /// benar-benar ditulis — angka yang diawasi terhadap anggaran 0,4 ms.
    /// `grids[i]` adalah medan jarak hasil bake untuk `meshes[i]`, atau nullptr
    /// bila mesh itu belum dibake — instance itu lalu memakai kotak batasnya,
    /// seperti sebelum M1. Span yang lebih pendek daripada `meshes` berlaku
    /// sebagai nullptr untuk sisanya.
    uint64_t Update(const Vec3& cameraPosition, std::span<const MeshInstance> meshes,
                    std::span<const SdfGrid* const> grids, rhi::DynamicBuffer& staging,
                    uint32_t slot);

    /// Berapa instance frame ini yang benar-benar memakai SDF hasil bake.
    /// Dilaporkan log dan panel: "1 dari 40 mesh ter-bake" adalah keadaan yang
    /// layak diketahui.
    std::size_t BakedCount() const { return field_.BakedCount(); }

    /// Merekam salinan atau dispatch yang disiapkan `Update` ke command buffer
    /// frame. Jalur mana yang direkam ditentukan `SetGpuFill`.
    void RecordUploads(VkCommandBuffer cmd);

    /// Menyiapkan jalur compute. Mengembalikan false — dan meninggalkan jalur
    /// CPU aktif — bila shadernya tidak ada atau format kaskadenya tidak bisa
    /// dipakai sebagai storage image di perangkat ini.
    bool CreateGpuFill(const std::filesystem::path& shaderDirectory);

    /// Menyalakan jalur compute. Diabaikan bila `CreateGpuFill` gagal.
    void SetGpuFill(bool enabled) { gpuFill_ = enabled && fill_.IsValid(); }
    bool GpuFillActive() const { return gpuFill_; }
    bool GpuFillAvailable() const { return fill_.IsValid(); }

    /// Banyaknya slot frame yang buffer entrinya disediakan. Sama dengan
    /// `VulkanRenderer`; dikunci `static_assert` di sana.
    static constexpr uint32_t kSlots = 3;
    /// Sisi grup kerja, sama dengan `numthreads` di Shaders/sdf_fill.comp.slang.
    static constexpr uint32_t kGroupSize = 64;

    /// Berapa medan jarak hasil bake yang bisa dibaca jalur compute sekaligus.
    ///
    /// **Larik tekstur berukuran tetap, bukan bindless.** Jumlahnya sama dengan
    /// `kMaxGrids` di sdf_fill.comp.slang, dan yang melewatinya kembali memakai
    /// kotak batasnya — dilaporkan, bukan didiamkan. Enam belas mesh berbeda
    /// yang masing-masing punya medan jarak sendiri sudah jauh di atas apa pun
    /// yang pernah diukur adegan uji ini; yang mengubahnya nanti adalah alasan
    /// yang terukur, bukan angka yang dinaikkan berjaga-jaga.
    static constexpr uint32_t kMaxGrids = 16;

    const rhi::Texture3D& Texture(uint32_t cascade) const { return textures_[cascade]; }

    /// Ada pekerjaan yang menunggu `RecordUploads`.
    ///
    /// Dipakai frame graph memutuskan apakah pass `sdf-fill` perlu dideklarasikan
    /// sama sekali frame ini: pass yang dideklarasikan tanpa isi tetap membawa
    /// perpindahan layout keempat kaskadenya, dan itu biaya yang dibayar untuk
    /// tidak menulis apa pun.
    bool HasPendingUploads() const { return !pendingFills_.empty() || !pending_.empty(); }

    /// Kaskade mana saja yang akan disentuh `RecordUploads`.
    bool Touches(uint32_t cascade) const;
    uint32_t CascadeCount() const { return volume_.Clipmap().CascadeCount(); }

    /// Byte staging terbesar yang mungkin dibutuhkan satu pembaruan: seluruh
    /// isi setiap kaskade, yaitu yang terjadi saat kamera melompat lebih jauh
    /// daripada lebar clipmap-nya sendiri.
    VkDeviceSize StagingBytes() const;

private:
    /// Satu salinan yang menunggu direkam.
    struct PendingCopy {
        uint32_t cascade = 0;
        glm::uvec3 offset{0};
        glm::uvec3 extent{0};
        VkDeviceSize sourceOffset = 0;
    };

    /// Satu dispatch yang menunggu direkam: kotak texel beserta voxel dunia
    /// yang bersesuaian dengannya.
    struct PendingFill {
        uint32_t cascade = 0;
        glm::uvec3 texelMin{0};
        glm::ivec3 worldMin{0};
        glm::uvec3 extent{0};
    };

    bool WriteFillDescriptors();
    /// Memastikan sebuah grid punya tekstur di GPU, dan menjawab slotnya.
    /// Negatif berarti tidak ada tempat lagi, atau unggahannya gagal.
    int EnsureGridUploaded(const SdfGrid* grid);
    /// Menunjuk ulang larik tekstur grid sesudah ada yang bertambah.
    void WriteGridDescriptor();
    /// Menunjuk ulang set entri **satu slot** ke buffer barunya.
    ///
    /// Terpisah dari `WriteFillDescriptors` karena keduanya dipanggil pada saat
    /// yang sangat berbeda: yang itu saat start, ketika belum ada satu pun
    /// command buffer yang berjalan; yang ini di tengah frame, ketika slot lain
    /// bisa saja masih dibaca GPU.
    void WriteEntryDescriptor(uint32_t slot);
    void RecordFills(VkCommandBuffer cmd);

    rhi::Device* device_ = nullptr;
    SdfVolume volume_;
    std::array<rhi::Texture3D, kMaxSdfCascades> textures_;
    std::vector<SdfClipmap::TexelBox> boxes_;
    std::vector<uint8_t> scratch_;
    std::vector<PendingCopy> pending_;
    VkBuffer pendingSource_ = VK_NULL_HANDLE;
    // Dipakai ulang tiap frame supaya medan jaraknya tidak mengalokasi vektor
    // baru setiap kali kamera bergerak satu voxel.
    //
    // **`BakedSceneField`, bukan `BoxSceneField` — dan ia memuat yang kedua.**
    // Adegan nyata berisi mesh yang sudah dibake dan mesh yang belum; yang punya
    // grid memakai grid, sisanya tetap memakai kotak lewat medan yang sama
    // persis seperti sebelumnya.
    BakedSceneField field_;

    // --- jalur compute (G4) ---
    rhi::ComputePipeline fill_;
    VkDescriptorSetLayout cascadeLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout entryLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool fillPool_ = VK_NULL_HANDLE;
    /// Satu set per kaskade: storage image-nya.
    std::array<VkDescriptorSet, kMaxSdfCascades> cascadeSets_{};
    /// Satu set dan satu buffer per slot frame: entri medan jaraknya, yang
    /// berganti tiap frame.
    std::array<VkDescriptorSet, kSlots> entrySets_{};
    std::array<rhi::DynamicBuffer, kSlots> entryBuffers_;
    std::vector<BoxSceneField::GpuEntry> gpuEntries_;
    std::vector<BakedSceneField::GpuGrid> gpuGrids_;
    /// Satu tekstur per grid yang dipakai adegan, beserta grid yang ditempatinya.
    /// Pointer dipakai sebagai identitas: bakery memiliki gridnya untuk seluruh
    /// sesi, jadi alamatnya stabil, dan dua instance mesh yang sama menunjuk
    /// grid yang sama persis — satu tekstur, bukan dua.
    std::array<rhi::Texture3D, kMaxGrids> gridTextures_;
    std::array<const SdfGrid*, kMaxGrids> gridSources_{};
    uint32_t gridCount_ = 0;
    bool reportedGridLimit_ = false;
    VkDescriptorSetLayout gridLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet gridSet_ = VK_NULL_HANDLE;
    std::array<rhi::DynamicBuffer, kSlots> gridBuffers_;
    /// Isi slot tekstur yang belum terpakai. Lihat catatan di `Create`.
    rhi::Texture3D dummyGrid_;
    std::vector<PendingFill> pendingFills_;
    uint32_t fillSlot_ = 0;
    bool storageCapable_ = false;
    uint32_t fillEntryCount_ = 0;
    uint32_t fillGridCount_ = 0;
    bool gpuFill_ = false;
    /// Sekali saja: sebuah pesan per frame tentang keadaan yang tidak berubah
    /// adalah log yang tidak bisa dibaca.
    bool reportedCpuFallback_ = false;
};

}  // namespace sim::render
