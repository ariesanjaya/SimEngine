#include "Sim/Animation/AnimationGraph.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sim::animation {
namespace {

constexpr std::array<const char*, 3> kParameterNames{"Bool", "Float", "Trigger"};
constexpr std::array<const char*, 6> kComparisonNames{"Greater",   "Less",  "GreaterEqual",
                                                      "LessEqual", "Equal", "NotEqual"};
constexpr std::array<const char*, 3> kMotionNames{"Clip", "Blend1D", "Blend2D"};

}  // namespace

// --- bobot blend tree ---------------------------------------------------------

void Blend1DWeights(const std::vector<float>& positions, float value, std::vector<float>& out) {
    out.assign(positions.size(), 0.0f);
    if (positions.empty()) {
        return;
    }
    if (positions.size() == 1) {
        out[0] = 1.0f;
        return;
    }

    // Indeks diurutkan, bukan posisinya: keluarannya harus mengikuti urutan
    // masukan, karena pemanggil memasangkannya dengan daftar klipnya sendiri.
    std::vector<int> order(positions.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<int>(i);
    }
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        return positions[static_cast<std::size_t>(a)] < positions[static_cast<std::size_t>(b)];
    });

    const float low = positions[static_cast<std::size_t>(order.front())];
    const float high = positions[static_cast<std::size_t>(order.back())];
    if (value <= low) {
        out[static_cast<std::size_t>(order.front())] = 1.0f;
        return;
    }
    if (value >= high) {
        out[static_cast<std::size_t>(order.back())] = 1.0f;
        return;
    }
    for (std::size_t i = 0; i + 1 < order.size(); ++i) {
        const int a = order[i];
        const int b = order[i + 1];
        const float pa = positions[static_cast<std::size_t>(a)];
        const float pb = positions[static_cast<std::size_t>(b)];
        if (value < pa || value > pb) {
            continue;
        }
        const float span = pb - pa;
        // Dua simpul di posisi yang sama: yang pertama menang seluruhnya.
        // Membagi rata di antara keduanya hanya menyembunyikan kesalahan
        // penyetelan yang seharusnya terlihat.
        const float t = span > 0.0f ? (value - pa) / span : 0.0f;
        out[static_cast<std::size_t>(a)] = 1.0f - t;
        out[static_cast<std::size_t>(b)] = t;
        return;
    }
}

void Blend2DWeights(const std::vector<Vec2>& positions, const Vec2& value,
                    std::vector<float>& out) {
    out.assign(positions.size(), 0.0f);
    if (positions.empty()) {
        return;
    }
    if (positions.size() == 1) {
        out[0] = 1.0f;
        return;
    }

    float total = 0.0f;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const Vec2 toSample = value - positions[i];
        float weight = 1.0f;
        for (std::size_t j = 0; j < positions.size(); ++j) {
            if (j == i) {
                continue;
            }
            const Vec2 toOther = positions[j] - positions[i];
            const float lengthSq = glm::dot(toOther, toOther);
            if (lengthSq <= 1e-12f) {
                // Dua simpul berimpit. Dilewati alih-alih membagi dengan nol —
                // keduanya akan mendapat bobot yang sama, yang memang satu-satunya
                // jawaban yang masuk akal.
                continue;
            }
            const float band = 1.0f - glm::dot(toSample, toOther) / lengthSq;
            weight = std::min(weight, std::clamp(band, 0.0f, 1.0f));
            if (weight <= 0.0f) {
                break;
            }
        }
        out[i] = weight;
        total += weight;
    }

    if (total <= 0.0f) {
        // Tidak bisa terjadi menurut rumusnya — simpul terluar ke arah sampel
        // selalu berbobot 1 — tapi dijaga tetap begitu: bobot yang seluruhnya
        // nol berarti pose kosong, dan pose kosong terlihat sebagai karakter
        // yang runtuh.
        auto nearest = static_cast<std::size_t>(0);
        float best = glm::dot(value - positions[0], value - positions[0]);
        for (std::size_t i = 1; i < positions.size(); ++i) {
            const float distance = glm::dot(value - positions[i], value - positions[i]);
            if (distance < best) {
                best = distance;
                nearest = i;
            }
        }
        out[nearest] = 1.0f;
        return;
    }
    for (float& weight : out) {
        weight /= total;
    }
}

// --- parameter ----------------------------------------------------------------

const char* ToString(ParameterType type) {
    const auto index = static_cast<std::size_t>(type);
    return index < kParameterNames.size() ? kParameterNames[index] : kParameterNames[1];
}

ParameterType ParameterTypeFromString(std::string_view text) {
    for (std::size_t i = 0; i < kParameterNames.size(); ++i) {
        if (text == kParameterNames[i]) {
            return static_cast<ParameterType>(i);
        }
    }
    return ParameterType::Float;
}

const Parameter& ParameterSet::At(int index) const {
    static const Parameter kFallback;
    if (index < 0 || index >= Count()) {
        return kFallback;
    }
    return parameters_[static_cast<std::size_t>(index)];
}

Parameter& ParameterSet::At(int index) {
    static Parameter fallback;
    if (index < 0 || index >= Count()) {
        fallback = Parameter{};
        return fallback;
    }
    return parameters_[static_cast<std::size_t>(index)];
}

