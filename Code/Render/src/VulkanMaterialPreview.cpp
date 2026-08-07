#include "MaterialPreviewMesh.h"

#include "Sim/Core/Log.h"
#include "Sim/RHI/Buffer.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/RenderTarget.h"
#include "Sim/RHI/Texture.h"
#include "Sim/RHI/TextureRegistry.h"
#include "Sim/Render/Frustum.h"
#include "Sim/Render/IMaterialPreview.h"
#include "Sim/Render/RendererFactory.h"

#include <array>
#include <cstring>

namespace sim::render {
namespace {

/// Nomor set dan binding, disalin dari `MaterialBindings` di modul Material.
///
/// Disalin, bukan di-include: modul Render tidak boleh bergantung pada Material
/// — arah ketergantungannya justru sebaliknya nanti. Yang menjaga keduanya tidak
/// berpisah adalah test di `Tests/MaterialTests.cpp`, yang memeriksa nomor
/// binding pada teks modul yang benar-benar dihasilkan kompiler graph.
constexpr uint32_t kFrameSet = 0;
constexpr uint32_t kObjectSet = 1;
constexpr uint32_t kMaterialSet = 2;

constexpr uint32_t kFrameParamsBinding = 0;
constexpr uint32_t kBoneMatricesBinding = 1;
constexpr uint32_t kInstanceTransformsBinding = 2;
constexpr uint32_t kObjectParamsBinding = 0;
constexpr uint32_t kMaterialParamsBinding = 0;

/// Cermin dari cbuffer `FrameParams` di modul yang dirakit. std140.
struct FrameParams {
    Mat4 viewProjection{1.0f};
    Vec3 cameraPosition{0.0f};
    float time = 0.0f;
    Vec3 lightDirection{0.0f, 1.0f, 0.0f};
    float alphaCutoff = 0.5f;
    Vec3 lightRadiance{1.0f};
    float pad0 = 0.0f;
};
static_assert(sizeof(FrameParams) == 112, "FrameParams harus sama dengan tata letak std140-nya");

struct ObjectParams {
    Mat4 world{1.0f};
};

VkShaderModule MakeShaderModule(VkDevice device, std::span<const uint32_t> spirv) {
    if (spirv.empty()) {
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = spirv.size() * sizeof(uint32_t);
    info.pCode = spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return module;
}

class VulkanMaterialPreview final : public IMaterialPreview {
public:
    VulkanMaterialPreview(rhi::Device& device, rhi::ITextureRegistry& textures)
        : device_(device), textures_(textures) {}

    ~VulkanMaterialPreview() override { Shutdown(); }

    bool Initialize(uint32_t width, uint32_t height) {
        if (!device_.SupportsVulkan13()) {
            SIM_WARN("Render", "material preview needs Vulkan 1.3");
            return false;
        }
        if (!target_.Create(device_, width, height)) {
            return false;
        }
        if (!CreateMeshes() || !CreateUniforms() || !CreateFallbackTexture() ||
            !CreateDescriptorPool()) {
            return false;
        }
        AdoptTargetLayout();
        RefreshTextureHandle();
        SIM_INFO("Render", "material preview ready ({}x{})", target_.Width(), target_.Height());
        return true;
    }

    // --- IMaterialPreview ---------------------------------------------------

    bool SetMaterial(const MaterialPreviewShaders& shaders) override {
        // Pipeline lama dibuang lebih dulu, dan device ditunggu sampai diam.
        // Membuang pipeline yang masih dipakai frame yang belum selesai adalah
        // kerusakan memori, bukan pesan galat — dan preview memang membangun
        // ulang tepat ketika pengguna sedang menyunting, yaitu saat frame
        // sebelumnya paling mungkin masih berjalan.
        device_.WaitIdle();
        DestroyMaterial();
        error_.clear();

        VkShaderModule vertex = MakeShaderModule(device_.Handle(), shaders.vertexSpirv);
        VkShaderModule fragment = MakeShaderModule(device_.Handle(), shaders.fragmentSpirv);
        if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) {
            error_ = "SPIR-V rejected by the driver";
            DestroyModules(vertex, fragment);
            return false;
        }

        textureCount_ = shaders.textureCount;
        // Blok minimal 16 byte meski materialnya tanpa parameter: descriptor
        // tetap mendeklarasikan binding-nya, dan buffer berukuran nol tidak sah.
        parameterBytes_ = shaders.parameterBytes == 0 ? 16u : shaders.parameterBytes;

        const bool built = CreateMaterialLayouts() && CreateMaterialPipeline(vertex, fragment,
                                                                            shaders.alphaTest) &&
                           AllocateMaterialSets();
        DestroyModules(vertex, fragment);
        if (!built) {
            DestroyMaterial();
            if (error_.empty()) {
                error_ = "failed to build the preview pipeline";
            }
            return false;
        }
        parameters_.assign(parameterBytes_, 0u);
        parametersDirty_ = true;
        return true;
    }

    void ClearMaterial() override {
        device_.WaitIdle();
        DestroyMaterial();
    }

    bool HasMaterial() const override { return pipeline_ != VK_NULL_HANDLE; }

    void SetParameters(std::span<const uint8_t> block) override {
        if (block.empty()) {
            return;
        }
        // Dipotong, bukan ditolak. Blok yang lebih besar dari yang dibangun
        // pipeline berarti panel sudah mengompilasi ulang graph-nya sementara
        // preview belum menerima shader barunya — keadaan yang berlangsung satu
        // frame, dan menolaknya hanya membuat preview membeku pada nilai lama.
        const size_t count = std::min(block.size(), parameters_.size());
        std::memcpy(parameters_.data(), block.data(), count);
        parametersDirty_ = true;
    }

    void Render(const MaterialPreviewDesc& desc) override {
        if (desc.width > 0 && desc.height > 0 && target_.Resize(desc.width, desc.height)) {
            AdoptTargetLayout();
            RefreshTextureHandle();
        }
        if (!target_.IsValid()) {
            return;
        }

        const MeshRange& mesh = meshes_[static_cast<size_t>(desc.shape)];
        const bool drawable = HasMaterial() && mesh.indexCount > 0;

        UpdateUniforms(desc);

        VkCommandBuffer cmd = device_.BeginTransient();
        TransitionTarget(cmd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        BeginRendering(cmd, desc);
        if (drawable) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
            std::array<VkDescriptorSet, 3> sets{};
            sets[kFrameSet] = frameSet_;
            sets[kObjectSet] = objectSet_;
            sets[kMaterialSet] = materialSet_;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0,
                                    static_cast<uint32_t>(sets.size()), sets.data(), 0, nullptr);
            const VkDeviceSize offset = 0;
            const VkBuffer vertexBuffer = vertices_.Handle();
            vkCmdBindVertexBuffers(cmd, 0, 1, &vertexBuffer, &offset);
            vkCmdBindIndexBuffer(cmd, indices_.Handle(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, mesh.indexCount, 1, mesh.firstIndex, mesh.vertexOffset, 0);
        }
        vkCmdEndRendering(cmd);
        TransitionTarget(cmd, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        device_.SubmitTransient(cmd);
    }

    TextureHandle ColorTarget() const override { return textureHandle_; }
    Vec2 ColorTargetUvMax() const override { return {target_.UvMaxU(), target_.UvMaxV()}; }
    const char* LastError() const override { return error_.c_str(); }
    const char* Name() const override { return "VulkanMaterialPreview"; }

private:
    struct MeshRange {
        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        int32_t vertexOffset = 0;
    };

    // --- pembangunan sekali -------------------------------------------------

    bool CreateMeshes() {
        std::vector<PreviewVertex> vertices;
        std::vector<uint32_t> indices;
        for (const PreviewShape shape :
             {PreviewShape::Sphere, PreviewShape::Cube, PreviewShape::Plane}) {
            const PreviewMeshData data = BuildPreviewMesh(shape);
            MeshRange& range = meshes_[static_cast<size_t>(shape)];
            range.firstIndex = static_cast<uint32_t>(indices.size());
            range.indexCount = static_cast<uint32_t>(data.indices.size());
            range.vertexOffset = static_cast<int32_t>(vertices.size());
            vertices.insert(vertices.end(), data.vertices.begin(), data.vertices.end());
            indices.insert(indices.end(), data.indices.begin(), data.indices.end());
        }

        const VkDeviceSize vertexBytes = sizeof(PreviewVertex) * vertices.size();
        const VkDeviceSize indexBytes = sizeof(uint32_t) * indices.size();
        // Ketiga bentuk masuk ke satu pasang buffer, dibedakan hanya oleh
        // rentang indeksnya. Mengganti bentuk karena itu tidak menyentuh GPU
        // sama sekali — ia hanya mengubah tiga angka di vkCmdDrawIndexed.
        return vertices_.Create(device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vertexBytes) &&
               vertices_.Write(vertices.data(), vertexBytes) &&
               indices_.Create(device_, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, indexBytes) &&
               indices_.Write(indices.data(), indexBytes);
    }

    bool CreateUniforms() {
        if (!frameUniform_.Create(device_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                  sizeof(FrameParams)) ||
            !objectUniform_.Create(device_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                   sizeof(ObjectParams))) {
            return false;
        }
        // Buffer tulang dan instance tidak pernah dibaca preview — kSkinned dan
        // kInstanced keduanya mati. Tapi keduanya tetap ada di layout descriptor
        // modul, dan descriptor yang dideklarasikan tapi tidak diikat adalah
        // pelanggaran meski shader tidak membacanya. Satu matriks identitas
        // sudah cukup untuk memenuhinya.
        const Mat4 identity{1.0f};
        return dummyStorage_.Create(device_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, sizeof(Mat4)) &&
               dummyStorage_.Write(&identity, sizeof(Mat4));
    }

    bool CreateFallbackTexture() {
        // Putih, bukan magenta. Tekstur yang belum dimuat harus membuat material
        // terlihat seperti material tanpa tekstur — kalikan dengan putih dan
        // hasilnya nilai dasarnya — bukan membuat seluruh preview berteriak.
        const uint32_t white = 0xffffffffu;
        return fallbackTexture_.CreateFromRgba(device_, 1, 1, &white);
    }

    bool CreateDescriptorPool() {
        // Cukup besar untuk satu material dengan sejumlah tekstur yang wajar.
        // Pool dibuat sekali dan set-nya dibebaskan per material, jadi batas ini
        // membatasi satu material, bukan seluruh sesi.
        const std::array<VkDescriptorPoolSize, 3> sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures * 2},
        };
        VkDescriptorPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = 8;
        info.poolSizeCount = static_cast<uint32_t>(sizes.size());
        info.pPoolSizes = sizes.data();
        return vkCreateDescriptorPool(device_.Handle(), &info, nullptr, &pool_) == VK_SUCCESS;
    }

