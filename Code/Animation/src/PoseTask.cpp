#include "Sim/Animation/PoseTask.h"

#include "Sim/Animation/Clip.h"

#include <algorithm>

namespace sim::animation {

// --- kolam buffer -------------------------------------------------------------

void PoseBufferPool::Reset(const Skeleton& skeleton) {
    skeleton_ = &skeleton;
    for (Pose& pose : buffers_) {
        pose.Reset(skeleton);
    }
    free_.assign(buffers_.size(), true);
    peak_ = 0;
}

int PoseBufferPool::Acquire() {
    for (std::size_t i = 0; i < free_.size(); ++i) {
        if (free_[i]) {
            free_[i] = false;
            // Bind pose, bukan sisa pemakaian sebelumnya: task bermask hanya
            // menulis sebagian bone, dan sisanya akan mewarisi pose milik
            // gerakan yang tidak ada hubungannya.
            if (skeleton_ != nullptr) {
                buffers_[i].Reset(*skeleton_);
            }
            peak_ = std::max(peak_, InUse());
            return static_cast<int>(i);
        }
    }
    buffers_.emplace_back(skeleton_ != nullptr ? Pose(*skeleton_) : Pose());
    free_.push_back(false);
    peak_ = std::max(peak_, InUse());
    return static_cast<int>(buffers_.size() - 1);
}

void PoseBufferPool::Release(int index) {
    if (index < 0 || index >= static_cast<int>(free_.size())) {
        return;
    }
    free_[static_cast<std::size_t>(index)] = true;
}

Pose& PoseBufferPool::At(int index) {
    return buffers_[static_cast<std::size_t>(index)];
}

const Pose& PoseBufferPool::At(int index) const {
    return buffers_[static_cast<std::size_t>(index)];
}

int PoseBufferPool::InUse() const {
    return static_cast<int>(std::count(free_.begin(), free_.end(), false));
}

// --- task ---------------------------------------------------------------------

int SampleTask::Execute(const TaskContext& context) {
    const int buffer = context.pool->Acquire();
    SampleClip(*clip_, *binding_, *context.skeleton, time_, context.pool->At(buffer));
    return buffer;
}

int BlendTask::Execute(const TaskContext& context) {
    const int sourceBuffer = (*context.results)[static_cast<std::size_t>(dependencies_[0])];
    const int targetBuffer = (*context.results)[static_cast<std::size_t>(dependencies_[1])];
    Pose& source = context.pool->At(sourceBuffer);
    const Pose& target = context.pool->At(targetBuffer);
    if (mask_ != nullptr) {
        Blend(source, target, weight_, *mask_, source);
    } else {
        Blend(source, target, weight_, source);
    }
    // Hasilnya menempati buffer masukan pertama. Yang melepas buffer kedua
    // adalah `TaskSystem`, yang tahu apakah ia masih dibutuhkan task lain.
    return sourceBuffer;
}

int ReferencePoseTask::Execute(const TaskContext& context) {
    const int buffer = context.pool->Acquire();
    context.pool->At(buffer).Reset(*context.skeleton);
    return buffer;
}

// --- sistem -------------------------------------------------------------------

void TaskSystem::Reset(const Skeleton& skeleton) {
    tasks_.clear();
    skeleton_ = &skeleton;
    pool_.Reset(skeleton);
    results_.clear();
    visited_.clear();
    refCount_.clear();
    executed_ = 0;
}

TaskIndex TaskSystem::Register(std::unique_ptr<PoseTask> task) {
    tasks_.push_back(std::move(task));
    return static_cast<TaskIndex>(tasks_.size() - 1);
}

bool TaskSystem::Execute(TaskIndex root, Pose& out, float deltaSeconds) {
    executed_ = 0;
    if (skeleton_ == nullptr) {
        return false;
    }
    if (root < 0 || root >= static_cast<TaskIndex>(tasks_.size())) {
        // Bind pose, bukan pose kosong. Karakter yang runtuh ke titik asal jauh
        // lebih sulit dilacak daripada karakter yang berdiri diam.
        out.Reset(*skeleton_);
        return false;
    }

    const std::size_t count = tasks_.size();
    results_.assign(count, -1);
    visited_.assign(count, 0);
    refCount_.assign(count, 0);

    // Dua lintasan. Yang pertama menghitung **berapa banyak task yang
    // membutuhkan hasil sebuah task**, dan angka itulah yang membuat buffer bisa
    // dilepas tepat waktu: melepasnya sesudah pemakai pertama akan merusak
    // pemakai kedua, dan tidak melepasnya sama sekali membuat kolam tumbuh
    // sebesar seluruh graph.
    CountReferences(root);

    order_.clear();
    Sort(root);

    TaskContext context;
    context.skeleton = skeleton_;
    context.pool = &pool_;
    context.results = &results_;
    context.deltaSeconds = deltaSeconds;

    for (const TaskIndex index : order_) {
        PoseTask& task = *tasks_[static_cast<std::size_t>(index)];
        results_[static_cast<std::size_t>(index)] = task.Execute(context);
        ++executed_;

        // Melepas buffer dependensi yang sudah tidak dibutuhkan siapa pun.
        // Buffer hasil task ini sendiri bisa **sama** dengan salah satu buffer
        // dependensinya — blend menulis ke buffer masukan pertama — jadi yang
        // dilepas harus diperiksa terhadapnya.
        for (const TaskIndex dependency : task.Dependencies()) {
            const auto slot = static_cast<std::size_t>(dependency);
            if (--refCount_[slot] > 0) {
                continue;
            }
            const int buffer = results_[slot];
            if (buffer >= 0 && buffer != results_[static_cast<std::size_t>(index)]) {
                pool_.Release(buffer);
            }
        }
    }

    const int resultBuffer = results_[static_cast<std::size_t>(root)];
    if (resultBuffer < 0) {
        out.Reset(*skeleton_);
        return false;
    }
    out = pool_.At(resultBuffer);
    pool_.Release(resultBuffer);
    return true;
}

void TaskSystem::CountReferences(TaskIndex index) {
    const auto slot = static_cast<std::size_t>(index);
    // Sebuah task boleh dirujuk lebih dari satu task lain — itulah yang membuat
    // satu klip yang dicuplik sekali bisa masuk ke dua blend tanpa dicuplik dua
    // kali. Penelusurannya berhenti pada kunjungan kedua supaya anak-anaknya
    // tidak ikut dihitung berulang.
    const bool first = visited_[slot] == 0;
    visited_[slot] = 1;
    if (!first) {
        return;
    }
    for (const TaskIndex dependency : tasks_[slot]->Dependencies()) {
        if (dependency < 0 || dependency >= static_cast<TaskIndex>(tasks_.size())) {
            continue;
        }
        ++refCount_[static_cast<std::size_t>(dependency)];
        CountReferences(dependency);
    }
}

void TaskSystem::Sort(TaskIndex index) {
    const auto slot = static_cast<std::size_t>(index);
    if (visited_[slot] == 2) {
        return;
    }
    visited_[slot] = 2;
    for (const TaskIndex dependency : tasks_[slot]->Dependencies()) {
        if (dependency < 0 || dependency >= static_cast<TaskIndex>(tasks_.size())) {
            continue;
        }
        Sort(dependency);
    }
    order_.push_back(index);
}

}  // namespace sim::animation
