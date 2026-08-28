#include "Sim/Reference/PathTracer.h"

#include "Sim/Raycast/Query.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace sim::reference {
namespace {

constexpr float kPi = 3.14159265358979323846f;

/// PCG32. **Bukan RNG pustaka standar**, alasan yang sama dengan penabur
/// vegetasi: `std::mt19937` menghasilkan urutan yang sama, tetapi
/// `std::uniform_real_distribution` tidak dijamin sama antar-implementasi. Acuan
/// yang gambarnya berbeda antar-mesin tidak bisa dipakai sebagai acuan.
class Rng {
public:
    explicit Rng(uint64_t seed) : state_(seed * 6364136223846793005ull + 1442695040888963407ull) {
        Next();
    }

    float Uniform() {
        // 24 bit mantissa — cukup untuk sampling, dan menjamin hasilnya < 1.
        return static_cast<float>(Next() >> 8) * (1.0f / 16777216.0f);
    }

private:
    uint32_t Next() {
        const uint64_t previous = state_;
        state_ = previous * 6364136223846793005ull + 1442695040888963407ull;
        const uint32_t xorshifted = static_cast<uint32_t>(((previous >> 18u) ^ previous) >> 27u);
        const uint32_t rot = static_cast<uint32_t>(previous >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
    }

    uint64_t state_;
};

/// Arah pada belahan, dibobot kosinus. PDF-nya `cos / pi`.
///
/// **Ini "strategi BSDF" kita, dan ia sengaja sederhana.** `openpbr.slang`
/// hanya mengevaluasi, tidak menyampel — itu bentuk yang benar untuk model
/// shading rasterizer. Menuliskan penyampel yang cocok untuk kesembilan
/// lobenya adalah proyek tersendiri, dan yang dibutuhkan acuan lebih dulu
/// adalah **benar**, bukan cepat konvergen. Kosinus tak-bias untuk BSDF apa
/// pun selama pembaginya PDF yang sama.
Vec3 SampleCosineHemisphere(const Vec3& normal, float u1, float u2) {
    const float r = std::sqrt(u1);
    const float phi = 2.0f * kPi * u2;
    const float x = r * std::cos(phi);
    const float y = r * std::sin(phi);
    const float z = std::sqrt(std::max(0.0f, 1.0f - u1));

    const Vec3 axis =
        std::abs(normal.y) < 0.99f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
    const Vec3 tangent = glm::normalize(glm::cross(axis, normal));
    const Vec3 bitangent = glm::cross(normal, tangent);
    return glm::normalize(tangent * x + bitangent * y + normal * z);
}

float CosineHemispherePdf(const Vec3& normal, const Vec3& direction) {
    const float cosine = glm::dot(normal, direction);
    return cosine > 0.0f ? cosine / kPi : 0.0f;
}

float MaxComponent(const Vec3& v) { return std::max(v.x, std::max(v.y, v.z)); }

/// Basis SH real orde dua. **Cerminan `ShBasis` di `Ibl.cpp`**, dan keduanya
/// harus tetap sama: yang di sana memproyeksikan lingkungan, yang di sini
/// memproyeksikan transport, dan sebuah probe di ruang terbuka wajib menjawab
/// angka yang sama dengan lingkungannya. Basis yang berselisih membuat
/// perbandingan itu gagal karena alasan yang tidak ada hubungannya dengan
/// transport.
void ShBasis(const Vec3& d, std::array<float, 9>& out) {
    out[0] = 0.282095f;
    out[1] = 0.488603f * d.y;
    out[2] = 0.488603f * d.z;
    out[3] = 0.488603f * d.x;
    out[4] = 1.092548f * d.x * d.y;
    out[5] = 1.092548f * d.y * d.z;
    out[6] = 0.315392f * (3.0f * d.z * d.z - 1.0f);
    out[7] = 1.092548f * d.x * d.z;
    out[8] = 0.546274f * (d.x * d.x - d.y * d.y);
}

/// Radical inverse basis 2 — separuh Hammersley, dan separuh yang lain sudah
/// dipakai sebagai `(i + 0,5) / N`.
float RadicalInverse(uint32_t bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

/// Benih yang ditentukan posisi probe, bukan urutan pengerjaannya.
uint64_t HashProbeSeed(const Vec3& position, uint32_t sample, uint32_t seed) {
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](uint32_t word) {
        hash = (hash ^ word) * 1099511628211ull;
    };
    // Bit float-nya apa adanya: dua probe yang berjarak sepersejuta meter memang
    // harus mendapat benih yang berbeda, dan pembulatan ke kisi apa pun di sini
    // akan menyamakan probe yang berbeda tanpa satu pun galat.
    uint32_t bits = 0;
    for (int axis = 0; axis < 3; ++axis) {
        std::memcpy(&bits, &position[axis], sizeof(bits));
        mix(bits);
    }
    mix(sample);
    mix(seed);
    return hash;
}

/// Radiansi yang datang dari `origin` menuju `direction`, satu jalur penuh.
///
/// **Estimator tunggal untuk kedua pemakainya**, dan itu bukan kerapian
/// melainkan syarat: gambar acuan dan kisi probe harus menjawab transport yang
/// sama, dan dua salinan loop ini akan berselisih pada pantulan ke berapa pun
/// yang pertama kali disunting salah satunya. Yang membedakan keduanya cuma dari
/// mana sinarnya berangkat.
///
/// `rng` dipegang pemanggil supaya benihnya bisa ditentukan per-sampel — gambar
/// yang sama harus keluar berapa pun urutan pikselnya dikerjakan, termasuk
/// ketika diparalelkan.
Vec3 TracePath(const raycast::RayScene& scene, const SurfaceResolver& resolve,
               const LightList& lights, Vec3 origin, Vec3 direction,
               const TraceSettings& settings, Rng& rng, std::size_t& rays,
               std::size_t& shadingCalls) {
    Vec3 radiance(0.0f);
    Vec3 throughput(1.0f);

    for (uint32_t depth = 0; depth < settings.maxDepth; ++depth) {
        ++rays;
        const raycast::RayHit hit = raycast::Raycast(scene, origin, direction);
        if (!hit.hit) {
            if (settings.sky) {
                radiance += throughput * settings.sky(direction);
            }
            break;
        }

        const SurfaceHit surface = resolve(hit, origin, direction);

        // **Estimatornya ditulis eksplisit**, bukan disembunyikan di dalam
        // akumulator: yang dipancarkan permukaan ditambah apa yang
        // dihamburkannya, dan yang kedua dibobot `f*cos / pdf`. Setiap faktornya
        // bisa dicetak sendiri.
        radiance += throughput * surface.emission;

        // Normal dibalik menghadap sinar datang. Permukaan satu-sisi yang
        // ditembak dari belakang bukan permukaan gelap melainkan permukaan yang
        // sama dilihat dari sisi lain.
        Vec3 normal = surface.normal;
        if (glm::dot(normal, -direction) < 0.0f) {
            normal = -normal;
        }
        const Frame frame = Frame::FromNormal(normal, -direction);

        // **Matahari lewat next-event estimation, dan hanya di sini.** Ia tidak
        // pernah ditemukan sampling BSDF karena ia bukan bagian dari `sky`;
        // alasannya di `TraceSettings::sunIrradiance`. Konsekuensi yang
        // dimanfaatkan S2: sinar yang berangkat dari titik kosong tidak pernah
        // menemuinya, jadi sebuah probe memanggang pantulan mataharinya tanpa
        // memanggang mataharinya sendiri.
        if (MaxComponent(settings.sunIrradiance) > 0.0f) {
            const Vec3 toSun = -glm::normalize(settings.sunDirection);
            if (glm::dot(normal, toSun) > 0.0f) {
                ++rays;
                const raycast::RayHit blocker =
                    raycast::Raycast(scene, surface.position + normal * 1e-4f, toSun);
                if (!blocker.hit) {
                    ++shadingCalls;
                    radiance += throughput * EvaluateDirect(surface.surface, frame, toSun,
                                                            settings.sunIrradiance);
                }
            }
        }

        // **Campuran dua strategi, satu sampel.** Salah satunya dipilih untuk
        // *menghasilkan* arah; PDF-nya tetap gabungan keduanya. Kalau tidak,
        // yang terjadi bukan campuran melainkan dua estimator yang dijumlahkan —
        // dan itu bias.
        const bool useLight = !lights.Empty() && rng.Uniform() < 0.5f;
        Vec3 scattered;
        if (useLight) {
            const LightSample ls =
                lights.Sample(surface.position, rng.Uniform(), rng.Uniform(), rng.Uniform());
            if (ls.pdf <= 0.0f) {
                break;
            }
            scattered = ls.direction;
        } else {
            scattered = SampleCosineHemisphere(normal, rng.Uniform(), rng.Uniform());
        }

        const float lightPdf = lights.Empty() ? 0.0f : lights.Pdf(surface.position, scattered);
        const float cosinePdf = CosineHemispherePdf(normal, scattered);
        const float pdf = lights.Empty() ? cosinePdf : 0.5f * lightPdf + 0.5f * cosinePdf;
        if (pdf <= 1e-9f) {
            break;
        }

        // `EvaluateDirect` dengan radiansi satu mengembalikan `f * cos` — model
        // shading itu sudah mengalikan kosinusnya sendiri, dan mengalikannya
        // lagi di sini akan menggelapkan seluruh gambar tanpa satu pun galat.
        ++shadingCalls;
        const Vec3 fcos = EvaluateDirect(surface.surface, frame, scattered, Vec3(1.0f));
        throughput *= fcos / pdf;
        if (MaxComponent(throughput) <= 1e-6f) {
            break;
        }

        // **Russian roulette, bukan potongan keras.** Berhenti di `maxDepth`
        // membuang energi, dan buangan itu tumbuh justru di adegan paling terang
        // — yaitu adegan yang dipakai menguji GI. Sebuah acuan yang seluruh
        // gunanya tak-bias tidak boleh memungut bias demi kesederhanaan.
        if (depth >= settings.minRouletteDepth) {
            const float survival = std::min(0.95f, MaxComponent(throughput));
            if (survival <= 0.0f || rng.Uniform() >= survival) {
                break;
            }
            throughput /= survival;
        }

        // Digeser sepanjang normal, bukan sepanjang arah sinar: arah yang hampir
        // sejajar permukaan butuh geseran besar untuk keluar darinya, dan
        // geseran besar melompati geometri tipis.
        origin = surface.position + normal * 1e-4f;
        direction = scattered;
    }
    return radiance;
}

/// Kuadrat sempurna terkecil yang tidak lebih kecil dari `n`.
uint32_t RoundUpToSquare(uint32_t n, uint32_t& outSide) {
    uint32_t side = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<double>(n))));
    side = std::max(side, 1u);
    outSide = side;
    return side * side;
}

}  // namespace