    // --- pembangunan per material -------------------------------------------

    bool CreateMaterialLayouts() {
        const auto make = [this](std::span<const VkDescriptorSetLayoutBinding> bindings,
                                 VkDescriptorSetLayout& out) {
            VkDescriptorSetLayoutCreateInfo info{};
            info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            info.bindingCount = static_cast<uint32_t>(bindings.size());
            info.pBindings = bindings.data();
            return vkCreateDescriptorSetLayout(device_.Handle(), &info, nullptr, &out) ==
                   VK_SUCCESS;
        };

        // `pImmutableSamplers` disebut eksplisit di setiap entri. Tanpa itu
        // Release menolak kompilasi (-Wmissing-field-initializers), dan
        // peringatannya benar: sampler tetap adalah medan yang sengaja kosong di
        // sini, bukan medan yang kebetulan tidak terpikirkan.
        const auto bind = [](uint32_t binding, VkDescriptorType type, VkShaderStageFlags stages) {
            return VkDescriptorSetLayoutBinding{binding, type, 1, stages, nullptr};
        };

        const std::array<VkDescriptorSetLayoutBinding, 3> frame{
            bind(kFrameParamsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                 VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
            bind(kBoneMatricesBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 VK_SHADER_STAGE_VERTEX_BIT),
            bind(kInstanceTransformsBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                 VK_SHADER_STAGE_VERTEX_BIT),
        };
        const std::array<VkDescriptorSetLayoutBinding, 1> object{
            bind(kObjectParamsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                 VK_SHADER_STAGE_VERTEX_BIT),
        };

        std::vector<VkDescriptorSetLayoutBinding> material;
        material.push_back(bind(kMaterialParamsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                VK_SHADER_STAGE_FRAGMENT_BIT));
        for (uint32_t i = 0; i < textureCount_ && i < kMaxTextures; ++i) {
            // Tekstur dan sampler terpisah di Slang, jadi terpisah juga di sini —
            // dan nomornya berselang-seling, sama dengan yang ditulis kompiler.
            material.push_back(bind(1 + i * 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                    VK_SHADER_STAGE_FRAGMENT_BIT));
            material.push_back(
                bind(2 + i * 2, VK_DESCRIPTOR_TYPE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT));
        }

        return make(frame, frameLayout_) && make(object, objectLayout_) &&
               make(material, materialLayout_);
    }

    bool CreateMaterialPipeline(VkShaderModule vertex, VkShaderModule fragment, bool alphaTest) {
        // Diindeks nomor set-nya, bukan disusun berurutan begitu saja.
        // `vkCmdBindDescriptorSets` menerima larik yang urutannya *adalah*
        // nomor set, jadi dua baris yang tertukar akan mengikat data per-frame
        // ke slot material — dan pipeline tetap dibuat tanpa keluhan.
        std::array<VkDescriptorSetLayout, 3> layouts{};
        layouts[kFrameSet] = frameLayout_;
        layouts[kObjectSet] = objectLayout_;
        layouts[kMaterialSet] = materialLayout_;
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = static_cast<uint32_t>(layouts.size());
        layoutInfo.pSetLayouts = layouts.data();
        if (vkCreatePipelineLayout(device_.Handle(), &layoutInfo, nullptr, &pipelineLayout_) !=
            VK_SUCCESS) {
            return false;
        }

        // Konstanta spesialisasi. Nomornya terkunci di modul: 0 skinned, 1
        // instanced, 2 alpha-test. Yang tidak ada di modul sebuah tahap
        // diabaikan Vulkan, jadi ketiganya boleh dikirim ke kedua tahap.
        const std::array<uint32_t, 3> constants{0u, 0u, alphaTest ? 1u : 0u};
        const std::array<VkSpecializationMapEntry, 3> entries{
            VkSpecializationMapEntry{0, 0, sizeof(uint32_t)},
            VkSpecializationMapEntry{1, sizeof(uint32_t), sizeof(uint32_t)},
            VkSpecializationMapEntry{2, sizeof(uint32_t) * 2, sizeof(uint32_t)},
        };
        VkSpecializationInfo specialization{};
        specialization.mapEntryCount = static_cast<uint32_t>(entries.size());
        specialization.pMapEntries = entries.data();
        specialization.dataSize = sizeof(constants);
        specialization.pData = constants.data();

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[0].pSpecializationInfo = &specialization;
        stages[1] = stages[0];
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;

        const VkVertexInputBindingDescription binding{0, sizeof(PreviewVertex),
                                                      VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array<VkVertexInputAttributeDescription, 7> attributes{
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(PreviewVertex, position)},
            VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(PreviewVertex, normal)},
            VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(PreviewVertex, tangent)},
            VkVertexInputAttributeDescription{3, 0, VK_FORMAT_R32G32_SFLOAT,
                                              offsetof(PreviewVertex, uv)},
            VkVertexInputAttributeDescription{4, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(PreviewVertex, color)},
            VkVertexInputAttributeDescription{5, 0, VK_FORMAT_R32G32B32A32_UINT,
                                              offsetof(PreviewVertex, boneIndices)},
            VkVertexInputAttributeDescription{6, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(PreviewVertex, boneWeights)},
        };
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &binding;
        vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertexInput.pVertexAttributeDescriptions = attributes.data();

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
        // Plane digambar dua sisi supaya memutarnya ke belakang tidak membuatnya
        // hilang; bola dan kubus tertutup, jadi tidak ada yang hilang karenanya.
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        // Reversed-Z, sama dengan viewport utama: yang terdekat bernilai besar.
        depth.depthCompareOp = VK_COMPARE_OP_GREATER;

        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                               VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blend;

