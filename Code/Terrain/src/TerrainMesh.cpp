#include "Sim/Terrain/TerrainMesh.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <glm/geometric.hpp>
#include <limits>
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

/// Menjahit sebuah simpul tepi ke ruas tepi tetangga yang lebih kasar.
///
/// `line` adalah daftar sampel milik ubin **kasar** di sepanjang tepi itu —
/// dihasilkan `Columns` yang sama persis dengan yang dipakai ubin kasar untuk
/// menyusun simpulnya. Dipakai bersama, bukan dihitung ulang: dua rumus yang
/// "seharusnya sama" adalah dua rumus yang suatu saat tidak sama lagi, dan yang
/// tidak sama di sini adalah retakan yang justru sedang ditutup.
///
/// Tinggi **dan** normalnya diinterpolasi. Tinggi saja menutup retakannya, dan
/// normal yang tidak ikut meninggalkan garis terang di tempat retakan tadi —
/// cacat yang lebih halus, dan karena itu lebih lama dicari.
void SnapToCoarseEdge(const std::vector<int>& line, int at, float& height, Vec3& normal,
                      const std::function<float(int)>& heightAt,
                      const std::function<Vec3(int)>& normalAt) {
    if (line.size() < 2 || at <= line.front() || at >= line.back()) {
        return;
    }
    const auto upper = std::upper_bound(line.begin(), line.end(), at);
    const int b = *upper;
    const int a = *(upper - 1);
    if (a == at) {
        return;  // simpul ini juga dimiliki yang kasar; tidak ada yang digeser
    }
    const float t = static_cast<float>(at - a) / static_cast<float>(b - a);
    height = heightAt(a) * (1.0f - t) + heightAt(b) * t;
    normal = glm::normalize(normalAt(a) * (1.0f - t) + normalAt(b) * t);
}

}  // namespace

int LodStep(int lod) { return 1 << std::clamp(lod, 0, 16); }

int SelectLod(float distanceMeters, float tileSizeMeters, int maxLod, float quality) {
    if (maxLod <= 0 || tileSizeMeters <= 0.0f || quality <= 0.0f) {
        return 0;
    }
    const float threshold = tileSizeMeters * quality;
    if (!(distanceMeters > threshold)) {
        // Termasuk NaN: `!(a > b)` bukan `a <= b`, dan jarak NaN yang menjawab
        // LOD terkasar akan membuat ubin di depan hidung tiba-tiba menjadi
        // empat segitiga.
        return 0;
    }
    // Satu tingkat per penggandaan. `log2` lalu dibulatkan ke bawah: yang
    // membulatkan ke terdekat membuat ambangnya jatuh di tengah penggandaan,
    // sehingga sebuah ubin berganti perincian saat ia masih menempati piksel
    // dua kali lebih banyak daripada yang dianggarkan.
    const int lod = static_cast<int>(std::floor(std::log2(distanceMeters / threshold))) + 1;
    return std::clamp(lod, 0, maxLod);
}

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

Vec3 SampleColor(const Terrain& terrain, int x, int y) {
    const int layers = terrain.LayerCount();
    if (layers <= 0) {
        return Vec3(0.5f);
    }

    Vec3 blended(0.0f);
    int used = 0;
    // Layer dasar dilewati di sini dan dibayar dengan sisanya di bawah: bobotnya
    // memang tidak tersimpan, dan membacanya lewat `WeightAt(0, ...)` berarti
    // meminta angka yang harus dihitung ulang tiap kali.
    for (int layer = 1; layer < layers; ++layer) {
        const int weight = terrain.WeightAt(layer, x, y);
        if (weight == 0) {
            continue;
        }
        blended += terrain.Layer(layer).color * (static_cast<float>(weight) / 255.0f);
        used += weight;
    }

    const float base =
        static_cast<float>(kWeightMax - std::min(used, static_cast<int>(kWeightMax))) / 255.0f;
    return blended + terrain.Layer(0).color * base;
}

assets::MeshData BuildTileMesh(const Terrain& terrain, int tileX, int tileY, int lod,
                               const TileNeighborLods& neighbors) {
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

    // Tepi yang bertetangga dengan ubin lebih kasar dijahit ke sana. Daftar
    // simpulnya dibangun dengan `Columns` yang sama, jadi ia sama persis dengan
    // yang dipakai ubin kasar itu sendiri.
    const std::vector<int> coarseLeft =
        neighbors.negativeX > lod ? Columns(y0, y1, LodStep(neighbors.negativeX))
                                  : std::vector<int>{};
    const std::vector<int> coarseRight =
        neighbors.positiveX > lod ? Columns(y0, y1, LodStep(neighbors.positiveX))
                                  : std::vector<int>{};
    const std::vector<int> coarseBack =
        neighbors.negativeY > lod ? Columns(x0, x1, LodStep(neighbors.negativeY))
                                  : std::vector<int>{};
    const std::vector<int> coarseFront =
        neighbors.positiveY > lod ? Columns(x0, x1, LodStep(neighbors.positiveY))
                                  : std::vector<int>{};

    for (const int y : rows) {
        for (const int x : columns) {
            float height = terrain.HeightAt(x, y);
            Vec3 normal = SampleNormal(terrain, x, y);

            // Sudut ubin tidak dijahit dua kali: ia sudah menjadi ujung kedua
            // tepi, dan `SnapToCoarseEdge` memang menolak ujung. Yang menjahit
            // sudut dua kali akan menggesernya ke tempat yang bukan milik tepi
            // mana pun.
            if (x == x0 && !coarseLeft.empty()) {
                SnapToCoarseEdge(
                    coarseLeft, y, height, normal,
                    [&](int at) { return terrain.HeightAt(x0, at); },
                    [&](int at) { return SampleNormal(terrain, x0, at); });
            } else if (x == x1 && !coarseRight.empty()) {
                SnapToCoarseEdge(
                    coarseRight, y, height, normal,
                    [&](int at) { return terrain.HeightAt(x1, at); },
                    [&](int at) { return SampleNormal(terrain, x1, at); });
            } else if (y == y0 && !coarseBack.empty()) {
                SnapToCoarseEdge(
                    coarseBack, x, height, normal,
                    [&](int at) { return terrain.HeightAt(at, y0); },
                    [&](int at) { return SampleNormal(terrain, at, y0); });
            } else if (y == y1 && !coarseFront.empty()) {
                SnapToCoarseEdge(
                    coarseFront, x, height, normal,
                    [&](int at) { return terrain.HeightAt(at, y1); },
                    [&](int at) { return SampleNormal(terrain, at, y1); });
            }

            const Vec3 position(static_cast<float>(x) * desc.sampleSpacing, height,
                                static_cast<float>(y) * desc.sampleSpacing);

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
            // Warnanya diambil dari sampelnya sendiri, bukan dijahit ke
            // tetangga seperti tinggi dan normal. Cat tidak menghasilkan
            // retakan — yang berselisih di tepi hanya rona, dan menjahitnya
            // berarti membaca peta bobot tetangga di setiap simpul tepi untuk
            // selisih yang tidak terlihat.
            vertex.color = Vec4(SampleColor(terrain, x, y), 1.0f);
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
