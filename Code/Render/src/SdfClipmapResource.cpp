#include "SdfClipmapResource.h"

#include "Sim/Core/Log.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>

namespace sim::render {
namespace {

constexpr VkFormat kSdfFormat = VK_FORMAT_R8_UNORM;

/// Harus sama persis dengan `Push` di Shaders/sdf_fill.comp.slang.
struct FillPush {
    std::array<int32_t, 4> texelMin{};
    std::array<int32_t, 4> worldMin{};
    std::array<int32_t, 4> extent{};
    std::array<float, 4> params{};
};

}  // namespace

bool SdfClipmapResource::Create(rhi::Device& device, const SdfClipmapSettings& settings) {
    Destroy();
    device_ = &device;
    volume_.Configure(settings);

    const uint32_t resolution = volume_.Clipmap().Settings().resolution;
    const glm::uvec3 extent(resolution);
    // **Ditanyakan sekali, di sini.** Usage storage harus diminta saat image
    // dibuat, dan jalur compute baru diputuskan belakangan — jadi pertanyaannya
    // harus dijawab lebih dulu daripada keputusannya. Harganya sebuah bit usage
    // pada tekstur 64³ satu byte per texel; yang dibelinya adalah tidak perlu
    // membuat ulang ketiganya saat sakelar jalur ditekan.
    storageCapable_ = rhi::Texture3D::SupportsStorage(device, kSdfFormat);
    for (uint32_t cascade = 0; cascade < volume_.Clipmap().CascadeCount(); ++cascade) {
        if (!textures_[cascade].Create(device, extent, kSdfFormat, 1, storageCapable_)) {
            Destroy();
            return false;
        }
    }
    scratch_.reserve(static_cast<std::size_t>(resolution) * resolution);
    return true;
}

bool SdfClipmapResource::CreateGpuFill(const std::filesystem::path& shaderDirectory) {
    if (device_ == nullptr || !IsValid()) {
        return false;
    }
    if (!storageCapable_) {
        SIM_WARN("Render",
                 "R8_UNORM is not a storage image format here; the SDF composite stays on the CPU");
        return false;
    }

    const VkDescriptorSetLayoutBinding cascadeBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                                      VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    const VkDescriptorSetLayoutBinding entryBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &cascadeBinding;
    if (vkCreateDescriptorSetLayout(device_->Handle(), &layoutInfo, nullptr, &cascadeLayout_) !=
        VK_SUCCESS) {
        return false;
    }
    layoutInfo.pBindings = &entryBinding;
    if (vkCreateDescriptorSetLayout(device_->Handle(), &layoutInfo, nullptr, &entryLayout_) !=
        VK_SUCCESS) {
        return false;
    }

    // **Dua set, bukan satu.** Yang pertama berganti per kaskade dan yang kedua
    // per slot frame; menyatukannya berarti kaskade × slot kombinasi descriptor
    // yang harus dipelihara, untuk membedakan dua hal yang memang berubah pada
    // irama yang berbeda.
    const std::array<VkDescriptorPoolSize, 2> sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxSdfCascades},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSlots}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kMaxSdfCascades + kSlots;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(device_->Handle(), &poolInfo, nullptr, &fillPool_) != VK_SUCCESS) {
        return false;
    }

    std::array<VkDescriptorSetLayout, kMaxSdfCascades> cascadeLayouts{};
    cascadeLayouts.fill(cascadeLayout_);
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = fillPool_;
    allocate.descriptorSetCount = kMaxSdfCascades;
    allocate.pSetLayouts = cascadeLayouts.data();
    if (vkAllocateDescriptorSets(device_->Handle(), &allocate, cascadeSets_.data()) !=
        VK_SUCCESS) {
        return false;
    }
    std::array<VkDescriptorSetLayout, kSlots> entryLayouts{};
    entryLayouts.fill(entryLayout_);
    allocate.descriptorSetCount = kSlots;
    allocate.pSetLayouts = entryLayouts.data();
    if (vkAllocateDescriptorSets(device_->Handle(), &allocate, entrySets_.data()) != VK_SUCCESS) {
        return false;
    }

    for (rhi::DynamicBuffer& buffer : entryBuffers_) {
        if (!buffer.Create(*device_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           sizeof(BoxSceneField::GpuEntry) * 64)) {
            return false;
        }
    }

    const std::array<VkDescriptorSetLayout, 2> setLayouts{cascadeLayout_, entryLayout_};
    rhi::ComputePipelineDesc desc;
    desc.shader = shaderDirectory / "sdf_fill.comp.spv";
    desc.setLayouts = setLayouts;
    desc.pushConstantBytes = sizeof(FillPush);
    if (!fill_.Create(*device_, desc)) {
        return false;
    }
    return WriteFillDescriptors();
}