        const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,
                                                          VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();

        const VkFormat colorFormat = target_.ColorFormat();
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;
        rendering.depthAttachmentFormat = target_.DepthFormat();

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
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &colorBlend;
        info.pDynamicState = &dynamic;
        info.layout = pipelineLayout_;

        return vkCreateGraphicsPipelines(device_.Handle(), VK_NULL_HANDLE, 1, &info, nullptr,
                                         &pipeline_) == VK_SUCCESS;
    }

    bool AllocateMaterialSets() {
        if (!materialUniform_.Create(device_, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                     parameterBytes_)) {
            return false;
        }
        std::array<VkDescriptorSetLayout, 3> layouts{};
        layouts[kFrameSet] = frameLayout_;
        layouts[kObjectSet] = objectLayout_;
        layouts[kMaterialSet] = materialLayout_;
        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = pool_;
        info.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        info.pSetLayouts = layouts.data();
        std::array<VkDescriptorSet, 3> sets{};
        if (vkAllocateDescriptorSets(device_.Handle(), &info, sets.data()) != VK_SUCCESS) {
            error_ = "descriptor pool exhausted";
            return false;
        }
        frameSet_ = sets[kFrameSet];
        objectSet_ = sets[kObjectSet];
        materialSet_ = sets[kMaterialSet];

        std::vector<VkDescriptorBufferInfo> buffers;
        buffers.reserve(5);
        buffers.push_back({frameUniform_.Handle(), 0, sizeof(FrameParams)});
        buffers.push_back({dummyStorage_.Handle(), 0, sizeof(Mat4)});
        buffers.push_back({dummyStorage_.Handle(), 0, sizeof(Mat4)});
        buffers.push_back({objectUniform_.Handle(), 0, sizeof(ObjectParams)});
        buffers.push_back({materialUniform_.Handle(), 0, parameterBytes_});

        const VkDescriptorImageInfo image{VK_NULL_HANDLE, fallbackTexture_.View(),
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo sampler{fallbackTexture_.Sampler(), VK_NULL_HANDLE,
                                            VK_IMAGE_LAYOUT_UNDEFINED};

        std::vector<VkWriteDescriptorSet> writes;
        const auto write = [&](VkDescriptorSet set, uint32_t binding, VkDescriptorType type,
                               const VkDescriptorBufferInfo* buffer,
                               const VkDescriptorImageInfo* imageInfo) {
            VkWriteDescriptorSet entry{};
            entry.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            entry.dstSet = set;
            entry.dstBinding = binding;
            entry.descriptorCount = 1;
            entry.descriptorType = type;
            entry.pBufferInfo = buffer;
            entry.pImageInfo = imageInfo;
            writes.push_back(entry);
        };

        write(frameSet_, kFrameParamsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &buffers[0],
              nullptr);
        write(frameSet_, kBoneMatricesBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &buffers[1],
              nullptr);
        write(frameSet_, kInstanceTransformsBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
              &buffers[2], nullptr);
        write(objectSet_, kObjectParamsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &buffers[3],
              nullptr);
        write(materialSet_, kMaterialParamsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &buffers[4],
              nullptr);
        for (uint32_t i = 0; i < textureCount_ && i < kMaxTextures; ++i) {
            write(materialSet_, 1 + i * 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, nullptr, &image);
            write(materialSet_, 2 + i * 2, VK_DESCRIPTOR_TYPE_SAMPLER, nullptr, &sampler);
        }

        vkUpdateDescriptorSets(device_.Handle(), static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
        return true;
    }

    // --- per frame ----------------------------------------------------------

    void UpdateUniforms(const MaterialPreviewDesc& desc) {
        const float aspect =
            static_cast<float>(target_.Width()) / static_cast<float>(target_.Height());
        // Kamera mengorbit objek, bukan objek yang berputar: itu membuat arah
        // cahaya tetap terhadap dunia, jadi memutar preview memperlihatkan
        // bagaimana material menangkap cahaya dari sudut lain — yang justru
        // yang ingin dilihat orang.
        const float cosPitch = std::cos(desc.pitch);
        const Vec3 eye{desc.distance * cosPitch * std::sin(desc.yaw),
                       desc.distance * std::sin(desc.pitch),
                       desc.distance * cosPitch * std::cos(desc.yaw)};
        const Mat4 view = LookAt(eye, Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f));
        const Mat4 projection = PerspectiveReversedZ(40.0f * kDegToRad, aspect, 0.05f, 100.0f);

        FrameParams frame;
        frame.viewProjection = projection * view;
        frame.cameraPosition = eye;
        frame.time = desc.time;
        frame.lightDirection = glm::normalize(desc.lightDirection);
        frame.alphaCutoff = desc.alphaCutoff;
        frame.lightRadiance = desc.lightRadiance;
        frameUniform_.Write(&frame, sizeof(frame));

        ObjectParams object;
        objectUniform_.Write(&object, sizeof(object));

        if (parametersDirty_ && !parameters_.empty() && materialUniform_.IsValid()) {
            materialUniform_.Write(parameters_.data(), parameters_.size());
            parametersDirty_ = false;
        }
    }

    void BeginRendering(VkCommandBuffer cmd, const MaterialPreviewDesc& desc) {
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = target_.ColorView();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {
            {desc.clearColor.x, desc.clearColor.y, desc.clearColor.z, desc.clearColor.w}};

        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = target_.DepthView();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        // Nol: pada reversed-Z yang terjauh adalah nol.
        depth.clearValue.depthStencil = {0.0f, 0};

        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea.extent = {target_.Width(), target_.Height()};
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &color;
        info.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &info);

        const VkViewport viewport{0.0f,
                                  0.0f,
                                  static_cast<float>(target_.Width()),
                                  static_cast<float>(target_.Height()),
                                  0.0f,
                                  1.0f};
        const VkRect2D scissor{{0, 0}, {target_.Width(), target_.Height()}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    }

    void TransitionTarget(VkCommandBuffer cmd, VkImageLayout from, VkImageLayout to) {
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        const bool toAttachment = to == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcStageMask = toAttachment ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT
                                            : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = toAttachment ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT
                                             : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = toAttachment ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                            : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = toAttachment ? VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT
                                             : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = from;
        barrier.newLayout = to;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = target_.ColorImage();
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dependency);
    }

