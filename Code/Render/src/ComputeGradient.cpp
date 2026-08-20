#include "ComputeGradient.h"

#include "Sim/Core/Log.h"

#include <array>

namespace sim::render {
namespace {

/// Delapan bit per kanal, bukan setengah presisi. Yang ditulis pass ini adalah
/// warna yang siap tampil — ia berjalan sesudah tone mapping — jadi jangkauan
/// dinamis di atas satu tidak berarti apa-apa di sini, dan RGBA8 adalah format
/// yang dijamin bisa dipakai sebagai storage image oleh setiap perangkat Vulkan.
constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;

/// Harus sama persis dengan blok push_constant di
/// `Shaders/debug_gradient.comp.slang`.
struct GradientPush {
    uint32_t width = 0;
    uint32_t height = 0;
};

/// Harus sama persis dengan `Push` di `Shaders/present.frag.slang`.
struct BlitPush {
    float sourceUvScaleU = 1.0f;
    float sourceUvScaleV = 1.0f;
};

}  // namespace

bool ComputeGradient::Create(rhi::Device& device, const std::filesystem::path& shaderDirectory,
                             VkFormat outputFormat) {
    Destroy();
    device_ = &device;
    VkDevice handle = device.Handle();

    VkDescriptorSetLayoutBinding storage{};
    storage.binding = 0;
    storage.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    storage.descriptorCount = 1;
    storage.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo storageInfo{};
    storageInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    storageInfo.bindingCount = 1;
    storageInfo.pBindings = &storage;
    if (vkCreateDescriptorSetLayout(handle, &storageInfo, nullptr, &storageLayout_) != VK_SUCCESS) {
        Destroy();
        return false;
    }

    VkDescriptorSetLayoutBinding sampled{};
    sampled.binding = 0;
    sampled.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampled.descriptorCount = 1;
    sampled.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo sampledInfo{};
    sampledInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    sampledInfo.bindingCount = 1;
    sampledInfo.pBindings = &sampled;
    if (vkCreateDescriptorSetLayout(handle, &sampledInfo, nullptr, &sampledLayout_) != VK_SUCCESS) {
        Destroy();
        return false;
    }

    const std::array<VkDescriptorSetLayout, 1> computeSets{storageLayout_};
    rhi::ComputePipelineDesc compute;
    compute.shader = shaderDirectory / "debug_gradient.comp.spv";
    compute.setLayouts = computeSets;
    compute.pushConstantBytes = sizeof(GradientPush);
    if (!pipeline_.Create(device, compute)) {
        Destroy();
        return false;
    }

    // --- pipeline yang menampilkannya ---------------------------------------
    //
    // Shader-nya milik presenter, dipakai apa adanya. Yang dikerjakan keduanya
    // memang satu hal yang sama: menyalin sebuah image yang dicuplik ke
    // lampiran yang sedang aktif, dengan skala uv yang mengoreksi target yang
    // dialokasikan lebih besar daripada viewport. Menyalin enam baris shader
    // untuk mengatakan hal yang sama akan membuat dua tempat yang harus
    // diperbaiki ketika kesepakatan skala uv itu berubah.
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(BlitPush);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &sampledLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(handle, &layoutInfo, nullptr, &blitLayout_) != VK_SUCCESS) {
        Destroy();
        return false;
    }

    VkShaderModule vertex =
        rhi::LoadShaderModule(handle, shaderDirectory / "fullscreen_uv.vert.spv");
    VkShaderModule fragment = rhi::LoadShaderModule(handle, shaderDirectory / "present.frag.spv");
    if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) {
        if (vertex != VK_NULL_HANDLE) {
            vkDestroyShaderModule(handle, vertex, nullptr);
        }
        if (fragment != VK_NULL_HANDLE) {
            vkDestroyShaderModule(handle, fragment, nullptr);
        }
        Destroy();
        return false;
    }

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main",
                                        nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main",
                                        nullptr}};

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
    attachment.blendEnable = VK_FALSE;
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &outputFormat;

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
    info.layout = blitLayout_;

    const VkResult created = vkCreateGraphicsPipelines(handle, device.PipelineCache(), 1, &info,
                                                       nullptr, &blitPipeline_);
    vkDestroyShaderModule(handle, vertex, nullptr);
    vkDestroyShaderModule(handle, fragment, nullptr);
    if (created != VK_SUCCESS) {
        Destroy();
        return false;
    }

    // Satu set masing-masing, bukan satu per frame in-flight. Yang ditunjuk
    // keduanya hanya berpindah saat image-nya dialokasi ulang, dan alokasi ulang
    // sudah menunggu device idle lebih dulu.
    const std::array<VkDescriptorPoolSize, 2> sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = static_cast<uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();
    if (vkCreateDescriptorPool(handle, &poolInfo, nullptr, &pool_) != VK_SUCCESS) {
        Destroy();
        return false;
    }
    const std::array<VkDescriptorSetLayout, 2> layouts{storageLayout_, sampledLayout_};
    std::array<VkDescriptorSet, 2> sets{};
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocate.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(handle, &allocate, sets.data()) != VK_SUCCESS) {
        Destroy();
        return false;
    }
    storageSet_ = sets[0];
    sampledSet_ = sets[1];

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // NEAREST: yang diperiksa gambar ini adalah piksel yang benar-benar ditulis
    // dispatch, dan penapisan linear akan menutupi baris terakhir yang tidak
    // pernah ditulis dengan warna tetangganya.
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(handle, &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        Destroy();
        return false;
    }
    return true;
}

