#include "Sim/Render/RendererFactory.h"

#include "FrameGraphExecutor.h"
#include "Sim/Core/Log.h"
#include "Sim/RHI/Buffer.h"
#include "Sim/RHI/Device.h"
#include "Sim/RHI/RenderTarget.h"
#include "Sim/RHI/TextureRegistry.h"
#include "Sim/Render/FrameGraph.h"
#include "Sim/Render/Frustum.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <vector>

namespace sim::render {
namespace {

/// Harus sama persis dengan blok push_constant di Shaders/box.vert.
struct BoxPush {
    Mat4 viewProj;
};

/// Harus sama persis dengan blok push_constant di Shaders/grid.{vert,frag}.
struct GridPush {
    Mat4 invViewProj;
    Vec4 cameraPos;
    Vec4 params;
};

/// Harus sama persis dengan blok push_constant di Shaders/line.vert.
struct LinePush {
    Mat4 viewProj;
};

struct LineVertex {
    Vec3 position;
    Vec4 color;
};

struct BoxVertex {
    Vec3 position;
    Vec3 normal;
};

/// Satu instance kotak. Tata letaknya harus sama persis dengan atribut instance
/// di Shaders/box.vert.
struct BoxInstance {
    Vec4 row0;
    Vec4 row1;
    Vec4 row2;
    Vec4 row3;
    Vec4 color;
};

constexpr Vec4 kSelectedColor{1.0f, 0.62f, 0.20f, 1.0f};

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

/// Enam sisi kubus satuan, masing-masing dua segitiga, dengan normal per sisi.
///
/// Ditulis apa adanya alih-alih diindeks: normal per sisi berarti sudut kubus
/// tidak bisa dibagi antar-sisi, jadi indeks tidak menghemat apa pun — dan 36
/// vertex yang dibuat sekali saat start bukan tempat yang layak dioptimasi.
std::vector<BoxVertex> BuildUnitCube() {
    const std::array<Vec3, 6> normals{Vec3(0, 0, 1),  Vec3(0, 0, -1), Vec3(1, 0, 0),
                                      Vec3(-1, 0, 0), Vec3(0, 1, 0),  Vec3(0, -1, 0)};
    const std::array<Vec3, 6> tangents{Vec3(1, 0, 0),  Vec3(-1, 0, 0), Vec3(0, 0, -1),
                                       Vec3(0, 0, 1),  Vec3(1, 0, 0),  Vec3(1, 0, 0)};
    std::vector<BoxVertex> vertices;
    vertices.reserve(36);
    for (std::size_t face = 0; face < 6; ++face) {
        const Vec3 n = normals[face];
        const Vec3 t = tangents[face];
        const Vec3 b = glm::cross(n, t);
        const Vec3 centre = n * 0.5f;
        const Vec3 a0 = centre - t * 0.5f - b * 0.5f;
        const Vec3 a1 = centre + t * 0.5f - b * 0.5f;
        const Vec3 a2 = centre + t * 0.5f + b * 0.5f;
        const Vec3 a3 = centre - t * 0.5f + b * 0.5f;
        vertices.push_back({a0, n});
        vertices.push_back({a1, n});
        vertices.push_back({a2, n});
        vertices.push_back({a0, n});
        vertices.push_back({a2, n});
        vertices.push_back({a3, n});
    }
    return vertices;
}

BoxInstance MakeInstance(const Mat4& model, const Vec4& color) {
    BoxInstance instance;
    // Kolom glm ditulis apa adanya sebagai empat atribut. Shader menyusunnya
    // kembali dengan `mat4(...)`, yang juga kolom-mayor — jadi keduanya cocok
    // tanpa transpose di mana pun.
    instance.row0 = model[0];
    instance.row1 = model[1];
    instance.row2 = model[2];
    instance.row3 = model[3];
    instance.color = color;
    return instance;
}

/// Renderer Vulkan E8.1.
///
/// **Yang digambar masih kotak, dan itu bukan penambal sementara yang malas.**
/// `ViewportScene::MeshInstance` belum membawa geometri sama sekali — hanya
/// transform, kotak batas, dan warna — karena importir mesh baru datang di E8.4.
/// Yang sedang dibuktikan di E8.1 bukan "mesh terlihat benar", melainkan bahwa
/// frame graph benar-benar menjalankan pass Vulkan dengan barrier yang
/// disimpulkannya sendiri, dengan depth prepass, transparansi tersortir, dan
/// reversed-Z. Kotak adalah geometri yang cukup untuk membuktikan semua itu, dan
/// begitu mesh sungguhan masuk, yang berganti hanya sumber vertex-nya.
class VulkanRenderer final : public IViewportRenderer {
public:
    VulkanRenderer(rhi::Device& device, rhi::ITextureRegistry& textures)
        : device_(device), textures_(textures) {}

