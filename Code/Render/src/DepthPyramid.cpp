#include "DepthPyramid.h"

#include "Sim/Core/Log.h"
#include "Sim/Render/ScreenTrace.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

namespace sim::render {
namespace {

constexpr VkFormat kFormat = VK_FORMAT_R32_SFLOAT;

/// Harus sama persis dengan blok push_constant di Shaders/hiz_reduce.frag.
struct ReducePush {
    int32_t sourceWidth = 0;
    int32_t sourceHeight = 0;
    int32_t copyOnly = 0;
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

/// Ukuran mip berikutnya, aturan Vulkan: dibulatkan ke bawah, minimal satu.
uint32_t HalfFloor(uint32_t value) {
    return std::max(1u, value / 2);
}

}  // namespace

uint32_t DepthPyramid::LevelsFor(uint32_t width, uint32_t height) {
    return HiZPyramid::LevelsFor(width, height);
}

bool DepthPyramid::Create(rhi::Device& device, const std::filesystem::path& shaderDirectory) {
    Destroy();
    device_ = &device;

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

    // Satu set per tingkat. Enam belas cukup untuk 32768 piksel; layar yang
    // lebih besar dari itu belum ada.
    constexpr uint32_t kMaxLevels = 16;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxLevels};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kMaxLevels;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    if (vkCreateDescriptorPool(device_->Handle(), &poolInfo, nullptr, &pool_) != VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(ReducePush);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device_->Handle(), &pipelineLayoutInfo, nullptr,
                               &pipelineLayout_) != VK_SUCCESS) {
        return false;
    }

    VkShaderModule vertex = LoadModule(device_->Handle(), shaderDirectory / "hiz_reduce.vert.spv");
    VkShaderModule fragment =
        LoadModule(device_->Handle(), shaderDirectory / "hiz_reduce.frag.spv");
    if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) {
        SIM_WARN("Render", "HiZ shaders unavailable; screen-space GI tracing stays off");
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

    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
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

    VkFormat colorFormat = kFormat;
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
    info.layout = pipelineLayout_;

    const VkResult result = vkCreateGraphicsPipelines(device_->Handle(), VK_NULL_HANDLE, 1, &info,
                                                      nullptr, &pipeline_);
    vkDestroyShaderModule(device_->Handle(), vertex, nullptr);
    vkDestroyShaderModule(device_->Handle(), fragment, nullptr);
    if (result != VK_SUCCESS) {
        SIM_ERROR("Render", "cannot create the HiZ reduce pipeline");
        pipeline_ = VK_NULL_HANDLE;
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // NEAREST di kedua arah dan tanpa interpolasi antar-mip. Depth yang
    // dirata-ratakan bukan depth apa pun: ia bukan permukaan yang ada, dan
    // penelusur yang membandingkannya membandingkan sesuatu yang tidak nyata.
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device_->Handle(), &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        return false;
    }
    return true;
}

bool DepthPyramid::Adopt(uint32_t allocatedWidth, uint32_t allocatedHeight,
                         VkImageView depthView, VkSampler depthSampler) {
    if (device_ == nullptr || pipeline_ == VK_NULL_HANDLE || allocatedWidth == 0 ||
        allocatedHeight == 0) {
        return false;
    }
    if (image_ != VK_NULL_HANDLE && width_ == allocatedWidth && height_ == allocatedHeight) {
        return false;
    }
    DestroyImage();

    width_ = allocatedWidth;
    height_ = allocatedHeight;
    levels_ = LevelsFor(width_, height_);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kFormat;
    imageInfo.extent = {width_, height_, 1};
    imageInfo.mipLevels = levels_;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(device_->Allocator(), &imageInfo, &allocation, &image_, &allocation_,
                       nullptr) != VK_SUCCESS) {
        SIM_ERROR("Render", "cannot allocate the depth pyramid");
        image_ = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = levels_;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &view_) != VK_SUCCESS) {
        return false;
    }

    levelViews_.resize(levels_);
    viewInfo.subresourceRange.levelCount = 1;
    for (uint32_t level = 0; level < levels_; ++level) {
        viewInfo.subresourceRange.baseMipLevel = level;
        if (vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &levelViews_[level]) !=
            VK_SUCCESS) {
            return false;
        }
    }

    std::vector<VkDescriptorSetLayout> layouts(levels_, setLayout_);
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = pool_;
    allocateInfo.descriptorSetCount = levels_;
    allocateInfo.pSetLayouts = layouts.data();
    sources_.resize(levels_);
    if (vkAllocateDescriptorSets(device_->Handle(), &allocateInfo, sources_.data()) !=
        VK_SUCCESS) {
        SIM_ERROR("Render", "cannot allocate depth pyramid descriptor sets");
        sources_.clear();
        return false;
    }

    std::vector<VkDescriptorImageInfo> images(levels_);
    std::vector<VkWriteDescriptorSet> writes(levels_);
    for (uint32_t level = 0; level < levels_; ++level) {
        // Tingkat nol membaca depth buffer; sisanya membaca mip sebelumnya.
        images[level] = {level == 0 ? depthSampler : sampler_,
                         level == 0 ? depthView : levelViews_[level - 1],
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[level] = VkWriteDescriptorSet{};
        writes[level].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[level].dstSet = sources_[level];
        writes[level].dstBinding = 0;
        writes[level].descriptorCount = 1;
        writes[level].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[level].pImageInfo = &images[level];
    }
    vkUpdateDescriptorSets(device_->Handle(), levels_, writes.data(), 0, nullptr);
    return true;
}

