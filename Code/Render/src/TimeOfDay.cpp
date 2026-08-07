#include "Sim/Render/TimeOfDay.h"

#include <algorithm>
#include <cmath>

namespace sim::render {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegrees = kPi / 180.0f;

}  // namespace

float WrapHour(float hour) {
    const float wrapped = std::fmod(hour, 24.0f);
    return wrapped < 0.0f ? wrapped + 24.0f : wrapped;
}

Quat LookRotation(const Vec3& forward) {
    const float length = glm::length(forward);
    if (length < 1e-6f) {
        return Quat(1.0f, 0.0f, 0.0f, 0.0f);
    }
    const Vec3 direction = forward / length;
    // Sumbu atas dipilih menjauhi arah hadapnya. Memakai +Y selalu membuat
    // hasilnya tidak terdefinisi tepat saat menghadap lurus ke atas atau ke
    // bawah — dan matahari melewati keduanya.
    const Vec3 up = std::abs(direction.y) > 0.999f ? Vec3(0.0f, 0.0f, 1.0f)
                                                   : Vec3(0.0f, 1.0f, 0.0f);
    return glm::quatLookAt(direction, up);
}

SunPosition ComputeSunPosition(const SunPlacement& placement) {
    const float latitude = placement.latitudeDegrees * kDegrees;
    // Deklinasi: kemiringan sumbu bumi diproyeksikan ke tanggalnya. Inilah yang
    // membuat matahari terbit di timur laut pada bulan Juni dan di tenggara pada
    // Desember — dan yang membuat editor ini bisa menyatakan musim sama sekali.
    const float day = static_cast<float>(std::clamp<uint32_t>(placement.dayOfYear, 1, 366));
    const float declination =
        23.45f * kDegrees * std::sin(2.0f * kPi * (284.0f + day) / 365.0f);

    // Sudut jam: nol di tengah hari, negatif sebelumnya, positif sesudahnya.
    const float hourAngle = (WrapHour(placement.hour) - 12.0f) * 15.0f * kDegrees;

    const float sinLat = std::sin(latitude);
    const float cosLat = std::cos(latitude);
    const float sinDec = std::sin(declination);
    const float cosDec = std::cos(declination);
    const float sinHour = std::sin(hourAngle);
    const float cosHour = std::cos(hourAngle);

    // Komponen dalam kerangka horizontal: timur, utara, atas. Dihitung langsung
    // sebagai vektor, bukan lewat azimut lalu dikembalikan ke vektor: rumus
    // azimut punya pembagian yang tidak terdefinisi tepat di zenit, dan zenit
    // adalah keadaan yang pasti dilewati di lintang rendah.
    const float east = -cosDec * sinHour;
    const float north = sinDec * cosLat - cosDec * sinLat * cosHour;
    const float up = sinLat * sinDec + cosLat * cosDec * cosHour;

    // Utara adegan adalah −Z, dan pemakainya boleh memutarnya: adegan jarang
    // dibangun menghadap utara sungguhan, dan memutar seluruh adegan agar cocok
    // jauh lebih mahal daripada memutar mataharinya.
    const float rotation = placement.northOffsetDegrees * kDegrees;
    const float cosRotation = std::cos(rotation);
    const float sinRotation = std::sin(rotation);
    const float rotatedEast = east * cosRotation + north * sinRotation;
    const float rotatedNorth = -east * sinRotation + north * cosRotation;

    SunPosition position;
    position.direction = glm::normalize(Vec3(rotatedEast, up, -rotatedNorth));
    position.altitude = std::asin(std::clamp(up, -1.0f, 1.0f));
    position.azimuth = std::atan2(rotatedEast, rotatedNorth);
    return position;
}

TimeOfDayCurve::TimeOfDayCurve(std::vector<TimeOfDayKey> keys) : keys_(std::move(keys)) {
    std::sort(keys_.begin(), keys_.end(),
              [](const TimeOfDayKey& a, const TimeOfDayKey& b) { return a.hour < b.hour; });
}

void TimeOfDayCurve::Set(float hour, const Vec3& value) {
    const float wrapped = WrapHour(hour);
    const auto found = std::find_if(keys_.begin(), keys_.end(), [wrapped](const TimeOfDayKey& key) {
        return std::abs(key.hour - wrapped) < 1e-4f;
    });
    if (found != keys_.end()) {
        // Diganti, bukan ditambahkan. Dua kunci pada jam yang sama membuat
        // hasilnya bergantung urutan penyisipan — dan itu tidak terlihat di mana
        // pun kecuali pada berkas yang disimpan.
        found->value = value;
        return;
    }
    const auto at = std::lower_bound(
        keys_.begin(), keys_.end(), wrapped,
        [](const TimeOfDayKey& key, float hour) { return key.hour < hour; });
    keys_.insert(at, TimeOfDayKey{wrapped, value});
}

void TimeOfDayCurve::Remove(std::size_t index) {
    if (index < keys_.size()) {
        keys_.erase(keys_.begin() + static_cast<std::ptrdiff_t>(index));
    }
}

void TimeOfDayCurve::Clear() {
    keys_.clear();
}

