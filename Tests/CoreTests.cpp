#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Core/FrameLimiter.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/MainThreadQueue.h"
#include "Sim/Core/Math.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

TEST_CASE("LogRing menyimpan entri terbaru dan membuang yang terlama") {
    sim::LogRing& ring = sim::LogRing::Get();
    ring.Clear();
    ring.SetCapacity(4);

    for (int i = 0; i < 10; ++i) {
        ring.Push(spdlog::level::info, "Test", "pesan " + std::to_string(i));
    }

    const std::vector<sim::LogEntry> entries = ring.Snapshot();
    REQUIRE(entries.size() == 4);
    CHECK(entries.front().message == "pesan 6");
    CHECK(entries.back().message == "pesan 9");
    // Sequence bersifat global dan terus naik, dipakai panel Console untuk
    // mendeteksi ada log baru tanpa membandingkan isi.
    CHECK(entries.back().sequence > entries.front().sequence);

    ring.SetCapacity(8192);
    ring.Clear();
}

TEST_CASE("LogRing aman ditulis banyak thread sambil dibaca") {
    // Inilah jalur yang dipakai panel Console: penulis datang dari thread job
    // import, file watcher, dan nanti server MCP, sementara pembacanya adalah
    // main thread yang menggambar UI. Dijalankan di bawah TSan lewat preset
    // linux-clang-tsan untuk membuktikan tidak ada race.
    sim::LogRing& ring = sim::LogRing::Get();
    ring.Clear();
    ring.SetCapacity(256);
    // Sequence sengaja monoton global dan tidak di-reset Clear(), jadi yang
    // diperiksa adalah selisihnya, bukan nilai absolutnya.
    const uint64_t sequenceBefore = ring.Sequence();

    constexpr int kWriters = 4;
    constexpr int kPerWriter = 500;
    std::atomic<bool> stop{false};

    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (int w = 0; w < kWriters; ++w) {
        writers.emplace_back([w, &ring]() {
            for (int i = 0; i < kPerWriter; ++i) {
                ring.Push(spdlog::level::info, "Worker" + std::to_string(w),
                          "message " + std::to_string(i));
            }
        });
    }

    // Pembaca meniru panel: mengambil salinan berulang kali selama penulis
    // masih bekerja.
    std::size_t observed = 0;
    std::thread reader([&ring, &stop, &observed]() {
        while (!stop.load(std::memory_order_relaxed)) {
            observed = std::max(observed, ring.Snapshot().size());
        }
    });

    for (std::thread& writer : writers) {
        writer.join();
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();

    // Kapasitas ditegakkan meski ditulis serentak, dan urutan global tetap utuh.
    CHECK(ring.Snapshot().size() == 256);
    CHECK(ring.Sequence() - sequenceBefore == kWriters * kPerWriter);
    CHECK(observed <= 256);

    ring.SetCapacity(8192);
    ring.Clear();
}

TEST_CASE("MainThreadQueue menjalankan pekerjaan dari thread lain di main thread") {
    sim::MainThreadQueue& queue = sim::MainThreadQueue::Get();
    queue.BindMainThread();

    const std::thread::id mainThread = std::this_thread::get_id();
    std::atomic<bool> submitted{false};
    std::future<std::thread::id> future;

    std::thread worker([&]() {
        future = queue.Submit([]() { return std::this_thread::get_id(); });
        submitted = true;
    });
    worker.join();

    REQUIRE(submitted.load());
    CHECK(queue.PendingCount() == 1);

    queue.Drain();
    CHECK(future.get() == mainThread);
    CHECK(queue.PendingCount() == 0);
}

TEST_CASE("Pekerjaan yang mengantri saat Drain ditunda ke putaran berikutnya") {
    sim::MainThreadQueue& queue = sim::MainThreadQueue::Get();
    queue.BindMainThread();

    int order = 0;
    int innerOrder = -1;
    queue.Submit([&]() {
        ++order;
        queue.Submit([&]() { innerOrder = ++order; });
    });

    queue.Drain();
    // Tanpa penundaan ini, sebuah pekerjaan yang mengantrikan dirinya sendiri
    // akan menggantung frame selamanya.
    CHECK(innerOrder == -1);
    CHECK(queue.PendingCount() == 1);

    queue.Drain();
    CHECK(innerOrder == 2);
}

TEST_CASE("FrameLimiter menahan frame mendekati periode target") {
    sim::FrameLimiter limiter(120.0);
    CHECK(limiter.IsLimited());

    limiter.EndFrame();  // frame pertama hanya menyetel jadwal

    const auto start = std::chrono::steady_clock::now();
    constexpr int kFrames = 6;
    for (int i = 0; i < kFrames; ++i) {
        limiter.EndFrame();
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    const double expected = kFrames / 120.0;
    // Batas bawah ketat (penahanan memang harus terjadi), batas atas longgar
    // karena penjadwal mesin CI bisa meleset.
    CHECK(elapsed >= expected * 0.85);
    CHECK(elapsed <= expected * 3.0);
}

TEST_CASE("FrameLimiter tanpa batas tidak menunda") {
    sim::FrameLimiter limiter(0.0);
    CHECK_FALSE(limiter.IsLimited());

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        limiter.EndFrame();
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    CHECK(elapsed < 0.05);
}

TEST_CASE("Proyeksi perspektif memakai konvensi Vulkan") {
    const sim::Mat4 proj = sim::Perspective(60.0f * sim::kDegToRad, 16.0f / 9.0f, 0.1f, 100.0f);

    // Y dibalik dibanding OpenGL — kalau ini pernah berubah, seluruh UI dan
    // gizmo akan tampak terbalik secara vertikal.
    CHECK(proj[1][1] < 0.0f);

    // GLM_FORCE_DEPTH_ZERO_TO_ONE: titik di bidang dekat harus memetakan ke z=0.
    const sim::Vec4 nearPoint = proj * sim::Vec4(0.0f, 0.0f, -0.1f, 1.0f);
    CHECK(nearPoint.z / nearPoint.w == doctest::Approx(0.0f).epsilon(0.001));
}
