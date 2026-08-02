#pragma once

#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

namespace sim {

/// Antrian pekerjaan yang dijamin dieksekusi di main thread.
///
/// Ini seam #5 di docs/ARCHITECTURE.md. Semua yang datang dari luar main thread
/// — hasil import aset, event file watcher, dan setiap request MCP — masuk lewat
/// sini. Tanpa ini, handler MCP akan menyentuh World/ImGui/Vulkan dari thread
/// jaringan, dan bug seperti itu muncul sebagai crash acak yang sangat mahal
/// dilacak.
///
/// Drain() dipanggil sekali per frame pada titik tetap: setelah event diproses,
/// sebelum panel menggambar. Titiknya harus tetap supaya perilaku editor
/// deterministik terhadap urutan request agen.
class MainThreadQueue {
public:
    static MainThreadQueue& Get();

    /// Dipanggil sekali dari main() sebelum thread lain berjalan.
    void BindMainThread();
    bool IsMainThread() const;

    /// Menjalankan `work` di main thread dan mengembalikan future hasilnya.
    /// Bila dipanggil dari main thread saat sedang Drain(), pekerjaan tetap
    /// diantrikan (tidak dijalankan langsung) supaya urutannya tidak terbalik.
    template <class F>
    auto Submit(F&& work) -> std::future<std::invoke_result_t<F>> {
        using Result = std::invoke_result_t<F>;
        auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<F>(work));
        std::future<Result> future = task->get_future();
        Enqueue([task]() { (*task)(); });
        return future;
    }

    /// Jalankan semua pekerjaan yang tertunda. Hanya boleh dari main thread.
    /// Pekerjaan yang mengantri selama Drain() ditunda ke frame berikutnya,
    /// supaya satu request yang mengantrikan dirinya sendiri tidak menggantung
    /// frame selamanya.
    void Drain();

    std::size_t PendingCount() const;

private:
    void Enqueue(std::function<void()> work);

    mutable std::mutex mutex_;
    std::vector<std::function<void()>> pending_;
    std::thread::id mainThread_{};
    bool draining_ = false;
};

}  // namespace sim
