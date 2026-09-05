#include "Sim/Render/RendererFactory.h"

#include "Sim/Core/Assert.h"
#include "Sim/Core/Log.h"
#include "Sim/RHI/Buffer.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/RenderTarget.h"
#include "Sim/RHI/TextureRegistry.h"
#include "Sim/Render/Frustum.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <utility>
#include <vector>

namespace sim::render {
namespace {

/// Harus sama persis dengan blok push_constant di Shaders/grid.{vert,frag}.
struct GridPush {
    Mat4 invViewProj;
    Vec4 cameraPos;
    Vec4 params;  // x = ukuran petak, y = jarak pudar, z = tebal sumbu
};

/// Harus sama persis dengan blok push_constant di Shaders/line.vert.
struct LinePush {
    Mat4 viewProj;
};

struct LineVertex {
    Vec3 position;
    Vec4 color;
};

/// Warna sorot untuk objek terpilih. Ditentukan renderer, bukan panel, supaya
/// seluruh viewport memakai satu warna yang sama tanpa panel perlu menghafalnya.
constexpr Vec4 kSelectedColor{1.0f, 0.62f, 0.20f, 1.0f};

/// Menyusun 12 rusuk AABB yang sudah ditransformasi ke ruang dunia.
void AppendBoxEdges(std::vector<LineVertex>& out, const Mat4& transform, const Vec3& boundsMin,
                    const Vec3& boundsMax, const Vec4& color) {
    std::array<Vec3, 8> corners{};
    for (int i = 0; i < 8; ++i) {
        const Vec3 local((i & 1) ? boundsMax.x : boundsMin.x, (i & 2) ? boundsMax.y : boundsMin.y,
                         (i & 4) ? boundsMax.z : boundsMin.z);
        corners[static_cast<std::size_t>(i)] = Vec3(transform * Vec4(local, 1.0f));
    }
    // Pasangan indeks yang berbeda tepat satu bit adalah rusuk kubus.
    static constexpr std::array<std::pair<int, int>, 12> kEdges{{{0, 1},
                                                                 {2, 3},
                                                                 {4, 5},
                                                                 {6, 7},
                                                                 {0, 2},
                                                                 {1, 3},
                                                                 {4, 6},
                                                                 {5, 7},
                                                                 {0, 4},
                                                                 {1, 5},
                                                                 {2, 6},
                                                                 {3, 7}}};
    for (const auto& [a, b] : kEdges) {
        out.push_back({corners[static_cast<std::size_t>(a)], color});
        out.push_back({corners[static_cast<std::size_t>(b)], color});
    }
}

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
        if (!CreatePipeline(desc.shaderDirectory) || !CreateLinePipeline(desc.shaderDirectory)) {
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

    void Render(const ViewportDesc& desc, const ViewportScene& scene) override {
        if (!target_.IsValid()) {
            return;
        }

        const float aspect =
            static_cast<float>(target_.Width()) / static_cast<float>(target_.Height());
        const Mat4 viewProj = desc.camera.Projection(aspect) * desc.camera.View();
        BuildLineGeometry(scene, desc.camera.position);

        // Slot yang akan ditulis frame ini mungkin masih dibaca GPU dari frame
        // sebelumnya. Penungguannya harus di sini — sebelum BeginTransient —
        // karena BeginTransient boleh memakai ulang slot fence mana pun, dan
        // menunggu setelahnya berarti bisa menunggu submit yang justru sedang
        // kita rekam sendiri.
        LineSlot& slot = lineSlots_[slotIndex_];
        bool slotReady = false;
        if (!lineVertices_.empty() && linePipeline_ != VK_NULL_HANDLE) {
            device_.WaitTransient(slot.submitId);
            slot.submitId = 0;
            slotReady = slot.buffer.Write(lineVertices_.data(),
                                          lineVertices_.size() * sizeof(LineVertex));
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

        const VkViewport viewport{0.0f,
                                  0.0f,
                                  static_cast<float>(target_.Width()),
                                  static_cast<float>(target_.Height()),
                                  0.0f,
                                  1.0f};
        const VkRect2D scissor{{0, 0}, {target_.Width(), target_.Height()}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        if (desc.showGrid && pipeline_ != VK_NULL_HANDLE) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

            GridPush push{};
            push.invViewProj = glm::inverse(viewProj);
            push.cameraPos = Vec4(desc.camera.position, 1.0f);
            // w = 0: proyeksi biasa, bidang dekat di depth 0.
            push.params = Vec4(desc.gridCellSize, desc.gridFadeDistance, 1.0f, 0.0f);
            vkCmdPushConstants(cmd, pipelineLayout_,
                               VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(GridPush), &push);

            vkCmdDraw(cmd, 3, 1, 0, 0);  // segitiga penuh layar
        }

        bool drewLines = false;
        if (slotReady) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_);

            LinePush push{};
            push.viewProj = viewProj;
            vkCmdPushConstants(cmd, linePipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(LinePush), &push);

            const VkDeviceSize offset = 0;
            const VkBuffer handle = slot.buffer.Handle();
            vkCmdBindVertexBuffers(cmd, 0, 1, &handle, &offset);
            vkCmdDraw(cmd, static_cast<uint32_t>(lineVertices_.size()), 1, 0, 0);
            drewLines = true;
        }

        vkCmdEndRenderPass(cmd);
        const uint64_t submitId = device_.SubmitTransient(cmd);
        if (drewLines) {
            slot.submitId = submitId;
            slotIndex_ = (slotIndex_ + 1) % kLineSlotCount;
        }
    }

