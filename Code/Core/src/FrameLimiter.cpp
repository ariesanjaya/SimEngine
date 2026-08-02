#include "Sim/Core/FrameLimiter.h"

#include <algorithm>
#include <thread>

namespace sim {
namespace {

/// Sisa waktu yang diselesaikan dengan spin, bukan sleep. 1,5 ms cukup untuk
/// menutupi meleset penjadwal Linux tanpa membakar CPU berarti.
constexpr std::chrono::nanoseconds kSpinMargin{1'500'000};

}  // namespace

void FrameLimiter::SetTargetFps(double fps) {
    targetFps_ = fps;
    if (fps > 0.0) {
        targetPeriod_ = std::chrono::nanoseconds(static_cast<int64_t>(1'000'000'000.0 / fps));
    } else {
        targetPeriod_ = std::chrono::nanoseconds{0};
    }
    // Jadwal disetel ulang supaya perubahan target tidak menyebabkan satu
    // lonjakan frame panjang atau serentetan frame tanpa penundaan.
    started_ = false;
}

double FrameLimiter::EndFrame() {
    const Clock::time_point now = Clock::now();

    if (!started_) {
        started_ = true;
        lastFrame_ = now;
        nextFrame_ = now + targetPeriod_;
        lastDelta_ = targetFps_ > 0.0 ? 1.0 / targetFps_ : 0.0;
        return lastDelta_;
    }

    if (IsLimited()) {
        if (now < nextFrame_) {
            const Clock::time_point sleepUntil = nextFrame_ - kSpinMargin;
            if (now < sleepUntil) {
                std::this_thread::sleep_until(sleepUntil);
            }
            while (Clock::now() < nextFrame_) {
                std::this_thread::yield();
            }
        }

        nextFrame_ += targetPeriod_;
        // Kalau frame sebelumnya terlalu lama (kompilasi shader, buka dialog),
        // jadwal bisa tertinggal jauh. Tanpa penyesuaian ini editor akan
        // "mengejar" dengan sederet frame tanpa jeda, yang terlihat sebagai
        // sentakan. Lebih baik membuang keterlambatan dan mulai dari sekarang.
        const Clock::time_point after = Clock::now();
        if (nextFrame_ < after) {
            nextFrame_ = after + targetPeriod_;
        }
    }

    const Clock::time_point frameEnd = Clock::now();
    lastDelta_ = std::chrono::duration<double>(frameEnd - lastFrame_).count();
    lastFrame_ = frameEnd;

    const double instantFps = lastDelta_ > 0.0 ? 1.0 / lastDelta_ : 0.0;
    smoothedFps_ = smoothedFps_ == 0.0 ? instantFps : smoothedFps_ * 0.92 + instantFps * 0.08;
    return lastDelta_;
}

}  // namespace sim
