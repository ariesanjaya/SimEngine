#include "Sim/RHI/Buffer.h"
#include "PostProcess.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace sim::render {
namespace {

constexpr VkFormat kLuminanceFormat = VK_FORMAT_R16_SFLOAT;
/// **32 bit, bukan 16.** Eksposur adegan siang hari cerah berada di sekitar
/// 1,3·10⁻⁵, dan nilai normal terkecil half-float adalah 6·10⁻⁵ — seluruh
/// rentang siang hari akan menjadi nol atau subnormal. Yang terlihat bukan
/// galat melainkan layar hitam di bawah matahari.
constexpr VkFormat kExposureFormat = VK_FORMAT_R32_SFLOAT;

struct SeedPush {
    float viewportUvMaxX = 1.0f;
    float viewportUvMaxY = 1.0f;
    int32_t seedWidth = 0;
    int32_t seedHeight = 0;
};

struct ReducePush {
    int32_t sourceWidth = 0;
    int32_t sourceHeight = 0;
};

struct ExposurePush {
    Vec4 params{0.0f};
    Vec4 adaptation{0.0f};
};

struct BloomPush {
    float sourceTexelX = 0.0f;
    float sourceTexelY = 0.0f;
    float sourceUvScaleX = 1.0f;
    float sourceUvScaleY = 1.0f;
    Vec4 params{0.0f};
};

struct ResolvePush {
    Vec4 params{0.0f};
};

/// Ukuran sebuah tingkat bloom: dibagi dua dibulatkan ke atas, minimal satu.
/// **Ke atas, bukan ke bawah:** dibulatkan ke bawah, baris terakhir viewport
/// ganjil tidak punya texel yang menanggungnya, dan yang hilang itu tepi layar.
uint32_t HalfCeil(uint32_t value) {
    return std::max((value + 1u) / 2u, 1u);
}

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

bool PostProcess::CreateSetLayout(uint32_t bindingCount, VkDescriptorSetLayout& outLayout) {
    // Ukurannya dari `bindingCount`, bukan larik tetap. Bentuk pertama saya
    // memakai `std::array<..., 2>`, dan set ketiga — penyelesaian yang membaca
    // adegan, eksposur, dan bloom — menulis di luarnya: **stack corruption, bukan
    // galat.** Crash-nya bahkan muncul di dalam driver, jauh dari sebabnya.
    // Angka yang harus diperbarui setiap kali sebuah binding ditambahkan adalah
    // angka yang suatu saat lupa diperbarui.
    std::vector<VkDescriptorSetLayoutBinding> bindings(bindingCount);
    for (uint32_t i = 0; i < bindingCount; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = bindingCount;
    info.pBindings = bindings.data();
    return vkCreateDescriptorSetLayout(device_->Handle(), &info, nullptr, &outLayout) ==
           VK_SUCCESS;
}

bool PostProcess::CreatePipeline(const std::filesystem::path& shaderDirectory,
                                 const char* fragmentName, VkDescriptorSetLayout setLayout,
                                 uint32_t pushSize, VkFormat colorFormat,
                                 VkPipelineLayout& outLayout, VkPipeline& outPipeline,
                                 VkShaderModule vertexModule) {
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = pushSize;
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device_->Handle(), &layoutInfo, nullptr, &outLayout) !=
        VK_SUCCESS) {
        return false;
    }

