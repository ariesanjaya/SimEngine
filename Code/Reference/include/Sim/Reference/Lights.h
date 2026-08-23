#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <vector>

namespace sim::reference {

/// Lampu bidang berbentuk kuad, dengan sampling dan PDF-nya sendiri.
///
/// **Daftar lampu berdiri sendiri, dan itu satu-satunya hal yang benar-benar
/// bertabrakan dengan backend.** Di *Ray Tracing in One Weekend*, PDF sebuah
/// cahaya adalah metode pada objek geometrinya — sebuah kuad menghitung PDF-nya
/// dengan menembak dirinya sendiri. Backend BVH mana pun, termasuk milik kita,
/// tidak memberikan objek: ia memberikan pasangan `(geometri, primitif)`. Jadi
/// yang tahu "benda ini sebuah lampu, bentuknya begini, luasnya sekian" harus
/// tinggal di sisi kita.
///
/// Kuad, bukan segitiga, karena hampir seluruh lampu bidang di adegan uji
/// berbentuk persegi — dan sebuah kuad yang dipecah dua tidak menyederhanakan
/// apa pun selain menggandakan entri daftarnya.
struct QuadLight {
    /// Sudut asal, lalu dua sisi yang membentangkannya. Bukan pusat dan
    /// setengah-lebar: bentuk ini yang langsung memberi titik sampel lewat
    /// `origin + u*edgeU + v*edgeV`, tanpa satu pun perkalian tambahan.
    Vec3 origin{0.0f};
    Vec3 edgeU{1.0f, 0.0f, 0.0f};
    Vec3 edgeV{0.0f, 0.0f, 1.0f};

    /// Radiansi yang dipancarkan, per satuan luas per steradian. Emitor
    /// Lambert: sama ke segala arah di sisi depannya.
    Vec3 radiance{1.0f};

    /// **Memancar dua sisi atau satu.** Lampu langit-langit Cornell box
    /// memancar ke bawah saja; membiarkannya memancar dua sisi menambahkan
    /// cahaya yang tidak ada di adegan acuannya.
    bool doubleSided = false;

    Vec3 Normal() const;
    float Area() const;
};

/// Satu sampel dari sebuah lampu.
struct LightSample {
    /// Titik di permukaan lampu.
    Vec3 position{0.0f};
    /// Arah dari titik teduh **ke** lampu, ternormalisasi.
    Vec3 direction{0.0f};
    /// Jarak ke titik sampel. Dipakai sinar bayangan, dan batasnya dipendekkan
    /// sedikit supaya lampunya sendiri tidak menghalangi dirinya.
    float distance = 0.0f;
    /// Radiansi yang datang dari arah itu. Nol kalau titiknya di sisi belakang
    /// lampu satu-sisi.
    Vec3 radiance{0.0f};
    /// PDF sampel ini **dalam sudut ruang**, bukan dalam luas. Nol berarti
    /// sampelnya tidak sah dan pemanggil harus membuangnya alih-alih membagi.
    float pdf = 0.0f;
};

/// Kumpulan lampu yang bisa disampel sebagai satu.
class LightList {
public:
    void Add(const QuadLight& light) { lights_.push_back(light); }
    bool Empty() const { return lights_.empty(); }
    std::size_t Size() const { return lights_.size(); }
    const QuadLight& At(std::size_t index) const { return lights_[index]; }

    /// Menyampel satu titik pada salah satu lampu, seragam menurut luas.
    ///
    /// `u1` memilih lampunya, `u2` dan `u3` memilih titiknya. Ketiganya di
    /// [0,1). PDF yang dikembalikan sudah memperhitungkan peluang memilih
    /// lampu itu, jadi pemanggil tidak perlu tahu ada berapa lampu.
    LightSample Sample(const Vec3& from, float u1, float u2, float u3) const;

    /// PDF menembakkan `direction` dari `from` lewat sampling lampu.
    ///
    /// **Dibutuhkan estimator campuran, dan hanya itu.** Ketika sebuah arah
    /// datang dari sampling BSDF, bobotnya menuntut PDF yang *sama arah* dari
    /// strategi yang lain — kalau tidak, campurannya bukan campuran melainkan
    /// dua estimator yang dijumlahkan, dan hasilnya bias.
    float Pdf(const Vec3& from, const Vec3& direction) const;

private:
    std::vector<QuadLight> lights_;
};

}  // namespace sim::reference
