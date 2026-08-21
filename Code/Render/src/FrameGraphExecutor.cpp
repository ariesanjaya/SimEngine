#include "FrameGraphExecutor.h"

#include "Sim/Core/Assert.h"
#include "Sim/Core/Log.h"

#include <array>

namespace sim::render {

void FrameGraphExecutor::Reserve(ResourceId resource) {
    if (resource < bound_.size()) {
        return;
    }
    bound_.resize(resource + 1);
    buffers_.resize(resource + 1);
    layout_.resize(resource + 1, VK_IMAGE_LAYOUT_UNDEFINED);
}

void FrameGraphExecutor::Bind(ResourceId resource, const BoundImage& image) {
    Reserve(resource);
    bound_[resource] = image;
}

void FrameGraphExecutor::Bind(ResourceId resource, const BoundBuffer& buffer) {
    Reserve(resource);
    buffers_[resource] = buffer;
}

void FrameGraphExecutor::Clear() {
    bound_.clear();
    buffers_.clear();
    layout_.clear();
}

VkImageLayout FrameGraphExecutor::LayoutOf(ResourceId resource) const {
    return resource < layout_.size() ? layout_[resource] : VK_IMAGE_LAYOUT_UNDEFINED;
}

FrameGraphExecutor::Stage FrameGraphExecutor::Translate(Access access) {
    switch (access) {
        case Access::None:
            // Tidak ada yang perlu ditunggu, dan isinya boleh dibuang. Layout
            // UNDEFINED di sisi *sumber* adalah izin bagi driver untuk tidak
            // menyalin isi lama — dan isi lama memang tidak ada gunanya pada
            // resource yang belum dipakai.
            return {VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, VK_ACCESS_2_NONE,
                    VK_IMAGE_LAYOUT_UNDEFINED};
        case Access::ColorWrite:
            return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        case Access::DepthWrite:
            // Dua stage, bukan satu: uji depth terjadi di EARLY_FRAGMENT_TESTS
            // untuk yang lolos lebih awal dan di LATE untuk yang tertunda shader.
            // Menyebut salah satunya saja meninggalkan lubang yang hanya terlihat
            // pada shader yang membuang fragmen.
            return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL};
        case Access::DepthRead:
            return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                        VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
                    VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL};
        case Access::ShaderRead:
            return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        case Access::ShaderWrite:
            return {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                    VK_IMAGE_LAYOUT_GENERAL};
        case Access::TransferRead:
            return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
        case Access::TransferWrite:
            return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
        case Access::IndirectRead:
            // Layout `UNDEFINED`: yang memakainya selalu buffer, dan buffer
            // tidak punya layout. Menyebut layout apa pun di sini akan membuat
            // barrier image yang tidak pernah ada pemakainya.
            return {VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED};
        case Access::Present:
            // "Present" di sini berarti diserahkan ke ImGui sebagai tekstur,
            // bukan ke swapchain: yang membacanya adalah fragment shader UI.
            return {VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    return {};
}

bool FrameGraphExecutor::Execute(const CompiledGraph& compiled, VkCommandBuffer cmd,
                                 std::span<const Recorder> recorders,
                                 rhi::GpuProfiler* profiler) {
    // Satu segmen, satu antrean: keluarga yang sama di kedua sisi membuat setiap
    // perpindahan kepemilikan menjadi tidak berarti apa-apa, dan graph memang
    // tidak memancarkannya kalau tidak ada pass yang berpindah antrean.
    const std::array<VkCommandBuffer, 1> single{cmd};
    return Execute(compiled, single, recorders, 0, 0, profiler);
}

bool FrameGraphExecutor::Execute(const CompiledGraph& compiled,
                                 std::span<const VkCommandBuffer> segments,
                                 std::span<const Recorder> recorders, uint32_t graphicsFamily,
                                 uint32_t computeFamily, rhi::GpuProfiler* profiler) {
    if (!compiled.ok) {
        return false;
    }
    SIM_VERIFY(!segments.empty(), "Execute needs at least one command buffer");
    const auto familyOf = [&](Queue queue) {
        return queue == Queue::AsyncCompute ? computeFamily : graphicsFamily;
    };
    VkCommandBuffer cmd = segments.front();

    std::vector<VkImageMemoryBarrier2> barriers;
    std::vector<VkBufferMemoryBarrier2> bufferBarriers;
    // `releasing` menentukan sisi mana dari sepasang barrier perpindahan
    // kepemilikan yang sedang dipancarkan. **Sisi seberangnya wajib kosong:**
    // Vulkan mengabaikan stage tujuan pada operasi lepas dan stage sumber pada
    // operasi ambil, dan menyebut tahap yang tidak ada di antrean itu — tahap
    // fragment di antrean compute — adalah pelanggaran, bukan sekadar
    // pemborosan.
    // Tahap yang sah di antrean compute. **Menyebut tahap grafis di sana adalah
    // pelanggaran, bukan pemborosan** — dan tahap grafis memang muncul di sisi
    // "dari" barrier pass compute yang menimpa resource yang frame sebelumnya
    // dibaca pass forward. Ketergantungan itu sudah dijaga fence slot frame;
    // yang tersisa di sini hanya namanya.
    const auto narrow = [](Queue queue, VkPipelineStageFlags2 stage) {
        if (queue != Queue::AsyncCompute) {
            return stage;
        }
        constexpr VkPipelineStageFlags2 kComputeLegal =
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT | VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT |
            VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
            VK_PIPELINE_STAGE_2_HOST_BIT;
        return stage & kComputeLegal;
    };
    const auto flush = [&](const std::vector<Barrier>& list, VkCommandBuffer into, bool releasing,
                           Queue queue) -> bool {
        barriers.clear();
        bufferBarriers.clear();
        for (const Barrier& barrier : list) {
            const bool known = barrier.resource < bound_.size();
            const VkBuffer buffer = known ? buffers_[barrier.resource].buffer : VK_NULL_HANDLE;
            const VkImage image = known ? bound_[barrier.resource].image : VK_NULL_HANDLE;
            if (buffer == VK_NULL_HANDLE && image == VK_NULL_HANDLE) {
                SIM_ERROR("Render", "frame graph resource {} has nothing bound", barrier.resource);
                return false;
            }
            Stage from = Translate(barrier.from);
            Stage to = Translate(barrier.to);
            if (barrier.CrossesQueue()) {
                if (releasing) {
                    to.stage = VK_PIPELINE_STAGE_2_NONE;
                    to.access = VK_ACCESS_2_NONE;
                } else {
                    from.stage = VK_PIPELINE_STAGE_2_NONE;
                    from.access = VK_ACCESS_2_NONE;
                }
            }
            from.stage = narrow(queue, from.stage);
            to.stage = narrow(queue, to.stage);
            if (from.stage == VK_PIPELINE_STAGE_2_NONE) {
                from.access = VK_ACCESS_2_NONE;
            }
            if (to.stage == VK_PIPELINE_STAGE_2_NONE) {
                to.access = VK_ACCESS_2_NONE;
            }

            if (buffer != VK_NULL_HANDLE) {
                const BoundBuffer& bound = buffers_[barrier.resource];
                VkBufferMemoryBarrier2 vk{};
                vk.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                vk.srcStageMask = from.stage;
                vk.srcAccessMask = from.access;
                vk.dstStageMask = to.stage;
                vk.dstAccessMask = to.access;
                if (barrier.CrossesQueue()) {
                    vk.srcQueueFamilyIndex = familyOf(barrier.fromQueue);
                    vk.dstQueueFamilyIndex = familyOf(barrier.toQueue);
                } else {
                    vk.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    vk.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                }
                vk.buffer = bound.buffer;
                vk.offset = bound.offset;
                vk.size = bound.size;
                bufferBarriers.push_back(vk);
                continue;
            }

            VkImageMemoryBarrier2 vk{};
            vk.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            vk.srcStageMask = from.stage;
            vk.srcAccessMask = from.access;
            vk.dstStageMask = to.stage;
            vk.dstAccessMask = to.access;
            vk.oldLayout = from.layout;
            vk.newLayout = to.layout;
            if (barrier.CrossesQueue()) {
                vk.srcQueueFamilyIndex = familyOf(barrier.fromQueue);
                vk.dstQueueFamilyIndex = familyOf(barrier.toQueue);
            } else {
                vk.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                vk.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            }
            vk.image = image;
            vk.subresourceRange.aspectMask = bound_[barrier.resource].aspect;
            vk.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
            vk.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
            barriers.push_back(vk);
            layout_[barrier.resource] = to.layout;
        }
        if (barriers.empty() && bufferBarriers.empty()) {
            return true;
        }
        // Seluruh barrier sebuah pass dipancarkan dalam satu panggilan. Satu
        // panggilan per barrier akan memaksa driver menyusun beberapa titik
        // sinkronisasi berurutan padahal semuanya bisa dipenuhi sekaligus.
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size());
        dependency.pBufferMemoryBarriers = bufferBarriers.data();
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(into, &dependency);
        return true;
    };

