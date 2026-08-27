#pragma once

#include "Sim/Core/Math.h"

#include <cmath>

/// Uji perpotongan sinar terhadap primitif — matematika murni, tanpa scene.
///
/// **Di `Sim::Core` supaya hanya ada satu salinannya.** Möller–Trumbore di bawah
/// sebelumnya tinggal di `Code/Whitebox/src/Picking.cpp` sebagai fungsi anonim,
/// dan `Sim::Raycast` membutuhkan yang persis sama. Dua salinan rumus
/// perpotongan adalah dua salinan yang suatu saat berselisih satu epsilon — dan
/// selisih itu muncul sebagai klik yang memilih benda berbeda dari yang
/// ditembak baker, bukan sebagai galat.
///
/// Berkas ini sengaja tidak tahu apa-apa tentang mesh, entity, atau BVH. Yang
/// tahu bentuk datanya adalah pemanggilnya.
namespace sim {

/// Hasil uji sinar terhadap satu segitiga.
struct TriangleHit {
    bool hit = false;
    /// Jarak sepanjang `direction`. **Berskala menurut panjang `direction`**:
    /// arah yang tidak bernorma satu menghasilkan jarak dalam satuan lain.
    float distance = 0.0f;
    /// Barycentric `(u, v)`, sehingga titik kenanya `a + u·(b−a) + v·(c−a)`.
    ///
    /// Ikut dikembalikan karena yang membutuhkannya tidak bisa menghitungnya
    /// ulang dengan murah: interpolasi normal, uv, dan warna simpul semuanya
    /// memakainya, dan menurunkannya kembali dari titik kena menuntut
    /// menyelesaikan sistem yang baru saja diselesaikan di sini.
    Vec2 barycentric{0.0f};

    explicit operator bool() const { return hit; }
};

/// Möller–Trumbore, sisi depan maupun belakang.
///
/// **Sisi belakang ikut diterima**, dan itu disengaja: perancang kerap bekerja
/// dari dalam ruangan yang baru dibuatnya, dan sisi yang tidak bisa diklik dari
/// dalam berarti dinding yang tidak bisa dipindahkan tanpa memutar kamera
/// keluar. Yang membutuhkan penyaringan muka melakukannya sendiri dari tanda
/// `dot(direction, normal)` — di sini keterangannya belum hilang.
///
/// `minDistance` menolak perpotongan tepat di titik asal sinar. Ia yang membuat
/// sinar sekunder tidak langsung mengenai permukaan tempat ia berangkat.
inline TriangleHit RayTriangle(const Vec3& origin, const Vec3& direction, const Vec3& a,
                               const Vec3& b, const Vec3& c, float maxDistance = 1e30f,
                               float minDistance = 1e-8f) {
    constexpr float kParallelEpsilon = 1e-8f;
    TriangleHit result;

    const Vec3 edge1 = b - a;
    const Vec3 edge2 = c - a;
    const Vec3 pvec = glm::cross(direction, edge2);
    const float determinant = glm::dot(edge1, pvec);
    if (std::abs(determinant) < kParallelEpsilon) {
        return result;  // sinar sejajar bidang segitiga
    }

    const float inverse = 1.0f / determinant;
    const Vec3 tvec = origin - a;
    const float u = glm::dot(tvec, pvec) * inverse;
    if (u < 0.0f || u > 1.0f) {
        return result;
    }
    const Vec3 qvec = glm::cross(tvec, edge1);
    const float v = glm::dot(direction, qvec) * inverse;
    if (v < 0.0f || u + v > 1.0f) {
        return result;
    }

    const float distance = glm::dot(edge2, qvec) * inverse;
    if (distance < minDistance || distance > maxDistance) {
        return result;
    }

    result.hit = true;
    result.distance = distance;
    result.barycentric = Vec2(u, v);
    return result;
}

/// Uji slab sinar terhadap kotak sejajar sumbu.
///
/// **Menerima kebalikan arah, bukan arahnya.** Sebuah penelusuran BVH menguji
/// puluhan kotak dengan sinar yang sama, dan pembagian per sumbu per kotak
/// adalah pekerjaan yang seluruhnya bisa dikeluarkan dari gelung. Pemanggil yang
/// hanya menguji satu kotak boleh menghitungnya di tempat.
///
/// Pembagian dengan nol sengaja tidak dijaga: aritmetika IEEE menghasilkan
/// ±inf, dan perbandingan di bawah tetap menjawab benar untuk sinar yang
/// sejajar sebuah sumbu. Yang tidak dijawabnya hanya sinar yang asalnya tepat di
/// bidang slab **dan** sejajar dengannya — di sana ia menghasilkan NaN, dan
/// `outNear` dibiarkan seperti yang diberikan pemanggil.
inline bool RayAabb(const Vec3& origin, const Vec3& inverseDirection, const Vec3& boundsMin,
                    const Vec3& boundsMax, float maxDistance, float& outNear) {
    const Vec3 t0 = (boundsMin - origin) * inverseDirection;
    const Vec3 t1 = (boundsMax - origin) * inverseDirection;

    const Vec3 tNear = glm::min(t0, t1);
    const Vec3 tFar = glm::max(t0, t1);

    const float enter = glm::max(glm::max(tNear.x, tNear.y), glm::max(tNear.z, 0.0f));
    const float exit = glm::min(glm::min(tFar.x, tFar.y), glm::min(tFar.z, maxDistance));

    if (enter > exit) {
        return false;
    }
    outNear = enter;
    return true;
}

/// Titik terdekat pada sebuah segitiga terhadap `point`.
///
/// Dipakai query jarak — snapping ke permukaan, dan penyaringan kandidat saat
/// mencari titik terdekat di seluruh scene. Rumusnya menyelesaikan wilayah
/// Voronoi segitiga: tiga sudut, tiga rusuk, dan bagian dalamnya.
inline Vec3 ClosestPointOnTriangle(const Vec3& point, const Vec3& a, const Vec3& b,
                                   const Vec3& c) {
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 ap = point - a;

    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) {
        return a;
    }

    const Vec3 bp = point - b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) {
        return b;
    }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        return a + ab * (d1 / (d1 - d3));
    }

    const Vec3 cp = point - c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) {
        return c;
    }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        return a + ac * (d2 / (d2 - d6));
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }

    const float denom = 1.0f / (va + vb + vc);
    return a + ab * (vb * denom) + ac * (vc * denom);
}

}  // namespace sim
