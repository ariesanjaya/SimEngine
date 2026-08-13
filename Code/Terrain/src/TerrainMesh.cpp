#include "Sim/Terrain/TerrainMesh.h"

#include <algorithm>
#include <glm/geometric.hpp>
#include <vector>

namespace sim::terrain {
namespace {

/// Kolom sampel yang menjadi simpul sebuah ubin, dari `x0` sampai `x1` inklusif.
///
/// **`x1` selalu ikut, walaupun langkahnya tidak mendarat tepat padanya.** Ubin
/// terakhir peta membentang S−1 sampel, bukan S, dan S−1 tidak habis dibagi
/// langkah LOD mana pun kecuali satu. Yang membulatkan ke bawah meninggalkan
/// jalur kosong selebar sampai satu langkah di pinggir peta — cacat yang justru
/// paling terlihat, karena di situlah orang berdiri untuk melihat batas dunia.
std::vector<int> Columns(int x0, int x1, int step) {
    std::vector<int> columns;
    if (x1 <= x0) {
        return columns;
    }
    columns.reserve(static_cast<std::size_t>((x1 - x0) / step + 2));
    for (int x = x0; x < x1; x += step) {
        columns.push_back(x);
    }
    columns.push_back(x1);
    return columns;
}

/// True bila salah satu quad halus di dalam petak `[x, x+step) × [y, y+step)`
/// berlubang.
///
/// Petak kasar dibuang bila **ada** yang berlubang di dalamnya, bukan bila
/// seluruhnya berlubang: lubang yang hilang saat kamera menjauh adalah lantai
/// yang tiba-tiba muncul di bawah kaki pemain.
bool AnyHole(const Terrain& terrain, int x, int y, int stepX, int stepY) {
    for (int j = 0; j < stepY; ++j) {
        for (int i = 0; i < stepX; ++i) {
            if (terrain.HoleAt(x + i, y + j)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

int LodStep(int lod) { return 1 << std::clamp(lod, 0, 16); }

Vec3 SampleNormal(const Terrain& terrain, int x, int y) {
    const float spacing = terrain.Desc().sampleSpacing;
    // Beda tengah. `RawAt` menjepit di tepi peta, jadi di pinggir ini menjadi
    // beda maju/mundur dengan sendirinya — tanpa satu pun cabang, dan tanpa
    // pemanggil yang bisa lupa memeriksanya.
    const float left = terrain.HeightAt(x - 1, y);
    const float right = terrain.HeightAt(x + 1, y);
    const float back = terrain.HeightAt(x, y - 1);
    const float front = terrain.HeightAt(x, y + 1);

    const float dx = (right - left) / (2.0f * spacing);
    const float dz = (front - back) / (2.0f * spacing);
    return glm::normalize(Vec3(-dx, 1.0f, -dz));
}

assets::MeshData BuildTileMesh(const Terrain& terrain, int tileX, int tileY, int lod) {
    assets::MeshData mesh;

    const TerrainDesc& desc = terrain.Desc();
    if (tileX < 0 || tileY < 0 || tileX >= desc.tilesX || tileY >= desc.tilesY) {
        return mesh;
    }

    // **Kepemilikan sampel setengah terbuka**, seperti penyimpanannya: ubin
    // (tx,ty) memiliki `[tx·S, (tx+1)·S)`. Meshnya membentang satu sampel lebih
    // jauh, sampai sampel pertama milik tetangganya — kalau tidak, ada jalur
    // selebar satu sampel di antara dua ubin yang tidak digambar siapa pun.
    // Yang dibaca dari tetangga hanya dibaca; tidak ada baris tepi yang
    // disalin, jadi tidak ada dua salinan yang bisa berbeda.
    const int step = LodStep(lod);
    const int x0 = tileX * desc.tileSamples;
    const int y0 = tileY * desc.tileSamples;
    const int x1 = std::min((tileX + 1) * desc.tileSamples, terrain.SamplesX() - 1);
    const int y1 = std::min((tileY + 1) * desc.tileSamples, terrain.SamplesY() - 1);

    const std::vector<int> columns = Columns(x0, x1, step);
    const std::vector<int> rows = Columns(y0, y1, step);
    if (columns.size() < 2 || rows.size() < 2) {
        return mesh;
    }

    mesh.vertices.reserve(columns.size() * rows.size());
    Vec3 boundsMin(std::numeric_limits<float>::max());
    Vec3 boundsMax(std::numeric_limits<float>::lowest());

    for (const int y : rows) {
        for (const int x : columns) {
            const Vec3 position(static_cast<float>(x) * desc.sampleSpacing, terrain.HeightAt(x, y),
                                static_cast<float>(y) * desc.sampleSpacing);
            const Vec3 normal = SampleNormal(terrain, x, y);

            assets::MeshVertex vertex;
            vertex.position = position;
            vertex.normal = normal;
            // UV dalam **meter**, bukan 0..1 per ubin. Layer terrain menyebut
            // ukurannya dalam meter per pengulangan (`TerrainLayer::tileSize`),
            // dan UV yang diregangkan per ubin membuat pengulangan itu berubah
            // ukuran setiap kali seseorang mengganti jumlah ubin.
            vertex.uv = Vec2(position.x, position.z);
            // Tangent mengikuti +X yang diproyeksikan ke permukaan. Dihitung
            // dari normalnya, bukan dari heightmap lagi: keduanya harus tegak
            // lurus, dan dua perhitungan terpisah adalah dua yang bisa
            // berselisih.
            const Vec3 tangent = glm::normalize(Vec3(1.0f, 0.0f, 0.0f) -
                                                normal * normal.x);
            vertex.tangent = Vec4(tangent, 1.0f);
            mesh.vertices.push_back(vertex);

            boundsMin = glm::min(boundsMin, position);
            boundsMax = glm::max(boundsMax, position);
        }
    }

    const std::size_t stride = columns.size();
    for (std::size_t j = 0; j + 1 < rows.size(); ++j) {
        for (std::size_t i = 0; i + 1 < columns.size(); ++i) {
            const int x = columns[i];
            const int y = rows[j];
            if (AnyHole(terrain, x, y, columns[i + 1] - x, rows[j + 1] - y)) {
                continue;
            }
            const uint32_t a = static_cast<uint32_t>(j * stride + i);
            const uint32_t b = a + 1;
            const uint32_t c = static_cast<uint32_t>((j + 1) * stride + i);
            const uint32_t d = c + 1;
            // Berlawanan arah jarum jam dilihat dari atas, sehingga normal
            // segitiganya menunjuk +Y seperti normal simpulnya. Yang terbalik
            // tidak terlihat sebagai galat melainkan sebagai terrain yang
            // menghilang dari satu sisi.
            mesh.indices.insert(mesh.indices.end(), {a, c, b, b, c, d});
        }
    }

    if (mesh.indices.empty()) {
        // Ubin yang seluruhnya berlubang tidak menghasilkan apa-apa — termasuk
        // simpul. Mengunggah simpul tanpa segitiga adalah memori GPU untuk
        // sesuatu yang tidak akan pernah tergambar.
        mesh.vertices.clear();
        return mesh;
    }

    mesh.parts.push_back(assets::SubMesh{0, static_cast<uint32_t>(mesh.indices.size()), -1});
    mesh.boundsMin = boundsMin;
    mesh.boundsMax = boundsMax;
    return mesh;
}

}  // namespace sim::terrain