void ComputeGradient::Destroy() {
    if (device_ == nullptr) {
        return;
    }
    VkDevice handle = device_->Handle();
    DestroyImage();
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(handle, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(handle, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
        storageSet_ = VK_NULL_HANDLE;
        sampledSet_ = VK_NULL_HANDLE;
    }
    if (blitPipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(handle, blitPipeline_, nullptr);
        blitPipeline_ = VK_NULL_HANDLE;
    }
    if (blitLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(handle, blitLayout_, nullptr);
        blitLayout_ = VK_NULL_HANDLE;
    }
    pipeline_.Destroy();
    if (sampledLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(handle, sampledLayout_, nullptr);
        sampledLayout_ = VK_NULL_HANDLE;
    }
    if (storageLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(handle, storageLayout_, nullptr);
        storageLayout_ = VK_NULL_HANDLE;
    }
    device_ = nullptr;
}

void ComputeGradient::DestroyImage() {
    if (device_ == nullptr) {
        return;
    }
    if (view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_->Handle(), view_, nullptr);
        view_ = VK_NULL_HANDLE;
    }
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(device_->Allocator(), image_, allocation_);
        image_ = VK_NULL_HANDLE;
        allocation_ = VK_NULL_HANDLE;
    }
    allocatedWidth_ = 0;
    allocatedHeight_ = 0;
}

bool ComputeGradient::Adopt(uint32_t allocatedWidth, uint32_t allocatedHeight) {
    if (device_ == nullptr || !IsValid() || allocatedWidth == 0 || allocatedHeight == 0) {
        return false;
    }
    if (image_ != VK_NULL_HANDLE && allocatedWidth_ == allocatedWidth &&
        allocatedHeight_ == allocatedHeight) {
        return false;
    }
    DestroyImage();

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kFormat;
    imageInfo.extent = {allocatedWidth, allocatedHeight, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    if (vmaCreateImage(device_->Allocator(), &imageInfo, &allocation, &image_, &allocation_,
                       nullptr) != VK_SUCCESS) {
        SIM_ERROR("Render", "cannot allocate the compute gradient image");
        image_ = VK_NULL_HANDLE;
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image_;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = kFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &view_) != VK_SUCCESS) {
        DestroyImage();
        return false;
    }

    allocatedWidth_ = allocatedWidth;
    allocatedHeight_ = allocatedHeight;
    return WriteDescriptors();
}

bool ComputeGradient::WriteDescriptors() {
    VkDescriptorImageInfo storage{};
    // GENERAL, karena itulah layout image ini saat dispatch berjalan — dan
    // layout di sini bukan keterangan melainkan janji: descriptor yang menyebut
    // layout selain yang sedang berlaku adalah pembacaan yang tidak sah.
    storage.imageView = view_;
    storage.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo sampled{};
    sampled.sampler = sampler_;
    sampled.imageView = view_;
    sampled.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = storageSet_;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &storage;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = sampledSet_;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &sampled;
    vkUpdateDescriptorSets(device_->Handle(), static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
    return true;
}

void ComputeGradient::AdoptLayout(rhi::Device& device) const {
    if (image_ == VK_NULL_HANDLE) {
        return;
    }
    VkCommandBuffer cmd = device.BeginOneShot();
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
    device.EndOneShot(cmd);
}

void ComputeGradient::RecordFill(VkCommandBuffer cmd, uint32_t width, uint32_t height) const {
    if (image_ == VK_NULL_HANDLE || width == 0 || height == 0) {
        return;
    }
    const std::array<VkDescriptorSet, 1> sets{storageSet_};
    pipeline_.Bind(cmd, sets);
    const GradientPush push{width, height};
    pipeline_.Push(cmd, &push, sizeof(push));
    vkCmdDispatch(cmd, rhi::GroupCount(width, kGroupSize), rhi::GroupCount(height, kGroupSize), 1);
}

void ComputeGradient::RecordBlit(VkCommandBuffer cmd, uint32_t width, uint32_t height) const {
    if (image_ == VK_NULL_HANDLE || allocatedWidth_ == 0 || allocatedHeight_ == 0) {
        return;
    }
    BlitPush push{};
    push.sourceUvScaleU = static_cast<float>(width) / static_cast<float>(allocatedWidth_);
    push.sourceUvScaleV = static_cast<float>(height) / static_cast<float>(allocatedHeight_);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blitLayout_, 0, 1, &sampledSet_,
                            0, nullptr);
    vkCmdPushConstants(cmd, blitLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

}  // namespace sim::render
