#pragma once

#include "Sim/Animation/Pose.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace sim::animation {

class Clip;
class ClipBinding;

/// Indeks task di dalam satu daftar. −1 berarti "tidak ada".
using TaskIndex = int32_t;
inline constexpr TaskIndex kInvalidTask = -1;

/// Kolam buffer pose yang dipakai ulang selama satu eksekusi.
///
/// **Yang dipakai ulang adalah buffernya, bukan pose-nya.** Sebuah graph dengan
/// belasan blend menghasilkan belasan pose antara per frame per karakter; kalau
/// masing-masing memiliki `std::vector` sendiri, itu belasan alokasi per frame
/// per karakter. Kolam ini mengalokasi sekali lalu menyerahkan indeks.
class PoseBufferPool {
public:
    /// Menyiapkan kolam untuk sebuah rangka. Buffer yang sudah ada dipakai ulang.
    void Reset(const Skeleton& skeleton);

    /// Mengambil sebuah buffer. Isinya **bind pose**, bukan sisa pemakaian
    /// sebelumnya: task yang hanya menulis sebagian bone — mis. yang bermask —
    /// akan mewarisi sampah dari task yang tidak ada hubungannya, dan yang
    /// terlihat adalah bone yang sesekali meloncat ke pose milik gerakan lain.
    int Acquire();
    /// Mengembalikan buffer ke kolam.
    void Release(int index);

    Pose& At(int index);
    const Pose& At(int index) const;

    int Capacity() const { return static_cast<int>(buffers_.size()); }
    /// Banyaknya buffer yang sedang dipegang seseorang. Dipakai uji dan panel:
    /// angka yang tidak pernah turun berarti ada yang lupa dilepas.
    int InUse() const;
    /// Puncak pemakaian sejak `Reset`. Inilah angka yang menentukan berapa
    /// banyak memori yang sesungguhnya dibutuhkan sebuah graph.
    int PeakInUse() const { return peak_; }

private:
    std::vector<Pose> buffers_;
    std::vector<bool> free_;
    const Skeleton* skeleton_ = nullptr;
    int peak_ = 0;
};

/// Segala yang dibutuhkan sebuah task saat dijalankan.
struct TaskContext {
    const Skeleton* skeleton = nullptr;
    PoseBufferPool* pool = nullptr;
    /// Buffer hasil tiap task yang sudah berjalan, dialamati indeks task.
    std::vector<int>* results = nullptr;
    float deltaSeconds = 0.0f;
};

/// Satu langkah pembentukan pose.
///
/// **Ini inti rancangan yang diikuti dari Esoterica**, dan yang membedakannya
/// dari runtime yang mencampur pose langsung di dalam simpulnya. Simpul graph
/// tidak menghasilkan pose; ia **mendaftarkan task** dan mengembalikan indeksnya.
/// Yang didapat dari pemisahan itu ada tiga, dan ketiganya tidak mungkin
/// didapat kalau pencampuran terjadi di dalam simpul:
///
/// - Pembaruan graph menjadi murah dan bebas pose. Simpul yang bobotnya nol
///   tidak pernah mendaftarkan task, jadi cabang yang tidak terlihat tidak
///   pernah dicuplik — sementara runtime yang mencampur di tempat sudah terlanjur
///   mencuplik sebelum tahu bobotnya nol.
/// - Daftar task bisa dijalankan pada rangka LOD yang berbeda, atau tidak
///   dijalankan sama sekali untuk karakter di kejauhan, tanpa menyentuh graph.
/// - Daftar task adalah data. Ia bisa diserialisasi, dikirim lewat jaringan,
///   atau direkam untuk diputar ulang saat mencari sebab sebuah pose yang salah.
class PoseTask {
public:
    virtual ~PoseTask() = default;

    /// Task yang harus sudah berjalan sebelum yang ini. Paling banyak dua pada
    /// seluruh task yang ada sekarang — blend adalah operasi biner.
    const std::vector<TaskIndex>& Dependencies() const { return dependencies_; }

    /// Menjalankan task. Mengembalikan indeks buffer pose hasilnya.
    ///
    /// Buffer masukan **dilepas oleh pemanggil**, bukan oleh task: task tidak
    /// tahu apakah hasilnya masih dibutuhkan task lain, dan yang tahu hanyalah
    /// yang memegang seluruh daftarnya.
    virtual int Execute(const TaskContext& context) = 0;