void SdfClipmapResource::WriteEntryDescriptor(uint32_t slot) {
    if (slot >= kSlots) {
        return;
    }
    const VkDescriptorBufferInfo buffer{entryBuffers_[slot].Handle(), 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = entrySets_[slot];
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer;
    vkUpdateDescriptorSets(device_->Handle(), 1, &write, 0, nullptr);
}

bool SdfClipmapResource::WriteFillDescriptors() {
    std::vector<VkDescriptorImageInfo> images(kMaxSdfCascades);
    std::vector<VkDescriptorBufferInfo> buffers(kSlots);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(kMaxSdfCascades + kSlots);

    for (uint32_t cascade = 0; cascade < kMaxSdfCascades; ++cascade) {
        // Kaskade yang tidak dipakai adegan ini tetap harus punya descriptor
        // yang sah: yang dibiarkan kosong adalah pelanggaran pada setiap
        // dispatch, bahkan yang tidak menyentuhnya.
        const rhi::Texture3D& texture =
            textures_[cascade].IsValid() ? textures_[cascade] : textures_[0];
        images[cascade] = {VK_NULL_HANDLE, texture.View(), VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = cascadeSets_[cascade];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &images[cascade];
        writes.push_back(write);
    }
    for (uint32_t slot = 0; slot < kSlots; ++slot) {
        buffers[slot] = {entryBuffers_[slot].Handle(), 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = entrySets_[slot];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &buffers[slot];
        writes.push_back(write);
    }
    vkUpdateDescriptorSets(device_->Handle(), static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
    return true;
}

void SdfClipmapResource::Destroy() {
    for (rhi::Texture3D& texture : textures_) {
        texture.Destroy();
    }
    fill_.Destroy();
    for (rhi::DynamicBuffer& buffer : entryBuffers_) {
        buffer.Destroy();
    }
    if (device_ != nullptr) {
        if (fillPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_->Handle(), fillPool_, nullptr);
            fillPool_ = VK_NULL_HANDLE;
        }
        if (entryLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_->Handle(), entryLayout_, nullptr);
            entryLayout_ = VK_NULL_HANDLE;
        }
        if (cascadeLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_->Handle(), cascadeLayout_, nullptr);
            cascadeLayout_ = VK_NULL_HANDLE;
        }
    }
    cascadeSets_.fill(VK_NULL_HANDLE);
    entrySets_.fill(VK_NULL_HANDLE);
    gpuFill_ = false;
    storageCapable_ = false;
    device_ = nullptr;
}

VkDeviceSize SdfClipmapResource::StagingBytes() const {
    const uint64_t resolution = volume_.Clipmap().Settings().resolution;
    return static_cast<VkDeviceSize>(resolution * resolution * resolution *
                                     volume_.Clipmap().CascadeCount());
}

uint64_t SdfClipmapResource::Update(const Vec3& cameraPosition,
                                    std::span<const MeshInstance> meshes,
                                    rhi::DynamicBuffer& staging, uint32_t slot) {
    pending_.clear();
    pendingFills_.clear();
    pendingSource_ = VK_NULL_HANDLE;
    if (!IsValid()) {
        return 0;
    }

    const SdfScrollResult scroll = volume_.Clipmap().Scroll(cameraPosition);
    if (scroll.regions.empty()) {
        return 0;
    }
    // Medan jaraknya dibangun setelah `Scroll`, bukan sebelum: frame yang tidak
    // menggeser satu kaskade pun — yaitu kebanyakan frame saat kamera diam —
    // tidak perlu membalik satu matriks pun.
    field_.Build(meshes);

    if (gpuFill_ && slot < kSlots) {
        // **Yang disiapkan di sini hanya daftar kotaknya.** Evaluasi medan
        // jaraknya sendiri — bagian yang berharga 3,9 ms di CPU — terjadi di
        // `RecordUploads`, sebagai dispatch. Larik byte `SdfVolume` sengaja
        // tidak ikut diisi: ia salinan kedua dari data yang sama, dan mengisi
        // salinan yang tidak dibaca siapa pun adalah persis pekerjaan yang
        // sedang dipindahkan.
        fillSlot_ = slot;
        field_.WriteGpuEntries(gpuEntries_);
        fillEntryCount_ = static_cast<uint32_t>(gpuEntries_.size());
        if (gpuEntries_.empty()) {
            gpuEntries_.push_back(BoxSceneField::GpuEntry{});
        }
        const VkDeviceSize entryBytes = sizeof(BoxSceneField::GpuEntry) * gpuEntries_.size();
        const VkBuffer before = entryBuffers_[slot].Handle();
        if (!entryBuffers_[slot].Reserve(entryBytes)) {
            return 0;
        }
        entryBuffers_[slot].Write(gpuEntries_.data(), entryBytes);
        if (entryBuffers_[slot].Handle() != before) {
            // **Hanya set slot ini, bukan seluruhnya.** `WriteFillDescriptors`
            // menulis ulang set tiap kaskade **dan** set entri tiap slot; yang
            // berpindah di sini hanya buffer satu slot. Slot lain bisa saja
            // masih dibaca command buffer yang belum selesai, dan menulisi
            // descriptor yang sedang dipakai adalah pelanggaran — dilaporkan
            // validation layer sebagai "in use by VkCommandBuffer", dan terlihat
            // sebagai kerusakan pada frame yang salah.
            //
            // Tidak pernah muncul selama adegan ujinya kecil: buffer entri
            // dibuat cukup untuk 64 kotak, dan adegan yang tidak pernah
            // melewatinya tidak pernah menumbuhkannya. Yang menemukannya adegan
            // padat G6.
            WriteEntryDescriptor(slot);
        }

        uint64_t written = 0;
        for (const SdfScrollRegion& region : scroll.regions) {
            volume_.Clipmap().SplitWrapped(region, boxes_);
            for (const SdfClipmap::TexelBox& box : boxes_) {
                const glm::uvec3 size = box.max - box.min;
                if (size.x == 0 || size.y == 0 || size.z == 0) {
                    continue;
                }
                pendingFills_.push_back({region.cascade, box.min, box.worldMin, size});
                written += static_cast<uint64_t>(size.x) * size.y * size.z;
            }
        }
        return written;
    }

    volume_.ResetWriteCount();
    volume_.Fill(scroll, field_);

    if (!staging.Reserve(StagingBytes())) {
        return volume_.WrittenVoxels();
    }
    pendingSource_ = staging.Handle();

    // Voxel yang baru ditulis dikemas ke satu buffer staging, satu wilayah demi
    // satu wilayah. Pembagian toroidalnya sudah dilakukan `SplitWrapped`, jadi
    // setiap kotak di sini dijamin tidak melewati tepi tekstur — dan
    // `RecordRegionCopy` menolak yang melewatinya alih-alih menjepit, supaya
    // kelalaian di sini tidak tersembunyi di balik gambar yang hampir benar.
    const uint32_t resolution = volume_.Clipmap().Settings().resolution;
    VkDeviceSize at = 0;
    for (const SdfScrollRegion& region : scroll.regions) {
        volume_.Clipmap().SplitWrapped(region, boxes_);
        for (const SdfClipmap::TexelBox& box : boxes_) {
            const glm::uvec3 size = box.max - box.min;
            const std::size_t bytes = static_cast<std::size_t>(size.x) * size.y * size.z;
            scratch_.resize(bytes);
            // Baris demi baris, bukan voxel demi voxel: sebuah kotak texel rapat
            // pada sumbu X, jadi tiap barisnya satu salinan.
            const std::span<const uint8_t> source = volume_.Data(region.cascade);
            std::size_t cursor = 0;
            for (uint32_t z = box.min.z; z < box.max.z; ++z) {
                for (uint32_t y = box.min.y; y < box.max.y; ++y) {
                    const std::size_t rowStart =
                        (static_cast<std::size_t>(z) * resolution + y) * resolution + box.min.x;
                    std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(rowStart), size.x,
                                scratch_.begin() + static_cast<std::ptrdiff_t>(cursor));
                    cursor += size.x;
                }
            }
            if (!staging.WriteAt(at, scratch_.data(), bytes)) {
                SIM_ERROR("Render", "SDF staging buffer is too small for {} bytes at {}", bytes,
                          at);
                break;
            }
            pending_.push_back({region.cascade, box.min, size, at});
            at += bytes;
        }
    }
    return volume_.WrittenVoxels();
}