    /// Sama alasannya dengan `VulkanRenderer::AdoptTargetLayout` — image yang
    /// baru dibuat ada di UNDEFINED, sedangkan setiap frame mengandaikannya
    /// sudah berada di layout yang dijanjikan.
    ///
    /// **Depth ikut, dan itu yang sempat terlewat.** Warna yang salah layout
    /// terlihat langsung; depth tidak — preview-nya tetap tergambar benar dan
    /// satu-satunya yang memberi tahu adalah validation layer. Berbeda dengan
    /// warna, depth tidak pernah berpindah lagi sesudah ini karena tidak ada yang
    /// membacanya sebagai tekstur, jadi satu transisi cukup untuk seumur image.
    void AdoptTargetLayout() {
        VkCommandBuffer cmd = device_.BeginOneShot();
        TransitionTarget(cmd, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        VkImageMemoryBarrier2 depth{};
        depth.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        depth.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        // Dua stage, bukan satu: uji depth terjadi di EARLY untuk fragmen yang
        // lolos lebih awal dan di LATE untuk yang tertunda shader.
        depth.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        depth.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        depth.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depth.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        depth.image = target_.DepthImage();
        depth.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        depth.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        depth.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &depth;
        vkCmdPipelineBarrier2(cmd, &dependency);

        device_.EndOneShot(cmd);
    }

    void RefreshTextureHandle() {
        if (textureHandle_ != kInvalidTexture) {
            textures_.Release(textureHandle_);
        }
        textureHandle_ = textures_.Acquire(target_.ColorView(), target_.Sampler());
    }

    // --- pembongkaran -------------------------------------------------------

    void DestroyModules(VkShaderModule vertex, VkShaderModule fragment) {
        for (VkShaderModule module : {vertex, fragment}) {
            if (module != VK_NULL_HANDLE) {
                vkDestroyShaderModule(device_.Handle(), module, nullptr);
            }
        }
    }

    void DestroyMaterial() {
        if (pipeline_ != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_.Handle(), pipeline_, nullptr);
            pipeline_ = VK_NULL_HANDLE;
        }
        if (pipelineLayout_ != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_.Handle(), pipelineLayout_, nullptr);
            pipelineLayout_ = VK_NULL_HANDLE;
        }
        if (frameSet_ != VK_NULL_HANDLE) {
            const std::array<VkDescriptorSet, 3> sets{frameSet_, objectSet_, materialSet_};
            // Di sini urutannya tidak penting — yang dilakukan hanya membebaskan.
            vkFreeDescriptorSets(device_.Handle(), pool_, static_cast<uint32_t>(sets.size()),
                                 sets.data());
            frameSet_ = VK_NULL_HANDLE;
            objectSet_ = VK_NULL_HANDLE;
            materialSet_ = VK_NULL_HANDLE;
        }
        for (VkDescriptorSetLayout* layout : {&frameLayout_, &objectLayout_, &materialLayout_}) {
            if (*layout != VK_NULL_HANDLE) {
                vkDestroyDescriptorSetLayout(device_.Handle(), *layout, nullptr);
                *layout = VK_NULL_HANDLE;
            }
        }
        materialUniform_.Destroy();
        parameters_.clear();
        textureCount_ = 0;
        parameterBytes_ = 0;
    }

    void Shutdown() {
        device_.WaitIdle();
        DestroyMaterial();
        if (textureHandle_ != kInvalidTexture) {
            textures_.Release(textureHandle_);
            textureHandle_ = kInvalidTexture;
        }
        if (pool_ != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device_.Handle(), pool_, nullptr);
            pool_ = VK_NULL_HANDLE;
        }
        fallbackTexture_.Destroy();
        vertices_.Destroy();
        indices_.Destroy();
        frameUniform_.Destroy();
        objectUniform_.Destroy();
        dummyStorage_.Destroy();
        target_.Destroy();
    }

    static constexpr uint32_t kMaxTextures = 8;

    rhi::Device& device_;
    rhi::ITextureRegistry& textures_;
    rhi::RenderTarget target_;
    TextureHandle textureHandle_ = kInvalidTexture;

    rhi::DynamicBuffer vertices_;
    rhi::DynamicBuffer indices_;
    rhi::DynamicBuffer frameUniform_;
    rhi::DynamicBuffer objectUniform_;
    rhi::DynamicBuffer materialUniform_;
    rhi::DynamicBuffer dummyStorage_;
    rhi::Texture2D fallbackTexture_;
    std::array<MeshRange, 3> meshes_{};

    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout frameLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout objectLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout materialLayout_ = VK_NULL_HANDLE;
    VkDescriptorSet frameSet_ = VK_NULL_HANDLE;
    VkDescriptorSet objectSet_ = VK_NULL_HANDLE;
    VkDescriptorSet materialSet_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;

    uint32_t textureCount_ = 0;
    uint32_t parameterBytes_ = 0;
    std::vector<uint8_t> parameters_;
    bool parametersDirty_ = false;
    std::string error_;
};

}  // namespace

std::unique_ptr<IMaterialPreview> CreateMaterialPreview(rhi::Device& device,
                                                        rhi::ITextureRegistry& textures,
                                                        uint32_t width, uint32_t height) {
    auto preview = std::make_unique<VulkanMaterialPreview>(device, textures);
    if (!preview->Initialize(width, height)) {
        return nullptr;
    }
    return preview;
}

}  // namespace sim::render
