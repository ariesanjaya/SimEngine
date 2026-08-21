#include "ClusterAssign.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace sim::render {
namespace {

/// Harus sama persis dengan `Params` di Shaders/cluster_assign.comp.slang.
struct GpuParams {
    std::array<uint32_t, 4> counts{};
    std::array<float, 4> grid{};
    std::array<uint32_t, 4> limits{};
};

}  // namespace

bool ClusterAssign::Create(rhi::Device& device, const std::filesystem::path& shaderDirectory) {
    Destroy();
    device_ = &device;

    const std::array<VkDescriptorSetLayoutBinding, 5> bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device.Handle(), &layoutInfo, nullptr, &setLayout_) !=
        VK_SUCCESS) {
        Destroy();
        return false;
    }

    const std::array<VkDescriptorPoolSize, 2> sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlots},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSlots * 4}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kSlots;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(device.Handle(), &poolInfo, nullptr, &pool_) != VK_SUCCESS) {
        Destroy();
        return false;
    }

    std::array<VkDescriptorSetLayout, kSlots> layouts{};
    layouts.fill(setLayout_);
    std::array<VkDescriptorSet, kSlots> sets{};
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = kSlots;
    allocate.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device.Handle(), &allocate, sets.data()) != VK_SUCCESS) {
        Destroy();
        return false;
    }

    for (uint32_t i = 0; i < kSlots; ++i) {
        Slot& slot = slots_[i];
        slot.set = sets[i];
        if (!slot.params.Create(device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, sizeof(GpuParams)) ||
            !slot.lights.Create(device, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                sizeof(GpuViewLight)) ||
            !slot.overflow.Create(device, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sizeof(uint32_t))) {
            Destroy();
            return false;
        }
    }

    const std::array<VkDescriptorSetLayout, 1> setLayouts{setLayout_};
    rhi::ComputePipelineDesc desc;
    desc.shader = shaderDirectory / "cluster_assign.comp.spv";
    desc.setLayouts = setLayouts;
    if (!pipeline_.Create(device, desc)) {
        Destroy();
        return false;
    }
    return true;
}

void ClusterAssign::Destroy() {
    if (device_ == nullptr) {
        return;
    }
    pipeline_.Destroy();
    for (Slot& slot : slots_) {
        DestroyDeviceBuffer(slot.ranges);
        DestroyDeviceBuffer(slot.indices);
        slot.params.Destroy();
        slot.lights.Destroy();
        slot.overflow.Destroy();
        slot.set = VK_NULL_HANDLE;
    }
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_->Handle(), pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    if (setLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->Handle(), setLayout_, nullptr);
        setLayout_ = VK_NULL_HANDLE;
    }
    clusterCount_ = 0;
    stride_ = 0;
    device_ = nullptr;
}

bool ClusterAssign::CreateDeviceBuffer(DeviceBuffer& target, VkDeviceSize bytes) {
    DestroyDeviceBuffer(target);
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateBuffer(device_->Allocator(), &info, &allocation, &target.buffer,
                        &target.allocation, nullptr) != VK_SUCCESS) {
        target.buffer = VK_NULL_HANDLE;
        return false;
    }
    target.bytes = bytes;
    return true;
}

void ClusterAssign::DestroyDeviceBuffer(DeviceBuffer& target) {
    if (target.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(device_->Allocator(), target.buffer, target.allocation);
        target.buffer = VK_NULL_HANDLE;
        target.allocation = VK_NULL_HANDLE;
        target.bytes = 0;
    }
}

bool ClusterAssign::Adopt(uint32_t clusterCount, uint32_t maxLightsPerCluster) {
    if (device_ == nullptr || !IsValid() || clusterCount == 0) {
        return false;
    }
    const uint32_t stride = std::max(maxLightsPerCluster, 1u);
    if (clusterCount_ == clusterCount && stride_ == stride) {
        return false;
    }
    clusterCount_ = clusterCount;
    stride_ = stride;

    const VkDeviceSize rangeBytes = static_cast<VkDeviceSize>(clusterCount) * sizeof(uint32_t) * 2;
    const VkDeviceSize indexBytes =
        static_cast<VkDeviceSize>(clusterCount) * stride * sizeof(uint32_t);
    for (Slot& slot : slots_) {
        if (!CreateDeviceBuffer(slot.ranges, rangeBytes) ||
            !CreateDeviceBuffer(slot.indices, indexBytes)) {
            SIM_ERROR("Render", "cannot allocate the GPU cluster assignment buffers");
            clusterCount_ = 0;
            stride_ = 0;
            return false;
        }
    }
    // Kedua slot ditulis di sini, dan di sini saja itu aman: `Adopt` dipanggil
    // dari `Create`/`Resize`, dan keduanya sudah menunggu device menganggur.
    for (uint32_t i = 0; i < kSlots; ++i) {
        WriteSlotDescriptors(i);
    }
    return true;
}

