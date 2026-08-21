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
    // Dua buffer per slot: kotak untuk mesh yang belum dibake, grid untuk yang
    // sudah. Satu set, karena keduanya berganti pada irama yang sama persis.
    const std::array<VkDescriptorSetLayoutBinding, 2> entryBindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    // Tekstur grid: satu binding berisi larik, dan **berganti jauh lebih jarang
    // daripada entrinya** — hanya ketika sebuah mesh selesai dibake. Karena itu
    // ia set tersendiri alih-alih ikut set per slot.
    const VkDescriptorSetLayoutBinding gridBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxGrids,
                                                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &cascadeBinding;
    if (vkCreateDescriptorSetLayout(device_->Handle(), &layoutInfo, nullptr, &cascadeLayout_) !=
        VK_SUCCESS) {
        return false;
    }
    layoutInfo.bindingCount = static_cast<uint32_t>(entryBindings.size());
    layoutInfo.pBindings = entryBindings.data();
    if (vkCreateDescriptorSetLayout(device_->Handle(), &layoutInfo, nullptr, &entryLayout_) !=
        VK_SUCCESS) {
        return false;
    }
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &gridBinding;
    if (vkCreateDescriptorSetLayout(device_->Handle(), &layoutInfo, nullptr, &gridLayout_) !=
        VK_SUCCESS) {
        return false;
    }

    // **Dua set, bukan satu.** Yang pertama berganti per kaskade dan yang kedua
    // per slot frame; menyatukannya berarti kaskade × slot kombinasi descriptor
    // yang harus dipelihara, untuk membedakan dua hal yang memang berubah pada
    // irama yang berbeda.
    const std::array<VkDescriptorPoolSize, 3> sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kMaxSdfCascades},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSlots * 2},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxGrids}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kMaxSdfCascades + kSlots + 1;
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

    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &gridLayout_;
    if (vkAllocateDescriptorSets(device_->Handle(), &allocate, &gridSet_) != VK_SUCCESS) {
        return false;
    }

    for (rhi::DynamicBuffer& buffer : entryBuffers_) {
        if (!buffer.Create(*device_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           sizeof(BoxSceneField::GpuEntry) * 64)) {
            return false;
        }
    }
    for (rhi::DynamicBuffer& buffer : gridBuffers_) {
        if (!buffer.Create(*device_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                           sizeof(BakedSceneField::GpuGrid) * kMaxGrids)) {
            return false;
        }
    }
    // **Tekstur boneka satu texel untuk slot yang belum terisi.** Descriptor
    // yang dibiarkan kosong adalah pelanggaran pada setiap dispatch, termasuk
    // dispatch yang tidak pernah membacanya — dan larik enam belas slot hampir
    // selalu sebagian besar kosong.
    if (!dummyGrid_.Create(*device_, glm::uvec3(1), VK_FORMAT_R32_SFLOAT, sizeof(float))) {
        return false;
    }

    const std::array<VkDescriptorSetLayout, 3> setLayouts{cascadeLayout_, entryLayout_,
                                                          gridLayout_};
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
    const std::array<VkDescriptorBufferInfo, 2> buffers{
        VkDescriptorBufferInfo{entryBuffers_[slot].Handle(), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{gridBuffers_[slot].Handle(), 0, VK_WHOLE_SIZE}};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (uint32_t i = 0; i < writes.size(); ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = entrySets_[slot];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &buffers[i];
    }
    vkUpdateDescriptorSets(device_->Handle(), static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
}

bool SdfClipmapResource::WriteFillDescriptors() {
    std::vector<VkDescriptorImageInfo> images(kMaxSdfCascades);
    std::vector<VkDescriptorBufferInfo> buffers(kSlots * 2);
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(kMaxSdfCascades + kSlots * 2);

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
        buffers[slot * 2] = {entryBuffers_[slot].Handle(), 0, VK_WHOLE_SIZE};
        buffers[slot * 2 + 1] = {gridBuffers_[slot].Handle(), 0, VK_WHOLE_SIZE};
        for (uint32_t binding = 0; binding < 2; ++binding) {
            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = entrySets_[slot];
            write.dstBinding = binding;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &buffers[slot * 2 + binding];
            writes.push_back(write);
        }
    }
    vkUpdateDescriptorSets(device_->Handle(), static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
    WriteGridDescriptor();
    return true;
}

void SdfClipmapResource::WriteGridDescriptor() {
    std::array<VkDescriptorImageInfo, kMaxGrids> images{};
    for (uint32_t slot = 0; slot < kMaxGrids; ++slot) {
        const rhi::Texture3D& texture =
            gridTextures_[slot].IsValid() ? gridTextures_[slot] : dummyGrid_;
        images[slot] = {VK_NULL_HANDLE, texture.View(),
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = gridSet_;
    write.dstBinding = 0;
    write.descriptorCount = kMaxGrids;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = images.data();
    vkUpdateDescriptorSets(device_->Handle(), 1, &write, 0, nullptr);
}

int SdfClipmapResource::EnsureGridUploaded(const SdfGrid* grid) {
    if (grid == nullptr || grid->Empty()) {
        return -1;
    }
    for (uint32_t slot = 0; slot < gridCount_; ++slot) {
        if (gridSources_[slot] == grid) {
            return static_cast<int>(slot);
        }
    }
    if (gridCount_ >= kMaxGrids) {
        if (!reportedGridLimit_) {
            reportedGridLimit_ = true;
            SIM_WARN("Render",
                     "the SDF clipmap holds at most {} baked fields; the rest keep using their "
                     "bounding boxes",
                     kMaxGrids);
        }
        return -1;
    }

    const uint32_t slot = gridCount_;
    const glm::uvec3 extent(grid->sizeX, grid->sizeY, grid->sizeZ);
    // **R32_SFLOAT, dan itu bukan kemewahan.** Jalur CPU membaca float32 yang
    // sama persis dari `SdfGrid`, dan kriteria selesai G4 menuntut kedua jalur
    // menghasilkan byte yang sama. Format setengah presisi akan membuat sebagian
    // voxel berbeda satu satuan pada penyandian 8-bitnya — perbedaan yang tidak
    // salah, tapi yang membuat satu-satunya alat pembanding yang ada berhenti
    // menjawab ya atau tidak.
    if (!gridTextures_[slot].Create(*device_, extent, VK_FORMAT_R32_SFLOAT, sizeof(float))) {
        SIM_WARN("Render", "cannot create a {}x{}x{} texture for a baked distance field",
                 extent.x, extent.y, extent.z);
        gridTextures_[slot].Destroy();
        return -1;
    }
    const std::span<const std::byte> bytes{
        reinterpret_cast<const std::byte*>(grid->distances.data()),
        grid->distances.size() * sizeof(float)};
    if (!gridTextures_[slot].UploadRegion(glm::uvec3(0), extent, bytes)) {
        SIM_WARN("Render", "cannot upload a baked distance field");
        gridTextures_[slot].Destroy();
        return -1;
    }
    // Warnanya, kalau gridnya membawanya. **Palet diselesaikan di sini, sekali,
    // bukan di shader.** Yang tersimpan di grid indeks material; yang dibaca
    // penelusur warna. Menyerahkan indeksnya berarti setiap sinar probe
    // membawa satu pengalihan tambahan dan satu binding tambahan, untuk
    // menghemat memori yang toh sudah dibayar empat kali lipat oleh medan
    // jaraknya sendiri.
    std::size_t albedoKb = 0;
    if (grid->HasAlbedo()) {
        std::vector<uint32_t> colors(grid->materials.size());
        for (std::size_t i = 0; i < colors.size(); ++i) {
            const uint8_t index = grid->materials[i];
            if (index == 0 || index >= grid->palette.size()) {
                colors[i] = 0;  // alfa nol: voxel ini tidak punya material
                continue;
            }
            const Vec3 albedo = glm::clamp(grid->palette[index], Vec3(0.0f), Vec3(1.0f));
            const auto r = static_cast<uint32_t>(albedo.x * 255.0f + 0.5f);
            const auto g = static_cast<uint32_t>(albedo.y * 255.0f + 0.5f);
            const auto b = static_cast<uint32_t>(albedo.z * 255.0f + 0.5f);
            colors[i] = r | (g << 8) | (b << 16) | (0xFFu << 24);
        }
        const std::span<const std::byte> colorBytes{
            reinterpret_cast<const std::byte*>(colors.data()), colors.size() * sizeof(uint32_t)};
        if (gridAlbedo_[slot].Create(*device_, extent, VK_FORMAT_R8G8B8A8_UNORM,
                                     sizeof(uint32_t)) &&
            gridAlbedo_[slot].UploadRegion(glm::uvec3(0), extent, colorBytes)) {
            albedoKb = colors.size() * sizeof(uint32_t) / 1024;
        } else {
            SIM_WARN("Render", "cannot upload the albedo of a baked distance field");
            gridAlbedo_[slot].Destroy();
        }
    }

    gridSources_[slot] = grid;
    gridCount_ = slot + 1;
    ++gridGeneration_;
    // **Menulis ulang descriptor di sini aman.** `UploadRegion` menyubmit
    // sendiri lalu menunggu queue idle, jadi tidak ada satu pun command buffer
    // yang masih memegang set ini saat baris berikut berjalan.
    WriteGridDescriptor();
    SIM_INFO("Render", "baked distance field {} uploaded ({}x{}x{}, {} KB{})", slot, extent.x,
             extent.y, extent.z, grid->distances.size() * sizeof(float) / 1024,
             albedoKb > 0 ? " + " + std::to_string(albedoKb) + " KB albedo" : "");
    return static_cast<int>(slot);
}

VkImageView SdfClipmapResource::GridAlbedoView(uint32_t slot) const {
    if (slot >= kMaxGrids || !gridAlbedo_[slot].IsValid()) {
        return VK_NULL_HANDLE;
    }
    return gridAlbedo_[slot].View();
}

VkBuffer SdfClipmapResource::GridBuffer(uint32_t slot) const {
    if (slot >= kSlots) {
        return VK_NULL_HANDLE;
    }
    return gridBuffers_[slot].Handle();
}

void SdfClipmapResource::Destroy() {
    for (rhi::Texture3D& texture : textures_) {
        texture.Destroy();
    }
    for (rhi::Texture3D& texture : gridTextures_) {
        texture.Destroy();
    }
    for (rhi::Texture3D& texture : gridAlbedo_) {
        texture.Destroy();
    }
    dummyGrid_.Destroy();
    gridSources_.fill(nullptr);
    gridCount_ = 0;
    gridGeneration_ = 0;
    reportedGridLimit_ = false;
    fill_.Destroy();
    for (rhi::DynamicBuffer& buffer : entryBuffers_) {
        buffer.Destroy();
    }
    for (rhi::DynamicBuffer& buffer : gridBuffers_) {
        buffer.Destroy();
    }
    if (device_ != nullptr) {
        if (fillPool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_->Handle(), fillPool_, nullptr);
            fillPool_ = VK_NULL_HANDLE;
        }
        if (gridLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_->Handle(), gridLayout_, nullptr);
            gridLayout_ = VK_NULL_HANDLE;
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
    gridSet_ = VK_NULL_HANDLE;
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
                                    std::span<const SdfGrid* const> grids,
                                    rhi::DynamicBuffer& staging, uint32_t slot) {
    pending_.clear();
    pendingFills_.clear();
    pendingSource_ = VK_NULL_HANDLE;
    if (!IsValid()) {
        return 0;
    }

    // **Medan dan entrinya disusun tiap frame, bukan hanya saat kaskade
    // bergeser.** Dulu keduanya menunggu `Scroll` supaya frame yang kameranya
    // diam tidak membalik satu matriks pun. Sekarang entri grid punya pembaca
    // kedua yang bekerja setiap frame — penelusur probe, yang membaca albedo
    // dari grid per-mesh — dan buffernya berputar tiga slot. Menulisi satu slot
    // saja berarti dua dari tiga frame melihat adegan tanpa satu pun grid, dan
    // yang terlihat warna pantulan yang berkedip pada sepertiga frame.
    field_.Build(meshes, grids);

    // Medan jarak hasil bake diunggah sekali, saat pertama kali terlihat, lalu
    // dikenali lewat alamat gridnya.
    bool gridsComplete = true;
    for (const SdfGrid* grid : grids) {
        if (grid != nullptr && !grid->Empty() && EnsureGridUploaded(grid) < 0) {
            gridsComplete = false;
        }
    }

    if (slot < kSlots) {
        field_.WriteGpuGrids(gpuGrids_, [this](const SdfGrid* grid) {
            for (uint32_t at = 0; at < gridCount_; ++at) {
                if (gridSources_[at] == grid) {
                    return static_cast<int>(at);
                }
            }
            return -1;
        });
        fillGridCount_ = static_cast<uint32_t>(gpuGrids_.size());
        std::vector<BakedSceneField::GpuGrid> padded = gpuGrids_;
        if (padded.empty()) {
            padded.push_back(BakedSceneField::GpuGrid{});
        }
        const VkDeviceSize gridBytes = sizeof(BakedSceneField::GpuGrid) * padded.size();
        const uint64_t gridBefore = gridBuffers_[slot].Generation();
        if (gridBuffers_[slot].Reserve(gridBytes)) {
            gridBuffers_[slot].Write(padded.data(), gridBytes);
            if (gridBuffers_[slot].Generation() != gridBefore) {
                WriteEntryDescriptor(slot);
                ++gridGeneration_;
            }
        }
    }

    const SdfScrollResult scroll = volume_.Clipmap().Scroll(cameraPosition);
    if (scroll.regions.empty()) {
        return 0;
    }

    // Jalur compute membaca larik kotak dan larik grid bersama-sama; medan yang
    // kehilangan sebuah gedung jauh lebih buruk daripada gedung yang berbentuk
    // kotak, jadi slot grid yang tidak kebagian tempat memaksa jalur CPU.
    const bool gpuUsable = gpuFill_ && slot < kSlots && gridsComplete;
    if (gpuFill_ && !gpuUsable && !reportedCpuFallback_) {
        reportedCpuFallback_ = true;
        SIM_INFO("Render",
                 "SDF clipmap falls back to the CPU fill: not every baked field could be "
                 "uploaded, and a field that is missing a building is worse than one that "
                 "holds it as a box");
    }
    if (gpuUsable) {
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
        // **Generasi, bukan handle** — lihat catatan di
        // `DynamicBuffer::Generation`.
        const uint64_t before = entryBuffers_[slot].Generation();
        if (!entryBuffers_[slot].Reserve(entryBytes)) {
            return 0;
        }
        entryBuffers_[slot].Write(gpuEntries_.data(), entryBytes);
        if (entryBuffers_[slot].Generation() != before) {
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

bool SdfClipmapResource::Touches(uint32_t cascade) const {
    for (const PendingFill& fill : pendingFills_) {
        if (fill.cascade == cascade) {
            return true;
        }
    }
    for (const PendingCopy& copy : pending_) {
        if (copy.cascade == cascade) {
            return true;
        }
    }
    return false;
}

void SdfClipmapResource::RecordUploads(VkCommandBuffer cmd) {
    if (!pendingFills_.empty()) {
        RecordFills(cmd);
        return;
    }
    if (pending_.empty() || pendingSource_ == VK_NULL_HANDLE) {
        return;
    }
    // **Perpindahan layoutnya diurus frame graph sejak G7.** Ia harus: pass ini
    // bisa berjalan di antrean compute, dan perpindahan kepemilikan keluarga
    // antrean berbentuk sepasang yang hanya bisa disusun oleh yang melihat kedua
    // sisinya. Yang tersisa di sini hanya salinannya.
    for (const PendingCopy& copy : pending_) {
        textures_[copy.cascade].RecordRegionCopy(cmd, pendingSource_, copy.sourceOffset,
                                                 copy.offset, copy.extent);
    }
    pending_.clear();
    pendingSource_ = VK_NULL_HANDLE;
}

void SdfClipmapResource::RecordFills(VkCommandBuffer cmd) {
    // Perpindahan layoutnya diurus frame graph sejak G7; lihat catatan di
    // `RecordUploads`.
    //
    // **Tidak ada barrier di antara dispatch.** Kotak-kotak ini saling lepas:
    // `SplitWrapped` menjamin tidak ada dua kotak yang menyentuh texel yang
    // sama, dan tidak satu pun membaca apa yang ditulis yang lain. Memisahkan
    // mereka dengan barrier akan menderetkan belasan dispatch kecil yang justru
    // paling diuntungkan oleh berjalan bersamaan.
    for (const PendingFill& fill : pendingFills_) {
        const std::array<VkDescriptorSet, 3> sets{cascadeSets_[fill.cascade],
                                                  entrySets_[fillSlot_], gridSet_};
        fill_.Bind(cmd, sets);

        const float voxel = volume_.Clipmap().VoxelSize(fill.cascade);
        const float band = volume_.Clipmap().BandRadius(fill.cascade);
        FillPush push;
        push.texelMin = {static_cast<int32_t>(fill.texelMin.x),
                         static_cast<int32_t>(fill.texelMin.y),
                         static_cast<int32_t>(fill.texelMin.z),
                         static_cast<int32_t>(fillEntryCount_)};
        push.worldMin = {fill.worldMin.x, fill.worldMin.y, fill.worldMin.z,
                         static_cast<int32_t>(fillGridCount_)};
        push.extent = {static_cast<int32_t>(fill.extent.x), static_cast<int32_t>(fill.extent.y),
                       static_cast<int32_t>(fill.extent.z), 0};
        push.params = {voxel, band, 255.0f / (band * 2.0f), 0.0f};
        fill_.Push(cmd, &push, sizeof(push));

        const uint32_t voxels = fill.extent.x * fill.extent.y * fill.extent.z;
        vkCmdDispatch(cmd, rhi::GroupCount(voxels, kGroupSize), 1, 1);
    }

    pendingFills_.clear();
}

}  // namespace sim::render
