#pragma once

#include "Sim/Animation/AnimationGraph.h"

#include <string>
#include <vector>

namespace sim::animation {

/// Sumber klip bagi sebuah graph, dicari lewat GUID.
///
/// **Sebuah antarmuka, bukan `AssetDatabase`.** Modul animasi tidak boleh tahu
/// apa pun tentang basis data aset: runtime yang memuat dari paket dan editor
/// yang memuat dari folder proyek harus bisa memutar graph yang sama, dan
/// menjahitkan salah satunya ke dalam modul ini akan memaksa yang lain memalsukan
/// dirinya.
class ClipLibrary {
public:
    virtual ~ClipLibrary() = default;
    virtual const Clip* Find(const Uuid& guid) const = 0;
};

/// Event yang menyala pada satu pembaruan, beserta asalnya.
struct FiredEvent {
    /// Nama event pada klipnya. Inilah yang diteruskan ke Lua.
    std::string name;
    int layer = 0;
    /// Bobot lapis dan klip yang menyalakannya. Event dari klip yang nyaris
    /// tidak terdengar tetap dilaporkan beserta bobotnya, bukan disaring diam-
    /// diam: ambang yang benar bergantung pada apa yang dilakukan event itu, dan
    /// hanya pemanggil yang tahu.
    float weight = 1.0f;
};

/// Satu pemutaran graph.
///
/// **Terpisah dari `AnimationGraph`.** Graph adalah dokumen — dibaca dari berkas
/// dan dibagi ke seluruh karakter yang memakainya. Yang berubah tiap frame
/// adalah state yang sedang aktif dan waktunya, dan itu milik masing-masing
/// karakter. Menyimpannya di dalam graph berarti seratus musuh yang berbagi satu
/// pemutaran.
class GraphInstance {
public:
    /// Memasang graph ke sebuah rangka dan pustaka klip. Semua pemetaan nama →
    /// indeks dihitung di sini, sekali.
    void Bind(const AnimationGraph& graph, const Skeleton& skeleton, const ClipLibrary& clips);
    bool Bound() const { return graph_ != nullptr; }

    /// Nilai parameter yang menggerakkan pemutaran ini. Disalin dari graph saat
    /// `Bind`, lalu menjadi milik instance — dua karakter yang memakai graph yang
    /// sama harus bisa berlari dengan kecepatan berbeda.
    ParameterSet parameters;

    void Update(float deltaSeconds);
    /// Melompat ke sebuah state tanpa crossfade. Dipakai saat memulai dan saat
    /// panel meminta pratinjau satu state tertentu.
    void Play(int layer, int state);

    const Pose& Result() const { return result_; }
    /// Event yang menyala pada `Update` terakhir.
    const std::vector<FiredEvent>& Events() const { return fired_; }
    /// Perpindahan root pada `Update` terakhir, dari lapis dasar.
    const BoneTransform& RootMotion() const { return rootMotion_; }

    int CurrentState(int layer) const;
    float CurrentTime(int layer) const;
    /// 0 kalau tidak sedang berpindah, selain itu kemajuan crossfade 0..1.
    float TransitionProgress(int layer) const;

private:
    /// Satu klip yang sedang diputar di dalam sebuah state.
    struct ActiveClip {
        const Clip* clip = nullptr;
        ClipBinding binding;
        float weight = 0.0f;
        float speed = 1.0f;
    };

    struct Playback {
        int state = -1;
        /// Waktu ternormalisasi 0..1, bukan detik.
        ///
        /// **Blend tree mencampur klip berdurasi berbeda**, jadi "detik ke
        /// berapa" tidak punya arti tunggal di dalam sebuah state. Yang punya
        /// arti adalah seberapa jauh state itu berjalan; waktu tiap klip
        /// diturunkan darinya, lewat penanda fase kalau ada.
        float normalized = 0.0f;
        float previousNormalized = 0.0f;
    };

    struct LayerState {
        Playback current;
        Playback previous;
        float transitionElapsed = 0.0f;
        float transitionDuration = 0.0f;
        BoneMask mask;
    };

    void ResolveMotion(const Motion& motion, std::vector<ActiveClip>& out) const;
    float StateDuration(int layer, const Playback& playback) const;
    void SamplePlayback(int layer, const Playback& playback, Pose& out, bool collectEvents,
                        float eventWeight);
    bool TryTransition(int layer, LayerState& state);

    const AnimationGraph* graph_ = nullptr;
    const Skeleton* skeleton_ = nullptr;
    const ClipLibrary* clips_ = nullptr;

    std::vector<LayerState> layers_;
    Pose result_;
    Pose scratchA_;
    Pose scratchB_;
    Pose scratchC_;
    Pose bindPose_;
    std::vector<FiredEvent> fired_;
    BoneTransform rootMotion_;

    /// Buffer kerja yang dipakai ulang tiap frame — alasan yang sama dengan
    /// buffer di `Pose`: jalur ini berjalan tiap frame per karakter.
    mutable std::vector<ActiveClip> activeScratch_;
    mutable std::vector<float> weightScratch_;
    mutable std::vector<float> positionScratch_;
    mutable std::vector<Vec2> position2DScratch_;
    std::vector<const Event*> eventScratch_;
};

}  // namespace sim::animation
