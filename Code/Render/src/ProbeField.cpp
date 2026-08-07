#include "ProbeField.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <vector>

namespace sim::render {
namespace {

constexpr VkFormat kShFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

/// Harus sama persis dengan blok push_constant di Shaders/gi_probe.frag.slang.
struct ProbePush {
    std::array<uint32_t, 4> grid{};
    std::array<float, 4> params{};
};

std::vector<uint32_t> ReadSpirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        SIM_ERROR("Render", "cannot open shader {}", path.string());
        return {};
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || size % 4 != 0) {
        return {};
    }
    std::vector<uint32_t> code(static_cast<std::size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);
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

}  // namespace

bool ProbeField::Create(rhi::Device& device, const std::filesystem::path& shaderDirectory,
                        VkDescriptorSetLayout frameSetLayout) {
    Destroy();
    device_ = &device;

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(ProbePush);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &frameSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device_->Handle(), &layoutInfo, nullptr, &pipelineLayout_) !=
        VK_SUCCESS) {
        return false;
    }

    VkShaderModule vertex = LoadModule(device_->Handle(), shaderDirectory / "hiz_reduce.vert.spv");
    VkShaderModule fragment = LoadModule(device_->Handle(), shaderDirectory / "gi_probe.frag.spv");
    if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) {
        SIM_WARN("Render", "probe shaders unavailable; screen probes stay off");
        return false;
    }

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main",
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

    // **Akumulasi temporal dikerjakan blend unit.** `dst = src·c + dst·(1−c)`
    // dengan `c` konstanta dinamis — persis `AccumulateProbe` di sisi C++.
    VkPipelineColorBlendAttachmentState blendState{};
    blendState.blendEnable = VK_TRUE;
    blendState.srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
    blendState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    blendState.colorBlendOp = VK_BLEND_OP_ADD;
    blendState.srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
    blendState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    blendState.alphaBlendOp = VK_BLEND_OP_ADD;
    blendState.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    const std::array<VkPipelineColorBlendAttachmentState, kShChannels> attachments{
        blendState, blendState, blendState};

    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = static_cast<uint32_t>(attachments.size());
    blend.pAttachments = attachments.data();

    const std::array<VkDynamicState, 3> dynamicStates{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();

    const std::array<VkFormat, kShChannels> formats{kShFormat, kShFormat, kShFormat};
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = static_cast<uint32_t>(formats.size());
    rendering.pColorAttachmentFormats = formats.data();

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
    info.layout = pipelineLayout_;

    const VkResult result = vkCreateGraphicsPipelines(device_->Handle(), VK_NULL_HANDLE, 1, &info,
                                                      nullptr, &pipeline_);
    vkDestroyShaderModule(device_->Handle(), vertex, nullptr);
    vkDestroyShaderModule(device_->Handle(), fragment, nullptr);
    if (result != VK_SUCCESS) {
        SIM_ERROR("Render", "cannot create the probe trace pipeline");
        pipeline_ = VK_NULL_HANDLE;
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // NEAREST: normal yang dirata-ratakan di tepi siluet bukan normal permukaan
    // mana pun, dan koefisien SH dua probe yang berbeda permukaan tidak boleh
    // dicampur perangkat keras — pencampurannya harus lewat bobot bilateral,
    // yang justru tugasnya menolak pasangan seperti itu.
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_->Handle(), &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        return false;
    }
    return true;
}

bool ProbeField::CreateAttachment(Attachment& attachment, const glm::uvec2& size,
                                  VkFormat format) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {size.x, size.y, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(device_->Allocator(), &imageInfo, &allocation, &attachment.image,
                       &attachment.allocation, nullptr) != VK_SUCCESS) {
        attachment.image = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = attachment.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    return vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &attachment.view) ==
           VK_SUCCESS;
}

bool ProbeField::Adopt(uint32_t allocatedWidth, uint32_t allocatedHeight,
                       VkFormat normalFormat) {
    if (device_ == nullptr || pipeline_ == VK_NULL_HANDLE || allocatedWidth == 0 ||
        allocatedHeight == 0) {
        return false;
    }
    if (normalImage_ != VK_NULL_HANDLE && allocated_ == glm::uvec2(allocatedWidth, allocatedHeight)) {
        return false;
    }
    DestroyImages();
    allocated_ = {allocatedWidth, allocatedHeight};

    Attachment normal;
    if (!CreateAttachment(normal, allocated_, normalFormat)) {
        SIM_ERROR("Render", "cannot allocate the G-buffer normal target");
        return false;
    }
    normalImage_ = normal.image;
    normalAllocation_ = normal.allocation;
    normalView_ = normal.view;

    // Kapasitas probe diturunkan dari ukuran alokasi dengan ubin terkecil yang
    // mungkin dipakai, supaya viewport yang mengecil tidak menuntut alokasi
    // ulang. Ukuran yang benar-benar dipakai datang per frame lewat render area.
    ProbeGridSettings smallest;
    smallest.tileSize = 8;
    ProbeGrid grid;
    grid.Configure(allocated_.x, allocated_.y, smallest);
    probeCapacity_ = grid.Counts();

    for (Attachment& channel : sh_) {
        if (!CreateAttachment(channel, probeCapacity_, kShFormat)) {
            SIM_ERROR("Render", "cannot allocate a probe SH target");
            return false;
        }
    }
    shUndefined_ = true;
    return true;
}