    VkShaderModule fragment = LoadModule(device_->Handle(), shaderDirectory / fragmentName);
    if (fragment == VK_NULL_HANDLE) {
        return false;
    }

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertexModule,
                                        "main", nullptr},
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

    VkFormat format = colorFormat;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;

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
    info.layout = outLayout;

    const VkResult result = vkCreateGraphicsPipelines(device_->Handle(), device_->PipelineCache(), 1, &info,
                                                      nullptr, &outPipeline);
    vkDestroyShaderModule(device_->Handle(), fragment, nullptr);
    if (result != VK_SUCCESS) {
        SIM_ERROR("Render", "cannot create post-process pipeline {}", fragmentName);
        outPipeline = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool PostProcess::Create(rhi::Device& device, const std::filesystem::path& shaderDirectory,
                         VkFormat outputFormat) {
    Destroy();
    device_ = &device;
    outputFormat_ = outputFormat;

    vertex_ = LoadModule(device_->Handle(), shaderDirectory / "fullscreen.vert.spv");
    vertexUv_ = LoadModule(device_->Handle(), shaderDirectory / "fullscreen_uv.vert.spv");
    if (vertex_ == VK_NULL_HANDLE || vertexUv_ == VK_NULL_HANDLE) {
        SIM_ERROR("Render", "post-process needs fullscreen.vert.spv and fullscreen_uv.vert.spv");
        return false;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(device_->Handle(), &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        return false;
    }
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(device_->Handle(), &samplerInfo, nullptr, &pointSampler_) != VK_SUCCESS) {
        return false;
    }

    if (!CreateSetLayout(1, oneSourceLayout_) || !CreateSetLayout(2, twoSourceLayout_) ||
        !CreateSetLayout(3, threeSourceLayout_)) {
        return false;
    }

    // Satu set petak awal, satu per tingkat reduksi, dua eksposur, dua
    // penyelesaian.
    constexpr uint32_t kMaxSets = 1 + 16 + 2 + 2 + 32;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSets * 3};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = kMaxSets;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    if (vkCreateDescriptorPool(device_->Handle(), &poolInfo, nullptr, &pool_) != VK_SUCCESS) {
        return false;
    }

    if (!CreatePipeline(shaderDirectory, "lum_seed.frag.spv", oneSourceLayout_, sizeof(SeedPush),
                        kLuminanceFormat, seedLayout_, seedPipeline_, vertex_) ||
        !CreatePipeline(shaderDirectory, "lum_reduce.frag.spv", oneSourceLayout_,
                        sizeof(ReducePush), kLuminanceFormat, reduceLayout_, reducePipeline_,
                        vertex_) ||
        !CreatePipeline(shaderDirectory, "exposure.frag.spv", twoSourceLayout_,
                        sizeof(ExposurePush), kExposureFormat, exposureLayout_, exposurePipeline_,
                        vertex_) ||
        !CreatePipeline(shaderDirectory, "bloom_down.frag.spv", oneSourceLayout_,
                        sizeof(BloomPush), kSceneFormat, bloomDownLayout_, bloomDownPipeline_,
                        vertexUv_) ||
        !CreatePipeline(shaderDirectory, "bloom_up.frag.spv", twoSourceLayout_, sizeof(BloomPush),
                        kSceneFormat, bloomUpLayout_, bloomUpPipeline_, vertexUv_) ||
        !CreatePipeline(shaderDirectory, "tonemap.frag.spv", threeSourceLayout_,
                        sizeof(ResolvePush), outputFormat_, resolveLayout_, resolvePipeline_,
                        vertex_)) {
        return false;
    }
    return true;
}