    uint32_t segmentAt = 0;
    uint32_t remaining = compiled.segments.empty()
                             ? static_cast<uint32_t>(compiled.order.size())
                             : compiled.segments.front().count;
    for (const CompiledPass& pass : compiled.order) {
        // Pindah ke command buffer segmen berikutnya tepat di batasnya. Perekam
        // pass tidak pernah tahu ia berada di segmen yang mana — yang tahu hanya
        // di sini, dan itulah yang membuat menambah pass tidak menyentuh
        // pembagian batch sama sekali.
        while (remaining == 0 && segmentAt + 1 < compiled.segments.size()) {
            ++segmentAt;
            remaining = compiled.segments[segmentAt].count;
            SIM_VERIFY(segmentAt < segments.size(), "a segment has no command buffer");
            cmd = segments[segmentAt];
        }
        --remaining;
        // Barrier ikut diukur bersama pass-nya. Ia memang bagian dari biaya
        // pass itu — barrier yang mahal adalah barrier yang disimpulkan graph
        // untuk pass itu, dan memisahkannya menyembunyikan justru hal yang
        // ingin dilihat orang yang membuka tabel ini.
        if (profiler != nullptr) {
            profiler->BeginScope(cmd, pass.name, pass.queue != Queue::AsyncCompute);
        }
        bool ok = flush(pass.barriers, cmd, /*releasing=*/false, pass.queue);
        if (ok && pass.pass < recorders.size() && recorders[pass.pass]) {
            recorders[pass.pass](cmd);
        }
        // Pelepasan kepemilikan berjalan **sesudah** pass-nya, di antrean yang
        // melepaskannya. Ia tidak boleh masuk ke `barriers` pass mana pun: yang
        // di sana berjalan sebelum, dan sisi lepas yang berjalan sebelum
        // penulisnya adalah sisi lepas yang melepaskan isi yang belum ada.
        if (ok && !pass.releases.empty()) {
            ok = flush(pass.releases, cmd, /*releasing=*/true, pass.queue);
        }
        if (profiler != nullptr) {
            profiler->EndScope(cmd);
        }
        if (!ok) {
            return false;
        }
    }
    return flush(compiled.finalBarriers, cmd, /*releasing=*/false, Queue::Graphics);
}

}  // namespace sim::render
