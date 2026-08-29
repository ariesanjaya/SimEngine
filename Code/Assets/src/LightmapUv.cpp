#include "Sim/Assets/LightmapUv.h"

#include "Sim/Core/AtomicWrite.h"
#include "Sim/Core/Log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
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

    // **Segitiga berluas nol menolak mesh-nya, dan itu ditemukan uji.** Sebuah
    // mesh yang seluruh UV-nya nol — whitebox dan primitif bawaan lahir begitu —
    // tidak punya satu pun pasangan yang tumpang tindih, karena segitiga tanpa
    // luas tidak bisa memuat apa pun. Pemeriksa yang hanya melihat tumpang
    // tindih menyatakannya layak, dan yang keluar adalah seluruh permukaan
    // membaca satu texel.
    //
    // Ambangnya nol, bukan sebagian kecil: segitiga yang tidak punya texel tidak
    // akan pernah menerima cahaya, dan itu bercak hitam betapapun sedikit
    // jumlahnya. Unwrap adalah obatnya, dan ia tidak mahal.
    if (result.degenerateTriangles > 0) {
        result.reason = std::to_string(result.degenerateTriangles) + " of " +
                        std::to_string(result.triangleCount) +
                        " triangles have no area in UV space";
        return result;
    }

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

// --- artefak masak ----------------------------------------------------------

namespace {

/// Dinaikkan setiap kali arti isi berkasnya berubah — tata letaknya, atau
/// keputusan yang menghasilkannya. Ia bagian dari kunci, jadi menaikkannya
/// membuat artefak lama tidak pernah terbaca lagi alih-alih terbaca salah.
constexpr uint32_t kLightmapUvVersion = 1;

/// Empat byte pertama berkasnya. Yang bukan milik kita ditolak sebelum satu pun
/// angka di dalamnya dipercaya.
constexpr char kLightmapUvMagic[4] = {'S', 'L', 'M', 'U'};

struct LightmapUvHeader {
    char magic[4];
    uint32_t version;
    uint32_t fromFirstUv;
    uint32_t sourceVertexCount;
    uint64_t remapCount;
    uint64_t indexCount;
    uint64_t uvCount;
};

uint64_t HashInto(uint64_t hash, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

}  // namespace

LightmapUvArtifact CookLightmapUv(const MeshData& mesh, LightmapUvSuitability& outCheck,
                                  std::string& error) {
    LightmapUvArtifact artifact;
    outCheck = CheckLightmapUv(mesh);
    if (!mesh.IsValid()) {
        error = "mesh has no triangles";
        return artifact;
    }

    artifact.sourceVertexCount = static_cast<uint32_t>(mesh.vertices.size());

    if (outCheck.suitable) {
        // **Yang sudah layak melewati unwrap, dan waktunya tidak dibayar.**
        // Itu seluruh guna pemeriksanya; tanpa cabang ini setiap ubin terrain
        // membayar parameterisasi baru untuk UV yang sudah unik.
        MeshData copy = mesh;
        AdoptFirstUvAsLightmapUv(copy);
        artifact.fromFirstUv = true;
        artifact.lightmapUv.reserve(copy.vertices.size());
        for (const MeshVertex& vertex : copy.vertices) {
            artifact.lightmapUv.push_back(vertex.lightmapUv);
        }
        return artifact;
    }

    MeshData unwrapped = mesh;
    const LightmapUnwrapResult result = GenerateLightmapUv(unwrapped);
    if (!result.ok) {
        error = result.error;
        return artifact;
    }

    // **Remap-nya datang dari unwrapper, bukan dicocokkan ulang.** Mencocokkan
    // hasil ke sumbernya lewat posisi akan menebak: dua vertex yang posisinya
    // sama persis tetapi normalnya berbeda adalah dua vertex yang berbeda, dan
    // menebaknya berarti menukar keduanya sesekali. `GenerateLightmapUv`
    // mencatatnya di `lightmapVertexSource`.
    artifact.fromFirstUv = false;
    artifact.indices = unwrapped.indices;
    artifact.lightmapUv.reserve(unwrapped.vertices.size());
    artifact.vertexRemap.reserve(unwrapped.vertices.size());
    for (const MeshVertex& vertex : unwrapped.vertices) {
        artifact.lightmapUv.push_back(vertex.lightmapUv);
    }
    artifact.vertexRemap = unwrapped.lightmapVertexSource;
    if (artifact.vertexRemap.size() != unwrapped.vertices.size()) {
        error = "unwrapper did not report where each vertex came from";
        artifact = LightmapUvArtifact{};
        return artifact;
    }
    return artifact;
}

bool ApplyLightmapUv(MeshData& mesh, const LightmapUvArtifact& artifact, std::string& error) {
    if (!artifact.IsValid()) {
        error = "lightmap UV artefact is empty";
        return false;
    }
    if (artifact.sourceVertexCount != mesh.vertices.size()) {
        // **Artefak yang dipanggang untuk mesh lain adalah remap yang menunjuk
        // ke luar daftar vertex.** Ditolak di sini, bukan dibiarkan membaca
        // melewati ujung buffer.
        error = "lightmap UV artefact was baked for a mesh with " +
                std::to_string(artifact.sourceVertexCount) + " vertices, not " +
                std::to_string(mesh.vertices.size());
        return false;
    }

    if (artifact.fromFirstUv) {
        if (artifact.lightmapUv.size() != mesh.vertices.size()) {
            error = "lightmap UV artefact has the wrong number of UVs";
            return false;
        }
        for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
            mesh.vertices[i].lightmapUv = artifact.lightmapUv[i];
        }
        mesh.hasLightmapUv = true;
        return true;
    }