void SdfClipmapResource::RecordUploads(VkCommandBuffer cmd) {
    if (!pendingFills_.empty()) {
        RecordFills(cmd);
        return;
    }
    if (pending_.empty() || pendingSource_ == VK_NULL_HANDLE) {
        return;
    }
    // Barrier per tekstur, bukan per wilayah. Sebuah kaskade yang menerima tiga
    // lempeng dan delapan potongan toroidal tetap hanya berpindah layout dua
    // kali.
    std::array<bool, kMaxSdfCascades> touched{};
    for (const PendingCopy& copy : pending_) {
        touched[copy.cascade] = true;
    }
    for (uint32_t cascade = 0; cascade < kMaxSdfCascades; ++cascade) {
        if (touched[cascade]) {
            textures_[cascade].RecordTransition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        }
    }
    for (const PendingCopy& copy : pending_) {
        textures_[copy.cascade].RecordRegionCopy(cmd, pendingSource_, copy.sourceOffset,
                                                 copy.offset, copy.extent);
    }
    for (uint32_t cascade = 0; cascade < kMaxSdfCascades; ++cascade) {
        if (touched[cascade]) {
            textures_[cascade].RecordTransition(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    pending_.clear();
    pendingSource_ = VK_NULL_HANDLE;
}

void SdfClipmapResource::RecordFills(VkCommandBuffer cmd) {
    // Barrier per tekstur, bukan per kotak. Sebuah kaskade yang menerima tiga
    // lempeng dan delapan potongan toroidal tetap hanya berpindah layout dua
    // kali — alasan yang sama dengan jalur salinan yang digantikannya.
    std::array<bool, kMaxSdfCascades> touched{};
    for (const PendingFill& fill : pendingFills_) {
        touched[fill.cascade] = true;
    }
    for (uint32_t cascade = 0; cascade < kMaxSdfCascades; ++cascade) {
        if (touched[cascade] && textures_[cascade].IsValid()) {
            textures_[cascade].RecordTransition(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                                VK_IMAGE_LAYOUT_GENERAL);
        }
    }

    // **Tidak ada barrier di antara dispatch.** Kotak-kotak ini saling lepas:
    // `SplitWrapped` menjamin tidak ada dua kotak yang menyentuh texel yang
    // sama, dan tidak satu pun membaca apa yang ditulis yang lain. Memisahkan
    // mereka dengan barrier akan menderetkan belasan dispatch kecil yang justru
    // paling diuntungkan oleh berjalan bersamaan.
    const std::array<VkDescriptorSet, 1> entrySet{entrySets_[fillSlot_]};
    for (const PendingFill& fill : pendingFills_) {
        const std::array<VkDescriptorSet, 2> sets{cascadeSets_[fill.cascade], entrySet[0]};
        fill_.Bind(cmd, sets);

        const float voxel = volume_.Clipmap().VoxelSize(fill.cascade);
        const float band = volume_.Clipmap().BandRadius(fill.cascade);
        FillPush push;
        push.texelMin = {static_cast<int32_t>(fill.texelMin.x),
                         static_cast<int32_t>(fill.texelMin.y),
                         static_cast<int32_t>(fill.texelMin.z),
                         static_cast<int32_t>(fillEntryCount_)};
        push.worldMin = {fill.worldMin.x, fill.worldMin.y, fill.worldMin.z, 0};
        push.extent = {static_cast<int32_t>(fill.extent.x), static_cast<int32_t>(fill.extent.y),
                       static_cast<int32_t>(fill.extent.z), 0};
        push.params = {voxel, band, 255.0f / (band * 2.0f), 0.0f};
        fill_.Push(cmd, &push, sizeof(push));

        const uint32_t voxels = fill.extent.x * fill.extent.y * fill.extent.z;
        vkCmdDispatch(cmd, rhi::GroupCount(voxels, kGroupSize), 1, 1);
    }

    for (uint32_t cascade = 0; cascade < kMaxSdfCascades; ++cascade) {
        if (touched[cascade] && textures_[cascade].IsValid()) {
            textures_[cascade].RecordTransition(cmd, VK_IMAGE_LAYOUT_GENERAL,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    }
    pendingFills_.clear();
}

}  // namespace sim::render
