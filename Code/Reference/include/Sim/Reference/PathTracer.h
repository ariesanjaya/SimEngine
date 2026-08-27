#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Raycast/Query.h"
#include "Sim/Raycast/RayScene.h"
#include "Sim/Reference/Lights.h"
#include "Sim/Reference/Shading.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace sim::reference {

/// Kamera pinhole dengan cakram defokus.
///
/// **Stratifikasi bukan hiasan.** Pada spp rendah ia yang membedakan derau yang
/// turun rapi dari derau yang menggumpal, dan gambar acuan dinilai justru pada
/// konvergensinya — bukan pada rupanya.
struct Camera {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Vec3 target{0.0f, 0.0f, -1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
    /// Sudut pandang vertikal, derajat.
    float verticalFov = 40.0f;
    /// Nol berarti pinhole murni: seluruh adegan tajam.
    float apertureRadius = 0.0f;
    float focusDistance = 1.0f;

    /// Sinar lewat koordinat layar `[0,1)²`, dengan `lensU`/`lensV` untuk
    /// cakram defokusnya.
    void GenerateRay(float sx, float sy, float aspect, float lensU, float lensV,
                     Vec3& outOrigin, Vec3& outDirection) const;
};

/// Apa yang ditemukan sinar di sebuah titik permukaan.
///
/// **Backend menjawab "kena apa", bukan "bahannya apa".** Ia memberi pasangan
/// `(instance, primitif)` dan sebuah jarak; yang menerjemahkannya menjadi
/// permukaan yang bisa diteduhkan adalah pemanggil. Titik sambung itu sengaja
/// berupa callback: adegan uji yang seluruhnya satu material tidak perlu
/// membangun tabel apa pun, dan adegan sungguhan bisa membacanya dari mana pun
/// ia menyimpannya.
struct SurfaceHit {
    Vec3 position{0.0f};
    Vec3 normal{0.0f, 0.0f, 1.0f};
    Surface surface;
    /// Radiansi yang dipancarkan permukaan itu sendiri, kalau ada.
    Vec3 emission{0.0f};
};

/// Menerjemahkan hasil penelusuran menjadi permukaan yang bisa diteduhkan.
using SurfaceResolver =
    std::function<SurfaceHit(const raycast::RayHit& hit, const Vec3& origin, const Vec3& direction)>;

/// Radiansi langit pada sebuah arah pandang.
///
/// Arahnya ternormalisasi dan menunjuk **menjauhi** permukaan, yaitu arah sinar
/// yang lolos dari adegan.
using SkySampler = std::function<Vec3(const Vec3& direction)>;

/// Langit satu warna, untuk adegan yang tidak sedang menguji langitnya.
SkySampler ConstantSky(const Vec3& radiance);

struct TraceSettings {
    uint32_t width = 128;
    uint32_t height = 128;
    /// Sampel per piksel. **Dibulatkan ke kuadrat sempurna** supaya
    /// stratifikasi √spp × √spp-nya utuh; yang tidak pas dinaikkan.
    uint32_t samplesPerPixel = 16;
    /// Jaring pengaman, bukan pemotong. Yang menghentikan jalur adalah Russian
    /// roulette; batas ini hanya menjaga adegan patologis tidak menggantung.
    uint32_t maxDepth = 64;
    /// Kedalaman minimum sebelum roulette mulai bekerja. Memotong pantulan
    /// pertama secara acak menambah derau jauh lebih banyak daripada waktu yang
    /// dihematnya.
    uint32_t minRouletteDepth = 3;
    /// Radiansi langit untuk sinar yang tidak mengenai apa pun.
    ///
    /// **Sebuah fungsi, bukan `render::IEnvironmentSampler`.** Antarmuka itu
    /// tinggal di `Sim::Render`, dan menautkannya dari sini akan menyeret
    /// seluruh rantai Vulkan ke dalam path tracer yang seluruh gunanya berjalan
    /// di CPU — sebuah acuan yang menuntut perangkat grafis berhenti bisa
    /// dijalankan di tempat yang paling membutuhkannya. Yang dibutuhkan cuma
    /// "arah → radiansi", dan itu satu tanda tangan; yang menjembatani keduanya
    /// pemanggil, dalam satu lambda.
    ///
    /// Kosong berarti langit hitam — keadaan setiap adegan tertutup.
    SkySampler sky;
    uint32_t seed = 1u;
};

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    /// Radiansi linier, bukan warna yang sudah dipetakan nada. **Acuan yang
    /// keluarannya sudah dipetakan nada tidak bisa dibandingkan angkanya.**
    std::vector<Vec3> pixels;

    const Vec3& At(uint32_t x, uint32_t y) const { return pixels[y * width + x]; }
    Vec3 Mean() const;

    /// **Berapa kerja yang benar-benar dilakukan, bukan berapa yang diminta.**
    /// Russian roulette memotong jalur pada kedalaman yang berbeda-beda, jadi
    /// `samplesPerPixel * maxDepth` bukan jawaban — dan tanpa angka yang
    /// sebenarnya, "intersector mendominasi atau tidak" hanya bisa ditebak.
    std::size_t raysTraced = 0;
    std::size_t shadingCalls = 0;
};

/// Path tracer unidirectional dengan next-event estimation.
///
/// **Tak-bias, dan itu satu-satunya sifat yang benar-benar dituntut darinya.**
/// Renderer yang seluruh gunanya menjadi acuan tidak boleh memungut bias demi
/// kesederhanaan — termasuk bias yang kecil. Karena itu kedalamannya dihentikan
/// Russian roulette, bukan dipotong keras: pemotongan keras membuang energi
/// yang justru tumbuh di adegan paling terang, yaitu adegan yang dipakai
/// menguji GI.
///
/// **Estimatornya ditulis eksplisit**, bukan disembunyikan di dalam akumulator.
/// Setiap faktornya bisa dicetak dan diperiksa satu per satu, dan itulah yang
/// membuat sebuah acuan bisa dipercaya. Acuan yang benar tapi tidak bisa dibaca
/// ulang tidak menyelesaikan apa pun.
Image Render(const raycast::RayScene& scene, const SurfaceResolver& resolve,
             const LightList& lights, const Camera& camera, const TraceSettings& settings);

}  // namespace sim::reference
