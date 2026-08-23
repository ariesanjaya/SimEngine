#include "Sim/Reference/Lights.h"

#include <algorithm>
#include <cmath>

namespace sim::reference {
namespace {

/// Sinar kuad: mengembalikan jarak, atau negatif kalau meleset.
///
/// Dipakai `Pdf`, bukan penelusuran — yang menelusuri adalah `Sim::Raycast`.
/// Yang dibutuhkan di sini justru pertanyaan sebaliknya: "kalau saya menembak
/// ke arah ini, lampu mana yang kena, dan berapa peluang sampling lampu
/// menghasilkan arah yang sama".
float IntersectQuad(const QuadLight& light, const Vec3& origin, const Vec3& direction,
                    float& outU, float& outV) {
    const Vec3 normal = light.Normal();
    const float denom = glm::dot(normal, direction);
    if (std::abs(denom) < 1e-8f) {
        return -1.0f;
    }
    const float t = glm::dot(normal, light.origin - origin) / denom;
    if (t <= 1e-4f) {
        return -1.0f;
    }
    const Vec3 hit = origin + direction * t;
    const Vec3 local = hit - light.origin;

    // Koordinat di dalam bidang kuadnya. Dibagi panjang kuadrat sisinya, bukan
    // panjangnya, supaya tidak ada akar kuadrat di jalur yang dipanggil per
    // sampel per lampu.
    const float uu = glm::dot(light.edgeU, light.edgeU);
    const float vv = glm::dot(light.edgeV, light.edgeV);
    if (uu < 1e-12f || vv < 1e-12f) {
        return -1.0f;
    }
    outU = glm::dot(local, light.edgeU) / uu;
    outV = glm::dot(local, light.edgeV) / vv;
    if (outU < 0.0f || outU > 1.0f || outV < 0.0f || outV > 1.0f) {
        return -1.0f;
    }
    return t;
}

}  // namespace

Vec3 QuadLight::Normal() const { return glm::normalize(glm::cross(edgeU, edgeV)); }

float QuadLight::Area() const { return glm::length(glm::cross(edgeU, edgeV)); }

LightSample LightList::Sample(const Vec3& from, float u1, float u2, float u3) const {
    LightSample sample;
    if (lights_.empty()) {
        return sample;
    }

    // Lampu dipilih seragam, bukan menurut dayanya. Sampling menurut daya
    // memang mengurangi derau di adegan berlampu banyak — tetapi ia juga
    // menjadi sumber bias baru kalau bobotnya tidak ikut masuk PDF, dan acuan
    // ini lebih membutuhkan sesuatu yang bisa dibaca ulang daripada sesuatu
    // yang cepat konvergen.
    const std::size_t count = lights_.size();
    const std::size_t index =
        std::min(static_cast<std::size_t>(u1 * static_cast<float>(count)), count - 1);
    const QuadLight& light = lights_[index];

    const Vec3 position = light.origin + light.edgeU * u2 + light.edgeV * u3;
    const Vec3 delta = position - from;
    const float distanceSquared = glm::dot(delta, delta);
    if (distanceSquared < 1e-12f) {
        return sample;
    }
    const float distance = std::sqrt(distanceSquared);
    const Vec3 direction = delta / distance;

    const Vec3 normal = light.Normal();
    const float cosLight = glm::dot(normal, -direction);
    const float facing = light.doubleSided ? std::abs(cosLight) : cosLight;
    if (facing <= 1e-6f) {
        return sample;
    }

    // **Luas ke sudut ruang.** PDF sampling seragam menurut luas adalah 1/A;
    // yang dibutuhkan estimator adalah PDF dalam sudut ruang, dan konversinya
    // mengalikan dengan jarak kuadrat dibagi kosinus di sisi lampu. Lampu yang
    // hampir sejajar garis pandang karena itu punya PDF besar — dan itu benar:
    // sampling seragam jarang menghasilkan arah ke sana.
    const float area = light.Area();
    if (area <= 1e-12f) {
        return sample;
    }
    sample.pdf = distanceSquared / (facing * area) / static_cast<float>(count);
    sample.position = position;
    sample.direction = direction;
    // Dipendekkan supaya lampu tidak menghalangi dirinya sendiri di sinar
    // bayangan. Yang dijaga bukan ketelitian melainkan permukaan yang menjadi
    // hitam seluruhnya karena setiap sinar bayangan mengenai lampunya.
    sample.distance = distance * (1.0f - 1e-3f);
    sample.radiance = light.radiance;
    return sample;
}

float LightList::Pdf(const Vec3& from, const Vec3& direction) const {
    if (lights_.empty()) {
        return 0.0f;
    }
    // **Dijumlahkan atas seluruh lampu, bukan diambil yang pertama kena.** Satu
    // arah bisa mengenai beberapa lampu yang berjajar, dan peluang total
    // menghasilkan arah itu adalah jumlahnya. Mengambil yang pertama membuat
    // PDF terlalu kecil, dan estimator yang membaginya menjadi terlalu terang —
    // persis di tempat yang paling terang.
    float total = 0.0f;
    for (const QuadLight& light : lights_) {
        float u = 0.0f;
        float v = 0.0f;
        const float t = IntersectQuad(light, from, direction, u, v);
        if (t < 0.0f) {
            continue;
        }
        const Vec3 normal = light.Normal();
        const float cosLight = glm::dot(normal, -direction);
        const float facing = light.doubleSided ? std::abs(cosLight) : cosLight;
        if (facing <= 1e-6f) {
            continue;
        }
        const float area = light.Area();
        if (area <= 1e-12f) {
            continue;
        }
        total += (t * t) / (facing * area);
    }
    return total / static_cast<float>(lights_.size());
}

}  // namespace sim::reference