    ~VulkanRenderer() override { Shutdown(); }

    bool Initialize(const StubRendererDesc& desc) {
        if (!device_.SupportsVulkan13()) {
            // Dynamic rendering dan synchronization2 keduanya inti 1.3. Jalur
            // mundurnya adalah render pass tradisional — dan itu jalur kedua
            // yang harus ikut diuji setiap kali pass berubah. Selama belum ada
            // yang menuntutnya, lebih jujur menolak daripada menyediakan jalur
            // yang tidak pernah dijalankan siapa pun.
            SIM_WARN("Render", "VulkanRenderer needs Vulkan 1.3; falling back to StubRenderer");
            return false;
        }
        if (!target_.Create(device_, desc.initialWidth, desc.initialHeight)) {
            return false;
        }
        if (!CreateCube() || !CreatePipelines(desc.shaderDirectory) ||
            !CreateOverlayPipelines(desc.shaderDirectory)) {
            return false;
        }
        for (InstanceSlot& slot : slots_) {
            if (!slot.buffer.Create(device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                    sizeof(BoxInstance) * 256) ||
                !slot.lineBuffer.Create(device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                        sizeof(LineVertex) * 1024)) {
                return false;
            }
        }
        AdoptTargetLayout();
        RefreshTextureHandle();
        SIM_INFO("Render", "VulkanRenderer ready ({}x{}, frame graph, reversed-Z)", target_.Width(),
                 target_.Height());
        return true;
    }

    /// Memindahkan target warna ke layout yang dijanjikannya, sekali, saat ia
    /// baru dibuat.
    ///
    /// **Ditemukan oleh validation layer, bukan dengan membaca kode.** Graph
    /// menyatakan target warna dimulai dalam keadaan `Present` — yaitu
    /// SHADER_READ_ONLY, karena ImGui membacanya sebagai tekstur. Itu benar
    /// untuk setiap frame kecuali yang pertama: image yang baru dibuat berada di
    /// UNDEFINED, dan kalau ImGui sempat membacanya sebelum frame pertama
    /// selesai — mis. karena panel Viewport tertutup jendela lain dan
    /// `Render()` tidak pernah dipanggil — ia membaca image dalam layout yang
    /// tidak sah. Memindahkannya di sini membuat janji graph benar sejak detik
    /// pertama, alih-alih benar mulai frame kedua.
    void AdoptTargetLayout() {
        VkCommandBuffer cmd = device_.BeginOneShot();
        VkImageMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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
        device_.EndOneShot(cmd);
    }

    void Resize(uint32_t width, uint32_t height) override {
        if (width == 0 || height == 0 || !target_.Resize(width, height)) {
            return;
        }
        // Resize membangun image baru, jadi janji layoutnya harus dibuat ulang.
        AdoptTargetLayout();
        RefreshTextureHandle();
    }