void Camera::GenerateRay(float sx, float sy, float aspect, float lensU, float lensV,
                         Vec3& outOrigin, Vec3& outDirection) const {
    const Vec3 forward = glm::normalize(target - position);

    // **Pandangan yang sejajar `up` tidak punya basis, dan itu sudut yang
    // paling sering dipakai** — kamera yang melihat lurus ke bawah pada
    // lantai. `cross` menjawab nol di sana, `normalize` menjawab NaN, dan
    // seluruh sinarnya meleset tanpa satu pun galat: yang terlihat hanya
    // gambar yang seluruhnya langit. Sebuah acuan yang diam-diam merender
    // langit kosong akan **lulus** uji tungku putih, karena langit seragam
    // memang jawabannya.
    Vec3 reference = up;
    if (std::abs(glm::dot(forward, glm::normalize(up))) > 0.999f) {
        reference = std::abs(forward.z) < 0.9f ? Vec3(0.0f, 0.0f, 1.0f) : Vec3(1.0f, 0.0f, 0.0f);
    }
    const Vec3 right = glm::normalize(glm::cross(forward, reference));
    const Vec3 trueUp = glm::cross(right, forward);

    const float halfHeight = std::tan(verticalFov * 0.5f * kPi / 180.0f);
    const float halfWidth = halfHeight * aspect;

    // Layar dibalik pada y: koordinat piksel tumbuh ke bawah, dunia ke atas.
    const float px = (sx * 2.0f - 1.0f) * halfWidth;
    const float py = (1.0f - sy * 2.0f) * halfHeight;

    const Vec3 focusPoint = position + (forward + right * px + trueUp * py) * focusDistance;

    outOrigin = position;
    if (apertureRadius > 0.0f) {
        // Cakram lewat pemetaan konsentris sederhana: akar radiusnya menjaga
        // sebarannya seragam menurut luas, bukan menurut jari-jari.
        const float r = apertureRadius * std::sqrt(lensU);
        const float phi = 2.0f * kPi * lensV;
        outOrigin += right * (r * std::cos(phi)) + trueUp * (r * std::sin(phi));
    }
    outDirection = glm::normalize(focusPoint - outOrigin);
}

