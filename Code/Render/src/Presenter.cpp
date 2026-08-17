#include "Sim/Render/Presenter.h"

#include "Sim/Core/Log.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/Swapchain.h"
#include "Sim/Render/IViewportRenderer.h"
#include "PresentSource.h"

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
        SIM_ERROR("Render", "present: cannot read {}", path.string());
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

/// Tata letaknya harus sama persis dengan `Push` di `Shaders/present.frag.slang`.
struct PresentPush {
    float sourceUvScaleU = 1.0f;
    float sourceUvScaleV = 1.0f;
};

}  // namespace

struct Presenter::Impl {
    rhi::Device* device = nullptr;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    /// Satu set per frame in-flight. Satu set bersama akan ditulis ulang
    /// sementara frame sebelumnya masih membacanya — yang terlihat bukan galat
    /// melainkan gambar yang sesekali tertinggal satu frame.
    std::array<VkDescriptorSet, rhi::Swapchain::kFramesInFlight> sets{};
    uint32_t nextSet = 0;
    /// View yang sedang terpasang di tiap set, supaya set yang isinya sudah
    /// benar tidak ditulis ulang setiap frame.
    std::array<VkImageView, rhi::Swapchain::kFramesInFlight> bound{};
};

Presenter::Presenter() : impl_(std::make_unique<Impl>()) {}
Presenter::~Presenter() { Destroy(); }

bool Presenter::Create(rhi::Device& device, rhi::Swapchain& swapchain,
                       const std::filesystem::path& shaderDirectory) {
    Destroy();
    impl_->device = &device;

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = 1;
    setInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(device.Handle(), &setInfo, nullptr, &impl_->setLayout) !=
        VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(PresentPush);

    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &impl_->setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device.Handle(), &layoutInfo, nullptr, &impl_->layout) !=
        VK_SUCCESS) {
        Destroy();
        return false;
    }

    VkShaderModule vertex = LoadModule(device.Handle(), shaderDirectory / "fullscreen_uv.vert.spv");
    VkShaderModule fragment = LoadModule(device.Handle(), shaderDirectory / "present.frag.spv");
    if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) {
        if (vertex != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device.Handle(), vertex, nullptr);
        }
        if (fragment != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device.Handle(), fragment, nullptr);
        }
        Destroy();
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

    // Tanpa blending: ini titik akhir, dan yang di bawahnya adalah gambar
    // swapchain yang baru saja di-clear.
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

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = static_cast<uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = impl_->layout;
    // **Render pass tradisional, bukan dynamic rendering.** Pass lain di modul
    // ini menggambar ke targetnya sendiri dan memakai dynamic rendering; yang
    // ini menggambar ke swapchain, dan swapchain punya render pass sungguhan.
    info.renderPass = swapchain.RenderPass();
    info.subpass = 0;

    const VkResult created = vkCreateGraphicsPipelines(device.Handle(), VK_NULL_HANDLE, 1, &info,
                                                       nullptr, &impl_->pipeline);
    vkDestroyShaderModule(device.Handle(), vertex, nullptr);
    vkDestroyShaderModule(device.Handle(), fragment, nullptr);
    if (created != VK_SUCCESS) {
        Destroy();
        return false;
    }

    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = rhi::Swapchain::kFramesInFlight;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = rhi::Swapchain::kFramesInFlight;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;
    if (vkCreateDescriptorPool(device.Handle(), &poolInfo, nullptr, &impl_->pool) != VK_SUCCESS) {
        Destroy();
        return false;
    }

    const std::array<VkDescriptorSetLayout, rhi::Swapchain::kFramesInFlight> layouts{
        impl_->setLayout, impl_->setLayout};
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = impl_->pool;
    allocate.descriptorSetCount = rhi::Swapchain::kFramesInFlight;
    allocate.pSetLayouts = layouts.data();
    if (vkAllocateDescriptorSets(device.Handle(), &allocate, impl_->sets.data()) != VK_SUCCESS) {
        Destroy();
        return false;
    }
    impl_->bound.fill(VK_NULL_HANDLE);
    return true;
}

void Presenter::Destroy() {
    if (impl_->device == nullptr) {
        return;
    }
    VkDevice device = impl_->device->Handle();
    if (impl_->pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, impl_->pool, nullptr);
        impl_->pool = VK_NULL_HANDLE;
    }
    if (impl_->pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, impl_->pipeline, nullptr);
        impl_->pipeline = VK_NULL_HANDLE;
    }
    if (impl_->layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, impl_->layout, nullptr);
        impl_->layout = VK_NULL_HANDLE;
    }
    if (impl_->setLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, impl_->setLayout, nullptr);
        impl_->setLayout = VK_NULL_HANDLE;
    }
    impl_->sets.fill(VK_NULL_HANDLE);
    impl_->bound.fill(VK_NULL_HANDLE);
    impl_->device = nullptr;
}

bool Presenter::Draw(IViewportRenderer& renderer, rhi::Swapchain& swapchain) {
    if (impl_->pipeline == VK_NULL_HANDLE) {
        return false;
    }
    // Perender yang tidak bisa menyerahkan gambarnya tidak bisa dipresent, dan
    // itu jawaban yang jujur — bukan layar hitam yang terbaca sebagai adegan
    // kosong.
    auto* source = dynamic_cast<IPresentSource*>(&renderer);
    if (source == nullptr) {
        return false;
    }
    const VkImageView view = source->PresentView();
    const VkSampler sampler = source->PresentSampler();
    if (view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE) {
        return false;
    }
    const rhi::Swapchain::Frame& frame = swapchain.CurrentFrame();
    if (frame.commandBuffer == VK_NULL_HANDLE) {
        return false;
    }

    const uint32_t slot = impl_->nextSet;
    impl_->nextSet = (impl_->nextSet + 1) % rhi::Swapchain::kFramesInFlight;
    if (impl_->bound[slot] != view) {
        VkDescriptorImageInfo image{};
        image.sampler = sampler;
        image.imageView = view;
        image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = impl_->sets[slot];
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        vkUpdateDescriptorSets(impl_->device->Handle(), 1, &write, 0, nullptr);
        impl_->bound[slot] = view;
    }

    VkViewport viewport{};
    viewport.width = static_cast<float>(swapchain.Width());
    viewport.height = static_cast<float>(swapchain.Height());
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor.extent = {swapchain.Width(), swapchain.Height()};

    PresentPush push{};
    push.sourceUvScaleU = source->PresentUvMaxU();
    push.sourceUvScaleV = source->PresentUvMaxV();

    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->pipeline);
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, impl_->layout, 0,
                            1, &impl_->sets[slot], 0, nullptr);
    vkCmdPushConstants(frame.commandBuffer, impl_->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);
    return true;
}

}  // namespace sim::render