    std::vector<MeshVertex> vertices;
    vertices.reserve(artifact.vertexRemap.size());
    std::vector<SkinInfluence> influences;
    if (!mesh.influences.empty()) {
        influences.reserve(artifact.vertexRemap.size());
    }
    for (std::size_t i = 0; i < artifact.vertexRemap.size(); ++i) {
        const uint32_t source = artifact.vertexRemap[i];
        if (source >= mesh.vertices.size()) {
            error = "lightmap UV artefact points outside the vertex list";
            return false;
        }
        MeshVertex vertex = mesh.vertices[source];
        vertex.lightmapUv = artifact.lightmapUv[i];
        vertices.push_back(vertex);
        if (!mesh.influences.empty()) {
            influences.push_back(mesh.influences[source]);
        }
    }
    for (const uint32_t index : artifact.indices) {
        if (index >= vertices.size()) {
            error = "lightmap UV artefact has an index outside its own vertex list";
            return false;
        }
    }

    mesh.vertices = std::move(vertices);
    mesh.indices = artifact.indices;
    if (!influences.empty()) {
        mesh.influences = std::move(influences);
    }
    mesh.hasLightmapUv = true;
    return true;
}

uint64_t LightmapUvCacheKey(const std::filesystem::path& source) {
    std::error_code code;
    const auto size = std::filesystem::file_size(source, code);
    if (code) {
        return 0;
    }
    const auto written = std::filesystem::last_write_time(source, code);
    if (code) {
        return 0;
    }
    // Ukuran dan waktu tulisnya, bukan seluruh isinya — alasan yang sama dengan
    // `IblCacheKey`: berkas mesh bisa puluhan megabyte, dan membacanya
    // seluruhnya untuk memutuskan bahwa kita tidak perlu membacanya membuat
    // penghematannya sendiri ikut hilang.
    const auto stamp = written.time_since_epoch().count();
    uint64_t hash = HashInto(1469598103934665603ull, &size, sizeof(size));
    hash = HashInto(hash, &stamp, sizeof(stamp));
    const std::string text = source.generic_string();
    hash = HashInto(hash, text.data(), text.size());
    hash = HashInto(hash, &kLightmapUvVersion, sizeof(kLightmapUvVersion));
    return hash;
}

std::filesystem::path LightmapUvCachePath(const std::filesystem::path& cacheDir, uint64_t key) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.simlmuv", static_cast<unsigned long long>(key));
    return cacheDir / name;
}

bool WriteLightmapUvArtifact(const std::filesystem::path& file,
                             const LightmapUvArtifact& artifact, std::string& error) {
    if (!artifact.IsValid()) {
        error = "nothing to write";
        return false;
    }
    std::error_code code;
    if (!file.parent_path().empty()) {
        std::filesystem::create_directories(file.parent_path(), code);
    }

    const std::filesystem::path temporary = UniqueTemporaryPath(file);
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot open " + temporary.string() + " for writing";
        return false;
    }

    LightmapUvHeader header{};
    std::memcpy(header.magic, kLightmapUvMagic, sizeof(kLightmapUvMagic));
    header.version = kLightmapUvVersion;
    header.fromFirstUv = artifact.fromFirstUv ? 1u : 0u;
    header.sourceVertexCount = artifact.sourceVertexCount;
    header.remapCount = artifact.vertexRemap.size();
    header.indexCount = artifact.indices.size();
    header.uvCount = artifact.lightmapUv.size();

    const auto write = [&stream](const void* data, std::size_t bytes) {
        if (bytes != 0) {
            stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
        }
    };
    write(&header, sizeof(header));
    write(artifact.vertexRemap.data(), sizeof(uint32_t) * artifact.vertexRemap.size());
    write(artifact.indices.data(), sizeof(uint32_t) * artifact.indices.size());
    write(artifact.lightmapUv.data(), sizeof(Vec2) * artifact.lightmapUv.size());
    stream.close();
    if (!stream) {
        error = "write failed for " + temporary.string();
        std::filesystem::remove(temporary, code);
        return false;
    }

    std::filesystem::rename(temporary, file, code);
    if (code) {
        error = "cannot move " + temporary.string() + " into place: " + code.message();
        std::filesystem::remove(temporary, code);
        return false;
    }
    return true;
}