    /// Nama untuk debug. Bukan `std::string`: task dibuat puluhan kali per frame.
    virtual const char* Name() const = 0;

protected:
    explicit PoseTask(std::vector<TaskIndex> dependencies = {})
        : dependencies_(std::move(dependencies)) {}

    std::vector<TaskIndex> dependencies_;
};

/// Mencuplik sebuah klip pada satu waktu.
class SampleTask final : public PoseTask {
public:
    SampleTask(const Clip& clip, const ClipBinding& binding, float time)
        : clip_(&clip), binding_(&binding), time_(time) {}

    int Execute(const TaskContext& context) override;
    const char* Name() const override { return "Sample"; }

private:
    const Clip* clip_;
    const ClipBinding* binding_;
    float time_;
};

/// Mencampur hasil dua task.
///
/// **Hasilnya ditulis ke buffer masukan pertama, bukan ke buffer ketiga.**
/// `Blend` memang mengizinkan `out` sama dengan `a`, dan memakainya menghemat
/// satu buffer per blend — pada rantai blend yang dalam, itu selisih antara
/// pemakaian kolam yang tumbuh linear dan yang tetap.
class BlendTask final : public PoseTask {
public:
    BlendTask(TaskIndex source, TaskIndex target, float weight, const BoneMask* mask = nullptr)
        : PoseTask({source, target}), weight_(weight), mask_(mask) {}

    int Execute(const TaskContext& context) override;
    const char* Name() const override { return "Blend"; }

private:
    float weight_;
    const BoneMask* mask_;
};

/// Pose bind rangka. Dipakai simpul yang harus menghasilkan sesuatu tanpa klip.
class ReferencePoseTask final : public PoseTask {
public:
    int Execute(const TaskContext& context) override;
    const char* Name() const override { return "ReferencePose"; }
};

/// Daftar task satu frame, beserta eksekusinya.
///
/// **Sebuah DAG, bukan sebuah tumpukan.** Task dijalankan sesudah seluruh
/// dependensinya, dan sebuah task boleh dipakai lebih dari satu task lain —
/// itulah yang membuat satu klip yang dicuplik sekali bisa masuk ke dua blend
/// tanpa dicuplik dua kali.
class TaskSystem {
public:
    void Reset(const Skeleton& skeleton);

    /// Mendaftarkan sebuah task dan mengembalikan indeksnya.
    TaskIndex Register(std::unique_ptr<PoseTask> task);

    int TaskCount() const { return static_cast<int>(tasks_.size()); }
    const PoseTask& At(TaskIndex index) const { return *tasks_[static_cast<std::size_t>(index)]; }

    /// Menjalankan seluruh task yang dibutuhkan `root`, lalu menyalin hasilnya
    /// ke `out`.
    ///
    /// **Hanya yang dibutuhkan.** Task yang tidak menyumbang ke `root` tidak
    /// dijalankan sama sekali: simpul yang mendaftarkan task lalu dibuang oleh
    /// blend berbobot nol tidak boleh membayar pencuplikannya.
    ///
    /// `root` yang tidak sah menghasilkan bind pose — bukan pose kosong, karena
    /// karakter yang runtuh ke titik asal jauh lebih sulit dilacak daripada
    /// karakter yang berdiri di bind pose.
    bool Execute(TaskIndex root, Pose& out, float deltaSeconds);

    PoseBufferPool& Pool() { return pool_; }
    const PoseBufferPool& Pool() const { return pool_; }

    /// Banyaknya task yang benar-benar dijalankan pada `Execute` terakhir.
    int ExecutedCount() const { return executed_; }

private:
    /// Menghitung berapa banyak task yang membutuhkan hasil tiap task.
    void CountReferences(TaskIndex index);
    /// Urutan topologis: dependensi selalu mendahului yang membutuhkannya.
    void Sort(TaskIndex index);

    std::vector<std::unique_ptr<PoseTask>> tasks_;
    PoseBufferPool pool_;
    const Skeleton* skeleton_ = nullptr;
    std::vector<int> results_;
    std::vector<uint8_t> visited_;
    std::vector<int> refCount_;
    std::vector<TaskIndex> order_;
    int executed_ = 0;
};

}  // namespace sim::animation
