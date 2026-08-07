#include "ProbeField.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <vector>

namespace sim::render {
namespace {

constexpr VkFormat kShFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
/// Posisi dunia probe butuh presisi penuh: 16-bit float sudah kehilangan
/// sentimeter pada jarak puluhan meter, dan reproyeksi menolak riwayat justru
/// berdasarkan selisih sentimeter.
constexpr VkFormat kSurfaceFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

/// Harus sama persis dengan blok push_constant di Shaders/gi_probe.frag.slang.
struct ProbePush {
    std::array<uint32_t, 4> grid{};
    std::array<float, 4> params{};
};

/// Harus sama persis dengan blok push_constant di Shaders/gi_filter.frag.slang.
struct FilterPush {
    std::array<int32_t, 4> grid{};
    std::array<float, 4> weights{};
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

/// Keadaan pipeline yang sama untuk kedua pipeline di sini: satu segitiga
/// penutup, tanpa vertex input, tanpa depth, tanpa blending.
struct FullscreenState {
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    VkPipelineViewportStateCreateInfo viewport{};
    VkPipelineRasterizationStateCreateInfo raster{};
    VkPipelineMultisampleStateCreateInfo multisample{};
    VkPipelineDynamicStateCreateInfo dynamic{};
    std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,
                                                VK_DYNAMIC_STATE_SCISSOR};

    FullscreenState() {
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();
    }
};

VkPipelineColorBlendAttachmentState OpaqueAttachment() {
    VkPipelineColorBlendAttachmentState state{};
    state.blendEnable = VK_FALSE;
    state.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    return state;
}

}  // namespace

bool ProbeField::CreateTracePipeline(const std::filesystem::path& shaderDirectory,
                                     VkDescriptorSetLayout frameSetLayout) {
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(ProbePush);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &frameSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device_->Handle(), &layoutInfo, nullptr, &traceLayout_) !=
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

    FullscreenState state;
    // **Blending mati.** M3 memakainya untuk memadu riwayat di tempat; M5
    // memadu di dalam shader karena riwayatnya dibaca dari texel yang lain.
    const std::array<VkPipelineColorBlendAttachmentState, 4> attachments{
        OpaqueAttachment(), OpaqueAttachment(), OpaqueAttachment(), OpaqueAttachment()};
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = static_cast<uint32_t>(attachments.size());
    blend.pAttachments = attachments.data();

    const std::array<VkFormat, 4> formats{kShFormat, kShFormat, kShFormat, kSurfaceFormat};
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = static_cast<uint32_t>(formats.size());
    rendering.pColorAttachmentFormats = formats.data();

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.pNext = &rendering;
    info.stageCount = static_cast<uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &state.vertexInput;
    info.pInputAssemblyState = &state.assembly;
    info.pViewportState = &state.viewport;
    info.pRasterizationState = &state.raster;
    info.pMultisampleState = &state.multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &state.dynamic;
    info.layout = traceLayout_;