bool ReadLightmapUvArtifact(const std::filesystem::path& file, LightmapUvArtifact& out,
                            std::string& error) {
    std::ifstream stream(file, std::ios::binary);
    if (!stream) {
        error = "no cached lightmap UVs at " + file.string();
        return false;
    }

    LightmapUvHeader header{};
    stream.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!stream || std::memcmp(header.magic, kLightmapUvMagic, sizeof(kLightmapUvMagic)) != 0) {
        error = file.string() + " is not a lightmap UV artefact";
        return false;
    }
    if (header.version != kLightmapUvVersion) {
        error = "cached lightmap UVs were written by another cook version";
        return false;
    }
    // Batas atas yang masuk akal: jumlahnya datang dari berkas, dan
    // mengalokasikan sebanyak yang disebutnya berarti sebuah berkas rusak bisa
    // meminta puluhan gigabyte sebelum satu pun byte isinya dibaca.
    constexpr uint64_t kMaxVertices = 64ull * 1024 * 1024;
    if (header.uvCount == 0 || header.uvCount > kMaxVertices ||
        header.remapCount > kMaxVertices || header.indexCount > kMaxVertices * 3) {
        error = "cached lightmap UVs have an implausible size";
        return false;
    }
    // **Bentuk yang disebutnya harus konsisten dengan dirinya sendiri.** Yang
    // lolos dari sini tidak akan membaca melewati ujung buffer.
    const bool fromFirstUv = header.fromFirstUv != 0;
    if (fromFirstUv) {
        if (header.remapCount != 0 || header.indexCount != 0) {
            error = "cached lightmap UVs claim to reuse the first UV but carry a remap";
            return false;
        }
    } else if (header.remapCount != header.uvCount || header.indexCount == 0) {
        error = "cached lightmap UVs have a remap that does not match their UVs";
        return false;
    }

    LightmapUvArtifact artifact;
    artifact.fromFirstUv = fromFirstUv;
    artifact.sourceVertexCount = header.sourceVertexCount;
    artifact.vertexRemap.resize(static_cast<std::size_t>(header.remapCount));
    artifact.indices.resize(static_cast<std::size_t>(header.indexCount));
    artifact.lightmapUv.resize(static_cast<std::size_t>(header.uvCount));

    const auto read = [&stream](void* data, std::size_t bytes) {
        if (bytes != 0) {
            stream.read(static_cast<char*>(data), static_cast<std::streamsize>(bytes));
        }
    };
    read(artifact.vertexRemap.data(), sizeof(uint32_t) * artifact.vertexRemap.size());
    read(artifact.indices.data(), sizeof(uint32_t) * artifact.indices.size());
    read(artifact.lightmapUv.data(), sizeof(Vec2) * artifact.lightmapUv.size());
    if (!stream) {
        error = "cached lightmap UVs are shorter than the header promises";
        return false;
    }

    out = std::move(artifact);
    return true;
}

MeshData LoadMeshWithLightmapUv(const std::filesystem::path& source,
                                const std::filesystem::path& cacheDir, std::string& error) {
    MeshData mesh = LoadMesh(source, error);
    if (cacheDir.empty() || !mesh.IsValid()) {
        return mesh;
    }

    const uint64_t key = LightmapUvCacheKey(source);
    if (key == 0) {
        // Berkas yang tidak bisa dibaca stat-nya tidak punya kunci yang stabil,
        // dan artefak berkunci nol akan dibagi seluruh berkas seperti itu.
        return mesh;
    }
    const std::filesystem::path file = LightmapUvCachePath(cacheDir, key);

    LightmapUvArtifact artifact;
    std::string cacheError;
    if (ReadLightmapUvArtifact(file, artifact, cacheError) &&
        ApplyLightmapUv(mesh, artifact, cacheError)) {
        return mesh;
    }

    LightmapUvSuitability check;
    std::string cookError;
    artifact = CookLightmapUv(mesh, check, cookError);
    if (!artifact.IsValid()) {
        // **Gagal memanggang bukan gagal memuat.** Mesh-nya sah dan tetap
        // dipakai; yang hilang cuma lightmap-nya, dan itu disebutkan sekali.
        SIM_WARN("Assets", "cannot cook lightmap UVs for {}: {}",
                 source.filename().string(),
                 cookError.empty() ? check.reason : cookError);
        return mesh;
    }
    if (!ApplyLightmapUv(mesh, artifact, cookError)) {
        SIM_WARN("Assets", "lightmap UVs for {} could not be applied: {}",
                 source.filename().string(), cookError);
        return mesh;
    }
    SIM_INFO("Assets", "lightmap UVs cooked for {}: {}, {} vertices",
             source.filename().string(),
             artifact.fromFirstUv ? "first UV reused" : "unwrapped",
             artifact.lightmapUv.size());
    if (!WriteLightmapUvArtifact(file, artifact, cookError)) {
        // Gagal menulis juga bukan gagal memuat: UV-nya sudah ada di memori.
        // Yang hilang cuma kesempatan melewatkan cook berikutnya.
        SIM_WARN("Assets", "lightmap UVs for {} could not be cached: {}",
                 source.filename().string(), cookError);
    }
    return mesh;
}

}  // namespace sim::assets