void DepthPyramid::AdoptLayouts() {
    if (!IsValid()) {
        return;
    }
    VkCommandBuffer cmd = device_->BeginOneShot();
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_NONE;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
    device_->EndOneShot(cmd);
}

void DepthPyramid::Record(VkCommandBuffer cmd, uint32_t viewportWidth,
                          uint32_t viewportHeight) {
    if (!IsValid() || viewportWidth == 0 || viewportHeight == 0) {
        return;
    }
    const uint32_t used = std::min(levels_, LevelsFor(viewportWidth, viewportHeight));

    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;

    uint32_t sourceWidth = viewportWidth;
    uint32_t sourceHeight = viewportHeight;

    for (uint32_t level = 0; level < used; ++level) {
        const uint32_t levelWidth = level == 0 ? viewportWidth : HalfFloor(sourceWidth);
        const uint32_t levelHeight = level == 0 ? viewportHeight : HalfFloor(sourceHeight);

        // UNDEFINED sebagai layout lama: isi mip ini akan ditimpa seluruhnya,
        // jadi tidak ada apa pun yang perlu dipertahankan. Menyebut layout
        // sebelumnya justru memaksa driver menyalin isi yang langsung dibuang.
        barrier.subresourceRange.baseMipLevel = level;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dependency);

        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = levelViews_[level];
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        rendering.renderArea = {{0, 0}, {levelWidth, levelHeight}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &rendering);

        const VkViewport viewport{0.0f, 0.0f, static_cast<float>(levelWidth),
                                  static_cast<float>(levelHeight), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {levelWidth, levelHeight}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        ReducePush push;
        push.sourceWidth = static_cast<int32_t>(sourceWidth);
        push.sourceHeight = static_cast<int32_t>(sourceHeight);
        push.copyOnly = level == 0 ? 1 : 0;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1,
                                &sources_[level], 0, nullptr);
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                           &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);

        // Tingkat ini menjadi sumber tingkat berikutnya, jadi ia harus selesai
        // ditulis sebelum dibaca. Barrier per mip, bukan satu barrier di akhir:
        // yang dibaca dan yang ditulis adalah image yang sama.
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dependency);

        sourceWidth = levelWidth;
        sourceHeight = levelHeight;
    }

    // Mip yang tidak dipakai frame ini tetap harus punya layout yang sah: view
    // yang diikat penelusur mencakup seluruh mip, dan satu mip dalam layout
    // UNDEFINED membuat seluruh pembacaannya melanggar aturan.
    for (uint32_t level = used; level < levels_; ++level) {
        barrier.subresourceRange.baseMipLevel = level;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }
}

void DepthPyramid::DestroyImage() {
    if (device_ == nullptr) {
        return;
    }
    if (!sources_.empty()) {
        vkFreeDescriptorSets(device_->Handle(), pool_, static_cast<uint32_t>(sources_.size()),
                             sources_.data());
        sources_.clear();
    }
    for (VkImageView& level : levelViews_) {
        if (level != VK_NULL_HANDLE) {
            vkDestroyImageView(device_->Handle(), level, nullptr);
        }
    }
    levelViews_.clear();
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_->Handle(), view_, nullptr);
        view_ = VK_NULL_HANDLE;
    }
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(device_->Allocator(), image_, allocation_);
        image_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
    width_ = 0;
    height_ = 0;
    levels_ = 0;
}

void DepthPyramid::Destroy() {
    if (device_ == nullptr) {
        return;
    }
    DestroyImage();
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

}  // namespace sim::render
