#include "Sim/Core/Curve.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace sim {
namespace {

struct InterpolationName {
    Interpolation mode;
    const char* name;
};

constexpr std::array<InterpolationName, 3> kInterpolationNames{{
    {Interpolation::Constant, "constant"},
    {Interpolation::Linear, "linear"},
    {Interpolation::Bezier, "bezier"},
}};

/// Hermite kubik, bukan bezier dengan titik kendali.
///
/// Keduanya menghasilkan kurva yang sama; yang berbeda adalah cara menyimpannya.
/// Hermite menerima kemiringan langsung, sehingga kunci tetangga yang bergeser
/// tidak mengubah bentuk di sekitar kunci ini — lihat catatan di CurveKey.
float Hermite(float p0, float m0, float p1, float m1, float t, float span) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    // Kemiringan dikalikan panjang selang: ia dinyatakan per satuan waktu,
    // sementara `t` di sini sudah dinormalkan ke [0,1].
    return h00 * p0 + h10 * m0 * span + h01 * p1 + h11 * m1 * span;
}

template <typename Stops>
std::size_t InsertSorted(Stops& stops, const GradientStop& stop) {
    const auto it = std::lower_bound(
        stops.begin(), stops.end(), stop.position,
        [](const GradientStop& candidate, float position) {
            return candidate.position < position;
        });
    const std::size_t index = static_cast<std::size_t>(it - stops.begin());
    stops.insert(it, stop);
    return index;
}

}  // namespace

const char* ToString(Interpolation mode) {
    for (const InterpolationName& entry : kInterpolationNames) {
        if (entry.mode == mode) {
            return entry.name;
        }
    }
    return "bezier";
}

Interpolation InterpolationFromString(std::string_view text) {
    for (const InterpolationName& entry : kInterpolationNames) {
        if (text == entry.name) {
            return entry.mode;
        }
    }
    return Interpolation::Bezier;
}

Curve::Curve(float constant) {
    CurveKey key;
    key.time = 0.0f;
    key.value = constant;
    key.interpolation = Interpolation::Linear;
    keys_.push_back(key);
}

std::size_t Curve::AddKey(const CurveKey& key) {
    const auto same = std::find_if(keys_.begin(), keys_.end(), [&key](const CurveKey& candidate) {
        return candidate.time == key.time;
    });
    if (same != keys_.end()) {
        // Dua kunci pada waktu yang sama membuat evaluasinya bergantung urutan
        // penyimpanan — perilaku yang tidak terlihat di editor sama sekali.
        *same = key;
        return static_cast<std::size_t>(same - keys_.begin());
    }
    const auto it = std::lower_bound(keys_.begin(), keys_.end(), key.time,
                                     [](const CurveKey& candidate, float time) {
                                         return candidate.time < time;
                                     });
    const std::size_t index = static_cast<std::size_t>(it - keys_.begin());
    keys_.insert(it, key);
    return index;
}

void Curve::RemoveKey(std::size_t index) {
    if (index < keys_.size()) {
        keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

std::size_t Curve::MoveKey(std::size_t index, float time, float value) {
    if (index >= keys_.size()) {
        return index;
    }
    CurveKey moved = keys_[index];
    moved.time = time;
    moved.value = value;
    keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(index));
    return AddKey(moved);
}

float Curve::Evaluate(float time) const {
    if (keys_.empty()) {
        return 0.0f;
    }
    if (time <= keys_.front().time) {
        return keys_.front().value;
    }
    if (time >= keys_.back().time) {
        return keys_.back().value;
    }

    const auto next = std::lower_bound(keys_.begin(), keys_.end(), time,
                                       [](const CurveKey& candidate, float t) {
                                           return candidate.time < t;
                                       });
    const CurveKey& b = *next;
    const CurveKey& a = *(next - 1);

    if (a.interpolation == Interpolation::Constant) {
        return a.value;
    }
    const float span = b.time - a.time;
    if (span <= 0.0f) {
        return b.value;
    }
    const float t = (time - a.time) / span;
    if (a.interpolation == Interpolation::Linear) {
        return a.value + (b.value - a.value) * t;
    }
    return Hermite(a.value, a.outTangent, b.value, b.inTangent, t, span);
}

void Curve::ValueRange(float& outMin, float& outMax) const {
    if (keys_.empty()) {
        outMin = 0.0f;
        outMax = 1.0f;
        return;
    }
    outMin = keys_.front().value;
    outMax = keys_.front().value;
    for (const CurveKey& key : keys_) {
        outMin = std::min(outMin, key.value);
        outMax = std::max(outMax, key.value);
    }
    // Kurva datar tetap butuh tinggi supaya bisa digambar dan disunting.
    if (outMax - outMin < 1e-4f) {
        outMin -= 0.5f;
        outMax += 0.5f;
    }
}

Gradient::Gradient(const Vec3& constant) {
    colorStops_.push_back({0.0f, constant, 1.0f});
    alphaStops_.push_back({0.0f, constant, 1.0f});
}

std::size_t Gradient::AddColorStop(float position, const Vec3& color) {
    return InsertSorted(colorStops_, GradientStop{position, color, 1.0f});
}

std::size_t Gradient::AddAlphaStop(float position, float alpha) {
    return InsertSorted(alphaStops_, GradientStop{position, Vec3(1.0f), alpha});
}

void Gradient::RemoveColorStop(std::size_t index) {
    if (index < colorStops_.size()) {
        colorStops_.erase(colorStops_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void Gradient::RemoveAlphaStop(std::size_t index) {
    if (index < alphaStops_.size()) {
        alphaStops_.erase(alphaStops_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

Vec4 Gradient::Evaluate(float position) const {
    Vec3 color(1.0f);
    if (!colorStops_.empty()) {
        if (position <= colorStops_.front().position) {
            color = colorStops_.front().color;
        } else if (position >= colorStops_.back().position) {
            color = colorStops_.back().color;
        } else {
            const auto next = std::lower_bound(
                colorStops_.begin(), colorStops_.end(), position,
                [](const GradientStop& stop, float p) { return stop.position < p; });
            const GradientStop& b = *next;
            const GradientStop& a = *(next - 1);
            const float span = b.position - a.position;
            const float t = span > 0.0f ? (position - a.position) / span : 0.0f;
            color = a.color + (b.color - a.color) * t;
        }
    }

    float alpha = 1.0f;
    if (!alphaStops_.empty()) {
        if (position <= alphaStops_.front().position) {
            alpha = alphaStops_.front().alpha;
        } else if (position >= alphaStops_.back().position) {
            alpha = alphaStops_.back().alpha;
        } else {
            const auto next = std::lower_bound(
                alphaStops_.begin(), alphaStops_.end(), position,
                [](const GradientStop& stop, float p) { return stop.position < p; });
            const GradientStop& b = *next;
            const GradientStop& a = *(next - 1);
            const float span = b.position - a.position;
            const float t = span > 0.0f ? (position - a.position) / span : 0.0f;
            alpha = a.alpha + (b.alpha - a.alpha) * t;
        }
    }
    return Vec4(color, alpha);
}

}  // namespace sim
