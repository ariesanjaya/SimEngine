#include "DrawCull.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace sim::render {
namespace {

/// Harus sama persis dengan `Params` di Shaders/draw_cull.comp.slang.
struct GpuParams {
    std::array<Vec4, Frustum::kPlaneCount> planes{};
    Mat4 viewProjection{1.0f};
    std::array<uint32_t, 4> counts{};
};

/// Harus sama persis dengan `Push` di shader.
struct GpuPush {
    uint32_t phase = 0;
    uint32_t limit = 0xffffffffu;
    uint32_t first = 0;
    uint32_t debug = 0;
};

/// Harus sama persis dengan `VkDrawIndexedIndirectCommand` — dan ia memang
/// dipakai apa adanya sebagai ukuran dan langkah buffer perintah.
constexpr VkDeviceSize kCommandStride = sizeof(VkDrawIndexedIndirectCommand);
static_assert(kCommandStride == 20, "VkDrawIndexedIndirectCommand harus lima uint32");

}  // namespace

bool DrawCull::Create(rhi::Device& device, const std::filesystem::path& shaderDirectory) {
    Destroy();
    device_ = &device;

    const std::array<VkDescriptorSetLayoutBinding, 8> bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
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

    const std::array<VkDescriptorPoolSize, 3> sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSlots},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSlots * 5},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSlots * 2}};
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
        // Satu elemen untuk masing-masing: buffer berukuran nol tidak sah, dan
        // adegan kosong tetap harus menghasilkan descriptor set yang sah.
        if (!slot.params.Create(device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, sizeof(GpuParams)) ||
            !slot.bounds.Create(device, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sizeof(GpuBounds)) ||
            !slot.surfaces.Create(device, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  sizeof(GpuSurface)) ||
            !CreateCommandBuffer(slot.commands, kCommandStride) ||
            !CreateCommandBuffer(slot.visibleCommands, kCommandStride) ||
            !slot.debug.Create(device, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               sizeof(GpuCullDebug))) {
            Destroy();
            return false;
        }
        WriteSlotDescriptors(i);
    }

    const std::array<VkDescriptorSetLayout, 1> setLayouts{setLayout_};
    rhi::ComputePipelineDesc desc;
    desc.shader = shaderDirectory / "draw_cull.comp.spv";
    desc.setLayouts = setLayouts;
    desc.pushConstantBytes = sizeof(GpuPush);
    if (!pipeline_.Create(device, desc)) {
        Destroy();
        return false;
    }
    return true;
}

void DrawCull::Destroy() {
    if (device_ == nullptr) {
        return;
    }
    pipeline_.Destroy();
    for (Slot& slot : slots_) {
        DestroyDeviceBuffer(slot.commands);
        DestroyDeviceBuffer(slot.visibleCommands);
        slot.params.Destroy();
        slot.bounds.Destroy();
        slot.surfaces.Destroy();
        slot.debug.Destroy();
        slot.set = VK_NULL_HANDLE;
        slot.surfaceCount = 0;
    }
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_->Handle(), pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    if (setLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->Handle(), setLayout_, nullptr);
        setLayout_ = VK_NULL_HANDLE;
    }
    device_ = nullptr;
}

