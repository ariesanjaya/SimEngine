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

    /// Tugas yang diantre sesudah `Stop()` dibuang, bukan disimpan untuk
    /// nanti: tidak akan ada worker yang mengambilnya lagi.
    void Submit(std::function<void()> task);

    /// Menunggu seluruh tugas yang sudah diantre selesai. Dipakai test dan
    /// jalur mematikan editor; bukan sesuatu yang dipanggil per frame.
    void WaitIdle();

    /// Menghentikan worker: yang sedang berjalan diselesaikan, yang masih
    /// mengantre dibuang. Idempoten, dan destruktor memanggilnya juga.
    ///
    /// **Ini harus terjadi sementara pemilik tugasnya masih hidup.** Tugas
    /// yang diantre menangkap `this` milik pemiliknya — bakery tekstur,
    /// database aset, cache thumbnail — dan tidak ada satu pun dari mereka
    /// yang bisa membatalkan tugas yang sudah dikirim. Kalau pemiliknya
    /// dihancurkan lebih dulu, worker meneruskan pekerjaannya di atas memori
    /// yang sudah dibebaskan. Ditemukan persis begitu: sebuah jalan headless
    /// dengan 41 bake tekstur yang masih mengantre segfault di
    /// `std::filesystem::path::operator/` — direktori cache milik bakery yang
    /// sudah tidak ada lagi.
    void Stop();

    /// Penjaga lingkup untuk `Stop()`.
    ///
    /// Dideklarasikan **sesudah** semua yang mengantre pekerjaan ke pool, jadi
    /// ia dihancurkan lebih dulu — termasuk pada jalur `return` lebih awal,
    /// yang justru jalur yang paling mudah terlupakan.
    class StopGuard {
    public:
        explicit StopGuard(TaskPool& pool) : pool_(pool) {}
        StopGuard(const StopGuard&) = delete;
        StopGuard& operator=(const StopGuard&) = delete;
        ~StopGuard() { pool_.Stop(); }

    private:
        TaskPool& pool_;
    };

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