    const VkResult result = vkCreateGraphicsPipelines(device_->Handle(), VK_NULL_HANDLE, 1, &info,
                                                      nullptr, &tracePipeline_);
    vkDestroyShaderModule(device_->Handle(), vertex, nullptr);
    vkDestroyShaderModule(device_->Handle(), fragment, nullptr);
    if (result != VK_SUCCESS) {
        SIM_ERROR("Render", "cannot create the probe trace pipeline");
        tracePipeline_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool ProbeField::CreateFilterPipeline(const std::filesystem::path& shaderDirectory) {
    std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
    for (uint32_t i = 0; i < bindings.size(); ++i) {
        bindings[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    }
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_->Handle(), &layoutInfo, nullptr, &filterSetLayout_) !=
        VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device_->Handle(), &poolInfo, nullptr, &filterPool_) !=
        VK_SUCCESS) {
        return false;
    }
    const std::array<VkDescriptorSetLayout, 2> layouts{filterSetLayout_, filterSetLayout_};
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = filterPool_;
    allocateInfo.descriptorSetCount = static_cast<uint32_t>(layouts.size());
    allocateInfo.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device_->Handle(), &allocateInfo, filterSets_.data()) !=
        VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = sizeof(FilterPush);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &filterSetLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(device_->Handle(), &pipelineLayoutInfo, nullptr, &filterLayout_) !=
        VK_SUCCESS) {
        return false;
    }

    VkShaderModule vertex = LoadModule(device_->Handle(), shaderDirectory / "hiz_reduce.vert.spv");
    VkShaderModule fragment =
        LoadModule(device_->Handle(), shaderDirectory / "gi_filter.frag.spv");
    if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) {
        return false;
    }

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex, "main",
                                        nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment,
                                        "main", nullptr}};

    FullscreenState state;
    const std::array<VkPipelineColorBlendAttachmentState, kShChannels> attachments{
        OpaqueAttachment(), OpaqueAttachment(), OpaqueAttachment()};
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = static_cast<uint32_t>(attachments.size());
    blend.pAttachments = attachments.data();

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
    info.pVertexInputState = &state.vertexInput;
    info.pInputAssemblyState = &state.assembly;
    info.pViewportState = &state.viewport;
    info.pRasterizationState = &state.raster;
    info.pMultisampleState = &state.multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &state.dynamic;
    info.layout = filterLayout_;

    const VkResult result = vkCreateGraphicsPipelines(device_->Handle(), VK_NULL_HANDLE, 1, &info,
                                                      nullptr, &filterPipeline_);
    vkDestroyShaderModule(device_->Handle(), vertex, nullptr);
    vkDestroyShaderModule(device_->Handle(), fragment, nullptr);
    if (result != VK_SUCCESS) {
        SIM_ERROR("Render", "cannot create the probe filter pipeline");
        filterPipeline_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

bool ProbeField::Create(rhi::Device& device, const std::filesystem::path& shaderDirectory,
                        VkDescriptorSetLayout frameSetLayout) {
    Destroy();
    device_ = &device;

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
    return CreateTracePipeline(shaderDirectory, frameSetLayout) &&
           CreateFilterPipeline(shaderDirectory);
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
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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

void ProbeField::WriteFilterSets() {
    // Set 0 membaca `sh_`, set 1 membaca `scratch_`. Lintasan pertama menulis ke
    // scratch, yang kedua kembali ke `sh_` — jadi hasil akhirnya selalu di
    // `sh_`, dan descriptor yang membacanya tidak pernah berubah.
    std::array<VkDescriptorImageInfo, 8> images{};
    std::array<VkWriteDescriptorSet, 8> writes{};
    uint32_t index = 0;
    for (uint32_t set = 0; set < 2; ++set) {
        const std::array<Attachment, kShChannels>& source = set == 0 ? sh_ : scratch_;
        for (uint32_t channel = 0; channel < kShChannels; ++channel) {
            images[index] = {sampler_, source[channel].view,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            writes[index] = VkWriteDescriptorSet{};
            writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[index].dstSet = filterSets_[set];
            writes[index].dstBinding = channel;
            writes[index].descriptorCount = 1;
            writes[index].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[index].pImageInfo = &images[index];
            ++index;
        }
        images[index] = {sampler_, surface_.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        writes[index] = writes[index - 1];
        writes[index].dstBinding = 3;
        writes[index].pImageInfo = &images[index];
        ++index;
    }
    vkUpdateDescriptorSets(device_->Handle(), index, writes.data(), 0, nullptr);
}

bool ProbeField::Adopt(uint32_t allocatedWidth, uint32_t allocatedHeight,
                       VkFormat normalFormat) {
    if (device_ == nullptr || tracePipeline_ == VK_NULL_HANDLE || allocatedWidth == 0 ||
        allocatedHeight == 0) {
        return false;
    }
    if (normalImage_ != VK_NULL_HANDLE &&
        allocated_ == glm::uvec2(allocatedWidth, allocatedHeight)) {
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

    for (uint32_t channel = 0; channel < kShChannels; ++channel) {
        if (!CreateAttachment(sh_[channel], probeCapacity_, kShFormat) ||
            !CreateAttachment(scratch_[channel], probeCapacity_, kShFormat) ||
            !CreateAttachment(historySh_[channel], probeCapacity_, kShFormat)) {
            SIM_ERROR("Render", "cannot allocate a probe SH target");
            return false;
        }
    }
    if (!CreateAttachment(surface_, probeCapacity_, kSurfaceFormat) ||
        !CreateAttachment(historySurface_, probeCapacity_, kSurfaceFormat)) {
        SIM_ERROR("Render", "cannot allocate the probe surface target");
        return false;
    }

    WriteFilterSets();
    historyUndefined_ = true;
    return true;
}

void ProbeField::Transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from,
                            VkImageLayout to) {
    const bool toWrite = to == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL ||
                         to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.dstAccessMask = toWrite ? VK_ACCESS_2_MEMORY_WRITE_BIT
                                    : VK_ACCESS_2_MEMORY_READ_BIT;
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

void ProbeField::RecordFilterPass(VkCommandBuffer cmd, const glm::uvec2& counts, uint32_t stride,
                                  VkDescriptorSet source,
                                  const std::array<Attachment, kShChannels>& target) {
    std::array<VkRenderingAttachmentInfo, kShChannels> colors{};
    for (uint32_t i = 0; i < kShChannels; ++i) {
        Transition(cmd, target[i].image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colors[i].imageView = target[i].view;
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
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

    FilterPush push;
    push.grid = {static_cast<int32_t>(counts.x), static_cast<int32_t>(counts.y),
                 static_cast<int32_t>(stride), 0};
    push.weights = {0.15f, 0.7f, 0.0f, 0.0f};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, filterPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, filterLayout_, 0, 1, &source, 0,
                            nullptr);
    vkCmdPushConstants(cmd, filterLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    for (uint32_t i = 0; i < kShChannels; ++i) {
        Transition(cmd, target[i].image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

void ProbeField::Record(VkCommandBuffer cmd, VkDescriptorSet frameSet, const ProbeGrid& grid,
                        uint32_t frameIndex, bool resetHistory) {
    if (!IsValid() || grid.ProbeCount() == 0 || tracePipeline_ == VK_NULL_HANDLE) {
        return;
    }
    const glm::uvec2 counts = grid.Counts();

    // Riwayat harus punya layout yang sah sebelum dibaca, dan isinya nol pada
    // frame pertama — `surface.w` nol berarti "tidak ada riwayat di sini", yang
    // persis yang harus dibaca shader saat belum ada apa-apa.
    if (historyUndefined_ || resetHistory) {
        const VkClearColorValue clear{{0.0f, 0.0f, 0.0f, 0.0f}};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (uint32_t i = 0; i < kShChannels; ++i) {
            Transition(cmd, historySh_[i].image, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vkCmdClearColorImage(cmd, historySh_[i].image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            Transition(cmd, historySh_[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        Transition(cmd, historySurface_.image, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cmd, historySurface_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear, 1, &range);
        Transition(cmd, historySurface_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        historyUndefined_ = false;
    }

    // --- Penelusuran dan pemaduan riwayat ---
    std::array<VkRenderingAttachmentInfo, 4> colors{};
    const std::array<VkImage, 4> traceTargets{sh_[0].image, sh_[1].image, sh_[2].image,
                                              surface_.image};
    const std::array<VkImageView, 4> traceViews{sh_[0].view, sh_[1].view, sh_[2].view,
                                                surface_.view};
    for (uint32_t i = 0; i < colors.size(); ++i) {
        Transition(cmd, traceTargets[i], VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colors[i].imageView = traceViews[i];
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
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

    ProbePush push;
    push.grid = {counts.x, counts.y, grid.Settings().tileSize, grid.Settings().raysPerAxis};
    push.params = {static_cast<float>(frameIndex),
                   static_cast<float>(grid.Settings().jitterPeriod), 0.0f, 0.0f};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tracePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, traceLayout_, 0, 1, &frameSet,
                            0, nullptr);
    vkCmdPushConstants(cmd, traceLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);

    for (VkImage image : traceTargets) {
        Transition(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // --- Riwayat frame berikutnya ---
    //
    // Disalin **sebelum** disaring: menyaring lalu menyimpannya sebagai riwayat
    // membuat penyaring memakan keluarannya sendiri frame demi frame, dan
    // hasilnya kabur yang terus melebar tanpa batas.
    const std::array<std::pair<VkImage, VkImage>, 4> copies{
        std::pair{sh_[0].image, historySh_[0].image},
        std::pair{sh_[1].image, historySh_[1].image},
        std::pair{sh_[2].image, historySh_[2].image},
        std::pair{surface_.image, historySurface_.image}};
    for (const auto& [source, destination] : copies) {
        Transition(cmd, source, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(cmd, destination, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.extent = {counts.x, counts.y, 1};
        vkCmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        Transition(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        Transition(cmd, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }

    // --- Dua lintasan à-trous ---
    //
    // Lompatan 1 lalu 2: yang kedua menjangkau dua kali lebih jauh dengan
    // jumlah pengambilan yang sama. Hasil akhirnya kembali di `sh_`, jadi
    // descriptor yang membacanya tidak pernah berubah.
    if (filterPipeline_ != VK_NULL_HANDLE) {
        RecordFilterPass(cmd, counts, 1, filterSets_[0], scratch_);
        RecordFilterPass(cmd, counts, 2, filterSets_[1], sh_);
    }
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
    for (uint32_t channel = 0; channel < kShChannels; ++channel) {
        DestroyAttachment(sh_[channel]);
        DestroyAttachment(scratch_[channel]);
        DestroyAttachment(historySh_[channel]);
    }
    DestroyAttachment(surface_);
    DestroyAttachment(historySurface_);
    allocated_ = {0, 0};
    probeCapacity_ = {0, 0};
    historyUndefined_ = true;
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
    for (VkPipeline* pipeline : {&tracePipeline_, &filterPipeline_}) {
        if (*pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_->Handle(), *pipeline, nullptr);
            *pipeline = VK_NULL_HANDLE;
        }
    }
    for (VkPipelineLayout* layout : {&traceLayout_, &filterLayout_}) {
        if (*layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_->Handle(), *layout, nullptr);
            *layout = VK_NULL_HANDLE;
        }
    }
    if (filterPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_->Handle(), filterPool_, nullptr);
        filterPool_ = VK_NULL_HANDLE;
    }
    if (filterSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_->Handle(), filterSetLayout_, nullptr);
        filterSetLayout_ = VK_NULL_HANDLE;
    }
    device_ = nullptr;
}

void ProbeField::RecordNormalBegin(VkCommandBuffer cmd) {
    if (IsValid()) {
        Transition(cmd, normalImage_, VK_IMAGE_LAYOUT_UNDEFINED,
                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
}

void ProbeField::RecordNormalEnd(VkCommandBuffer cmd) {
    if (IsValid()) {
        Transition(cmd, normalImage_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

}  // namespace sim::render