    void Render(const ViewportDesc& desc, const ViewportScene& scene) override {
        if (!target_.IsValid()) {
            return;
        }
        const float aspect =
            static_cast<float>(target_.Width()) / static_cast<float>(target_.Height());
        // Reversed-Z: near di 1, far di 0. Konsekuensinya depth di-clear ke 0 dan
        // ujinya GREATER — ketiganya harus berpindah bersama, dan memisahkannya
        // menghasilkan layar yang kosong tanpa satu pun pesan galat.
        const Mat4 projection =
            PerspectiveReversedZ(desc.camera.fovYRadians, aspect, desc.camera.nearZ,
                                 desc.camera.farZ);
        const Mat4 viewProj = projection * desc.camera.View();

        Gather(desc, scene, viewProj);

        InstanceSlot& slot = slots_[slotIndex_];
        device_.WaitTransient(slot.submitId);
        slot.submitId = 0;
        const std::size_t instanceCount = opaque_.size() + transparent_.size();
        bool slotReady = instanceCount == 0;
        if (instanceCount > 0) {
            upload_.clear();
            upload_.insert(upload_.end(), opaque_.begin(), opaque_.end());
            upload_.insert(upload_.end(), transparent_.begin(), transparent_.end());
            slotReady = slot.buffer.Reserve(sizeof(BoxInstance) * upload_.size()) &&
                        slot.buffer.Write(upload_.data(), sizeof(BoxInstance) * upload_.size());
        }

        lineVertices_.clear();
        for (const LineSegment& line : scene.lines) {
            lineVertices_.push_back({line.a, line.color});
            lineVertices_.push_back({line.b, line.color});
        }
        bool linesReady = false;
        if (!lineVertices_.empty()) {
            linesReady =
                slot.lineBuffer.Reserve(sizeof(LineVertex) * lineVertices_.size()) &&
                slot.lineBuffer.Write(lineVertices_.data(),
                                      sizeof(LineVertex) * lineVertices_.size());
        }

        BuildGraph();

        VkCommandBuffer cmd = device_.BeginTransient();
        executor_.Clear();
        executor_.Bind(colorId_, BoundImage{target_.ColorImage(), target_.ColorView(),
                                            VK_IMAGE_ASPECT_COLOR_BIT});
        executor_.Bind(depthId_, BoundImage{target_.DepthImage(), target_.DepthView(),
                                            VK_IMAGE_ASPECT_DEPTH_BIT});

        const auto opaqueCount = static_cast<uint32_t>(opaque_.size());
        const auto transparentCount = static_cast<uint32_t>(transparent_.size());
        const BoxPush push{viewProj};

        const Mat4 invViewProj = glm::inverse(viewProj);
        std::array<FrameGraphExecutor::Recorder, 5> recorders{};
        recorders[gridId_] = [&](VkCommandBuffer command) {
            // Grid membersihkan warna dan menjadi latar. Ia tidak menyentuh depth
            // sama sekali, jadi apa pun yang digambar sesudahnya menutupinya tanpa
            // uji apa pun.
            BeginRendering(command, desc, /*clearColor=*/true, /*loadDepth=*/false,
                           /*writeColor=*/true, /*useDepth=*/false);
            if (desc.showGrid && gridPipeline_ != VK_NULL_HANDLE) {
                GridPush push{};
                push.invViewProj = invViewProj;
                push.cameraPos = Vec4(desc.camera.position, 1.0f);
                // w = 1: reversed-Z, bidang dekat ada di depth 1.
                push.params = Vec4(desc.gridCellSize, desc.gridFadeDistance, 1.0f, 1.0f);
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, gridPipeline_);
                vkCmdPushConstants(command, gridLayout_,
                                   VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(GridPush), &push);
                vkCmdDraw(command, 3, 1, 0, 0);
            }
            vkCmdEndRendering(command);
        };
        recorders[prepassId_] = [&](VkCommandBuffer command) {
            BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/false,
                           /*writeColor=*/false);
            if (slotReady && opaqueCount > 0) {
                DrawInstances(command, prepassPipeline_, push, slot, 0, opaqueCount);
            }
            vkCmdEndRendering(command);
        };
        recorders[opaqueId_] = [&](VkCommandBuffer command) {
            BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/true,
                           /*writeColor=*/true);
            if (slotReady && opaqueCount > 0) {
                DrawInstances(command, opaquePipeline_, push, slot, 0, opaqueCount);
            }
            vkCmdEndRendering(command);
        };
        recorders[transparentId_] = [&](VkCommandBuffer command) {
            BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/true,
                           /*writeColor=*/true);
            if (slotReady && transparentCount > 0) {
                DrawInstances(command, transparentPipeline_, push, slot, opaqueCount,
                              transparentCount);
            }
            vkCmdEndRendering(command);
        };
        recorders[linesId_] = [&](VkCommandBuffer command) {
            BeginRendering(command, desc, /*clearColor=*/false, /*loadDepth=*/true,
                           /*writeColor=*/true);
            if (linesReady && linePipeline_ != VK_NULL_HANDLE) {
                LinePush linePush{viewProj};
                vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipeline_);
                vkCmdPushConstants(command, lineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                   sizeof(LinePush), &linePush);
                const VkDeviceSize offset = 0;
                const VkBuffer handle = slot.lineBuffer.Handle();
                vkCmdBindVertexBuffers(command, 0, 1, &handle, &offset);
                vkCmdDraw(command, static_cast<uint32_t>(lineVertices_.size()), 1, 0, 0);
            }
            vkCmdEndRendering(command);
        };

        if (!executor_.Execute(compiled_, cmd, recorders)) {
            SIM_ERROR("Render", "frame graph execution failed: {}", compiled_.error);
        }
        slot.submitId = device_.SubmitTransient(cmd);
        slotIndex_ = (slotIndex_ + 1) % slots_.size();
        drawnOpaque_ = opaqueCount;
        drawnTransparent_ = transparentCount;
    }

    TextureHandle ColorTarget() const override { return textureHandle_; }
    Vec2 ColorTargetUvMax() const override { return {target_.UvMaxU(), target_.UvMaxV()}; }
    uint32_t Width() const override { return target_.Width(); }
    uint32_t Height() const override { return target_.Height(); }
    const char* Name() const override { return "VulkanRenderer"; }