bool PostProcess::Adopt(uint32_t allocatedWidth, uint32_t allocatedHeight) {
    if (device_ == nullptr || resolvePipeline_ == VK_NULL_HANDLE || allocatedWidth == 0 ||
        allocatedHeight == 0) {
        return false;
    }
    if (sceneImage_ != VK_NULL_HANDLE && allocatedWidth_ == allocatedWidth &&
        allocatedHeight_ == allocatedHeight) {
        return false;
    }
    DestroyImages();
    allocatedWidth_ = allocatedWidth;
    allocatedHeight_ = allocatedHeight;

    const auto make = [&](Image& target, VkFormat format, uint32_t width, uint32_t height,
                          uint32_t mips, bool clearable = false) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = format;
        imageInfo.extent = {width, height, 1};
        imageInfo.mipLevels = mips;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        // Hanya texel eksposur yang dibersihkan lewat `vkCmdClearColorImage`, dan
        // Vulkan menuntut penanda pemakaiannya disebutkan saat gambar dibuat —
        // bukan saat perintahnya direkam.
        if (clearable) {
            imageInfo.usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        }
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo allocation{};
        allocation.usage = VMA_MEMORY_USAGE_AUTO;
        if (vmaCreateImage(device_->Allocator(), &imageInfo, &allocation, &target.image,
                           &target.allocation, nullptr) != VK_SUCCESS) {
            target.image = VK_NULL_HANDLE;
            return false;
        }
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = target.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = mips;
        viewInfo.subresourceRange.layerCount = 1;
        return vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &target.view) ==
               VK_SUCCESS;
    };

    if (!make(scene_, kSceneFormat, allocatedWidth_, allocatedHeight_, 1)) {
        SIM_ERROR("Render", "cannot allocate the HDR scene colour target");
        return false;
    }
    sceneImage_ = scene_.image;
    sceneView_ = scene_.view;

    luminanceLevels_Count = 0;
    for (uint32_t size = kSeedSize; size >= 1; size /= 2) {
        ++luminanceLevels_Count;
        if (size == 1) {
            break;
        }
    }
    if (!make(luminance_, kLuminanceFormat, kSeedSize, kSeedSize, luminanceLevels_Count)) {
        SIM_ERROR("Render", "cannot allocate the luminance chain");
        return false;
    }
    luminanceLevels_.resize(luminanceLevels_Count);
    for (uint32_t level = 0; level < luminanceLevels_Count; ++level) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = luminance_.image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = kLuminanceFormat;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.baseMipLevel = level;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &luminanceLevels_[level]) !=
            VK_SUCCESS) {
            return false;
        }
    }

    for (Image& image : exposure_) {
        if (!make(image, kExposureFormat, 1, 1, 1, /*clearable=*/true)) {
            SIM_ERROR("Render", "cannot allocate the exposure texels");
            return false;
        }
    }

    // Rantai bloom bermula di setengah resolusi: pendaran adalah tapis
    // berjangkauan lebar, dan resolusi penuhnya tidak menyumbang apa pun yang
    // bisa dibedakan sesudah beberapa tingkat penurunan.
    bloomWidth_ = HalfCeil(allocatedWidth_);
    bloomHeight_ = HalfCeil(allocatedHeight_);
    bloomLevels_ = BloomChain::LevelsFor(bloomWidth_, bloomHeight_);
    if (!make(bloomDown_, kSceneFormat, bloomWidth_, bloomHeight_, bloomLevels_) ||
        !make(bloomUp_, kSceneFormat, bloomWidth_, bloomHeight_, bloomLevels_)) {
        SIM_ERROR("Render", "cannot allocate the bloom chain");
        return false;
    }
    bloomDownLevels_.resize(bloomLevels_);
    bloomUpLevels_.resize(bloomLevels_);
    for (uint32_t level = 0; level < bloomLevels_; ++level) {
        for (int which = 0; which < 2; ++which) {
            VkImageViewCreateInfo viewInfo{};
            viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            viewInfo.image = which == 0 ? bloomDown_.image : bloomUp_.image;
            viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
            viewInfo.format = kSceneFormat;
            viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            viewInfo.subresourceRange.baseMipLevel = level;
            viewInfo.subresourceRange.levelCount = 1;
            viewInfo.subresourceRange.layerCount = 1;
            VkImageView& target = which == 0 ? bloomDownLevels_[level] : bloomUpLevels_[level];
            if (vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &target) != VK_SUCCESS) {
                return false;
            }
        }
    }

    WriteSets();
    return true;
}

