#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace sim {

/// Kolam thread untuk pekerjaan latar yang tidak boleh menahan frame.
///
/// Dipakai impor aset dan pembuatan thumbnail: keduanya membaca berkas dan
/// mendekode gambar, pekerjaan yang bisa memakan puluhan milidetik dan akan
/// terlihat sebagai frame yang tersendat kalau dijalankan di main thread.
///
/// Sengaja sesederhana ini — satu antrian, tanpa work stealing, tanpa
/// prioritas. Beban di fase editor adalah puluhan tugas berdurasi menengah,
/// bukan jutaan tugas mikro, dan penjadwal yang lebih pintar hanya menambah
/// permukaan yang bisa salah tanpa mempercepat apa pun yang terukur.
///
/// Hasil pekerjaan tidak boleh langsung menyentuh state editor. Jalur baliknya
/// adalah MainThreadQueue (seam #5 di docs/ARCHITECTURE.md).
class TaskPool {
public:
    /// `threadCount` nol berarti pilih sendiri: satu lebih sedikit daripada
    /// jumlah inti, supaya main thread tetap punya inti untuk dirinya.
    explicit TaskPool(unsigned threadCount = 0);
    ~TaskPool();

    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;

    void Submit(std::function<void()> task);

    /// Menunggu seluruh tugas yang sudah diantre selesai. Dipakai test dan
    /// jalur mematikan editor; bukan sesuatu yang dipanggil per frame.
    void WaitIdle();

    std::size_t ThreadCount() const { return workers_.size(); }

    /// Jumlah tugas yang belum selesai — yang masih di antrian maupun yang
    /// sedang dikerjakan. Ditampilkan status bar sebagai penanda impor.
    std::size_t Pending() const { return pending_.load(std::memory_order_relaxed); }

private:
    void WorkerLoop();

    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> queue_;
    mutable std::mutex mutex_;
    std::condition_variable hasWork_;
    std::condition_variable idle_;
    std::atomic<std::size_t> pending_{0};
    bool stopping_ = false;
};

}  // namespace sim
