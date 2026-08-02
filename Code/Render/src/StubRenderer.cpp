#include "Sim/Render/RendererFactory.h"

#include "Sim/Core/Assert.h"
#include "Sim/Core/Log.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/RenderTarget.h"
#include "Sim/RHI/TextureRegistry.h"

#include <array>
#include <fstream>
#include <vector>

namespace sim::render {
namespace {

/// Harus sama persis dengan blok push_constant di Shaders/grid.{vert,frag}.
struct GridPush {
    Mat4 invViewProj;
    Vec4 cameraPos;
    Vec4 params;  // x = ukuran petak, y = jarak pudar, z = tebal sumbu
};

std::vector<uint32_t> ReadSpirv(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        SIM_ERROR("Render", "Cannot open shader: {}", path.string());
        return {};
    }
    const std::streamsize size = file.tellg();
    if (size <= 0 || size % 4 != 0) {
        SIM_ERROR("Render", "Invalid SPIR-V size ({} bytes): {}", size, path.string());
        return {};
    }
    std::vector<uint32_t> code(static_cast<std::size_t>(size) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);
    return code;
}

VkShaderModule CreateShaderModule(VkDevice device, const std::filesystem::path& path) {
    const std::vector<uint32_t> code = ReadSpirv(path);
    if (code.empty()) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size() * sizeof(uint32_t);
    info.pCode = code.data();

    VkShaderModule module = VK_NULL_HANDLE;
    SIM_VK_CHECK(vkCreateShaderModule(device, &info, nullptr, &module));
    return module;
}

/// Renderer sementara untuk fase editor.
///
/// Yang digambar: warna latar + grid tanah prosedural. Itu saja sudah cukup
/// untuk menjalankan seluruh alur kerja panel di E2..E7 — kamera bisa
/// diorientasikan, skala dunia terbaca, dan panel Viewport punya tekstur nyata
/// untuk ditampilkan. Mesh, gizmo, dan pencahayaan menyusul di E4/E8 lewat
/// antarmuka yang sama.
class StubRenderer final : public IViewportRenderer {
public:
    StubRenderer(rhi::Device& device, rhi::ITextureRegistry& textures)
        : device_(device), textures_(textures) {}

    ~StubRenderer() override { Shutdown(); }

    bool Initialize(const StubRendererDesc& desc) {
        if (!target_.Create(device_, desc.initialWidth, desc.initialHeight)) {
            SIM_ERROR("Render", "Failed to create render target {}x{}", desc.initialWidth,
                      desc.initialHeight);
            return false;
        }
        if (!CreatePipeline(desc.shaderDirectory)) {
            return false;
        }
        RefreshTextureHandle();
        SIM_INFO("Render", "StubRenderer ready ({}x{}, procedural grid)", target_.Width(),
                 target_.Height());
        return true;
    }

    void Resize(uint32_t width, uint32_t height) override {
        if (width == 0 || height == 0) {
            return;
        }
        if (!target_.Resize(width, height)) {
            return;  // ukuran tidak berubah
        }
        RefreshTextureHandle();
    }

    void Render(const ViewportDesc& desc, const ViewportScene& /*scene*/) override {
        if (!target_.IsValid()) {
            return;
        }

        // Command buffer transient: submit sendiri, tapi tanpa menunggu GPU.
        // Urutan eksekusi terhadap submit ImGui berikutnya terjaga karena
        // keduanya di queue yang sama, dan ketergantungan memorinya diurus
        // subpass dependency milik RenderTarget (COLOR_ATTACHMENT_OUTPUT ->
        // FRAGMENT_SHADER). E8 akan menggabungkannya ke command buffer frame
        // utama supaya tidak ada submit terpisah sama sekali.
        VkCommandBuffer cmd = device_.BeginTransient();

        const std::array<VkClearValue, 2> clears{
            VkClearValue{.color = {{desc.clearColor.x, desc.clearColor.y, desc.clearColor.z,
                                    desc.clearColor.w}}},
            VkClearValue{.depthStencil = {1.0f, 0}},
        };

        VkRenderPassBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = target_.RenderPass();
        begin.framebuffer = target_.Framebuffer();
        begin.renderArea.extent = {target_.Width(), target_.Height()};
        begin.clearValueCount = static_cast<uint32_t>(clears.size());
        begin.pClearValues = clears.data();
        vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);