Vec3 TimeOfDayCurve::Evaluate(float hour, const Vec3& fallback) const {
    if (keys_.empty()) {
        return fallback;
    }
    if (keys_.size() == 1) {
        return keys_.front().value;
    }
    const float wrapped = WrapHour(hour);

    // Kunci terakhir yang jamnya tidak melewati `wrapped`. Kalau tidak ada,
    // jamnya berada di ruas yang melintasi tengah malam — ruas dari kunci
    // terakhir ke kunci pertama, dan itulah ruas yang paling mudah terlupa.
    std::size_t before = keys_.size() - 1;
    for (std::size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i].hour <= wrapped) {
            before = i;
        }
    }
    const bool crossesMidnight = keys_[before].hour > wrapped || before == keys_.size() - 1;
    const std::size_t after = crossesMidnight && before == keys_.size() - 1 ? 0 : before + 1;
    if (after >= keys_.size()) {
        return keys_[before].value;
    }

    const float start = keys_[before].hour;
    float end = keys_[after].hour;
    float position = wrapped;
    if (after == 0) {
        // Ruas tengah malam: ujungnya dipindahkan ke hari berikutnya supaya
        // pecahan jaraknya tetap naik.
        end += 24.0f;
        if (position < start) {
            position += 24.0f;
        }
    }
    const float span = end - start;
    const float t = span > 1e-6f ? std::clamp((position - start) / span, 0.0f, 1.0f) : 0.0f;
    return glm::mix(keys_[before].value, keys_[after].value, t);
}

void TimeOfDayClock::Advance(float deltaSeconds) {
    if (!playing_ || deltaSeconds <= 0.0f) {
        return;
    }
    hour_ = WrapHour(hour_ + speed_ * deltaSeconds);
}

TimeOfDayPreset TimeOfDayPreset::Default() {
    TimeOfDayPreset preset;
    // Satu hari cerah. Kuncinya sengaja sedikit: preset yang penuh kunci tidak
    // bisa dibaca sebagai bentuk, dan yang pertama dilakukan siapa pun terhadap
    // preset bawaan adalah membacanya.
    preset.sunColor = TimeOfDayCurve({
        {0.0f, Vec3(0.05f, 0.06f, 0.12f)},   // tengah malam, cahaya bulan
        {5.5f, Vec3(0.35f, 0.20f, 0.15f)},   // fajar
        {7.0f, Vec3(1.00f, 0.70f, 0.45f)},   // matahari terbit
        {12.0f, Vec3(1.00f, 0.96f, 0.90f)},  // tengah hari
        {17.0f, Vec3(1.00f, 0.72f, 0.45f)},
        {18.5f, Vec3(0.60f, 0.28f, 0.18f)},  // senja
        {20.0f, Vec3(0.05f, 0.06f, 0.12f)},
    });
    preset.sunIntensity = TimeOfDayCurve({
        {0.0f, Vec3(0.05f)},
        {6.0f, Vec3(0.4f)},
        {9.0f, Vec3(3.0f)},
        {12.0f, Vec3(4.0f)},
        {15.0f, Vec3(3.0f)},
        {18.0f, Vec3(0.6f)},
        {20.0f, Vec3(0.05f)},
    });
    preset.skyZenith = TimeOfDayCurve({
        {0.0f, Vec3(0.010f, 0.012f, 0.030f)},
        {6.5f, Vec3(0.10f, 0.14f, 0.30f)},
        {12.0f, Vec3(0.18f, 0.32f, 0.62f)},
        {18.0f, Vec3(0.12f, 0.13f, 0.28f)},
        {20.0f, Vec3(0.010f, 0.012f, 0.030f)},
    });
    preset.skyHorizon = TimeOfDayCurve({
        {0.0f, Vec3(0.02f, 0.02f, 0.04f)},
        {6.0f, Vec3(0.55f, 0.35f, 0.25f)},
        {12.0f, Vec3(0.62f, 0.66f, 0.72f)},
        {18.0f, Vec3(0.60f, 0.32f, 0.20f)},
        {20.0f, Vec3(0.02f, 0.02f, 0.04f)},
    });
    return preset;
}

TimeOfDayState EvaluateTimeOfDay(const TimeOfDayPreset& preset, const SunPlacement& placement) {
    TimeOfDayState state;
    state.sun = ComputeSunPosition(placement);

    // Peredam terbit-terbenam. **Bukan pemutus mendadak.** Matahari yang
    // dimatikan tepat saat menyentuh horizon membuat seluruh adegan berkedip
    // dalam satu frame, dan kedipan itu justru jatuh pada saat yang paling
    // diperhatikan orang. Lebar pitanya kira-kira selebar matahari terbit
    // sungguhan.
    constexpr float kFadeBand = 0.10f;  // radian, ±5,7°
    state.daylight = std::clamp((state.sun.altitude + kFadeBand) / (2.0f * kFadeBand), 0.0f, 1.0f);

    const float hour = placement.hour;
    const Vec3 color = preset.sunColor.Evaluate(hour, Vec3(1.0f));
    const float intensity = preset.sunIntensity.Evaluate(hour, Vec3(1.0f)).x;
    state.sunRadiance = color * intensity * state.daylight;
    state.skyZenith = preset.skyZenith.Evaluate(hour, Vec3(0.0f));
    state.skyHorizon = preset.skyHorizon.Evaluate(hour, Vec3(0.0f));
    return state;
}

}  // namespace sim::render