void PostProcess::WriteSets() {
    const uint32_t reduceCount = luminanceLevels_Count > 0 ? luminanceLevels_Count - 1 : 0;
    std::vector<VkDescriptorSetLayout> layouts(1 + reduceCount, oneSourceLayout_);
    std::vector<VkDescriptorSet> sets(layouts.size());
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = pool_;
    allocateInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocateInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_->Handle(), &allocateInfo, sets.data()) != VK_SUCCESS) {
        SIM_ERROR("Render", "cannot allocate post-process descriptor sets");
        return;
    }
    seedSet_ = sets[0];
    reduceSets_.assign(sets.begin() + 1, sets.end());

    // Satu set penurunan bloom per tingkat, ditambah set petak awal di atas.
    std::vector<VkDescriptorSetLayout> downLayouts(bloomLevels_, oneSourceLayout_);
    bloomDownSets_.resize(bloomLevels_);
    allocateInfo.descriptorSetCount = bloomLevels_;
    allocateInfo.pSetLayouts = downLayouts.data();
    if (bloomLevels_ > 0 &&
        vkAllocateDescriptorSets(device_->Handle(), &allocateInfo, bloomDownSets_.data()) !=
            VK_SUCCESS) {
        SIM_ERROR("Render", "cannot allocate bloom descriptor sets");
        bloomDownSets_.clear();
        return;
    }

    const uint32_t upCount = bloomLevels_ > 1 ? bloomLevels_ - 1 : 0;
    std::vector<VkDescriptorSetLayout> upLayouts(upCount, twoSourceLayout_);
    bloomUpSets_.resize(upCount);
    if (upCount > 0) {
        allocateInfo.descriptorSetCount = upCount;
        allocateInfo.pSetLayouts = upLayouts.data();
        if (vkAllocateDescriptorSets(device_->Handle(), &allocateInfo, bloomUpSets_.data()) !=
            VK_SUCCESS) {
            SIM_ERROR("Render", "cannot allocate bloom descriptor sets");
            bloomUpSets_.clear();
            return;
        }
    }

    const std::array<VkDescriptorSetLayout, 2> exposureLayouts{twoSourceLayout_,
                                                               twoSourceLayout_};
    allocateInfo.descriptorSetCount = 2;
    allocateInfo.pSetLayouts = exposureLayouts.data();
    if (vkAllocateDescriptorSets(device_->Handle(), &allocateInfo, exposureSets_.data()) !=
        VK_SUCCESS) {
        SIM_ERROR("Render", "cannot allocate post-process descriptor sets");
        return;
    }
    const std::array<VkDescriptorSetLayout, 2> resolveLayouts{threeSourceLayout_,
                                                              threeSourceLayout_};
    allocateInfo.pSetLayouts = resolveLayouts.data();
    if (vkAllocateDescriptorSets(device_->Handle(), &allocateInfo, resolveSets_.data()) !=
        VK_SUCCESS) {
        SIM_ERROR("Render", "cannot allocate post-process descriptor sets");
        return;
    }

    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkWriteDescriptorSet> writes;
    images.reserve(64 + reduceCount + bloomLevels_ * 3);
    const auto push = [&](VkDescriptorSet set, uint32_t binding, VkSampler sampler,
                          VkImageView view) {
        images.push_back({sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes.push_back(write);
    };

    // Petak awal membaca gambar HDR dengan penyaringan bilinear: cuplikannya
    // jatuh di antara texel, dan itu memang yang diinginkan.
    push(seedSet_, 0, sampler_, sceneView_);
    for (uint32_t level = 0; level < reduceCount; ++level) {
        push(reduceSets_[level], 0, pointSampler_, luminanceLevels_[level]);
    }
    for (uint32_t i = 0; i < 2; ++i) {
        push(exposureSets_[i], 0, pointSampler_, luminanceLevels_[luminanceLevels_Count - 1]);
        push(exposureSets_[i], 1, pointSampler_, exposure_[1 - i].view);
        push(resolveSets_[i], 0, pointSampler_, sceneView_);
        push(resolveSets_[i], 1, pointSampler_, exposure_[i].view);
        // Bloom disampel bilinear: hasil akhirnya setengah resolusi, dan
        // membacanya per texel akan memunculkan kembali tepi tangga yang justru
        // dihaluskan seluruh rantai ini.
        push(resolveSets_[i], 2, sampler_, bloomUpLevels_[0]);
    }
    // Tingkat nol penurunan membaca gambar HDR; sisanya membaca tingkat di atasnya.
    for (uint32_t level = 0; level < bloomLevels_; ++level) {
        push(bloomDownSets_[level], 0, sampler_,
             level == 0 ? sceneView_ : bloomDownLevels_[level - 1]);
    }
    // Penaikan: sumbernya tingkat yang lebih kecil — untuk tingkat terkecil ia
    // masih ada di rantai penurunan — dan sasarannya rantai penurunan tingkat ini.
    for (uint32_t level = 0; level + 1 < bloomLevels_; ++level) {
        const bool fromDown = level + 2 == bloomLevels_;
        push(bloomUpSets_[level], 0, sampler_,
             fromDown ? bloomDownLevels_[level + 1] : bloomUpLevels_[level + 1]);
        push(bloomUpSets_[level], 1, pointSampler_, bloomDownLevels_[level]);
    }
    for (std::size_t i = 0; i < writes.size(); ++i) {
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_->Handle(), static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);
}

void PostProcess::AdoptLayouts() {
    if (!IsValid()) {
        return;
    }
    VkCommandBuffer cmd = device_->BeginOneShot();

    const auto barrier = [&](VkImage image, VkImageLayout from, VkImageLayout to,
                             VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess,
                             VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage) {
        VkImageMemoryBarrier2 info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        info.srcStageMask = srcStage;
        info.srcAccessMask = srcAccess;
        info.dstStageMask = dstStage;
        info.dstAccessMask = dstAccess;
        info.oldLayout = from;
        info.newLayout = to;
        info.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        info.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        info.image = image;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        info.subresourceRange.layerCount = 1;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &info;
        vkCmdPipelineBarrier2(cmd, &dependency);
    };

    barrier(scene_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_2_NONE, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    barrier(luminance_.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_2_NONE, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    for (Image* image : {&bloomDown_, &bloomUp_}) {
        barrier(image->image, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_NONE,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    }

    // Kedua texel eksposur dinolkan. Nol berarti "belum ada riwayat", dan itu
    // satu-satunya cara pass eksposur tahu bahwa frame ini harus mendarat di
    // sasarannya. Memori yang belum ditulis bisa berisi apa saja — termasuk
    // angka yang tampak masuk akal, yang lalu bertahan lewat riwayatnya sendiri.
    for (Image& image : exposure_) {
        barrier(image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_2_NONE, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT);
        const VkClearColorValue clear{{0.0f, 0.0f, 0.0f, 0.0f}};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1,
                             &range);
        barrier(image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    }
    device_->EndOneShot(cmd);
    exposureIndex_ = 0;
}

void PostProcess::Transition(VkCommandBuffer cmd, VkImage image, uint32_t baseMip,
                             VkImageLayout from, VkImageLayout to, bool toAttachment) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    if (toAttachment) {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    } else {
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    }
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = baseMip;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void PostProcess::DrawInto(VkCommandBuffer cmd, VkImageView target, uint32_t width,
                           uint32_t height, VkPipeline pipeline, VkPipelineLayout layout,
                           VkDescriptorSet set, const void* push, uint32_t pushSize) {
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = target;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea = {{0, 0}, {width, height}};
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &rendering);

    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height),
                              0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize, push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void PostProcess::RecordBloom(VkCommandBuffer cmd, uint32_t viewportWidth,
                              uint32_t viewportHeight, const BloomSettings& settings) {
    if (!IsValid() || bloomLevels_ == 0 || viewportWidth == 0 || viewportHeight == 0) {
        return;
    }

    // Ukuran terpakai dan ukuran alokasi setiap tingkat. Keduanya dibutuhkan:
    // yang pertama menentukan seberapa luas digambar, yang kedua menentukan uv.
    // **Menyampurnya berarti tapisnya membaca sisa frame sebelumnya di luar
    // petak yang digambar, dan sisa itu lalu diburamkan ke dalam gambar** — tepi
    // viewport menjadi berhalo, dan halonya bergerak saat jendela diubah ukuran.
    std::vector<glm::uvec2> used(bloomLevels_);
    std::vector<glm::uvec2> allocated(bloomLevels_);
    uint32_t uw = HalfCeil(viewportWidth);
    uint32_t uh = HalfCeil(viewportHeight);
    uint32_t aw = bloomWidth_;
    uint32_t ah = bloomHeight_;
    for (uint32_t level = 0; level < bloomLevels_; ++level) {
        used[level] = {uw, uh};
        allocated[level] = {aw, ah};
        uw = HalfCeil(uw);
        uh = HalfCeil(uh);
        aw = std::max(aw / 2u, 1u);
        ah = std::max(ah / 2u, 1u);
    }

    // Pecahan alokasi yang benar-benar terisi di sebuah tingkat. **Sebuah
    // skala, bukan batas:** segitiga penutup layar menghasilkan uv 0..1 atas
    // petak yang digambar, dan petak itu hanya sebagian dari alokasi sumbernya.
    const auto uvScale = [&](uint32_t level) {
        return glm::vec2(
            static_cast<float>(used[level].x) / static_cast<float>(allocated[level].x),
            static_cast<float>(used[level].y) / static_cast<float>(allocated[level].y));
    };

    // --- Turun ---
    for (uint32_t level = 0; level < bloomLevels_; ++level) {
        BloomPush push;
        if (level == 0) {
            push.sourceTexelX = 1.0f / static_cast<float>(allocatedWidth_);
            push.sourceTexelY = 1.0f / static_cast<float>(allocatedHeight_);
            push.sourceUvScaleX =
                static_cast<float>(viewportWidth) / static_cast<float>(allocatedWidth_);
            push.sourceUvScaleY =
                static_cast<float>(viewportHeight) / static_cast<float>(allocatedHeight_);
            push.params = Vec4(1.0f, settings.threshold, settings.knee, 0.0f);
        } else {
            push.sourceTexelX = 1.0f / static_cast<float>(allocated[level - 1].x);
            push.sourceTexelY = 1.0f / static_cast<float>(allocated[level - 1].y);
            const glm::vec2 scale = uvScale(level - 1);
            push.sourceUvScaleX = scale.x;
            push.sourceUvScaleY = scale.y;
            push.params = Vec4(0.0f);
        }
        Transition(cmd, bloomDown_.image, level, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
        DrawInto(cmd, bloomDownLevels_[level], used[level].x, used[level].y, bloomDownPipeline_,
                 bloomDownLayout_, bloomDownSets_[level], &push, sizeof(push));
        Transition(cmd, bloomDown_.image, level, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
    }

    // --- Naik ---
    for (uint32_t level = bloomLevels_ - 1; level-- > 0;) {
        BloomPush push;
        push.sourceTexelX = 1.0f / static_cast<float>(allocated[level + 1].x);
        push.sourceTexelY = 1.0f / static_cast<float>(allocated[level + 1].y);
        const glm::vec2 scale = uvScale(level + 1);
        push.sourceUvScaleX = scale.x;
        push.sourceUvScaleY = scale.y;
        push.params = Vec4(settings.scatter, 0.0f, 0.0f, 0.0f);
        Transition(cmd, bloomUp_.image, level, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
        DrawInto(cmd, bloomUpLevels_[level], used[level].x, used[level].y, bloomUpPipeline_,
                 bloomUpLayout_, bloomUpSets_[level], &push, sizeof(push));
        Transition(cmd, bloomUp_.image, level, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
    }
}

void PostProcess::RecordMeter(VkCommandBuffer cmd, uint32_t viewportWidth,
                              uint32_t viewportHeight, const PostProcessSettings& settings,
                              float deltaSeconds) {
    if (!IsValid() || viewportWidth == 0 || viewportHeight == 0) {
        return;
    }

    // Petak awal.
    Transition(cmd, luminance_.image, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
    SeedPush seed;
    seed.viewportUvMaxX = static_cast<float>(viewportWidth) / static_cast<float>(allocatedWidth_);
    seed.viewportUvMaxY =
        static_cast<float>(viewportHeight) / static_cast<float>(allocatedHeight_);
    seed.seedWidth = static_cast<int32_t>(kSeedSize);
    seed.seedHeight = static_cast<int32_t>(kSeedSize);
    DrawInto(cmd, luminanceLevels_[0], kSeedSize, kSeedSize, seedPipeline_, seedLayout_, seedSet_,
             &seed, sizeof(seed));
    Transition(cmd, luminance_.image, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);

    // Rantai reduksi. Barrier per tingkat, bukan satu di akhir: yang dibaca dan
    // yang ditulis adalah gambar yang sama.
    uint32_t sourceSize = kSeedSize;
    for (uint32_t level = 1; level < luminanceLevels_Count; ++level) {
        const uint32_t levelSize = std::max(sourceSize / 2u, 1u);
        Transition(cmd, luminance_.image, level, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
        ReducePush push;
        push.sourceWidth = static_cast<int32_t>(sourceSize);
        push.sourceHeight = static_cast<int32_t>(sourceSize);
        DrawInto(cmd, luminanceLevels_[level], levelSize, levelSize, reducePipeline_,
                 reduceLayout_, reduceSets_[level - 1], &push, sizeof(push));
        Transition(cmd, luminance_.image, level, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
        sourceSize = levelSize;
    }

    // Eksposur, ke texel yang bukan sedang dibaca.
    exposureIndex_ = 1 - exposureIndex_;
    ExposurePush push;
    push.params = Vec4(settings.exposureMode == ExposureMode::Manual ? 1.0f : 0.0f,
                       settings.manualEv100, settings.exposureCompensation,
                       std::max(deltaSeconds, 0.0f));
    push.adaptation = Vec4(settings.adaptationBrightenSeconds, settings.adaptationDarkenSeconds,
                           0.0f, 0.0f);
    Transition(cmd, exposure_[exposureIndex_].image, 0, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
    DrawInto(cmd, exposure_[exposureIndex_].view, 1, 1, exposurePipeline_, exposureLayout_,
             exposureSets_[exposureIndex_], &push, sizeof(push));
    Transition(cmd, exposure_[exposureIndex_].image, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
}

namespace {

/// float16 → float32. **Ditulis di sini, bukan dipinjam dari pustaka:** yang
/// dibutuhkan cuma satu arah konversi, dan menariknya dari mana pun berarti
/// menambah ketergantungan untuk dua belas baris.
float HalfToFloat(uint16_t bits) {
    const uint32_t sign = static_cast<uint32_t>(bits >> 15) << 31;
    uint32_t exponent = (bits >> 10) & 0x1Fu;
    uint32_t mantissa = bits & 0x3FFu;
    if (exponent == 0) {
        if (mantissa == 0) {
            const uint32_t zero = sign;
            float out = 0.0f;
            std::memcpy(&out, &zero, sizeof(out));
            return out;
        }
        // Subnormal: dinormalkan dengan menggeser sampai bit implisitnya muncul.
        while ((mantissa & 0x400u) == 0) {
            mantissa <<= 1;
            --exponent;
        }
        ++exponent;
        mantissa &= 0x3FFu;
    } else if (exponent == 0x1Fu) {
        const uint32_t special = sign | 0x7F800000u | (mantissa << 13);
        float out = 0.0f;
        std::memcpy(&out, &special, sizeof(out));
        return out;
    }
    const uint32_t bits32 = sign | ((exponent + 112u) << 23) | (mantissa << 13);
    float out = 0.0f;
    std::memcpy(&out, &bits32, sizeof(out));
    return out;
}

}  // namespace

bool PostProcess::ReadScene(std::vector<float>& outRgba, uint32_t width, uint32_t height,
                            VkImageLayout currentLayout, std::string& error) {
    if (!IsValid() || width == 0 || height == 0) {
        error = "the scene image has nothing in it yet";
        return false;
    }
    if (width > allocatedWidth_ || height > allocatedHeight_) {
        error = "the requested area is larger than the scene image";
        return false;
    }
    // **`UNDEFINED` berarti isinya memang tidak dijanjikan siapa pun.**
    // Menyalinnya tetap berhasil dan mengembalikan nol di mana-mana — sebuah
    // gambar yang meyakinkan dan salah. Menolak lebih baik daripada menjawab.
    if (currentLayout == VK_IMAGE_LAYOUT_UNDEFINED) {
        error = "the frame graph has not left the scene image in a readable layout";
        return false;
    }

    // Empat kanal float16. Baris di staging rapat karena `imageExtent` yang
    // diminta persis area terpakai, bukan seluruh alokasi.
    const VkDeviceSize bytes = VkDeviceSize(width) * height * 4 * sizeof(uint16_t);
    rhi::DynamicBuffer staging;
    if (!staging.Create(*device_, VK_BUFFER_USAGE_TRANSFER_DST_BIT, bytes)) {
        error = "cannot allocate a staging buffer for the HDR capture";
        return false;
    }
    device_->WaitIdle();

    const auto barrier = [this](VkCommandBuffer cmd, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier2 memory{};
        memory.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        memory.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        memory.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        memory.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        memory.oldLayout = from;
        memory.newLayout = to;
        memory.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memory.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        memory.image = sceneImage_;
        memory.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &memory;
        vkCmdPipelineBarrier2(cmd, &dependency);
    };

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};

    VkCommandBuffer cmd = device_->BeginOneShot();
    barrier(cmd, currentLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkCmdCopyImageToBuffer(cmd, sceneImage_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging.Handle(), 1, &region);
    // Dikembalikan ke tata letak semula: yang melacaknya graph frame, dan
    // membiarkannya di TRANSFER_SRC adalah pelanggaran pada draw berikutnya.
    barrier(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, currentLayout);
    device_->EndOneShot(cmd);

    const auto* source = static_cast<const uint16_t*>(staging.Mapped());
    if (source == nullptr) {
        error = "the staging buffer is not mapped";
        return false;
    }
    const std::size_t count = static_cast<std::size_t>(width) * height * 4;
    outRgba.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        outRgba[i] = HalfToFloat(source[i]);
    }
    return true;
}

void PostProcess::RecordResolve(VkCommandBuffer cmd, bool enabled,
                               const BloomSettings& bloom) {
    if (!IsValid()) {
        return;
    }
    ResolvePush push;
    // Kekuatan bloom nol saat ia mati — bukan cabang di shader. Rantai bloom
    // tetap dibaca descriptor-nya, dan layout-nya tetap harus sah walaupun
    // hasilnya tidak dipakai.
    const float strength = bloom.enabled ? std::max(bloom.strength, 0.0f) : 0.0f;
    // zw memetakan koordinat piksel target tampilan ke uv gambar bloom. Gambar
    // bloom setengah resolusi **alokasi**, bukan setengah viewport, jadi
    // pembaginya ukuran alokasi — memakai ukuran viewport akan meregangkan
    // pendaran setiap kali jendela tidak tepat sebesar alokasinya.
    push.params = Vec4(enabled ? 1.0f : 0.0f, strength,
                       0.5f / static_cast<float>(bloomWidth_),
                       0.5f / static_cast<float>(bloomHeight_));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolvePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolveLayout_, 0, 1,
                            &resolveSets_[exposureIndex_], 0, nullptr);
    vkCmdPushConstants(cmd, resolveLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void PostProcess::DestroyImages() {
    if (device_ == nullptr) {
        return;
    }
    if (seedSet_ != VK_NULL_HANDLE) {
        std::vector<VkDescriptorSet> all{seedSet_};
        all.insert(all.end(), reduceSets_.begin(), reduceSets_.end());
        all.insert(all.end(), exposureSets_.begin(), exposureSets_.end());
        all.insert(all.end(), resolveSets_.begin(), resolveSets_.end());
        all.insert(all.end(), bloomDownSets_.begin(), bloomDownSets_.end());
        all.insert(all.end(), bloomUpSets_.begin(), bloomUpSets_.end());
        vkFreeDescriptorSets(device_->Handle(), pool_, static_cast<uint32_t>(all.size()),
                             all.data());
        seedSet_ = VK_NULL_HANDLE;
        reduceSets_.clear();
        exposureSets_ = {};
        resolveSets_ = {};
    }
    for (VkImageView& view : luminanceLevels_) {
        if (view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_->Handle(), view, nullptr);
        }
    }
    luminanceLevels_.clear();
    luminanceLevels_Count = 0;
    for (std::vector<VkImageView>* levels : {&bloomDownLevels_, &bloomUpLevels_}) {
        for (VkImageView& view : *levels) {
            if (view != VK_NULL_HANDLE) {
                vkDestroyImageView(device_->Handle(), view, nullptr);
            }
        }
        levels->clear();
    }
    bloomDownSets_.clear();
    bloomUpSets_.clear();
    bloomLevels_ = 0;

    const auto drop = [&](Image& image) {
        if (image.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_->Handle(), image.view, nullptr);
            image.view = VK_NULL_HANDLE;
        }
        if (image.image != VK_NULL_HANDLE) {
            vmaDestroyImage(device_->Allocator(), image.image, image.allocation);
            image.image = VK_NULL_HANDLE;
            image.allocation = VK_NULL_HANDLE;
        }
    };
    drop(scene_);
    drop(luminance_);
    drop(bloomDown_);
    drop(bloomUp_);
    for (Image& image : exposure_) {
        drop(image);
    }
    sceneImage_ = VK_NULL_HANDLE;
    sceneView_ = VK_NULL_HANDLE;
    allocatedWidth_ = 0;
    allocatedHeight_ = 0;
}

void PostProcess::Destroy() {
    if (device_ == nullptr) {
        return;
    }
    DestroyImages();
    const auto dropPipeline = [&](VkPipeline& pipeline, VkPipelineLayout& layout) {
        if (pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_->Handle(), pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
        if (layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_->Handle(), layout, nullptr);
            layout = VK_NULL_HANDLE;
        }
    };
    dropPipeline(seedPipeline_, seedLayout_);
    dropPipeline(reducePipeline_, reduceLayout_);
    dropPipeline(exposurePipeline_, exposureLayout_);
    dropPipeline(bloomDownPipeline_, bloomDownLayout_);
    dropPipeline(bloomUpPipeline_, bloomUpLayout_);
    dropPipeline(resolvePipeline_, resolveLayout_);
    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_->Handle(), pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout* layout :
         {&oneSourceLayout_, &twoSourceLayout_, &threeSourceLayout_}) {
        if (*layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_->Handle(), *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_->Handle(), sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (pointSampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_->Handle(), pointSampler_, nullptr);
        pointSampler_ = VK_NULL_HANDLE;
    }
    for (VkShaderModule* module : {&vertex_, &vertexUv_}) {
        if (*module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_->Handle(), *module, nullptr);
            *module = VK_NULL_HANDLE;
        }
    }
    device_ = nullptr;
}

}  // namespace sim::render
