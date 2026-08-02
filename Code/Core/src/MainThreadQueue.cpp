#include "Sim/Core/MainThreadQueue.h"

#include "Sim/Core/Assert.h"

namespace sim {

MainThreadQueue& MainThreadQueue::Get() {
    static MainThreadQueue queue;
    return queue;
}

void MainThreadQueue::BindMainThread() {
    std::lock_guard lock(mutex_);
    mainThread_ = std::this_thread::get_id();
}

bool MainThreadQueue::IsMainThread() const {
    std::lock_guard lock(mutex_);
    return mainThread_ == std::this_thread::get_id();
}

void MainThreadQueue::Enqueue(std::function<void()> work) {
    std::lock_guard lock(mutex_);
    pending_.push_back(std::move(work));
}

void MainThreadQueue::Drain() {
    SIM_ASSERT(IsMainThread(), "Drain() called from a non-main thread");

    std::vector<std::function<void()>> batch;
    {
        std::lock_guard lock(mutex_);
        if (draining_) {
            return;
        }
        draining_ = true;
        batch.swap(pending_);
    }

    for (auto& work : batch) {
        work();
    }

    std::lock_guard lock(mutex_);
    draining_ = false;
}

std::size_t MainThreadQueue::PendingCount() const {
    std::lock_guard lock(mutex_);
    return pending_.size();
}

}  // namespace sim
