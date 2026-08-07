#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Render/Frustum.h"
#include "Sim/Render/Types.h"

#include <array>
#include <cstdint>
#include <vector>

namespace sim::render {

/// Banyaknya cascade paling banyak. Empat sudah cukup untuk jarak pandang
/// kilometer; yang kelima menambah satu pass demi ketelitian yang tidak
/// terlihat.
inline constexpr int kMaxCascades = 4;

struct CascadeSettings {
    int count = 4;
    /// Resolusi satu cascade, piksel. Persegi.
    uint32_t resolution = 2048;
    /// Jarak terjauh yang masih dibayangi. Biasanya jauh lebih dekat daripada
    /// `farZ` kamera — bayangan di kejauhan tidak terlihat, dan membentangkan
    /// cascade sampai ke sana hanya menipiskan yang dekat.
    float maxDistance = 150.0f;

    /// Campuran antara pembagian logaritmik dan seragam, 0..1.
    ///
    /// **Keduanya sendirian salah, dan salah ke arah yang berlawanan.**
    /// Logaritmik murni menaruh hampir seluruh resolusi di beberapa meter
    /// pertama, sehingga cascade terakhir membentang ratusan meter dan
    /// bayangannya jadi kotak-kotak. Seragam murni memberi jarak dekat porsi
    /// yang sama dengan jarak jauh, padahal di jarak dekatlah tepi bayangan
    /// benar-benar diperhatikan. 0,75 condong ke logaritmik tanpa ekstremnya.
    float splitLambda = 0.75f;

    /// Lebar pita tumpang tindih antar-cascade, sebagai pecahan panjang cascade.
    ///
    /// Tanpa tumpang tindih, perpindahan cascade adalah garis tajam tempat
    /// resolusi bayangan berubah mendadak. Pita ini yang memberi shader sesuatu
    /// untuk dicampur.
    float blendFraction = 0.1f;

    /// Seberapa jauh bidang dekat cahaya ditarik mundur, meter.
    ///
    /// **Ini yang membuat benda di belakang kamera tetap menjatuhkan bayangan.**
    /// Frustum cahaya yang dipas persis pada irisan frustum kamera akan memotong
    /// setiap caster yang berada di antara matahari dan irisan itu — dan
    /// akibatnya bayangan yang hilang justru dari benda tinggi seperti pohon dan
    /// bangunan, yang bayangannya paling diperhatikan.
    float casterPullback = 200.0f;
};

/// Satu cascade yang siap dipakai pass bayangan.
struct Cascade {
    Mat4 viewProjection{1.0f};
    /// Batas jauh cascade ini di ruang pandang, meter dari kamera.
    float splitFar = 0.0f;
    /// Awal pita campur menuju cascade berikutnya. Sama dengan `splitFar` pada
    /// cascade terakhir.
    float blendBegin = 0.0f;
    /// Ukuran dunia yang diwakili satu texel. Dipakai memilih bias yang
    /// sebanding dengan resolusi alih-alih angka ajaib per-cascade.
    float texelWorldSize = 0.0f;
    /// Jari-jari bola pembatas irisan frustum ini.
    float radius = 0.0f;
};

struct CascadeSet {
    std::array<Cascade, kMaxCascades> cascades{};
    int count = 0;
};

/// Batas belah cascade sepanjang sumbu pandang, meter.
///
/// Mengembalikan `count` nilai; yang terakhir selalu `maxDistance`.
void ComputeCascadeSplits(const CascadeSettings& settings, float nearZ,
                          std::array<float, kMaxCascades>& out);

/// Menyusun matriks tiap cascade untuk sebuah kamera dan arah cahaya.
///
/// `lightDirection` menunjuk **dari permukaan ke cahaya**, sama dengan yang
/// dipakai `openpbr.slang`. Arah yang terbalik tidak menghasilkan galat apa pun,
/// hanya adegan yang seluruhnya berada dalam bayangan.
///
/// **Tiap cascade dipas pada bola, bukan pada kotak yang ketat.** Kotak yang
/// ketat berubah ukuran saat kamera berputar, dan ukuran yang berubah berarti
/// perbandingan dunia-per-texel yang berubah — tepi bayangan lalu berkilat
/// setiap kali kamera bergerak sedikit. Bola pembatas irisan frustum tidak
/// bergantung pada orientasi, jadi ukurannya tetap untuk sebuah belahan. Yang
/// dibayar adalah peta bayangan yang sedikit lebih longgar daripada perlunya.
///
/// **Titik asalnya lalu dikancing ke kelipatan texel.** Bola menghilangkan
/// kilatan akibat rotasi; pengancingan menghilangkan yang akibat pergeseran.
/// Salah satunya saja masih menyisakan kilatan, dan keduanya sering dikira satu
/// perbaikan yang sama.
CascadeSet ComputeCascades(const Camera& camera, float aspect, const Vec3& lightDirection,
                           const CascadeSettings& settings);

/// Cascade yang harus dipakai untuk sebuah jarak pandang, dan bobot campurnya.
///
/// `weight` 0 berarti sepenuhnya cascade `index`, 1 berarti sepenuhnya cascade
/// berikutnya. Dipakai CPU untuk pengujian dan mencerminkan apa yang dihitung
/// shader.
struct CascadeChoice {
    int index = 0;
    float blendWeight = 0.0f;
};
CascadeChoice ChooseCascade(const CascadeSet& set, float viewDepth);

/// Bola pembatas irisan frustum antara dua jarak. Dipisah supaya bisa diuji
/// sendiri — di sinilah kestabilan cascade sebenarnya diputuskan.
struct BoundingSphere {
    Vec3 centre{0.0f};
    float radius = 0.0f;
};
BoundingSphere FrustumSliceSphere(const Camera& camera, float aspect, float nearDistance,
                                  float farDistance);

}  // namespace sim::render