        if (desc.showGrid && pipeline_ != VK_NULL_HANDLE) {
            const VkViewport viewport{0.0f,
                                      0.0f,
                                      static_cast<float>(target_.Width()),
                                      static_cast<float>(target_.Height()),
                                      0.0f,
                                      1.0f};
            const VkRect2D scissor{{0, 0}, {target_.Width(), target_.Height()}};
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

            const float aspect =
                static_cast<float>(target_.Width()) / static_cast<float>(target_.Height());
            GridPush push{};
            push.invViewProj =
                glm::inverse(desc.camera.Projection(aspect) * desc.camera.View());
            push.cameraPos = Vec4(desc.camera.position, 1.0f);
            push.params = Vec4(desc.gridCellSize, desc.gridFadeDistance, 1.0f, 0.0f);
            vkCmdPushConstants(cmd, pipelineLayout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(GridPush), &push);

            vkCmdDraw(cmd, 3, 1, 0, 0);  // segitiga penuh layar
        }

        vkCmdEndRenderPass(cmd);
        device_.SubmitTransient(cmd);
    }

    TextureHandle ColorTarget() const override { return textureHandle_; }
    Vec2 ColorTargetUvMax() const override {
        return Vec2(target_.UvMaxU(), target_.UvMaxV());
    }
    uint32_t Width() const override { return target_.Width(); }
    uint32_t Height() const override { return target_.Height(); }
    const char* Name() const override { return "Stub (grid, unlit)"; }

private:
    void RefreshTextureHandle() {
        if (textureHandle_ != kInvalidTexture) {
            textures_.Release(textureHandle_);
        }
        textureHandle_ = textures_.Acquire(target_.ColorView(), target_.Sampler());
    }

    bool CreatePipeline(const std::filesystem::path& shaderDirectory) {
        VkShaderModule vertexModule =
            CreateShaderModule(device_.Handle(), shaderDirectory / "grid.vert.spv");
        VkShaderModule fragmentModule =
            CreateShaderModule(device_.Handle(), shaderDirectory / "grid.frag.spv");
        if (vertexModule == VK_NULL_HANDLE || fragmentModule == VK_NULL_HANDLE) {
            SIM_WARN("Render", "Grid shader unavailable; viewport shows the clear colour only");
            if (vertexModule != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_.Handle(), vertexModule, nullptr);
            }
            if (fragmentModule != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_.Handle(), fragmentModule, nullptr);
            }
            return true;  // tetap jalan, hanya tanpa grid
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.size = sizeof(GridPush);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        SIM_VK_CHECK(
            vkCreatePipelineLayout(device_.Handle(), &layoutInfo, nullptr, &pipelineLayout_));

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentModule;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterization{};
        rasterization.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterization.polygonMode = VK_POLYGON_MODE_FILL;
        rasterization.cullMode = VK_CULL_MODE_NONE;
        rasterization.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterization.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        // Grid tidak menulis depth: ia adalah latar, dan geometri apa pun yang
        // digambar setelahnya (E4) harus bisa menutupinya tanpa uji depth.
        depthStencil.depthTestEnable = VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState blendAttachment{};
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blendAttachment;

        const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,
                                                          VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterization;
        pipelineInfo.pMultisampleState = &multisample;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlend;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = pipelineLayout_;
        pipelineInfo.renderPass = target_.RenderPass();
        pipelineInfo.subpass = 0;

        SIM_VK_CHECK(vkCreateGraphicsPipelines(device_.Handle(), device_.PipelineCache(), 1,
                                               &pipelineInfo, nullptr, &pipeline_));

        vkDestroyShaderModule(device_.Handle(), vertexModule, nullptr);
        vkDestroyShaderModule(device_.Handle(), fragmentModule, nullptr);
        return true;
    }

    void Shutdown() {
        device_.WaitIdle();
        if (textureHandle_ != kInvalidTexture) {
            textures_.Release(textureHandle_);
            textureHandle_ = kInvalidTexture;
        }
        if (pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_.Handle(), pipeline_, nullptr);
            pipeline_ = VK_NULL_HANDLE;
        }
        if (pipelineLayout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_.Handle(), pipelineLayout_, nullptr);
            pipelineLayout_ = VK_NULL_HANDLE;
        }
        target_.Destroy();
    }

    rhi::Device& device_;
    rhi::ITextureRegistry& textures_;
    rhi::RenderTarget target_;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    TextureHandle textureHandle_ = kInvalidTexture;
};

}  // namespace

std::unique_ptr<IViewportRenderer> CreateStubRenderer(rhi::Device& device,
                                                      rhi::ITextureRegistry& textures,
                                                      const StubRendererDesc& desc) {
    auto renderer = std::make_unique<StubRenderer>(device, textures);
    if (!renderer->Initialize(desc)) {
        return nullptr;
    }
    return renderer;
}

}  // namespace sim::render
