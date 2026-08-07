#include "SkyAtmosphere.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <fstream>
#include <vector>

namespace sim::render {
namespace {

/// **16 bit float, bukan 8 bit.** Transmitansi membentang dari satu sampai
/// hampir nol dalam beberapa derajat di dekat horizon, dan langit siang hari
/// jauh melewati satu. Delapan bit menjadikan matahari terbenam sebagai
/// pita-pita, bukan gradasi.
constexpr VkFormat kLutFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

/// Ambang gerak matahari sebelum LUT multiscattering dibangun ulang, radian.
/// Setengah derajat: pergeseran di bawah itu tidak menghasilkan perbedaan yang
/// bisa dibedakan pada LUT 32×32.
constexpr float kSunRebuildCosine = 0.99996f;  // cos(0,5°)

struct TransmittancePush {
    float width = 0.0f;
    float height = 0.0f;
};

struct MultiscatterPush {
    float width = 0.0f;
    float height = 0.0f;
    float transmittanceWidth = 0.0f;
    float transmittanceHeight = 0.0f;
};

struct SkyViewPush {
    float width = 0.0f;
    float height = 0.0f;
    float multiscatterWidth = 0.0f;
    float multiscatterHeight = 0.0f;
    Vec4 sun{0.0f};
};

struct DrawPush {
    Mat4 invViewProj{1.0f};
    Vec4 camera{0.0f};
    Vec4 sun{0.0f};
    Vec4 params{0.0f};
    Vec4 sunRadiance{0.0f};
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

bool SkyAtmosphere::CreateImage(Image& target, uint32_t width, uint32_t height) {
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = kLutFormat;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
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
    viewInfo.format = kLutFormat;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    return vkCreateImageView(device_->Handle(), &viewInfo, nullptr, &target.view) == VK_SUCCESS;
}

bool SkyAtmosphere::CreatePipeline(const std::filesystem::path& shaderDirectory,
                                   const char* fragmentName, VkDescriptorSetLayout setLayout,
                                   uint32_t pushSize, VkFormat format, VkShaderModule vertex,
                                   VkPipelineLayout& outLayout, VkPipeline& outPipeline) {
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = pushSize;
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = setLayout == VK_NULL_HANDLE ? 0u : 1u;
    layoutInfo.pSetLayouts = setLayout == VK_NULL_HANDLE ? nullptr : &setLayout;
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

    VkFormat colorFormat = format;
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
    info.layout = outLayout;

    const VkResult result = vkCreateGraphicsPipelines(device_->Handle(), VK_NULL_HANDLE, 1, &info,
                                                      nullptr, &outPipeline);
    vkDestroyShaderModule(device_->Handle(), fragment, nullptr);
    if (result != VK_SUCCESS) {
        SIM_ERROR("Render", "cannot create sky pipeline {}", fragmentName);
        outPipeline = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool SkyAtmosphere::Create(rhi::Device& device, const std::filesystem::path& shaderDirectory,
                           VkFormat sceneFormat) {
    Destroy();
    device_ = &device;

    vertex_ = LoadModule(device_->Handle(), shaderDirectory / "fullscreen.vert.spv");
    vertexUv_ = LoadModule(device_->Handle(), shaderDirectory / "fullscreen_uv.vert.spv");
    if (vertex_ == VK_NULL_HANDLE || vertexUv_ == VK_NULL_HANDLE) {
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
    if (vkCreateSampler(device_->Handle(), &samplerInfo, nullptr, &sampler_) != VK_SUCCESS) {
        return false;
    }

    if (!CreateImage(transmittance_, kTransmittanceWidth, kTransmittanceHeight) ||
        !CreateImage(multiscatter_, kMultiscatterSize, kMultiscatterSize) ||
        !CreateImage(skyView_, kSkyViewWidth, kSkyViewHeight)) {
        SIM_ERROR("Render", "cannot allocate the sky LUTs");
        return false;
    }

    // Tiga tata letak: satu sumber (multiscatter), dua sumber (sky-view dan
    // gambar).
    for (uint32_t index = 0; index < setLayouts_.size(); ++index) {
        const uint32_t count = index == 0 ? 1u : 2u;
        std::vector<VkDescriptorSetLayoutBinding> bindings(count);
        for (uint32_t i = 0; i < count; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        info.bindingCount = count;
        info.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device_->Handle(), &info, nullptr, &setLayouts_[index]) !=
            VK_SUCCESS) {
            return false;
        }
    }

    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 3;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device_->Handle(), &poolInfo, nullptr, &pool_) != VK_SUCCESS) {
        return false;
    }

    const std::array<VkDescriptorSetLayout, 3> layouts{setLayouts_[0], setLayouts_[1],
                                                       setLayouts_[2]};
    std::array<VkDescriptorSet, 3> sets{};
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = pool_;
    allocateInfo.descriptorSetCount = 3;
    allocateInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_->Handle(), &allocateInfo, sets.data()) != VK_SUCCESS) {
        SIM_ERROR("Render", "cannot allocate sky descriptor sets");
        return false;
    }
    multiscatterSet_ = sets[0];
    skyViewSet_ = sets[1];
    drawSet_ = sets[2];

    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkWriteDescriptorSet> writes;
    const auto bind = [&](VkDescriptorSet set, uint32_t binding, VkImageView view) {
        images.push_back({sampler_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = set;
        write.dstBinding = binding;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes.push_back(write);
    };
    bind(multiscatterSet_, 0, transmittance_.view);
    bind(skyViewSet_, 0, transmittance_.view);
    bind(skyViewSet_, 1, multiscatter_.view);
    bind(drawSet_, 0, skyView_.view);
    bind(drawSet_, 1, transmittance_.view);
    for (std::size_t i = 0; i < writes.size(); ++i) {
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(device_->Handle(), static_cast<uint32_t>(writes.size()), writes.data(),
                           0, nullptr);

    if (!CreatePipeline(shaderDirectory, "sky_transmittance.frag.spv", VK_NULL_HANDLE,
                        sizeof(TransmittancePush), kLutFormat, vertex_, transmittanceLayout_,
                        transmittancePipeline_) ||
        !CreatePipeline(shaderDirectory, "sky_multiscatter.frag.spv", setLayouts_[0],
                        sizeof(MultiscatterPush), kLutFormat, vertex_, multiscatterLayout_,
                        multiscatterPipeline_) ||
        !CreatePipeline(shaderDirectory, "sky_view.frag.spv", setLayouts_[1],
                        sizeof(SkyViewPush), kLutFormat, vertex_, skyViewLayout_,
                        skyViewPipeline_) ||
        !CreatePipeline(shaderDirectory, "sky_draw.frag.spv", setLayouts_[2], sizeof(DrawPush),
                        sceneFormat, vertexUv_, drawLayout_, drawPipeline_)) {
        return false;
    }
    return true;
}

void SkyAtmosphere::AdoptLayouts() {
    if (!IsValid()) {
        return;
    }
    VkCommandBuffer cmd = device_->BeginOneShot();
    for (const Image* image : {&transmittance_, &multiscatter_, &skyView_}) {
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
        barrier.image = image->image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }
    device_->EndOneShot(cmd);
    transmittanceReady_ = false;
    hasLastSun_ = false;
}

void SkyAtmosphere::Transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from,
                               VkImageLayout to, bool toAttachment) {
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
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd, &dependency);
}

void SkyAtmosphere::DrawInto(VkCommandBuffer cmd, const Image& target, uint32_t width,
                             uint32_t height, VkPipeline pipeline, VkPipelineLayout layout,
                             VkDescriptorSet set, const void* push, uint32_t pushSize) {
    VkRenderingAttachmentInfo color{};
    color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    color.imageView = target.view;
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
    if (set != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0,
                                nullptr);
    }
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pushSize, push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

void SkyAtmosphere::RecordLuts(VkCommandBuffer cmd, const Vec3& sunDirection,
                               float cameraHeightKm) {
    if (!IsValid()) {
        return;
    }
    const Vec3 sun = glm::length(sunDirection) > 1e-6f ? glm::normalize(sunDirection)
                                                       : Vec3(0.0f, 1.0f, 0.0f);

    if (!transmittanceReady_) {
        TransmittancePush push;
        push.width = static_cast<float>(kTransmittanceWidth);
        push.height = static_cast<float>(kTransmittanceHeight);
        Transition(cmd, transmittance_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
        DrawInto(cmd, transmittance_, kTransmittanceWidth, kTransmittanceHeight,
                 transmittancePipeline_, transmittanceLayout_, VK_NULL_HANDLE, &push,
                 sizeof(push));
        Transition(cmd, transmittance_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
        transmittanceReady_ = true;
        // Multiscattering membaca transmitansi, jadi ia harus ikut dibangun ulang.
        hasLastSun_ = false;
    }

    const bool sunMoved = !hasLastSun_ || glm::dot(sun, lastSunDirection_) < kSunRebuildCosine;
    if (sunMoved) {
        MultiscatterPush push;
        push.width = static_cast<float>(kMultiscatterSize);
        push.height = static_cast<float>(kMultiscatterSize);
        push.transmittanceWidth = static_cast<float>(kTransmittanceWidth);
        push.transmittanceHeight = static_cast<float>(kTransmittanceHeight);
        Transition(cmd, multiscatter_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
        DrawInto(cmd, multiscatter_, kMultiscatterSize, kMultiscatterSize, multiscatterPipeline_,
                 multiscatterLayout_, multiscatterSet_, &push, sizeof(push));
        Transition(cmd, multiscatter_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
        lastSunDirection_ = sun;
        hasLastSun_ = true;
    }

    // Sky-view tiap frame: ia bergantung pada matahari **dan** ketinggian
    // kamera, dan ketinggian berubah setiap kali kamera bergerak.
    SkyViewPush push;
    push.width = static_cast<float>(kSkyViewWidth);
    push.height = static_cast<float>(kSkyViewHeight);
    push.multiscatterWidth = static_cast<float>(kMultiscatterSize);
    push.multiscatterHeight = static_cast<float>(kMultiscatterSize);
    // Kerangka planet: +Z ke atas. Sumbu atas adegan adalah +Y, dan penukarannya
    // dilakukan di satu tempat saja — di sini dan di shader gambar.
    push.sun = Vec4(sun.x, -sun.z, sun.y, std::max(cameraHeightKm, 0.0f));
    Transition(cmd, skyView_.image, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
    DrawInto(cmd, skyView_, kSkyViewWidth, kSkyViewHeight, skyViewPipeline_, skyViewLayout_,
             skyViewSet_, &push, sizeof(push));
    Transition(cmd, skyView_.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false);
}

void SkyAtmosphere::RecordDraw(VkCommandBuffer cmd, const Mat4& invViewProj,
                               const Vec3& cameraPosition, float cameraHeightKm,
                               const Vec3& sunDirection, const Vec3& sunRadiance,
                               float intensity) {
    if (!IsValid()) {
        return;
    }
    const Vec3 sun = glm::length(sunDirection) > 1e-6f ? glm::normalize(sunDirection)
                                                       : Vec3(0.0f, 1.0f, 0.0f);
    DrawPush push;
    push.invViewProj = invViewProj;
    push.camera = Vec4(cameraPosition, std::max(cameraHeightKm, 0.0f));
    push.sun = Vec4(sun, 0.0f);
    push.params = Vec4(static_cast<float>(kSkyViewWidth), static_cast<float>(kSkyViewHeight),
                       intensity, 0.0f);
    // Kosinus jari-jari sudut matahari. Cakramnya digambar di sini, bukan
    // sebagai objek: ia berada di jarak tak hingga, dan objek di jarak tak
    // hingga adalah objek yang setiap sistem culling, bayangan, dan depth harus
    // punya kekecualian untuknya.
    const AtmosphereParameters atmosphere;
    push.sunRadiance = Vec4(sunRadiance, std::cos(atmosphere.sunAngularRadius));

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawLayout_, 0, 1, &drawSet_, 0,
                            nullptr);
    vkCmdPushConstants(cmd, drawLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

void SkyAtmosphere::Destroy() {
    if (device_ == nullptr) {
        return;
    }
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
    dropPipeline(transmittancePipeline_, transmittanceLayout_);
    dropPipeline(multiscatterPipeline_, multiscatterLayout_);
    dropPipeline(skyViewPipeline_, skyViewLayout_);
    dropPipeline(drawPipeline_, drawLayout_);

    if (pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_->Handle(), pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    for (VkDescriptorSetLayout& layout : setLayouts_) {
        if (layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device_->Handle(), layout, nullptr);
            layout = VK_NULL_HANDLE;
        }
    }
    for (Image* image : {&transmittance_, &multiscatter_, &skyView_}) {
        if (image->view != VK_NULL_HANDLE) {
            vkDestroyImageView(device_->Handle(), image->view, nullptr);
            image->view = VK_NULL_HANDLE;
        }
        if (image->image != VK_NULL_HANDLE) {
            vmaDestroyImage(device_->Allocator(), image->image, image->allocation);
            image->image = VK_NULL_HANDLE;
            image->allocation = VK_NULL_HANDLE;
        }
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_->Handle(), sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
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
