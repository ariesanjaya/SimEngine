#pragma once

#include "Sim/Core/Math.h"

#include <string>
#include <string_view>
#include <vector>

namespace sim {

/// Bagaimana sebuah kunci menyambung ke kunci berikutnya.
enum class Interpolation {
    /// Menahan nilainya sampai kunci berikutnya. Untuk saklar dan indeks.
    Constant,
    Linear,
    /// Bezier kubik lewat tangen masuk/keluar tiap kunci.
    Bezier,
};

const char* ToString(Interpolation mode);
Interpolation InterpolationFromString(std::string_view text);

/// Satu titik kunci pada kurva.
///
/// Tangen disimpan sebagai **kemiringan** (satuan nilai per satuan waktu), bukan
/// sebagai titik kendali absolut. Alasannya bukan selera: titik kendali absolut
/// harus dihitung ulang setiap kali kunci tetangganya digeser, dan kurva yang
/// bentuknya berubah ketika kunci LAIN dipindahkan adalah hal yang sangat sulit
/// dipakai. Kemiringan tetap berarti sama berapa pun jarak antar-kunci.
struct CurveKey {
    float time = 0.0f;
    float value = 0.0f;
    /// Kemiringan di sisi kiri dan kanan kunci. Berbeda satu sama lain berarti
    /// kurvanya patah di titik ini — dan itu sah, kerap justru yang diinginkan.
    float inTangent = 0.0f;
    float outTangent = 0.0f;
    Interpolation interpolation = Interpolation::Bezier;
};

/// Kurva satu dimensi terhadap waktu.
///
/// Dipakai ulang oleh Particle (E7.2), Terrain (E7.3), dan Animation (E7.5).
/// Datanya di `Sim::Core` sementara widget penyuntingnya di EditorFramework:
/// runtime harus bisa mengevaluasi kurva tanpa menyeret ImGui.
class Curve {
public:
    Curve() = default;
    /// Kurva datar bernilai tetap. Bentuk yang paling sering dibutuhkan sebagai
    /// nilai awal sebuah modul.
    explicit Curve(float constant);

    /// Menyisipkan kunci pada urutan waktu yang benar, lalu mengembalikan
    /// indeksnya. Kunci pada waktu yang sama persis diganti — dua kunci di satu
    /// waktu membuat evaluasinya bergantung urutan penyimpanan.
    std::size_t AddKey(const CurveKey& key);
    void RemoveKey(std::size_t index);
    /// Memindahkan sebuah kunci, menjaga urutan waktu. Mengembalikan indeks
    /// barunya.
    std::size_t MoveKey(std::size_t index, float time, float value);

    /// Nilai kurva pada sebuah waktu.
    ///
    /// Di luar rentang kunci, nilainya ditahan pada kunci terluar — bukan
    /// diekstrapolasi. Ekstrapolasi bezier menghasilkan angka yang meledak jauh
    /// di luar rentang yang terlihat di editor, dan tidak ada modul yang
    /// menginginkannya.
    float Evaluate(float time) const;

    const std::vector<CurveKey>& Keys() const { return keys_; }
    std::vector<CurveKey>& Keys() { return keys_; }
    bool Empty() const { return keys_.empty(); }

    /// Rentang nilai yang dicakup kurva, untuk menskalakan tampilan editor.
    void ValueRange(float& outMin, float& outMax) const;

private:
    std::vector<CurveKey> keys_;
};

/// Satu perhentian warna atau alpha pada gradient.
struct GradientStop {
    float position = 0.0f;
    Vec3 color{1.0f, 1.0f, 1.0f};
    float alpha = 1.0f;
};

/// Gradient warna terhadap waktu hidup.
///
/// **Perhentian warna dan alpha terpisah**, seperti di editor partikel lain yang
/// sudah mapan. Menyatukannya memaksa penulis efek menambah perhentian warna
/// hanya untuk mengubah transparansi di satu titik — dan warna yang ikut
/// tersisip di situ harus ditebak, biasanya salah.
class Gradient {
public:
    Gradient() = default;
    explicit Gradient(const Vec3& constant);

    std::size_t AddColorStop(float position, const Vec3& color);
    std::size_t AddAlphaStop(float position, float alpha);
    void RemoveColorStop(std::size_t index);
    void RemoveAlphaStop(std::size_t index);

    /// Warna dan alpha pada sebuah posisi, digabung menjadi satu Vec4.
    Vec4 Evaluate(float position) const;

    const std::vector<GradientStop>& ColorStops() const { return colorStops_; }
    const std::vector<GradientStop>& AlphaStops() const { return alphaStops_; }
    std::vector<GradientStop>& ColorStops() { return colorStops_; }
    std::vector<GradientStop>& AlphaStops() { return alphaStops_; }

private:
    std::vector<GradientStop> colorStops_;
    std::vector<GradientStop> alphaStops_;
};

}  // namespace sim
