#include "Sim/Assets/LightmapUv.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace sim::assets {
namespace {

/// Sedikit kelonggaran di tepi kotak satuan.
///
/// **Bukan kerapian: UV yang ditulis importir lewat float32 hampir tidak pernah
/// tepat 1,0.** Sebuah kotak yang memetakan penuh ke petaknya keluar sebagai
/// 1,0000001 di sebagian sudutnya, dan menolaknya berarti menolak satu-satunya
/// UV yang memang sudah layak di pohon ini.
constexpr float kUnitSlack = 1e-4f;

/// Luas bertanda segitiga di ruang UV.
float SignedArea(const Vec2& a, const Vec2& b, const Vec2& c) {
    return 0.5f * ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
}

struct UvTriangle {
    Vec2 a;
    Vec2 b;
    Vec2 c;
    Vec2 minimum;
    Vec2 maximum;
};

/// Apakah sebuah titik berada di dalam segitiga, koordinat barisentrik.
bool Contains(const UvTriangle& triangle, const Vec2& point) {
    const float area = SignedArea(triangle.a, triangle.b, triangle.c);
    if (std::abs(area) < 1e-12f) {
        return false;
    }
    const float u = SignedArea(point, triangle.b, triangle.c) / area;
    const float v = SignedArea(triangle.a, point, triangle.c) / area;
    const float w = 1.0f - u - v;
    // Tepi yang berbagi dihitung **tidak** tumpang tindih: dua segitiga yang
    // bersebelahan di dalam satu chart memang berbagi tepinya, dan menghitungnya
    // sebagai tumpang tindih membuat setiap mesh gagal.
    constexpr float kEdge = 1e-5f;
    return u > kEdge && v > kEdge && w > kEdge;
}

/// Apakah dua ruas garis benar-benar berpotongan — bukan sekadar bersentuhan di
/// ujungnya.
bool SegmentsCross(const Vec2& p1, const Vec2& p2, const Vec2& q1, const Vec2& q2) {
    const auto side = [](const Vec2& a, const Vec2& b, const Vec2& p) {
        const float value = SignedArea(a, b, p);
        constexpr float kEps = 1e-9f;
        return value > kEps ? 1 : (value < -kEps ? -1 : 0);
    };
    const int d1 = side(p1, p2, q1);
    const int d2 = side(p1, p2, q2);
    const int d3 = side(q1, q2, p1);
    const int d4 = side(q1, q2, p2);
    // Nol di salah satunya berarti kolinear atau bersentuhan; itu tepi yang
    // berbagi, bukan tumpang tindih.
    return d1 * d2 < 0 && d3 * d4 < 0;
}

bool Overlaps(const UvTriangle& lhs, const UvTriangle& rhs) {
    // Kotak batas lebih dulu: sebagian besar pasangan tersingkir di sini, dan
    // yang tersisa membayar uji yang sesungguhnya.
    if (lhs.maximum.x <= rhs.minimum.x || rhs.maximum.x <= lhs.minimum.x ||
        lhs.maximum.y <= rhs.minimum.y || rhs.maximum.y <= lhs.minimum.y) {
        return false;
    }
    // Satu sudut di dalam yang lain, atau dua tepi berpotongan. Keduanya
    // dibutuhkan: dua segitiga yang salah satunya sepenuhnya memuat yang lain
    // tidak punya tepi yang berpotongan sama sekali.
    const std::array<Vec2, 3> lhsPoints{lhs.a, lhs.b, lhs.c};
    const std::array<Vec2, 3> rhsPoints{rhs.a, rhs.b, rhs.c};

    // **Titik beratnya lebih dulu, dan itu bukan optimasi.** Dua segitiga yang
    // berimpit persis — UV yang diduplikasi, kasus pengulangan yang paling
    // sering — tidak punya satu pun sudut yang berada di *dalam* yang lain, dan
    // tidak punya satu pun tepi yang benar-benar berpotongan. Keduanya
    // bersentuhan di mana-mana dan memotong di mana pun tidak, jadi uji sudut
    // dan uji tepi sama-sama menjawab tidak. Titik beratnya selalu di dalam.
    const Vec2 lhsCentre = (lhs.a + lhs.b + lhs.c) / 3.0f;
    const Vec2 rhsCentre = (rhs.a + rhs.b + rhs.c) / 3.0f;
    if (Contains(rhs, lhsCentre) || Contains(lhs, rhsCentre)) {
        return true;
    }

    for (const Vec2& point : lhsPoints) {
        if (Contains(rhs, point)) {
            return true;
        }
    }
    for (const Vec2& point : rhsPoints) {
        if (Contains(lhs, point)) {
            return true;
        }
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            if (SegmentsCross(lhsPoints[i], lhsPoints[(i + 1) % 3], rhsPoints[j],
                              rhsPoints[(j + 1) % 3])) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

LightmapUvSuitability CheckLightmapUv(const MeshData& mesh, uint32_t maxOverlapPairs) {
    LightmapUvSuitability result;
    if (!mesh.IsValid()) {
        result.reason = "mesh has no triangles";
        return result;
    }

    Vec2 uvMinimum(std::numeric_limits<float>::max());
    Vec2 uvMaximum(std::numeric_limits<float>::lowest());

    std::vector<UvTriangle> triangles;
    triangles.reserve(mesh.indices.size() / 3);
    for (std::size_t at = 0; at + 2 < mesh.indices.size(); at += 3) {
        const Vec2& a = mesh.vertices[mesh.indices[at]].uv;
        const Vec2& b = mesh.vertices[mesh.indices[at + 1]].uv;
        const Vec2& c = mesh.vertices[mesh.indices[at + 2]].uv;

        UvTriangle triangle{a, b, c, glm::min(a, glm::min(b, c)), glm::max(a, glm::max(b, c))};
        if (triangle.minimum.x < -kUnitSlack || triangle.minimum.y < -kUnitSlack ||
            triangle.maximum.x > 1.0f + kUnitSlack || triangle.maximum.y > 1.0f + kUnitSlack) {
            ++result.outsideUnitSquare;
        }
        if (std::abs(SignedArea(a, b, c)) < 1e-12f) {
            ++result.degenerateTriangles;
        }
        uvMinimum = glm::min(uvMinimum, triangle.minimum);
        uvMaximum = glm::max(uvMaximum, triangle.maximum);
        triangles.push_back(triangle);
    }
    result.triangleCount = static_cast<uint32_t>(triangles.size());
    result.uvMinimum = uvMinimum;
    result.uvMaximum = uvMaximum;

    // Sapuan menurut sumbu Y: segitiga diurutkan menurut tepi bawahnya, dan
    // hanya yang rentang Y-nya bersinggungan yang diadu. Tanpa ini biayanya
    // kuadratik, dan sebuah mesh 68 ribu segitiga adalah 2,3 miliar pasangan.
    std::vector<uint32_t> order(triangles.size());
    for (uint32_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&triangles](uint32_t lhs, uint32_t rhs) {
        return triangles[lhs].minimum.y < triangles[rhs].minimum.y;
    });

    for (std::size_t i = 0; i < order.size(); ++i) {
        const UvTriangle& lhs = triangles[order[i]];
        for (std::size_t j = i + 1; j < order.size(); ++j) {
            const UvTriangle& rhs = triangles[order[j]];
            if (rhs.minimum.y >= lhs.maximum.y) {
                break;  // sisanya lebih tinggi lagi
            }
            if (Overlaps(lhs, rhs)) {
                ++result.overlappingPairs;
                if (maxOverlapPairs != 0 && result.overlappingPairs >= maxOverlapPairs) {
                    result.reason = "UV triangles overlap";
                    return result;
                }
            }
        }
    }

    if (result.overlappingPairs > 0) {
        result.reason = std::to_string(result.overlappingPairs) + " pairs of UV triangles overlap";
        return result;
    }

    result.suitable = true;
    result.needsRescale = result.outsideUnitSquare > 0;
    return result;
}

void AdoptFirstUvAsLightmapUv(MeshData& mesh) {
    if (mesh.vertices.empty()) {
        return;
    }
    Vec2 minimum(std::numeric_limits<float>::max());
    Vec2 maximum(std::numeric_limits<float>::lowest());
    for (const MeshVertex& vertex : mesh.vertices) {
        minimum = glm::min(minimum, vertex.uv);
        maximum = glm::max(maximum, vertex.uv);
    }

    // **Skala yang sama di kedua sumbu.** Skala per sumbu meregangkan texel
    // lightmap, sehingga sebuah ubin persegi panjang mendapat kerapatan cahaya
    // yang berbeda menurut arah — dan itu terlihat sebagai bayangan yang lebih
    // kabur di satu sumbu daripada di sumbu lain.
    const Vec2 span = maximum - minimum;
    const float extent = std::max(std::max(span.x, span.y), 1e-6f);
    const float scale = 1.0f / extent;
    for (MeshVertex& vertex : mesh.vertices) {
        vertex.lightmapUv = (vertex.uv - minimum) * scale;
    }
    mesh.hasLightmapUv = true;
}

}  // namespace sim::assets