    /// Stub tidak punya geometri sama sekali — ia menggambar wireframe kotak
    /// dari batas instance. Mengembalikan handle kubus satuan berarti pemanggil
    /// tetap mendapat batas yang masuk akal alih-alih mesh yang lenyap.
    MeshAsset AcquireMesh(std::string_view) override { return MeshAsset{}; }

    TextureHandle ColorTarget() const override { return textureHandle_; }
    Vec2 ColorTargetUvMax() const override {
        return Vec2(target_.UvMaxU(), target_.UvMaxV());
    }
    uint32_t Width() const override { return target_.Width(); }
    uint32_t Height() const override { return target_.Height(); }
    const char* Name() const override { return "Stub (wireframe)"; }

private:
    /// Membentangkan seluruh isi scene jadi segmen garis.
    ///
    /// Mesh jadi wireframe AABB. Renderer E8 akan menggambar mesh sungguhan
    /// dari MeshInstance yang sama dan hanya menyisakan garis bantu di sini.
    void BuildLineGeometry(const ViewportScene& scene, const Vec3& eye) {
        lineVertices_.clear();

        for (const MeshInstance& mesh : scene.meshes) {
            // **Satu-satunya culling yang dikenal perender ini, dan ia ada di
            // sini justru karena itu.** Sebuah setelan yang bekerja di
            // VulkanRenderer tetapi diam-diam tidak berpengaruh di perender
            // mundur adalah antarmuka yang berbohong kepada pengarang yang
            // kebetulan menjalankan mesin lama — dan yang terlihat bukan galat
            // melainkan angka yang tidak melakukan apa-apa.
            const Aabb world =
                TransformAabb(Aabb{mesh.boundsMin, mesh.boundsMax}, mesh.transform);
            if (!WithinDrawDistance(mesh.maxDrawDistance, eye, world)) {
                continue;
            }
            const Vec4 color = mesh.selected ? kSelectedColor : mesh.color;
            AppendBoxEdges(lineVertices_, mesh.transform, mesh.boundsMin, mesh.boundsMax, color);
        }

        for (const LineSegment& line : scene.lines) {
            lineVertices_.push_back({line.a, line.color});
            lineVertices_.push_back({line.b, line.color});
        }
    }

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

    /// Pipeline garis untuk wireframe, sumbu, dan ikon.
    ///
    /// Berbeda dari pipeline grid dalam tiga hal yang penting: ada vertex input,
    /// topologinya LINE_LIST, dan depth test menyala supaya wireframe di
    /// belakang tertutup yang di depan.
    bool CreateLinePipeline(const std::filesystem::path& shaderDirectory) {
        VkShaderModule vertexModule =
            CreateShaderModule(device_.Handle(), shaderDirectory / "line.vert.spv");
        VkShaderModule fragmentModule =
            CreateShaderModule(device_.Handle(), shaderDirectory / "line.frag.spv");
        if (vertexModule == VK_NULL_HANDLE || fragmentModule == VK_NULL_HANDLE) {
            SIM_WARN("Render", "Line shader unavailable; viewport shows no scene geometry");
            if (vertexModule != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_.Handle(), vertexModule, nullptr);
            }
            if (fragmentModule != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_.Handle(), fragmentModule, nullptr);
            }
            return true;
        }

        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.size = sizeof(LinePush);

        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pushRange;
        SIM_VK_CHECK(
            vkCreatePipelineLayout(device_.Handle(), &layoutInfo, nullptr, &linePipelineLayout_));

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertexModule;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragmentModule;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{};
        binding.binding = 0;
        binding.stride = sizeof(LineVertex);
        binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        const std::array<VkVertexInputAttributeDescription, 2> attributes{
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(LineVertex, position)},
            VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(LineVertex, color)},
        };

        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;

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
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

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
        pipelineInfo.layout = linePipelineLayout_;
        pipelineInfo.renderPass = target_.RenderPass();
        pipelineInfo.subpass = 0;

        SIM_VK_CHECK(vkCreateGraphicsPipelines(device_.Handle(), device_.PipelineCache(), 1,
                                               &pipelineInfo, nullptr, &linePipeline_));

        vkDestroyShaderModule(device_.Handle(), vertexModule, nullptr);
        vkDestroyShaderModule(device_.Handle(), fragmentModule, nullptr);

        for (LineSlot& slot : lineSlots_) {
            slot.buffer.Create(device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 64u * 1024u);
        }
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
        if (linePipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_.Handle(), linePipeline_, nullptr);
            linePipeline_ = VK_NULL_HANDLE;
        }
        if (linePipelineLayout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_.Handle(), linePipelineLayout_, nullptr);
            linePipelineLayout_ = VK_NULL_HANDLE;
        }
        for (LineSlot& slot : lineSlots_) {
            slot.buffer.Destroy();
            slot.submitId = 0;
        }
        target_.Destroy();
    }

    /// Satu buffer vertex per frame yang mungkin masih dalam penerbangan,
    /// beserta nomor submit terakhir yang membacanya.
    struct LineSlot {
        rhi::DynamicBuffer buffer;
        uint64_t submitId = 0;
    };
    static constexpr std::size_t kLineSlotCount = 3;

    rhi::Device& device_;
    rhi::ITextureRegistry& textures_;
    rhi::RenderTarget target_;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout linePipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline linePipeline_ = VK_NULL_HANDLE;
    std::array<LineSlot, kLineSlotCount> lineSlots_{};
    std::size_t slotIndex_ = 0;
    std::vector<LineVertex> lineVertices_;
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
