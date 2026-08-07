#include "Sim/Animation/GraphInstance.h"

#include <algorithm>
#include <cmath>

namespace sim::animation {

void GraphInstance::Bind(const AnimationGraph& graph, const Skeleton& skeleton,
                         const ClipLibrary& clips) {
    graph_ = &graph;
    skeleton_ = &skeleton;
    clips_ = &clips;
    parameters = graph.parameters;

    bindPose_.Reset(skeleton);
    result_ = bindPose_;
    fired_.clear();
    rootMotion_ = BoneTransform{};

    layers_.assign(static_cast<std::size_t>(graph.LayerCount()), LayerState{});
    for (int i = 0; i < graph.LayerCount(); ++i) {
        const Layer& layer = graph.LayerAt(i);
        LayerState& state = layers_[static_cast<std::size_t>(i)];
        if (!layer.maskRootBone.empty()) {
            const int root = skeleton.Find(layer.maskRootBone);
            if (root >= 0) {
                state.mask.Reset(skeleton.BoneCount(), 0.0f);
                state.mask.SetChainWeight(skeleton, root, 1.0f);
            }
        }
        Play(i, layer.defaultState);
    }
}

void GraphInstance::Play(int layer, int state) {
    if (layer < 0 || layer >= static_cast<int>(layers_.size())) {
        return;
    }
    LayerState& live = layers_[static_cast<std::size_t>(layer)];
    live.current.state = state;
    live.current.normalized = 0.0f;
    live.current.previousNormalized = 0.0f;
    live.previous.state = -1;
    live.transitionElapsed = 0.0f;
    live.transitionDuration = 0.0f;
}

int GraphInstance::CurrentState(int layer) const {
    if (layer < 0 || layer >= static_cast<int>(layers_.size())) {
        return -1;
    }
    return layers_[static_cast<std::size_t>(layer)].current.state;
}

float GraphInstance::CurrentTime(int layer) const {
    if (layer < 0 || layer >= static_cast<int>(layers_.size())) {
        return 0.0f;
    }
    return layers_[static_cast<std::size_t>(layer)].current.normalized;
}

float GraphInstance::TransitionProgress(int layer) const {
    if (layer < 0 || layer >= static_cast<int>(layers_.size())) {
        return 0.0f;
    }
    const LayerState& live = layers_[static_cast<std::size_t>(layer)];
    if (live.previous.state < 0 || live.transitionDuration <= 0.0f) {
        return 0.0f;
    }
    return std::clamp(live.transitionElapsed / live.transitionDuration, 0.0f, 1.0f);
}

void GraphInstance::ResolveMotion(const Motion& motion, std::vector<ActiveClip>& out) const {
    out.clear();
    if (clips_ == nullptr || skeleton_ == nullptr) {
        return;
    }

    const auto push = [&](const AssetRef& ref, float weight, float speed) {
        if (weight <= 0.0f) {
            return;
        }
        const Clip* clip = clips_->Find(ref.guid);
        if (clip == nullptr) {
            return;
        }
        ActiveClip active;
        active.clip = clip;
        active.binding.Bind(*clip, *skeleton_);
        active.weight = weight;
        active.speed = speed;
        out.push_back(std::move(active));
    };

    switch (motion.kind) {
        case MotionKind::Clip:
            push(motion.clip, 1.0f, motion.speed);
            return;
        case MotionKind::Blend1D: {
            positionScratch_.clear();
            for (const MotionChild& child : motion.children) {
                positionScratch_.push_back(child.position.x);
            }
            const int index = parameters.Find(motion.parameterX);
            Blend1DWeights(positionScratch_, parameters.Float(index), weightScratch_);
            break;
        }
        case MotionKind::Blend2D: {
            position2DScratch_.clear();
            for (const MotionChild& child : motion.children) {
                position2DScratch_.push_back(child.position);
            }
            const Vec2 value(parameters.Float(parameters.Find(motion.parameterX)),
                             parameters.Float(parameters.Find(motion.parameterY)));
            Blend2DWeights(position2DScratch_, value, weightScratch_);
            break;
        }
    }

    for (std::size_t i = 0; i < motion.children.size() && i < weightScratch_.size(); ++i) {
        push(motion.children[i].clip, weightScratch_[i], motion.children[i].speed * motion.speed);
    }
}

float GraphInstance::StateDuration(int layer, const Playback& playback) const {
    if (graph_ == nullptr || playback.state < 0) {
        return 1.0f;
    }
    const Layer& description = graph_->LayerAt(layer);
    if (playback.state >= static_cast<int>(description.states.size())) {
        return 1.0f;
    }
    const Motion& motion = description.states[static_cast<std::size_t>(playback.state)].motion;
    ResolveMotion(motion, activeScratch_);
    if (activeScratch_.empty()) {
        return 1.0f;
    }
    // Durasi state adalah rata-rata durasi klipnya yang ditimbang bobot, bukan
    // durasi klip pertama. Blend tree yang mencampur jalan (1,2 s) dengan lari
    // (0,7 s) harus mempercepat langkahnya secara mulus saat bobotnya bergeser —
    // memakai salah satu durasi saja membuat langkahnya melompat tepat pada saat
    // bobotnya berpindah dominan.
    float duration = 0.0f;
    float total = 0.0f;
    for (const ActiveClip& active : activeScratch_) {
        const float speed = active.speed != 0.0f ? std::abs(active.speed) : 1.0f;
        duration += (active.clip->duration / speed) * active.weight;
        total += active.weight;
    }
    return total > 0.0f ? std::max(duration / total, 1e-6f) : 1.0f;
}

void GraphInstance::SamplePlayback(int layer, const Playback& playback, Pose& out,
                                   bool collectEvents, float eventWeight) {
    out = bindPose_;
    if (graph_ == nullptr || skeleton_ == nullptr || playback.state < 0) {
        return;
    }
    const Layer& description = graph_->LayerAt(layer);
    if (playback.state >= static_cast<int>(description.states.size())) {
        return;
    }
    const State& state = description.states[static_cast<std::size_t>(playback.state)];
    ResolveMotion(state.motion, activeScratch_);
    if (activeScratch_.empty()) {
        return;
    }

    // Klip pertama yang berbobot menjadi acuan fase. Memilih yang berbobot
    // terbesar akan membuat acuannya berpindah di tengah blend, dan seluruh klip
    // lain melompat fase pada saat itu.
    const Clip* reference = activeScratch_.front().clip;
    float accumulated = 0.0f;
    for (std::size_t i = 0; i < activeScratch_.size(); ++i) {
        const ActiveClip& active = activeScratch_[i];
        float time = playback.normalized * active.clip->duration;
        if (state.motion.syncPhase && i > 0 && reference != nullptr) {
            time = reference->MatchPhase(playback.normalized * reference->duration, *active.clip);
        }

        Pose& target = i == 0 ? out : scratchC_;
        SampleClip(*active.clip, active.binding, *skeleton_, time, target);
        if (i > 0) {
            accumulated += active.weight;
            // Bobot yang dipakai adalah bagian klip ini terhadap yang sudah
            // terkumpul, bukan bobot mentahnya: mencampur berurutan dengan bobot
            // mentah membuat klip yang datang belakangan selalu menang.
            const float share = accumulated > 0.0f ? active.weight / accumulated : 0.0f;
            Blend(out, target, share, out);
        } else {
            accumulated = active.weight;
        }

        if (!collectEvents) {
            continue;
        }
        const float from = playback.previousNormalized * active.clip->duration;
        const float to = playback.normalized * active.clip->duration;
        active.clip->CollectEvents(from, to, eventScratch_);
        for (const Event* event : eventScratch_) {
            fired_.push_back(FiredEvent{event->name, layer, eventWeight * active.weight});
        }
    }
}

bool GraphInstance::TryTransition(int layer, LayerState& live) {
    const Layer& description = graph_->LayerAt(layer);
    for (const Transition& transition : description.transitions) {
        if (transition.from >= 0 && transition.from != live.current.state) {
            continue;
        }
        if (transition.to == live.current.state) {
            continue;
        }
        if (transition.to < 0 || transition.to >= static_cast<int>(description.states.size())) {
            continue;
        }
        if (transition.hasExitTime && live.current.normalized < transition.exitTime) {
            continue;
        }
        bool satisfied = true;
        for (const Condition& condition : transition.conditions) {
            if (!Evaluate(condition, parameters)) {
                satisfied = false;
                break;
            }
        }
        if (!satisfied) {
            continue;
        }

        live.previous = live.current;
        live.current.state = transition.to;
        live.current.normalized = 0.0f;
        live.current.previousNormalized = 0.0f;
        live.transitionElapsed = 0.0f;
        live.transitionDuration = std::max(transition.duration, 0.0f);
        if (live.transitionDuration <= 0.0f) {
            live.previous.state = -1;
        }
        return true;
    }
    return false;
}

void GraphInstance::Update(float deltaSeconds) {
    fired_.clear();
    rootMotion_ = BoneTransform{};
    if (graph_ == nullptr || skeleton_ == nullptr) {
        return;
    }
    const float dt = std::max(deltaSeconds, 0.0f);

    for (int i = 0; i < static_cast<int>(layers_.size()); ++i) {
        LayerState& live = layers_[static_cast<std::size_t>(i)];
        if (live.current.state < 0) {
            continue;
        }

        // Waktu dimajukan lebih dulu, baru transisi diuji. Urutan sebaliknya
        // membuat `exitTime` selalu terlambat satu frame — dan pada klip pendek,
        // satu frame adalah selisih yang terlihat.
        const float duration = StateDuration(i, live.current);
        live.current.previousNormalized = live.current.normalized;
        live.current.normalized += dt / duration;
        if (live.current.normalized >= 1.0f) {
            live.current.normalized = std::fmod(live.current.normalized, 1.0f);
        }
        if (live.previous.state >= 0) {
            const float previousDuration = StateDuration(i, live.previous);
            live.previous.previousNormalized = live.previous.normalized;
            live.previous.normalized += dt / previousDuration;
            if (live.previous.normalized >= 1.0f) {
                live.previous.normalized = std::fmod(live.previous.normalized, 1.0f);
            }
            live.transitionElapsed += dt;
            if (live.transitionElapsed >= live.transitionDuration) {
                live.previous.state = -1;
            }
        }

        TryTransition(i, live);
    }

    // Trigger dipadamkan **setelah** seluruh lapis mengevaluasi transisinya.
    // Dipadamkan per lapis, satu trigger hanya akan terlihat lapis pertama —
    // dan lapis atas yang seharusnya ikut bereaksi diam saja.
    parameters.ConsumeTriggers();

    for (int i = 0; i < static_cast<int>(layers_.size()); ++i) {
        LayerState& live = layers_[static_cast<std::size_t>(i)];
        const Layer& description = graph_->LayerAt(i);
        const float layerWeight = std::clamp(description.weight, 0.0f, 1.0f);
        if (live.current.state < 0 || layerWeight <= 0.0f) {
            continue;
        }

        const float progress = TransitionProgress(i);
        SamplePlayback(i, live.current, scratchA_, true, layerWeight * (progress > 0.0f ? progress : 1.0f));
        if (live.previous.state >= 0) {
            SamplePlayback(i, live.previous, scratchB_, true, layerWeight * (1.0f - progress));
            Blend(scratchB_, scratchA_, progress, scratchA_);
        }

        if (i == 0) {
            result_ = scratchA_;
        } else if (description.additive) {
            BlendAdditive(result_, scratchA_, bindPose_, layerWeight, live.mask, result_);
        } else {
            Blend(result_, scratchA_, layerWeight, live.mask, result_);
        }

        if (i != 0) {
            continue;
        }
        // Root motion diambil dari lapis dasar saja: lapis di atasnya
        // menganimasikan bagian badan lewat mask, dan tidak ada arti yang masuk
        // akal untuk dua lapis yang sama-sama menggerakkan karakter di dunia.
        const State& state = description.states[static_cast<std::size_t>(live.current.state)];
        ResolveMotion(state.motion, activeScratch_);
        for (const ActiveClip& active : activeScratch_) {
            if (!active.clip->extractRootMotion) {
                continue;
            }
            const BoneTransform delta = SampleRootMotion(
                *active.clip, active.binding, *skeleton_,
                live.current.previousNormalized * active.clip->duration,
                live.current.normalized * active.clip->duration);
            rootMotion_.translation += delta.translation * active.weight;
            rootMotion_.rotation =
                glm::normalize(glm::slerp(rootMotion_.rotation, delta.rotation, active.weight));
        }
    }
}

}  // namespace sim::animation
