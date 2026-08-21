#include "Sim/Core/TaskPool.h"

#include "Sim/Core/Log.h"

#include <algorithm>

namespace sim {

TaskPool::TaskPool(unsigned threadCount) {
    if (threadCount == 0) {
        const unsigned cores = std::max(std::thread::hardware_concurrency(), 2u);
        threadCount = cores - 1;
    }
    workers_.reserve(threadCount);
    for (unsigned i = 0; i < threadCount; ++i) {
        workers_.emplace_back([this]() { WorkerLoop(); });
    }
    SIM_INFO("Core", "Task pool started with {} worker threads", workers_.size());
}

TaskPool::~TaskPool() { Stop(); }

void TaskPool::Stop() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    hasWork_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    // Antrian dikosongkan supaya `Pending()` tidak berbohong sesudah ini, dan
    // penunggu di `WaitIdle()` — kalau ada — dibangunkan alih-alih menunggu
    // tugas yang tidak akan pernah dikerjakan siapa pun.
    const std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    pending_.store(0, std::memory_order_relaxed);
    idle_.notify_all();
}

void TaskPool::Submit(std::function<void()> task) {
    if (!task) {
        return;
    }
    // Dihitung sebelum diantre, bukan di dalam worker. Kalau dinaikkan di
    // worker, ada jendela waktu ketika tugas sudah ada di antrian tapi Pending()
    // masih nol — dan WaitIdle() akan kembali terlalu cepat.
    pending_.fetch_add(1, std::memory_order_relaxed);
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            // Tidak ada lagi yang akan mengambilnya. Menyimpannya di antrian
            // berarti `Pending()` tidak pernah kembali ke nol.
            pending_.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
        queue_.push_back(std::move(task));
    }
    hasWork_.notify_one();
}

void TaskPool::WaitIdle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this]() { return pending_.load(std::memory_order_relaxed) == 0; });
}

void TaskPool::WorkerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            hasWork_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            // **Berhenti berarti berhenti, bukan menghabiskan antrian dulu.**
            // Sempat sebaliknya, supaya berkas yang sedang ditulis tidak
            // ditinggalkan setengah jadi — tetapi yang menjaga itu adalah
            // tugas yang *sedang berjalan* diselesaikan, dan itu tetap
            // berlaku. Tugas yang belum sempat dimulai belum menulis apa pun.
            // Yang dibayar oleh versi lama: menutup editor di tengah 41
            // pemanggangan tekstur 4K yang masih mengantre berarti menunggu
            // semuanya selesai — semenit lebih layar beku — dan semuanya
            // dikerjakan di atas pemilik yang sudah dihancurkan.
            if (stopping_ || queue_.empty()) {
                return;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
        }

        // Pengecualian dari sebuah tugas tidak boleh mematikan worker — kalau
        // ia mati, seluruh impor sesudahnya menggantung tanpa penjelasan.
        try {
            task();
        } catch (const std::exception& exception) {
            SIM_ERROR("Core", "Task threw: {}", exception.what());
        } catch (...) {
            SIM_ERROR("Core", "Task threw an unknown exception");
        }

        if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            // Kunci diambil sebelum notify supaya tidak ada penunggu yang
            // memeriksa pending_ tepat sebelum sinyalnya dikirim lalu tertidur.
            const std::lock_guard<std::mutex> lock(mutex_);
            idle_.notify_all();
        }
    }
}

}  // namespace sim