Vec3 Image::Mean() const {
    if (pixels.empty()) {
        return Vec3(0.0f);
    }
    Vec3 total(0.0f);
    for (const Vec3& p : pixels) {
        total += p;
    }
    return total / static_cast<float>(pixels.size());
}

SkySampler ConstantSky(const Vec3& radiance) {
    if (radiance == Vec3(0.0f)) {
        // Langit hitam adalah ketiadaan langit, dan sebuah fungsi yang selalu
        // mengembalikan nol tetap dipanggil sekali per sinar yang lolos.
        return {};
    }
    return [radiance](const Vec3&) { return radiance; };
}

Image Render(const raycast::RayScene& scene, const SurfaceResolver& resolve,
             const LightList& lights, const Camera& camera, const TraceSettings& settings) {
    Image image;
    image.width = settings.width;
    image.height = settings.height;
    image.pixels.assign(static_cast<std::size_t>(settings.width) * settings.height, Vec3(0.0f));

    uint32_t side = 1;
    const uint32_t samples = RoundUpToSquare(settings.samplesPerPixel, side);
    const float invSide = 1.0f / static_cast<float>(side);
    const float aspect = static_cast<float>(settings.width) / static_cast<float>(settings.height);

    for (uint32_t y = 0; y < settings.height; ++y) {
        for (uint32_t x = 0; x < settings.width; ++x) {
            Vec3 accumulated(0.0f);

            for (uint32_t s = 0; s < samples; ++s) {
                // Benih dari koordinat piksel dan nomor sampel, bukan dari
                // penghitung berjalan: gambar yang sama harus keluar berapa pun
                // urutan pikselnya dikerjakan — termasuk kalau nanti diparalelkan.
                Rng rng((static_cast<uint64_t>(y) << 40) ^ (static_cast<uint64_t>(x) << 20) ^
                        (static_cast<uint64_t>(s) << 4) ^ settings.seed);

                // Stratifikasi √spp × √spp. Setiap sampel menempati petaknya
                // sendiri, lalu digoyang di dalam petak itu.
                const uint32_t sx = s % side;
                const uint32_t sy = s / side;
                const float jitterX = (static_cast<float>(sx) + rng.Uniform()) * invSide;
                const float jitterY = (static_cast<float>(sy) + rng.Uniform()) * invSide;

                Vec3 origin;
                Vec3 direction;
                camera.GenerateRay((static_cast<float>(x) + jitterX) / settings.width,
                                   (static_cast<float>(y) + jitterY) / settings.height, aspect,
                                   rng.Uniform(), rng.Uniform(), origin, direction);

                const Vec3 radiance =
                    TracePath(scene, resolve, lights, origin, direction, settings, rng,
                              image.raysTraced, image.shadingCalls);

                accumulated += radiance;
            }

            image.pixels[static_cast<std::size_t>(y) * settings.width + x] =
                accumulated / static_cast<float>(samples);
        }
    }

    return image;
}

