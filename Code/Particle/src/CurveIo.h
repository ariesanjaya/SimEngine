#pragma once

#include "Sim/Core/Curve.h"

#include <nlohmann/json.hpp>

namespace sim::particle {

/// Serialisasi Curve dan Gradient.
///
/// **Untuk sementara tinggal di modul Particle.** Keduanya tipe milik `Sim::Core`
/// dan akan dipakai Terrain (E7.3) dan Animation (E7.5) juga — tapi memindahkan
/// serialisasinya ke Core sekarang menuntut Core membocorkan nlohmann ke header
/// publiknya, dan bentuk API yang benar baru terlihat ketika ada pemakai kedua
/// yang nyata. Dipindahkan saat itu tiba, bukan ditebak sekarang.
///
/// Yang penting dijaga sampai saat itu: bentuk JSON-nya, karena berkas yang
/// sudah ditulis harus tetap terbaca.

using CurveJson = nlohmann::ordered_json;

inline CurveJson WriteCurve(const Curve& curve) {
    CurveJson keys = CurveJson::array();
    for (const CurveKey& key : curve.Keys()) {
        CurveJson entry;
        entry["t"] = key.time;
        entry["v"] = key.value;
        // Tangen hanya ditulis untuk kunci bezier: kunci linear dan constant
        // tidak memakainya, dan menuliskannya membuat berkas penuh angka nol
        // yang tidak berarti apa-apa.
        if (key.interpolation == Interpolation::Bezier) {
            entry["in"] = key.inTangent;
            entry["out"] = key.outTangent;
        }
        entry["mode"] = ToString(key.interpolation);
        keys.push_back(std::move(entry));
    }
    return keys;
}

inline void ReadCurve(const CurveJson& array, Curve& curve) {
    curve.Keys().clear();
    if (!array.is_array()) {
        return;
    }
    for (const CurveJson& entry : array) {
        CurveKey key;
        key.time = entry.value("t", 0.0f);
        key.value = entry.value("v", 0.0f);
        key.inTangent = entry.value("in", 0.0f);
        key.outTangent = entry.value("out", 0.0f);
        key.interpolation = InterpolationFromString(entry.value("mode", std::string("bezier")));
        curve.AddKey(key);
    }
}

inline CurveJson WriteGradient(const Gradient& gradient) {
    CurveJson root;
    CurveJson colors = CurveJson::array();
    for (const GradientStop& stop : gradient.ColorStops()) {
        colors.push_back(CurveJson::array({stop.position, stop.color.x, stop.color.y,
                                           stop.color.z}));
    }
    CurveJson alphas = CurveJson::array();
    for (const GradientStop& stop : gradient.AlphaStops()) {
        alphas.push_back(CurveJson::array({stop.position, stop.alpha}));
    }
    root["colors"] = std::move(colors);
    root["alphas"] = std::move(alphas);
    return root;
}

inline void ReadGradient(const CurveJson& object, Gradient& gradient) {
    gradient.ColorStops().clear();
    gradient.AlphaStops().clear();
    if (!object.is_object()) {
        return;
    }
    if (const auto colors = object.find("colors"); colors != object.end() && colors->is_array()) {
        for (const CurveJson& entry : *colors) {
            if (entry.is_array() && entry.size() == 4) {
                gradient.AddColorStop(entry[0].get<float>(),
                                      Vec3(entry[1].get<float>(), entry[2].get<float>(),
                                           entry[3].get<float>()));
            }
        }
    }
    if (const auto alphas = object.find("alphas"); alphas != object.end() && alphas->is_array()) {
        for (const CurveJson& entry : *alphas) {
            if (entry.is_array() && entry.size() == 2) {
                gradient.AddAlphaStop(entry[0].get<float>(), entry[1].get<float>());
            }
        }
    }
}

}  // namespace sim::particle
