#include "Sim/Editor/ToolApproval.h"

namespace sim::editor {

bool ToolApprovalGate::Ask(Question question, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopped_) {
        return false;
    }
    // Menunggu giliran. Tenggatnya dihitung dari sini, bukan dari saat
    // pertanyaan ini akhirnya tampil: yang menunggu di seberang adalah sebuah
    // koneksi HTTP, dan ia tidak peduli antreannya panjang karena apa.
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + timeout;
    if (!free_.wait_until(lock, deadline, [this] { return !busy_ || stopped_; })) {
        return false;
    }
    if (stopped_) {
        return false;
    }

    busy_ = true;
    hasAnswer_ = false;
    approved_ = false;
    question_ = std::move(question);

    const bool answered =
        answered_.wait_until(lock, deadline, [this] { return hasAnswer_ || stopped_; });
    // Tenggat habis dan penutupan editor keduanya berarti menolak. Menyetujui
    // sesuatu karena tidak ada yang menjawab adalah kebalikan dari yang diminta
    // mode `ask`.
    const bool result = answered && hasAnswer_ && approved_;

    busy_ = false;
    hasAnswer_ = false;
    question_ = {};
    lock.unlock();
    free_.notify_one();
    return result;
}

bool ToolApprovalGate::Pending(Question& out) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!busy_ || hasAnswer_ || stopped_) {
        return false;
    }
    out = question_;
    return true;
}

void ToolApprovalGate::Answer(bool approved) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!busy_ || hasAnswer_) {
            return;
        }
        approved_ = approved;
        hasAnswer_ = true;
    }
    answered_.notify_all();
}

void ToolApprovalGate::Shutdown() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
    }
    answered_.notify_all();
    free_.notify_all();
}

}  // namespace sim::editor