bool DrawCull::CreateCommandBuffer(DeviceBuffer& target, VkDeviceSize bytes) {
    DestroyDeviceBuffer(target);
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = bytes;
    // **`INDIRECT_BUFFER` di samping `STORAGE_BUFFER`, dan keduanya wajib.**
    // Yang menulisnya compute lewat storage descriptor; yang membacanya
    // `vkCmdDrawIndexedIndirect`. Buffer yang tidak menyebut keduanya ditolak
    // di salah satu sisi — dan mana yang ditolak bergantung pada mana yang
    // dipakai lebih dulu.
    info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
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

void DrawCull::DestroyDeviceBuffer(DeviceBuffer& target) {
    if (target.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(device_->Allocator(), target.buffer, target.allocation);
        target.buffer = VK_NULL_HANDLE;
        target.allocation = VK_NULL_HANDLE;
        target.bytes = 0;
    }
}

void DrawCull::WriteSlotDescriptors(uint32_t slot) {
    const Slot& target = slots_[slot];
    if (target.set == VK_NULL_HANDLE || target.commands.buffer == VK_NULL_HANDLE) {
        return;
    }
    // Piramida belum ada berarti descriptor-nya belum bisa ditulis, dan
    // descriptor yang dibiarkan kosong adalah pelanggaran pada setiap dispatch —
    // bahkan dispatch fase frustum yang tidak menyentuhnya.
    if (pyramidView_ == VK_NULL_HANDLE || depthView_ == VK_NULL_HANDLE) {
        return;
    }
    const std::array<VkDescriptorBufferInfo, 6> infos{
        VkDescriptorBufferInfo{target.params.Handle(), 0, sizeof(GpuParams)},
        VkDescriptorBufferInfo{target.bounds.Handle(), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{target.surfaces.Handle(), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{target.commands.buffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{target.visibleCommands.buffer, 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{target.debug.Handle(), 0, VK_WHOLE_SIZE}};
    const VkDescriptorImageInfo pyramid{pyramidSampler_, pyramidView_,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo depth{depthSampler_, depthView_,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::array<VkWriteDescriptorSet, 8> writes{};
    for (uint32_t binding = 0; binding < writes.size(); ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = target.set;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        if (binding == 4 || binding == 7) {
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].pImageInfo = binding == 4 ? &pyramid : &depth;
            continue;
        }
        writes[binding].descriptorType = binding == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                      : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding].pBufferInfo = &infos[binding < 4 ? binding : binding - 1];
    }
    vkUpdateDescriptorSets(device_->Handle(), static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
}

void DrawCull::AdoptPyramid(VkImageView view, VkSampler sampler, VkImageView depthView,
                            VkSampler depthSampler) {
    if (device_ == nullptr ||
        (view == pyramidView_ && sampler == pyramidSampler_ && depthView == depthView_ &&
         depthSampler == depthSampler_)) {
        return;
    }
    pyramidView_ = view;
    pyramidSampler_ = sampler;
    depthView_ = depthView;
    depthSampler_ = depthSampler;
    // Seluruh slot sekaligus, dan di sini itu aman: piramida hanya dibuat ulang
    // saat target render dibuat ulang, dan pemanggilnya sudah menunggu device
    // menganggur sebelum sampai ke sini.
    for (uint32_t i = 0; i < kSlots; ++i) {
        WriteSlotDescriptors(i);
    }
}

bool DrawCull::Upload(uint32_t slot, const Frustum& frustum, const Mat4& viewProjection,
                      uint32_t viewportWidth, uint32_t viewportHeight, uint32_t pyramidLevels,
                      std::span<const GpuBounds> bounds, std::span<const GpuSurface> surfaces) {
    if (!IsValid() || slot >= kSlots || bounds.size() != surfaces.size()) {
        return false;
    }
    Slot& target = slots_[slot];
    target.surfaceCount = static_cast<uint32_t>(bounds.size());
    if (target.surfaceCount == 0) {
        return true;
    }

    GpuParams params;
    const std::array<Vec4, Frustum::kPlaneCount>& planes = frustum.Planes();
    std::copy(planes.begin(), planes.end(), params.planes.begin());
    params.viewProjection = viewProjection;
    params.counts = {target.surfaceCount, pyramidLevels, viewportWidth, viewportHeight};
    target.params.Write(&params, sizeof(params));

    // Descriptor slot ini ditulis ulang hanya kalau buffernya benar-benar
    // berpindah. Slot ini sudah menunggu fence-nya, jadi menulisnya aman; slot
    // yang lain tidak disentuh sama sekali.
    bool moved = false;
    const VkDeviceSize boundBytes = sizeof(GpuBounds) * bounds.size();
    const VkBuffer beforeBounds = target.bounds.Handle();
    if (!target.bounds.Reserve(boundBytes)) {
        return false;
    }
    target.bounds.Write(bounds.data(), boundBytes);
    moved = moved || target.bounds.Handle() != beforeBounds;

    const VkDeviceSize surfaceBytes = sizeof(GpuSurface) * surfaces.size();
    const VkBuffer beforeSurfaces = target.surfaces.Handle();
    if (!target.surfaces.Reserve(surfaceBytes)) {
        return false;
    }
    target.surfaces.Write(surfaces.data(), surfaceBytes);
    moved = moved || target.surfaces.Handle() != beforeSurfaces;

    const VkDeviceSize debugBytes = sizeof(GpuCullDebug) * surfaces.size();
    const VkBuffer beforeDebug = target.debug.Handle();
    if (!target.debug.Reserve(debugBytes)) {
        return false;
    }
    moved = moved || target.debug.Handle() != beforeDebug;

    const VkDeviceSize commandBytes = kCommandStride * surfaces.size();
    if (target.visibleCommands.bytes < commandBytes) {
        if (!CreateCommandBuffer(target.visibleCommands, commandBytes + commandBytes / 2)) {
            SIM_ERROR("Render", "cannot allocate the GPU draw command buffer");
            target.surfaceCount = 0;
            return false;
        }
        moved = true;
    }
    if (target.commands.bytes < commandBytes) {
        // **Tumbuh dengan kelonggaran, bukan tepat.** Buffer device-local dibuat
        // ulang berarti descriptor ditulis ulang, dan adegan yang bertambah satu
        // permukaan tiap frame akan membuat keduanya terjadi setiap frame.
        if (!CreateCommandBuffer(target.commands, commandBytes + commandBytes / 2)) {
            SIM_ERROR("Render", "cannot allocate the GPU draw command buffer");
            target.surfaceCount = 0;
            return false;
        }
        moved = true;
    }
    if (moved) {
        WriteSlotDescriptors(slot);
    }
    return true;
}

void DrawCull::ReadDebug(uint32_t slot, std::vector<GpuCullDebug>& out) const {
    out.clear();
    if (slot >= kSlots) {
        return;
    }
    const Slot& target = slots_[slot];
    const void* mapped = target.debug.Mapped();
    if (mapped == nullptr || target.surfaceCount == 0) {
        return;
    }
    out.resize(target.surfaceCount);
    std::memcpy(out.data(), mapped, sizeof(GpuCullDebug) * out.size());
}

void DrawCull::Record(VkCommandBuffer cmd, uint32_t slot, Phase phase, bool debug,
                      uint32_t limit, uint32_t first) const {
    if (!IsValid() || slot >= kSlots || pyramidView_ == VK_NULL_HANDLE) {
        return;
    }
    const Slot& target = slots_[slot];
    if (target.surfaceCount == 0 || target.set == VK_NULL_HANDLE) {
        return;
    }
    const GpuPush push{static_cast<uint32_t>(phase), limit, first, debug ? 1u : 0u};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.Handle());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_.Layout(), 0, 1,
                            &target.set, 0, nullptr);
    pipeline_.Push(cmd, &push, sizeof(push));
    vkCmdDispatch(cmd, (target.surfaceCount + kGroupSize - 1) / kGroupSize, 1, 1);
}

}  // namespace sim::render