int ParameterSet::Add(const Parameter& parameter) {
    if (parameter.name.empty() || Find(parameter.name) >= 0) {
        // Nama kembar berarti kondisi yang menunjuk dua parameter sekaligus.
        return -1;
    }
    parameters_.push_back(parameter);
    return Count() - 1;
}

bool ParameterSet::Remove(int index) {
    if (index < 0 || index >= Count()) {
        return false;
    }
    parameters_.erase(parameters_.begin() + index);
    return true;
}

void ParameterSet::SetAll(const std::vector<Parameter>& parameters) {
    parameters_ = parameters;
}

int ParameterSet::Find(std::string_view name) const {
    for (int i = 0; i < Count(); ++i) {
        if (parameters_[static_cast<std::size_t>(i)].name == name) {
            return i;
        }
    }
    return -1;
}

float ParameterSet::Float(int index) const {
    return At(index).value;
}

bool ParameterSet::Bool(int index) const {
    return At(index).value != 0.0f;
}

void ParameterSet::SetFloat(int index, float value) {
    if (index >= 0 && index < Count()) {
        parameters_[static_cast<std::size_t>(index)].value = value;
    }
}

void ParameterSet::SetBool(int index, bool value) {
    SetFloat(index, value ? 1.0f : 0.0f);
}

void ParameterSet::Fire(int index) {
    if (index >= 0 && index < Count() &&
        parameters_[static_cast<std::size_t>(index)].type == ParameterType::Trigger) {
        parameters_[static_cast<std::size_t>(index)].value = 1.0f;
    }
}

void ParameterSet::SetFloat(std::string_view name, float value) {
    SetFloat(Find(name), value);
}

void ParameterSet::SetBool(std::string_view name, bool value) {
    SetBool(Find(name), value);
}

void ParameterSet::Fire(std::string_view name) {
    Fire(Find(name));
}

void ParameterSet::ConsumeTriggers() {
    for (Parameter& parameter : parameters_) {
        if (parameter.type == ParameterType::Trigger) {
            parameter.value = 0.0f;
        }
    }
}

// --- kondisi ------------------------------------------------------------------

const char* ToString(Comparison comparison) {
    const auto index = static_cast<std::size_t>(comparison);
    return index < kComparisonNames.size() ? kComparisonNames[index] : kComparisonNames[0];
}

Comparison ComparisonFromString(std::string_view text) {
    for (std::size_t i = 0; i < kComparisonNames.size(); ++i) {
        if (text == kComparisonNames[i]) {
            return static_cast<Comparison>(i);
        }
    }
    return Comparison::Greater;
}

bool Evaluate(const Condition& condition, const ParameterSet& parameters) {
    const int index = parameters.Find(condition.parameter);
    if (index < 0) {
        // Kondisi yang menunjuk parameter yang tidak ada tidak pernah benar.
        // Bukan "selalu benar": transisi yang menyala karena parameternya
        // terhapus adalah kejutan yang paling sulit dilacak.
        return false;
    }
    const Parameter& parameter = parameters.At(index);
    if (parameter.type == ParameterType::Trigger) {
        return parameter.value != 0.0f;
    }
    const float value = parameter.value;
    const float against = parameter.type == ParameterType::Bool
                              ? (condition.value != 0.0f ? 1.0f : 0.0f)
                              : condition.value;
    switch (condition.comparison) {
        case Comparison::Greater: return value > against;
        case Comparison::Less: return value < against;
        case Comparison::GreaterEqual: return value >= against;
        case Comparison::LessEqual: return value <= against;
        case Comparison::Equal: return value == against;
        case Comparison::NotEqual: return value != against;
    }
    return false;
}

// --- motion & graph -----------------------------------------------------------

const char* ToString(MotionKind kind) {
    const auto index = static_cast<std::size_t>(kind);
    return index < kMotionNames.size() ? kMotionNames[index] : kMotionNames[0];
}

MotionKind MotionKindFromString(std::string_view text) {
    for (std::size_t i = 0; i < kMotionNames.size(); ++i) {
        if (text == kMotionNames[i]) {
            return static_cast<MotionKind>(i);
        }
    }
    return MotionKind::Clip;
}

const Layer& AnimationGraph::LayerAt(int index) const {
    static const Layer kFallback;
    if (index < 0 || index >= LayerCount()) {
        return kFallback;
    }
    return layers_[static_cast<std::size_t>(index)];
}

Layer& AnimationGraph::LayerAt(int index) {
    static Layer fallback;
    if (index < 0 || index >= LayerCount()) {
        fallback = Layer{};
        return fallback;
    }
    return layers_[static_cast<std::size_t>(index)];
}

int AnimationGraph::AddLayer(const Layer& layer) {
    layers_.push_back(layer);
    return LayerCount() - 1;
}

bool AnimationGraph::RemoveLayer(int index) {
    if (index < 0 || index >= LayerCount()) {
        return false;
    }
    layers_.erase(layers_.begin() + index);
    return true;
}

void AnimationGraph::SetLayers(const std::vector<Layer>& layers) {
    layers_ = layers;
}

}  // namespace sim::animation