std::array<Vec3, 9> TraceProbeIrradiance(const raycast::RayScene& scene,
                                         const SurfaceResolver& resolve, const LightList& lights,
                                         const Vec3& position, uint32_t sampleCount,
                                         const TraceSettings& settings) {
    std::array<Vec3, 9> sh{};
    const uint32_t count = std::max(sampleCount, 1u);

    std::size_t rays = 0;
    std::size_t shadingCalls = 0;
    std::array<float, 9> basis{};

    for (uint32_t i = 0; i < count; ++i) {
        // Benih dari posisi probe dan nomor sampel, bukan dari penghitung
        // berjalan: panggangan yang sama harus keluar berapa pun urutan
        // probenya dikerjakan, termasuk ketika dibagi ke `TaskPool`.
        Rng rng(HashProbeSeed(position, i, settings.seed));

        // Sampel merata di **bola**, bukan merata di sudut — dan bukan di
        // belahan. Sebuah probe tidak punya normal; membelah bolanya berarti
        // memilih normal saat memanggang, yaitu keputusan yang baru diambil
        // permukaan yang membacanya.
        //
        // Stratifikasinya lewat Hammersley, sama dengan `ProjectIrradiance`:
        // dua urutan sampel yang berbeda akan berselisih pada jumlah sampel
        // rendah, dan selisih itu akan terbaca sebagai transport yang salah.
        const float u1 = (static_cast<float>(i) + 0.5f) / static_cast<float>(count);
        const float z = 1.0f - 2.0f * u1;
        const float r = std::sqrt(std::max(1.0f - z * z, 0.0f));
        const float phi = 2.0f * kPi * RadicalInverse(i);
        const Vec3 direction(r * std::cos(phi), r * std::sin(phi), z);

        const Vec3 radiance = TracePath(scene, resolve, lights, position, direction, settings, rng,
                                        rays, shadingCalls);
        ShBasis(direction, basis);
        for (int b = 0; b < 9; ++b) {
            sh[static_cast<std::size_t>(b)] +=
                radiance * basis[static_cast<std::size_t>(b)];
        }
    }

    // Bobot Monte Carlo untuk sampling seragam di bola: 4π / N. Angka yang sama
    // dengan `ProjectIrradiance`, dan harus tetap sama.
    const float weight = 4.0f * kPi / static_cast<float>(count);
    for (Vec3& coefficient : sh) {
        coefficient *= weight;
    }
    return sh;
}

}  // namespace sim::reference