namespace {

VkImageMemoryBarrier2 ColorBarrier(VkImage image) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    return barrier;
}

}  // namespace

void ProbeField::RecordNormalBegin(VkCommandBuffer cmd) {
    if (!IsValid()) {
        return;
    }
    VkImageMemoryBarrier2 barrier = ColorBarrier(normalImage_);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    // UNDEFINED: isinya ditimpa seluruhnya tiap frame, jadi tidak ada yang perlu
    // dipertahankan — dan menyebut layout sebelumnya memaksa driver menyalin isi
    // yang langsung dibuang.
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void ProbeField::RecordNormalEnd(VkCommandBuffer cmd) {
    if (!IsValid()) {
        return;
    }
    VkImageMemoryBarrier2 barrier = ColorBarrier(normalImage_);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void ProbeField::Record(VkCommandBuffer cmd, VkDescriptorSet frameSet, const ProbeGrid& grid,
                        uint32_t frameIndex, float blend) {
    if (!IsValid() || grid.ProbeCount() == 0) {
        return;
    }
    const glm::uvec2 counts = grid.Counts();

    std::array<VkImageMemoryBarrier2, kShChannels> barriers{};
    for (uint32_t i = 0; i < kShChannels; ++i) {
        barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barriers[i].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barriers[i].srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        // Frame pertama saja yang boleh menyebut UNDEFINED: sesudahnya isinya
        // adalah riwayat yang justru harus dipertahankan, dan UNDEFINED
        // mengizinkan driver membuangnya.
        barriers[i].oldLayout = shUndefined_ ? VK_IMAGE_LAYOUT_UNDEFINED
                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[i].image = sh_[i].image;
        barriers[i].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[i].subresourceRange.levelCount = 1;
        barriers[i].subresourceRange.layerCount = 1;
    }
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
    dependency.pImageMemoryBarriers = barriers.data();
    vkCmdPipelineBarrier2(cmd, &dependency);

    std::array<VkRenderingAttachmentInfo, kShChannels> colors{};
    for (uint32_t i = 0; i < kShChannels; ++i) {
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colors[i].imageView = sh_[i].view;
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // LOAD, bukan CLEAR: yang ada di sana riwayat yang sedang diakumulasi.
        colors[i].loadOp = shUndefined_ ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                        : VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colors[i].clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    }

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {counts.x, counts.y}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = static_cast<uint32_t>(colors.size());
    rendering.pColorAttachments = colors.data();
    vkCmdBeginRendering(cmd, &rendering);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(counts.x),
                              static_cast<float>(counts.y), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {counts.x, counts.y}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    const std::array<float, 4> constants{0.0f, 0.0f, 0.0f, std::clamp(blend, 0.0f, 1.0f)};
    vkCmdSetBlendConstants(cmd, constants.data());

    ProbePush push;
    push.grid = {counts.x, counts.y, grid.Settings().tileSize, grid.Settings().raysPerAxis};
    push.params = {static_cast<float>(frameIndex),
                   static_cast<float>(grid.Settings().accumulationFrames), 0.0f, 0.0f};

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                            &frameSet, 0, nullptr);
    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                       &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    for (uint32_t i = 0; i < kShChannels; ++i) {
        barriers[i].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[i].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barriers[i].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barriers[i].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    vkCmdPipelineBarrier2(cmd, &dependency);
    shUndefined_ = false;
}

void ProbeField::DestroyAttachment(Attachment& attachment) {
    if (attachment.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_->Handle(), attachment.view, nullptr);
        attachment.view = VK_NULL_HANDLE;
    }
    if (attachment.image != VK_NULL_HANDLE) {
        vmaDestroyImage(device_->Allocator(), attachment.image, attachment.allocation);
        attachment.image = VK_NULL_HANDLE;
        attachment.allocation = VK_NULL_HANDLE;
    }
}

void ProbeField::DestroyImages() {
    if (device_ == nullptr) {
        return;
    }
    Attachment normal{normalImage_, normalAllocation_, normalView_};
    DestroyAttachment(normal);
    normalImage_ = VK_NULL_HANDLE;
    normalAllocation_ = VK_NULL_HANDLE;
    normalView_ = VK_NULL_HANDLE;
    for (Attachment& channel : sh_) {
        DestroyAttachment(channel);
    }
    allocated_ = {0, 0};
    probeCapacity_ = {0, 0};
    shUndefined_ = true;
}

void ProbeField::Destroy() {
    if (device_ == nullptr) {
        return;
    }
    DestroyImages();
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_->Handle(), sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_->Handle(), pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_->Handle(), pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    device_ = nullptr;
}

}  // namespace sim::render
