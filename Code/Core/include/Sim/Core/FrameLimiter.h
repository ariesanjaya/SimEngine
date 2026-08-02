#pragma once

#include <chrono>
#include <cstdint>

namespace sim {

/// Penahan laju frame supaya editor berjalan pada laju tetap.
///
/// Kenapa ini perlu meski vsync sudah aktif: vsync mengunci ke laju refresh
/// monitor, jadi editor akan berjalan 144 fps di monitor 144 Hz dan 60 fps di
/// monitor 60 Hz. Dengan multi-monitor, memindahkan jendela antar-layar akan
/// mengubah laju di tengah sesi. Menguncinya di sini membuat perilaku editor —
/// termasuk kecepatan kamera, animasi UI, dan preview partikel — konsisten di
/// perangkat mana pun.
///
/// Sleep memakai sleep_until sampai menyisakan margin, lalu berputar (spin)
/// untuk sisanya. Sleep saja meleset 1-2 ms di Linux karena granularitas
/// penjadwal; spin saja membakar satu inti CPU penuh.
class FrameLimiter {
public:
    using Clock = std::chrono::steady_clock;

    explicit FrameLimiter(double targetFps = 60.0) { SetTargetFps(targetFps); }

    /// 0 atau negatif berarti tanpa batas (dipakai saat benchmark).
    void SetTargetFps(double fps);
    double TargetFps() const { return targetFps_; }
    bool IsLimited() const { return targetFps_ > 0.0; }

    /// Dipanggil di akhir setiap frame. Mengembalikan delta waktu frame yang
    /// baru saja selesai, dalam detik, sesudah penundaan diterapkan.
    double EndFrame();

    /// Delta terakhir tanpa memaksa penundaan lagi.
    double LastDeltaSeconds() const { return lastDelta_; }
    double SmoothedFps() const { return smoothedFps_; }

private:
    double targetFps_ = 60.0;
    std::chrono::nanoseconds targetPeriod_{0};
    Clock::time_point nextFrame_{};
    Clock::time_point lastFrame_{};
    double lastDelta_ = 0.0;
    double smoothedFps_ = 0.0;
    bool started_ = false;
};

}  // namespace sim
