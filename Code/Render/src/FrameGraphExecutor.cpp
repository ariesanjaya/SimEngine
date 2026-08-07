#include "FrameGraphExecutor.h"

#include "Sim/Core/Log.h"

namespace sim::render {

void FrameGraphExecutor::Bind(ResourceId resource, const BoundImage& image) {
    if (resource >= bound_.size()) {
        bound_.resize(resource + 1);
        layout_.resize(resource + 1, VK_IMAGE_LAYOUT_UNDEFINED);
    }
    bound_[resource] = image;
}

void FrameGraphExecutor::Clear() {
    bound_.clear();
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
    if (!compiled.ok) {
        return false;
    }

    std::vector<VkImageMemoryBarrier2> barriers;
    const auto flush = [&](const std::vector<Barrier>& list) -> bool {
        barriers.clear();
        for (const Barrier& barrier : list) {
            if (barrier.resource >= bound_.size() ||
                bound_[barrier.resource].image == VK_NULL_HANDLE) {
                SIM_ERROR("Render", "frame graph resource {} has no image bound",
                          barrier.resource);
                return false;
            }
            const Stage from = Translate(barrier.from);
            const Stage to = Translate(barrier.to);
            const BoundImage& image = bound_[barrier.resource];

            VkImageMemoryBarrier2 vk{};
            vk.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            vk.srcStageMask = from.stage;
            vk.srcAccessMask = from.access;
            vk.dstStageMask = to.stage;
            vk.dstAccessMask = to.access;
            vk.oldLayout = from.layout;
            vk.newLayout = to.layout;
            vk.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vk.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vk.image = image.image;
            vk.subresourceRange.aspectMask = image.aspect;
            vk.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
            vk.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
            barriers.push_back(vk);
            layout_[barrier.resource] = to.layout;
        }
        if (barriers.empty()) {
            return true;
        }
        // Seluruh barrier sebuah pass dipancarkan dalam satu panggilan. Satu
        // panggilan per barrier akan memaksa driver menyusun beberapa titik
        // sinkronisasi berurutan padahal semuanya bisa dipenuhi sekaligus.
        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.imageMemoryBarrierCount = static_cast<uint32_t>(barriers.size());
        dependency.pImageMemoryBarriers = barriers.data();
        vkCmdPipelineBarrier2(cmd, &dependency);
        return true;
    };

    for (const CompiledPass& pass : compiled.order) {
        // Barrier ikut diukur bersama pass-nya. Ia memang bagian dari biaya
        // pass itu — barrier yang mahal adalah barrier yang disimpulkan graph
        // untuk pass itu, dan memisahkannya menyembunyikan justru hal yang
        // ingin dilihat orang yang membuka tabel ini.
        if (profiler != nullptr) {
            profiler->BeginScope(cmd, pass.name);
        }
        const bool ok = flush(pass.barriers);
        if (ok && pass.pass < recorders.size() && recorders[pass.pass]) {
            recorders[pass.pass](cmd);
        }
        if (profiler != nullptr) {
            profiler->EndScope(cmd);
        }
        if (!ok) {
            return false;
        }
    }
    return flush(compiled.finalBarriers);
}

}  // namespace sim::render
