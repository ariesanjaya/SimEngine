#pragma once

#include "Sim/RHI/Device.h"
#include "Sim/Render/Types.h"
#include "VolumeResource.h"

#include <filesystem>

namespace sim::render {

/// Pass raymarch untuk volume `.vdb`.
///
/// **Acuan kebenarannya di CPU**, di `Code/Volume/src/VolumeRaymarch.cpp`, dan
/// shader-nya menjalankan rumus yang sama persis. Yang paling mudah hilang dari
/// keduanya adalah sifat "hasilnya tidak bergantung pada besar langkah", dan
/// itulah yang paling mahal ketika hilang: menaikkan kualitas lalu mengubah
/// kecerahan adegan, sehingga tiap preset harus dikalibrasi sendiri-sendiri.
///
/// **Tidak menautkan OpenVDB.** Yang sampai ke sini adalah `sim::VolumeGrid` —
/// float biasa di `Sim::Core` — dan yang membacanya dari berkas adalah
/// pemanggil, lewat `Sim::Volume`. Aturan yang sama yang menjaga OpenImageIO di
/// luar jalur runtime.
class VolumePass {
public:
    bool Create(rhi::Device& device, const std::filesystem::path& shaderDirectory,
                VkFormat sceneFormat);
    void Destroy();

    bool IsValid() const { return pipeline_ != VK_NULL_HANDLE; }
    /// Ada volume yang sudah terunggah dan siap digambar.
    bool HasVolume() const { return volume_.IsValid(); }

    /// Mengunggah sebuah grid, menggantikan yang sebelumnya.
    ///
    /// Dipanggil hanya ketika `revision` berubah — unggahan volume berharga
    /// puluhan megabyte dan tidak boleh terjadi tiap frame. Pemanggil yang
    /// menaikkan revision-nya adalah yang tahu isinya berubah.
    bool SetVolume(const VolumeGrid& grid, VolumeTextureFormat format);
    void ClearVolume();

    /// Kotak volume di ruang dunia, dari posisi dan skala yang diminta.
    void WorldBounds(const Vec3& position, float scale, Vec3& outMin, Vec3& outMax) const;

    void RecordDraw(VkCommandBuffer command, const Mat4& invViewProj, const Vec3& cameraPosition,
                    const ViewportVolume& settings);

private:
    bool CreateDescriptors();
    void WriteDescriptor();

    rhi::Device* device_ = nullptr;
    VolumeResource volume_;

    VkShaderModule vertex_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace sim::render
