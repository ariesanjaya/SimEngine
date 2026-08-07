#include "Sim/RHI/GpuProfiler.h"

#include "Sim/Core/Log.h"

#include <algorithm>

namespace sim::rhi {

bool GpuProfiler::Create(Device& device, uint32_t maxScopesPerFrame) {
    Destroy();
    device_ = &device;
    maxScopes_ = std::max(maxScopesPerFrame, 1u);

    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(device.PhysicalDevice(), &properties);
    if (properties.limits.timestampPeriod <= 0.0f) {
        SIM_WARN("RHI", "GPU timestamps unsupported; per-pass timing is off");
        device_ = nullptr;
        return false;
    }
    nanosecondsPerTick_ = static_cast<double>(properties.limits.timestampPeriod);

    // Antrean yang tidak punya bit timestamp yang sah akan mengembalikan angka
    // yang selalu nol. Diperiksa di sini supaya kegagalannya berupa profiler
    // yang mati, bukan tabel penuh nol yang tampak seperti pass gratis.
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.PhysicalDevice(), &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device.PhysicalDevice(), &familyCount,
                                             families.data());
    const uint32_t family = device.GraphicsQueueFamily();
    if (family >= familyCount || families[family].timestampValidBits == 0) {
        SIM_WARN("RHI", "graphics queue has no valid timestamp bits; per-pass timing is off");
        device_ = nullptr;
        return false;
    }

    VkQueryPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    // Dua timestamp per lingkup, dikali jumlah frame in-flight.
    info.queryCount = maxScopes_ * 2 * kFrameCount;
    if (vkCreateQueryPool(device.Handle(), &info, nullptr, &pool_) != VK_SUCCESS) {
        SIM_WARN("RHI", "cannot create the timestamp query pool");
        pool_ = VK_NULL_HANDLE;
        device_ = nullptr;
        return false;
    }
    readback_.assign(maxScopes_ * 2, 0);
    return true;
}

void GpuProfiler::Destroy() {
    if (device_ != nullptr && pool_ != VK_NULL_HANDLE) {
        vkDestroyQueryPool(device_->Handle(), pool_, nullptr);
    }
    pool_ = VK_NULL_HANDLE;
    device_ = nullptr;
    frames_ = {};
    results_.clear();
    openScope_ = -1;
}

void GpuProfiler::BeginFrame(VkCommandBuffer cmd) {
    if (!IsValid()) {
        return;
    }
    frameIndex_ = (frameIndex_ + 1) % kFrameCount;
    Frame& frame = frames_[frameIndex_];

    // Hasil frame yang menempati slot ini dipungut sekarang. Ia disubmit
    // `kFrameCount` frame lalu, jadi GPU hampir pasti sudah selesai — dan
    // "hampir pasti" cukup, karena pembacaannya tanpa WAIT_BIT dan yang belum
    // siap tinggal dilewati.
    Collect(frameIndex_);

    frame.first = frameIndex_ * maxScopes_ * 2;
    frame.count = 0;
    frame.names.clear();
    frame.pending = false;
    openScope_ = -1;

    // Query wajib di-reset sebelum ditulisi. Melewatkannya menghasilkan hasil
    // yang tidak terdefinisi, bukan galat — dan yang terlihat adalah angka yang
    // kadang benar dan kadang mustahil.
    vkCmdResetQueryPool(cmd, pool_, frame.first, maxScopes_ * 2);
}

void GpuProfiler::BeginScope(VkCommandBuffer cmd, std::string_view name) {
    if (!IsValid()) {
        return;
    }
    Frame& frame = frames_[frameIndex_];
    if (frame.count >= maxScopes_ || openScope_ >= 0) {
        return;
    }
    openScope_ = static_cast<int>(frame.count);
    frame.names.emplace_back(name);
    // TOP_OF_PIPE untuk pembuka: penanda ditulis begitu perintah sebelumnya
    // sampai di awal pipeline, yang paling dekat dengan "pass ini mulai".
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool_,
                        frame.first + frame.count * 2);
}

void GpuProfiler::EndScope(VkCommandBuffer cmd) {
    if (!IsValid() || openScope_ < 0) {
        return;
    }
    Frame& frame = frames_[frameIndex_];
    // BOTTOM_OF_PIPE untuk penutup: penanda ditulis setelah seluruh perintah
    // pass ini selesai. Memakai TOP di kedua ujung mengukur jarak antar-perintah
    // di CPU, bukan waktu kerja GPU — dan hasilnya mendekati nol untuk pass
    // yang justru paling berat.
    vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_,
                        frame.first + static_cast<uint32_t>(openScope_) * 2 + 1);
    frame.count = static_cast<uint32_t>(frame.names.size());
    openScope_ = -1;
}

void GpuProfiler::EndFrame(VkCommandBuffer) {
    if (!IsValid()) {
        return;
    }
    frames_[frameIndex_].pending = frames_[frameIndex_].count > 0;
}

void GpuProfiler::Collect(uint32_t frame) {
    Frame& entry = frames_[frame];
    if (!entry.pending || entry.count == 0) {
        return;
    }
    const uint32_t queries = entry.count * 2;
    const VkResult result = vkGetQueryPoolResults(
        device_->Handle(), pool_, entry.first, queries, queries * sizeof(uint64_t),
        readback_.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    entry.pending = false;
    if (result != VK_SUCCESS) {
        // VK_NOT_READY termasuk di sini. Dilewati, bukan ditunggu: hasil yang
        // hilang satu frame tidak berarti apa-apa bagi angka yang dibaca
        // manusia, sedangkan menunggunya mengubah yang diukur.
        return;
    }

    results_.clear();
    results_.reserve(entry.count);
    for (uint32_t i = 0; i < entry.count; ++i) {
        const uint64_t begin = readback_[i * 2];
        const uint64_t end = readback_[i * 2 + 1];
        const double ticks = end >= begin ? static_cast<double>(end - begin) : 0.0;
        results_.push_back(Scope{entry.names[i], ticks * nanosecondsPerTick_ * 1e-6});
    }
}

double GpuProfiler::TotalMilliseconds() const {
    double total = 0.0;
    for (const Scope& scope : results_) {
        total += scope.milliseconds;
    }
    return total;
}

}  // namespace sim::rhi
