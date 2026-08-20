#include "Sim/RHI/Pipeline.h"

#include "Sim/RHI/Device.h"

#include <fstream>
#include <vector>

namespace sim::rhi {
namespace {

std::vector<uint32_t> ReadSpirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        SIM_ERROR("RHI", "cannot open shader {}", path.string());
        return {};
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || size % 4 != 0) {
        SIM_ERROR("RHI", "shader {} is {} bytes — not a whole number of SPIR-V words",
                  path.string(), static_cast<long long>(size));
        return {};
    }
    std::vector<uint32_t> code(static_cast<std::size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);
    return code;
}

}  // namespace

VkShaderModule LoadShaderModule(VkDevice device, const std::filesystem::path& path) {
    const std::vector<uint32_t> code = ReadSpirv(path);
    if (code.empty()) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        SIM_ERROR("RHI", "vkCreateShaderModule failed for {}", path.string());
        return VK_NULL_HANDLE;
    }
    return module;
}

ComputePipeline::~ComputePipeline() { Destroy(); }

bool ComputePipeline::Create(Device& device, const ComputePipelineDesc& desc) {
    Destroy();
    device_ = device.Handle();
    pushConstantBytes_ = desc.pushConstantBytes;

    VkPushConstantRange range{};
    // COMPUTE, dan hanya COMPUTE. Rentang yang menyebut tahap lain ditolak
    // sebagian driver dan diterima diam-diam oleh sebagian yang lain — yaitu
    // bentuk kesalahan yang bekerja di satu mesin saja.
    range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    range.size = desc.pushConstantBytes;

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = static_cast<uint32_t>(desc.setLayouts.size());
    layoutInfo.pSetLayouts = desc.setLayouts.empty() ? nullptr : desc.setLayouts.data();
    layoutInfo.pushConstantRangeCount = desc.pushConstantBytes > 0 ? 1 : 0;
    layoutInfo.pPushConstantRanges = desc.pushConstantBytes > 0 ? &range : nullptr;
    if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr, &layout_) != VK_SUCCESS) {
        SIM_ERROR("RHI", "cannot create the compute pipeline layout for {}",
                  desc.shader.string());
        Destroy();
        return false;
    }

    VkShaderModule module = LoadShaderModule(device_, desc.shader);
    if (module == VK_NULL_HANDLE) {
        Destroy();
        return false;
    }

    VkComputePipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    info.stage.module = module;
    info.stage.pName = desc.entryPoint;
    info.layout = layout_;

    const VkResult created =
        vkCreateComputePipelines(device_, device.PipelineCache(), 1, &info, nullptr, &pipeline_);
    // Modulnya tidak dibutuhkan lagi begitu pipeline-nya jadi — SPIR-V-nya sudah
    // disalin ke dalamnya.
    vkDestroyShaderModule(device_, module, nullptr);
    if (created != VK_SUCCESS) {
        SIM_ERROR("RHI", "vkCreateComputePipelines failed for {}: {}", desc.shader.string(),
                  ResultToString(created));
        Destroy();
        return false;
    }
    return true;
}

void ComputePipeline::Destroy() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, layout_, nullptr);
        layout_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
    pushConstantBytes_ = 0;
}

void ComputePipeline::Bind(VkCommandBuffer cmd, std::span<const VkDescriptorSet> sets) const {
    if (pipeline_ == VK_NULL_HANDLE) {
        return;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
    if (!sets.empty()) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0,
                                static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
    }
}

void ComputePipeline::Push(VkCommandBuffer cmd, const void* data, uint32_t bytes) const {
    if (pipeline_ == VK_NULL_HANDLE) {
        return;
    }
    if (bytes != pushConstantBytes_) {
        SIM_ERROR("RHI", "compute push constant is {} bytes, the layout declares {}", bytes,
                  pushConstantBytes_);
        return;
    }
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes, data);
}

}  // namespace sim::rhi
