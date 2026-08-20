#include "VolumePass.h"

#include "Sim/Core/Log.h"

#include <array>
#include <fstream>
#include <vector>

namespace sim::render {
namespace {

std::vector<uint32_t> ReadSpirv(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {};
    }
    const std::streamsize size = stream.tellg();
    if (size <= 0 || size % 4 != 0) {
        return {};
    }
    std::vector<uint32_t> code(static_cast<std::size_t>(size) / 4);
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(code.data()), size);
    return code;
}

VkShaderModule LoadModule(VkDevice device, const std::filesystem::path& path) {
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
        return VK_NULL_HANDLE;
    }
    return module;
}

/// Push constant, tata letaknya harus sama persis dengan `Push` di
/// `Shaders/volume_raymarch.frag.slang`. Dua tata letak yang bergeser tidak
/// menghasilkan galat apa pun — hanya asap yang muncul di tempat yang salah,
/// atau tidak muncul sama sekali.
struct VolumePush {
    Mat4 invViewProj{1.0f};
    Vec4 cameraStep{0.0f};
    Vec4 boundsMinExtinction{0.0f};
    Vec4 boundsMaxCutoff{0.0f};
    Vec4 albedoSteps{0.0f};
    Vec4 lightScale{0.0f};
    Vec4 decode{0.0f};
};

}  // namespace

bool VolumePass::Create(rhi::Device& device, const std::filesystem::path& shaderDirectory,
                        VkFormat sceneFormat) {
    Destroy();
    device_ = &device;

    vertex_ = LoadModule(device.Handle(), shaderDirectory / "fullscreen_uv.vert.spv");
    if (vertex_ == VK_NULL_HANDLE) {
        SIM_WARN("Render", "volume pass: fullscreen_uv.vert.spv is missing");
        Destroy();
        return false;
    }
    if (!CreateDescriptors()) {
        Destroy();
        return false;
    }

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(VolumePush);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device.Handle(), &layoutInfo, nullptr, &layout_) != VK_SUCCESS) {
        Destroy();
        return false;
    }

    VkShaderModule fragment = LoadModule(device.Handle(), shaderDirectory / "volume_raymarch.frag.spv");
    if (fragment == VK_NULL_HANDLE) {
        SIM_WARN("Render", "volume pass: volume_raymarch.frag.spv is missing");
        Destroy();
        return false;
    }

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex_, "main",
                                        nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment,
                                        "main", nullptr}};

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // `dst = src + dst * src.a`, sama persis dengan kabut atmosferik: shader
    // mengembalikan cahaya terhambur di rgb dan transmitansi di alfa, jadi
    // komposit terjadi lewat faktor blending alih-alih dengan membaca kembali
    // gambar HDR yang sedang ditulisi.
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    // Alfa adegan tidak dibaca siapa pun di hilir; menulisinya dengan
    // transmitansi hanya menyimpan angka yang menyesatkan pembaca berikutnya.
    attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;

    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,
                                                      VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();

    VkFormat colorFormat = sceneFormat;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout_;

    const VkResult created =
        vkCreateGraphicsPipelines(device.Handle(), device.PipelineCache(), 1, &info, nullptr, &pipeline_);
    vkDestroyShaderModule(device.Handle(), fragment, nullptr);
    if (created != VK_SUCCESS) {
        Destroy();
        return false;
    }
    return true;
}

bool VolumePass::CreateDescriptors() {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device_->Handle(), &layoutInfo, nullptr, &setLayout_) !=
        VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device_->Handle(), &poolInfo, nullptr, &pool_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &setLayout_;
    return vkAllocateDescriptorSets(device_->Handle(), &allocate, &set_) == VK_SUCCESS;
}

void VolumePass::WriteDescriptor() {
    if (set_ == VK_NULL_HANDLE || !volume_.IsValid()) {
        return;
    }
    VkDescriptorImageInfo image{};
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image.imageView = volume_.Texture().View();
    image.sampler = volume_.Texture().Sampler();
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device_->Handle(), 1, &write, 0, nullptr);
}

bool VolumePass::SetVolume(const VolumeGrid& grid, VolumeTextureFormat format) {
    if (device_ == nullptr) {
        return false;
    }
    if (!volume_.Create(*device_, grid, format)) {
        return false;
    }
    WriteDescriptor();
    return true;
}

void VolumePass::ClearVolume() { volume_.Destroy(); }

void VolumePass::WorldBounds(const Vec3& position, float scale, Vec3& outMin,
                             Vec3& outMax) const {
    Vec3 localMin;
    Vec3 localMax;
    volume_.LocalBounds(localMin, localMax);
    const float safeScale = scale > 0.0f ? scale : 1.0f;
    outMin = position + localMin * safeScale;
    outMax = position + localMax * safeScale;
}

void VolumePass::RecordDraw(VkCommandBuffer command, const Mat4& invViewProj,
                            const Vec3& cameraPosition, const ViewportVolume& settings) {
    if (!IsValid() || !volume_.IsValid()) {
        return;
    }

    Vec3 boundsMin;
    Vec3 boundsMax;
    WorldBounds(settings.position, settings.scale, boundsMin, boundsMax);

    const VolumeTextureDesc& desc = volume_.Desc();
    VolumePush push;
    push.invViewProj = invViewProj;
    push.cameraStep = Vec4(cameraPosition, std::max(settings.stepSize, 1e-4f));
    push.boundsMinExtinction = Vec4(boundsMin, settings.extinction);
    push.boundsMaxCutoff = Vec4(boundsMax, settings.minTransmittance);
    push.albedoSteps = Vec4(settings.scatterAlbedo, static_cast<float>(settings.maxSteps));
    push.lightScale = Vec4(settings.incomingLight, desc.scale);
    push.decode = Vec4(desc.bias, 0.0f, 0.0f, 0.0f);

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0,
                            nullptr);
    vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(command, 3, 1, 0, 0);
}

void VolumePass::Destroy() {
    volume_.Destroy();
    if (device_ != nullptr) {
        VkDevice handle = device_->Handle();
        if (pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(handle, pipeline_, nullptr);
        }
        if (layout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(handle, layout_, nullptr);
        }
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(handle, pool_, nullptr);
        }
        if (setLayout_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(handle, setLayout_, nullptr);
        }
        if (vertex_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(handle, vertex_, nullptr);
        }
    }
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    vertex_ = VK_NULL_HANDLE;
}

}  // namespace sim::render