private:
    struct InstanceSlot {
        rhi::DynamicBuffer buffer;
        rhi::DynamicBuffer lineBuffer;
        uint64_t submitId = 0;
    };

    /// Menyusun graph frame ini.
    ///
    /// Dibangun ulang tiap frame walaupun bentuknya tetap. Menyimpannya akan
    /// menghemat beberapa mikrodetik dan menukar itu dengan pertanyaan "graph
    /// mana yang berlaku sekarang" setiap kali sebuah pass menjadi bersyarat —
    /// dan pass bersyarat justru alasan graph ini ada.
    void BuildGraph() {
        graph_.Clear();
        colorId_ = graph_.Import("viewport-color", Access::Present);
        depthId_ = graph_.Import("viewport-depth", Access::None);

        gridId_ = graph_.AddPass("grid");
        graph_.Write(gridId_, colorId_, Access::ColorWrite);

        prepassId_ = graph_.AddPass("depth-prepass");
        graph_.Write(prepassId_, depthId_, Access::DepthWrite);

        opaqueId_ = graph_.AddPass("forward-opaque");
        graph_.Read(opaqueId_, depthId_, Access::DepthWrite);
        graph_.Write(opaqueId_, colorId_, Access::ColorWrite);

        transparentId_ = graph_.AddPass("forward-transparent");
        graph_.Read(transparentId_, depthId_, Access::DepthWrite);
        graph_.Write(transparentId_, colorId_, Access::ColorWrite);

        linesId_ = graph_.AddPass("lines");
        graph_.Read(linesId_, depthId_, Access::DepthWrite);
        graph_.Write(linesId_, colorId_, Access::ColorWrite);

        graph_.SetOutput(colorId_, Access::Present);
        compiled_ = graph_.Compile();
    }

    /// Menyaring instance yang terlihat, memisahkan tembus pandang, lalu
    /// mengurutkannya dari belakang ke depan.
    void Gather(const ViewportDesc& desc, const ViewportScene& scene, const Mat4& viewProj) {
        opaque_.clear();
        transparent_.clear();
        const Frustum frustum(viewProj);
        const Vec3 eye = desc.camera.position;

        sorted_.clear();

        for (const MeshInstance& mesh : scene.meshes) {
            const Aabb local{mesh.boundsMin, mesh.boundsMax};
            const Aabb world = TransformAabb(local, mesh.transform);
            if (!frustum.Intersects(world)) {
                continue;
            }
            // Kotak batas dipetakan ke kubus satuan: geser ke pusatnya lalu skala
            // ke ukurannya. Begitu mesh sungguhan masuk di E8.4, langkah ini
            // hilang — transform instance-nya dipakai apa adanya.
            const Vec3 centre = (mesh.boundsMin + mesh.boundsMax) * 0.5f;
            const Vec3 size = glm::max(mesh.boundsMax - mesh.boundsMin, Vec3(1e-4f));
            Mat4 model = mesh.transform;
            model = glm::translate(model, centre);
            model = glm::scale(model, size);

            const Vec4 color = mesh.selected ? kSelectedColor : mesh.color;
            if (color.a >= 0.999f) {
                opaque_.push_back(MakeInstance(model, color));
                continue;
            }
            const float distance = glm::length(world.Centre() - eye);
            sorted_.push_back(SortedEntry{distance, MakeInstance(model, color)});
        }

        // Belakang ke depan. Alpha blending tidak komutatif: dua kaca yang
        // dicampur dengan urutan terbalik menghasilkan warna yang berbeda, dan
        // "berbeda" di sini berarti kaca yang lebih jauh terlihat di depan yang
        // lebih dekat.
        std::sort(sorted_.begin(), sorted_.end(),
                  [](const SortedEntry& a, const SortedEntry& b) { return a.distance > b.distance; });
        transparent_.reserve(sorted_.size());
        for (const SortedEntry& entry : sorted_) {
            transparent_.push_back(entry.instance);
        }
    }

    /// `useDepth` false berarti depth tidak dipasang sama sekali.
    ///
    /// Bukan sekadar penghematan: pass yang memasang depth tapi tidak
    /// menyatakannya di graph adalah pass yang barrier-nya tidak pernah
    /// disusun — dan lampiran depth selalu menyentuh image itu, walaupun
    /// shadernya tidak. Apa yang dipasang di sini harus sama persis dengan apa
    /// yang dideklarasikan `BuildGraph`.
    void BeginRendering(VkCommandBuffer cmd, const ViewportDesc& desc, bool clearColor,
                        bool loadDepth, bool writeColor, bool useDepth = true) {
        VkRenderingAttachmentInfo color{};
        color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color.imageView = target_.ColorView();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {
            {desc.clearColor.x, desc.clearColor.y, desc.clearColor.z, desc.clearColor.w}};

        VkRenderingAttachmentInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        depth.imageView = target_.DepthView();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = loadDepth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        // Pass grid tidak memakai depth sama sekali; membiarkannya meng-clear di
        // sana berarti prepass menemukan depth yang sudah bersih dan tetap benar,
        // tapi juga berarti dua clear pada image yang sama dalam satu frame.
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        // Nol, bukan satu: pada reversed-Z yang terjauh adalah nol.
        depth.clearValue.depthStencil = {0.0f, 0};

        VkRenderingInfo info{};
        info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        info.renderArea.extent = {target_.Width(), target_.Height()};
        info.layerCount = 1;
        info.colorAttachmentCount = writeColor || clearColor ? 1u : 0u;
        info.pColorAttachments = writeColor || clearColor ? &color : nullptr;
        info.pDepthAttachment = useDepth ? &depth : nullptr;
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

    void DrawInstances(VkCommandBuffer cmd, VkPipeline pipeline, const BoxPush& push,
                       InstanceSlot& slot, uint32_t first, uint32_t count) {
        if (pipeline == VK_NULL_HANDLE || count == 0) {
            return;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(BoxPush),
                           &push);
        const std::array<VkBuffer, 2> buffers{cubeBuffer_.Handle(), slot.buffer.Handle()};
        const std::array<VkDeviceSize, 2> offsets{0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers.data(), offsets.data());
        vkCmdDraw(cmd, cubeVertexCount_, count, 0, first);
    }

    bool CreateCube() {
        const std::vector<BoxVertex> vertices = BuildUnitCube();
        cubeVertexCount_ = static_cast<uint32_t>(vertices.size());
        if (!cubeBuffer_.Create(device_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                sizeof(BoxVertex) * vertices.size())) {
            return false;
        }
        return cubeBuffer_.Write(vertices.data(), sizeof(BoxVertex) * vertices.size());
    }

    /// Pipeline grid dan garis, dipinjam apa adanya dari shader yang sudah ada.
    ///
    /// Keduanya sengaja dibawa dari StubRenderer alih-alih ditinggalkan: grid
    /// dan garis bantu adalah cara utama orang membaca skala dan orientasi di
    /// viewport, dan renderer baru yang menghapusnya bukan kemajuan melainkan
    /// kemunduran yang kebetulan lebih benar secara teknis.
    bool CreateOverlayPipelines(const std::filesystem::path& shaderDirectory) {
        VkShaderModule gridVertex =
            CreateShaderModule(device_.Handle(), shaderDirectory / "grid.vert.spv");
        VkShaderModule gridFragment =
            CreateShaderModule(device_.Handle(), shaderDirectory / "grid.frag.spv");
        VkShaderModule lineVertex =
            CreateShaderModule(device_.Handle(), shaderDirectory / "line.vert.spv");
        VkShaderModule lineFragment =
            CreateShaderModule(device_.Handle(), shaderDirectory / "line.frag.spv");
        if (gridVertex == VK_NULL_HANDLE || gridFragment == VK_NULL_HANDLE ||
            lineVertex == VK_NULL_HANDLE || lineFragment == VK_NULL_HANDLE) {
            return false;
        }

        VkPushConstantRange gridRange{};
        gridRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        gridRange.size = sizeof(GridPush);
        VkPipelineLayoutCreateInfo gridLayoutInfo{};
        gridLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        gridLayoutInfo.pushConstantRangeCount = 1;
        gridLayoutInfo.pPushConstantRanges = &gridRange;
        SIM_VK_CHECK(
            vkCreatePipelineLayout(device_.Handle(), &gridLayoutInfo, nullptr, &gridLayout_));

        VkPushConstantRange lineRange{};
        lineRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        lineRange.size = sizeof(LinePush);
        VkPipelineLayoutCreateInfo lineLayoutInfo{};
        lineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        lineLayoutInfo.pushConstantRangeCount = 1;
        lineLayoutInfo.pPushConstantRanges = &lineRange;
        SIM_VK_CHECK(
            vkCreatePipelineLayout(device_.Handle(), &lineLayoutInfo, nullptr, &lineLayout_));

        gridPipeline_ = BuildOverlayPipeline(gridVertex, gridFragment, gridLayout_,
                                             VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                             /*useDepth=*/false, /*vertexInput=*/false);
        linePipeline_ = BuildOverlayPipeline(lineVertex, lineFragment, lineLayout_,
                                             VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                             /*useDepth=*/true, /*vertexInput=*/true);

        for (VkShaderModule module : {gridVertex, gridFragment, lineVertex, lineFragment}) {
            vkDestroyShaderModule(device_.Handle(), module, nullptr);
        }
        return gridPipeline_ != VK_NULL_HANDLE && linePipeline_ != VK_NULL_HANDLE;
    }

    VkPipeline BuildOverlayPipeline(VkShaderModule vertex, VkShaderModule fragment,
                                    VkPipelineLayout layout, VkPrimitiveTopology topology,
                                    bool useDepth, bool vertexInput) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";

        const VkVertexInputBindingDescription binding{0, sizeof(LineVertex),
                                                      VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array<VkVertexInputAttributeDescription, 2> attributes{
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(LineVertex, position)},
            VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(LineVertex, color)},
        };
        VkPipelineVertexInputStateCreateInfo vertexInputState{};
        vertexInputState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        if (vertexInput) {
            vertexInputState.vertexBindingDescriptionCount = 1;
            vertexInputState.pVertexBindingDescriptions = &binding;
            vertexInputState.vertexAttributeDescriptionCount =
                static_cast<uint32_t>(attributes.size());
            vertexInputState.pVertexAttributeDescriptions = attributes.data();
        }

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = topology;

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

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = useDepth ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;

        VkPipelineColorBlendAttachmentState blendState{};
        blendState.blendEnable = VK_TRUE;
        blendState.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendState.colorBlendOp = VK_BLEND_OP_ADD;
        blendState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blendState.alphaBlendOp = VK_BLEND_OP_ADD;
        blendState.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = 1;
        colorBlend.pAttachments = &blendState;

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
        rendering.depthAttachmentFormat =
            useDepth ? target_.DepthFormat() : VK_FORMAT_UNDEFINED;

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext = &rendering;
        info.stageCount = static_cast<uint32_t>(stages.size());
        info.pStages = stages.data();
        info.pVertexInputState = &vertexInputState;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &colorBlend;
        info.pDynamicState = &dynamic;
        info.layout = layout;

        VkPipeline pipeline = VK_NULL_HANDLE;
        SIM_VK_CHECK(vkCreateGraphicsPipelines(device_.Handle(), VK_NULL_HANDLE, 1, &info, nullptr,
                                               &pipeline));
        return pipeline;
    }

    bool CreatePipelines(const std::filesystem::path& shaderDirectory) {
        VkShaderModule vertex = CreateShaderModule(device_.Handle(), shaderDirectory / "box.vert.spv");
        VkShaderModule fragment =
            CreateShaderModule(device_.Handle(), shaderDirectory / "box.frag.spv");
        if (vertex == VK_NULL_HANDLE || fragment == VK_NULL_HANDLE) {
            return false;
        }

        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        range.size = sizeof(BoxPush);
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &range;
        SIM_VK_CHECK(
            vkCreatePipelineLayout(device_.Handle(), &layoutInfo, nullptr, &pipelineLayout_));

        // Tiga pipeline dari satu pasang shader. Yang berbeda hanya keadaan
        // depth dan blending — dan itu memang perbedaan antara prepass, opaque,
        // dan transparan.
        prepassPipeline_ = BuildPipeline(vertex, fragment, /*depthWrite=*/true,
                                         VK_COMPARE_OP_GREATER, /*blend=*/false,
                                         /*colorWrite=*/false);
        // Uji EQUAL, bukan GREATER: depth-nya sudah diisi prepass, jadi hanya
        // fragmen yang benar-benar terlihat yang boleh menjalankan shader.
        // Itulah gunanya prepass — bukan menghemat depth test, melainkan
        // menghemat shading yang akan ditimpa.
        opaquePipeline_ = BuildPipeline(vertex, fragment, /*depthWrite=*/false,
                                        VK_COMPARE_OP_EQUAL, /*blend=*/false, /*colorWrite=*/true);
        // Transparan diuji terhadap depth opaque tapi tidak menulisinya: dua
        // permukaan tembus pandang harus sama-sama terlihat, dan yang di depan
        // tidak boleh menghapus yang di belakang.
        transparentPipeline_ = BuildPipeline(vertex, fragment, /*depthWrite=*/false,
                                             VK_COMPARE_OP_GREATER, /*blend=*/true,
                                             /*colorWrite=*/true);

        vkDestroyShaderModule(device_.Handle(), vertex, nullptr);
        vkDestroyShaderModule(device_.Handle(), fragment, nullptr);
        return prepassPipeline_ != VK_NULL_HANDLE && opaquePipeline_ != VK_NULL_HANDLE &&
               transparentPipeline_ != VK_NULL_HANDLE;
    }

    VkPipeline BuildPipeline(VkShaderModule vertex, VkShaderModule fragment, bool depthWrite,
                             VkCompareOp depthCompare, bool blend, bool colorWrite) {
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        stages[1].pName = "main";
        // **Prepass jalan tanpa fragment shader sama sekali.**
        //
        // Bukan sekadar meredam peringatan validation bahwa `outColor` ditulis
        // ke lampiran yang tidak ada: itulah bentuk depth prepass yang benar.
        // Tanpa fragment shader, kartu grafis boleh memakai jalur depth-only
        // yang jauh lebih cepat — dan menjalankan shading yang hasilnya memang
        // dibuang adalah persis pekerjaan yang prepass ada untuk menghindarinya.
        const uint32_t stageCount = colorWrite ? 2u : 1u;

        const std::array<VkVertexInputBindingDescription, 2> bindings{
            VkVertexInputBindingDescription{0, sizeof(BoxVertex), VK_VERTEX_INPUT_RATE_VERTEX},
            VkVertexInputBindingDescription{1, sizeof(BoxInstance),
                                            VK_VERTEX_INPUT_RATE_INSTANCE},
        };
        const std::array<VkVertexInputAttributeDescription, 7> attributes{
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(BoxVertex, position)},
            VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(BoxVertex, normal)},
            VkVertexInputAttributeDescription{2, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(BoxInstance, row0)},
            VkVertexInputAttributeDescription{3, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(BoxInstance, row1)},
            VkVertexInputAttributeDescription{4, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(BoxInstance, row2)},
            VkVertexInputAttributeDescription{5, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(BoxInstance, row3)},
            VkVertexInputAttributeDescription{6, 1, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              offsetof(BoxInstance, color)},
        };
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertexInput.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
        vertexInput.pVertexBindingDescriptions = bindings.data();
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
        raster.cullMode = blend ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT;
        // **COUNTER_CLOCKWISE, walaupun proyeksinya membalik Y.**
        //
        // Ini salah pada percobaan pertama, dan salahnya terlihat sebagai kotak
        // yang memperlihatkan sisi dalamnya. Alasannya: arah muka dinilai di
        // ruang framebuffer, sesudah transformasi viewport — dan sumbu Y
        // framebuffer Vulkan memang sudah menghadap ke bawah. Pembalikan Y di
        // matriks proyeksi justru meniadakan pembalikan itu, jadi geometri yang
        // ditulis berlawanan arah jarum jam tetap berlawanan arah jarum jam di
        // sana. Dua pembalikan yang saling meniadakan mudah dihitung sebagai
        // satu.
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = depthCompare;

        VkPipelineColorBlendAttachmentState blendState{};
        blendState.colorWriteMask = colorWrite ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                  VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT)
                                               : 0;
        blendState.blendEnable = blend ? VK_TRUE : VK_FALSE;
        blendState.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blendState.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendState.colorBlendOp = VK_BLEND_OP_ADD;
        blendState.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendState.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blendState.alphaBlendOp = VK_BLEND_OP_ADD;

        VkPipelineColorBlendStateCreateInfo colorBlend{};
        colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlend.attachmentCount = colorWrite ? 1u : 0u;
        colorBlend.pAttachments = colorWrite ? &blendState : nullptr;

        const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT,
                                                          VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamic.pDynamicStates = dynamicStates.data();

        // **Prepass tidak punya lampiran warna sama sekali**, dan pipeline-nya
        // harus menyatakan hal yang sama. Menyatakan satu lampiran lalu
        // menjalankannya di pass yang tidak memasang apa pun adalah
        // ketidakcocokan yang hanya dilaporkan validation layer — tanpa layer,
        // yang terlihat adalah prepass yang kadang jalan dan kadang tidak,
        // bergantung driver. Ditemukan lewat validation, bukan dengan membaca
        // kode.
        const VkFormat colorFormat = target_.ColorFormat();
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = colorWrite ? 1u : 0u;
        rendering.pColorAttachmentFormats = colorWrite ? &colorFormat : nullptr;
        rendering.depthAttachmentFormat = target_.DepthFormat();

        VkGraphicsPipelineCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        info.pNext = &rendering;
        info.stageCount = stageCount;
        info.pStages = stages.data();
        info.pVertexInputState = &vertexInput;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depthStencil;
        info.pColorBlendState = &colorBlend;
        info.pDynamicState = &dynamic;
        info.layout = pipelineLayout_;

        VkPipeline pipeline = VK_NULL_HANDLE;
        SIM_VK_CHECK(vkCreateGraphicsPipelines(device_.Handle(), VK_NULL_HANDLE, 1, &info, nullptr,
                                               &pipeline));
        return pipeline;
    }

    void RefreshTextureHandle() {
        if (textureHandle_ != kInvalidTexture) {
            textures_.Release(textureHandle_);
        }
        textureHandle_ = textures_.Acquire(target_.ColorView(), target_.Sampler());
    }

    void Shutdown() {
        device_.WaitIdle();
        if (textureHandle_ != kInvalidTexture) {
            textures_.Release(textureHandle_);
            textureHandle_ = kInvalidTexture;
        }
        for (InstanceSlot& slot : slots_) {
            slot.buffer.Destroy();
            slot.lineBuffer.Destroy();
        }
        cubeBuffer_.Destroy();
        for (VkPipeline* pipeline :
             {&prepassPipeline_, &opaquePipeline_, &transparentPipeline_, &gridPipeline_,
              &linePipeline_}) {
            if (*pipeline != VK_NULL_HANDLE) {
                vkDestroyPipeline(device_.Handle(), *pipeline, nullptr);
                *pipeline = VK_NULL_HANDLE;
            }
        }
        for (VkPipelineLayout* layout : {&pipelineLayout_, &gridLayout_, &lineLayout_}) {
            if (*layout != VK_NULL_HANDLE) {
                vkDestroyPipelineLayout(device_.Handle(), *layout, nullptr);
                *layout = VK_NULL_HANDLE;
            }
        }
        target_.Destroy();
    }

    rhi::Device& device_;
    rhi::ITextureRegistry& textures_;
    rhi::RenderTarget target_;
    TextureHandle textureHandle_ = kInvalidTexture;

    FrameGraph graph_;
    CompiledGraph compiled_;
    FrameGraphExecutor executor_;
    ResourceId colorId_ = kInvalidResource;
    ResourceId depthId_ = kInvalidResource;
    PassId gridId_ = kInvalidPass;
    PassId prepassId_ = kInvalidPass;
    PassId linesId_ = kInvalidPass;
    PassId opaqueId_ = kInvalidPass;
    PassId transparentId_ = kInvalidPass;

    VkPipelineLayout gridLayout_ = VK_NULL_HANDLE;
    VkPipeline gridPipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout lineLayout_ = VK_NULL_HANDLE;
    VkPipeline linePipeline_ = VK_NULL_HANDLE;
    std::vector<LineVertex> lineVertices_;

    rhi::DynamicBuffer cubeBuffer_;
    uint32_t cubeVertexCount_ = 0;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline prepassPipeline_ = VK_NULL_HANDLE;
    VkPipeline opaquePipeline_ = VK_NULL_HANDLE;
    VkPipeline transparentPipeline_ = VK_NULL_HANDLE;

    std::array<InstanceSlot, 3> slots_;
    std::size_t slotIndex_ = 0;
    std::vector<BoxInstance> opaque_;
    std::vector<BoxInstance> transparent_;
    std::vector<BoxInstance> upload_;
    struct SortedEntry {
        float distance = 0.0f;
        BoxInstance instance;
    };
    std::vector<SortedEntry> sorted_;
    uint32_t drawnOpaque_ = 0;
    uint32_t drawnTransparent_ = 0;
};

}  // namespace

std::unique_ptr<IViewportRenderer> CreateVulkanRenderer(rhi::Device& device,
                                                        rhi::ITextureRegistry& textures,
                                                        const StubRendererDesc& desc) {
    auto renderer = std::make_unique<VulkanRenderer>(device, textures);
    if (!renderer->Initialize(desc)) {
        return nullptr;
    }
    return renderer;
}

}  // namespace sim::render