void ClusterAssign::WriteSlotDescriptors(uint32_t slot) {
    const Slot& target = slots_[slot];
    if (target.set == VK_NULL_HANDLE || target.ranges.buffer == VK_NULL_HANDLE) {
        return;
    }
    const std::array<VkDescriptorBufferInfo, 5> infos{
        VkDescriptorBufferInfo{target.params.Handle(), 0, sizeof(GpuParams)},
        VkDescriptorBufferInfo{target.lights.Handle(), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{target.ranges.buffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{target.indices.buffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{target.overflow.Handle(), 0, VK_WHOLE_SIZE}};
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = target.set;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = binding == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                      : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding].pBufferInfo = &infos[binding];
    }
    vkUpdateDescriptorSets(device_->Handle(), static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
}

void ClusterAssign::Upload(uint32_t slot, const ClusterGrid& grid, const Mat4& view,
                           std::span<const ClusterLight> lights, uint32_t maxLightsPerCluster) {
    if (!IsValid() || slot >= kSlots) {
        return;
    }
    Slot& target = slots_[slot];

    // Dikosongkan sebelum dispatch, dibaca sebelum dikosongkan. Nilai yang ada
    // di sana sekarang adalah hasil jalan terakhir slot ini, dan slot ini baru
    // dipakai ulang setelah fence-nya selesai.
    if (void* mapped = const_cast<void*>(target.overflow.Mapped()); mapped != nullptr) {
        const uint32_t zero = 0;
        std::memcpy(mapped, &zero, sizeof(zero));
    }

    // Batas dekat dan jauh kisi diambil dari irisan pertama dan terakhir, bukan
    // dari kamera: `ClusterGrid::Build` menjepit keduanya sendiri, dan shader
    // harus memakai angka yang sudah dijepit itu — bukan angka sebelum
    // penjepitan.
    GpuParams params;
    params.counts = {grid.TilesX(), grid.TilesY(), grid.Slices(),
                     static_cast<uint32_t>(lights.size())};
    params.grid = {grid.TanHalfX(), grid.TanHalfY(), grid.NearZ(), grid.FarZ()};
    params.limits = {std::max(maxLightsPerCluster, 1u), 0, 0, 0};
    target.params.Write(&params, sizeof(params));

    viewLights_.clear();
    viewLights_.reserve(lights.size());
    for (const ClusterLight& light : lights) {
        GpuViewLight entry;
        // Posisi memakai matriks penuh, arah hanya bagian 3x3-nya: translasi
        // tidak boleh ikut pada vektor arah. Sama persis dengan `AssignLights`.
        const Vec3 position = Vec3(view * Vec4(light.position, 1.0f));
        const Vec3 direction = glm::normalize(Mat3(view) * light.direction);
        entry.positionRange = Vec4(position, std::max(light.range, 0.0f));
        entry.directionCosOuter = Vec4(direction, light.cosOuterAngle);
        entry.flags = Vec4(light.type == ClusterLightType::Spot ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        viewLights_.push_back(entry);
    }
    // Buffer kosong tidak sah, dan `counts.w` nol membuat shader tidak pernah
    // membacanya.
    if (viewLights_.empty()) {
        viewLights_.push_back(GpuViewLight{});
    }
    const VkDeviceSize lightBytes = sizeof(GpuViewLight) * viewLights_.size();
    // Descriptor slot ini ditulis ulang hanya kalau buffernya benar-benar
    // berpindah. Slot ini sudah menunggu fence-nya, jadi menulisnya aman; slot
    // yang lain tidak disentuh sama sekali.
    // **Generasi, bukan handle** — lihat catatan di `DynamicBuffer::Generation`.
    const uint64_t before = target.lights.Generation();
    if (target.lights.Reserve(lightBytes)) {
        target.lights.Write(viewLights_.data(), lightBytes);
        if (target.lights.Generation() != before) {
            WriteSlotDescriptors(slot);
        }
    }
}

void ClusterAssign::Record(VkCommandBuffer cmd, uint32_t slot) const {
    if (!IsValid() || slot >= kSlots || clusterCount_ == 0) {
        return;
    }
    const std::array<VkDescriptorSet, 1> sets{slots_[slot].set};
    pipeline_.Bind(cmd, sets);
    vkCmdDispatch(cmd, rhi::GroupCount(clusterCount_, kGroupSize), 1, 1);
}

uint32_t ClusterAssign::Overflowed(uint32_t slot) const {
    if (slot >= kSlots) {
        return 0;
    }
    const void* mapped = slots_[slot].overflow.Mapped();
    if (mapped == nullptr) {
        return 0;
    }
    uint32_t value = 0;
    std::memcpy(&value, mapped, sizeof(value));
    return value;
}

}  // namespace sim::render
