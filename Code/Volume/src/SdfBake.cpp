// Bake mesh → SDF di atas OpenVDB.
//
// **Melengkapi M1 di docs/rencana-implementasi-gi.md**, yang menyebut "Bake SDF
// per-mesh offline → brick sparse" tapi belum pernah ada. Yang dipakai clipmap
// sampai sekarang adalah `BoxSceneField` — jarak ke kotak berorientasi per mesh,
// dan komentarnya sendiri menyebutnya hampiran. Hampiran itu benar untuk kotak
// dan meleset untuk yang lain: pada bola berjari-jari 1, jarak ke kotak
// pembungkusnya di arah diagonal menyebut 0 padahal permukaannya 0,41 jauhnya.
//
// Berkasnya tetap ikut dibangun tanpa OpenVDB dan menolak dengan pesan yang
// menyebut sebabnya, mengikuti pola `UsdImport.cpp`.

#include "Sim/Volume/SdfBake.h"

#include <cmath>
#include <string>

namespace sim::volume {
namespace {

/// Memeriksa masukan **di luar** cabang `#if`.
///
/// Masukan yang cacat harus dijawab dengan kalimat yang sama, ada atau tidak ada
/// OpenVDB. Kalau validasinya ikut hilang bersama backendnya, indeks yang bukan
/// kelipatan tiga akan dilaporkan sebagai "tidak ada OpenVDB" — mengirim orang
/// memasang pustaka untuk memperbaiki berkas yang rusak.
bool ValidateInput(std::span<const Vec3> positions, std::span<const uint32_t> indices,
                   const SdfBakeSettings& settings, std::string& error) {
    if (positions.empty() || indices.size() < 3) {
        error = "mesh has no triangles to bake";
        return false;
    }
    if (indices.size() % 3 != 0) {
        error = "index count " + std::to_string(indices.size()) + " is not a multiple of 3";
        return false;
    }
    if (settings.voxelSize <= 0.0f || settings.bandVoxels <= 0.0f) {
        error = "voxel size and band must both be positive";
        return false;
    }
    for (std::size_t i = 0; i < indices.size(); ++i) {
        if (indices[i] >= positions.size()) {
            error = "index out of range at triangle " + std::to_string(i / 3);
            return false;
        }
    }

    // Penjaga ukuran dihitung dari kotak batas **sebelum** membake, bukan
    // sesudah: menolak setelah tiga puluh dua megabyte teralokasi tidak
    // menolong siapa pun.
    Vec3 low = positions[0];
    Vec3 high = positions[0];
    for (const Vec3& p : positions) {
        low = glm::min(low, p);
        high = glm::max(high, p);
    }
    const float pad = settings.bandVoxels * settings.voxelSize + settings.voxelSize;
    const Vec3 extent = (high - low) + Vec3(2.0f * pad);
    const auto estimate = static_cast<double>(std::ceil(extent.x / settings.voxelSize) + 1.0) *
                          static_cast<double>(std::ceil(extent.y / settings.voxelSize) + 1.0) *
                          static_cast<double>(std::ceil(extent.z / settings.voxelSize) + 1.0);
    if (estimate > static_cast<double>(settings.maxVoxels)) {
        error = "bake would need about " + std::to_string(static_cast<uint64_t>(estimate)) +
                " voxels, over the limit of " + std::to_string(settings.maxVoxels) +
                "; use a coarser voxel size for this mesh";
        return false;
    }
    return true;
}

}  // namespace
}  // namespace sim::volume

#if SIM_WITH_OPENVDB

#include <openvdb/openvdb.h>
#include <openvdb/tools/MeshToVolume.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>

namespace sim::volume {
namespace {

/// `openvdb::initialize()` harus dipanggil sekali sebelum apa pun, dan aman
/// dipanggil berkali-kali — tapi tidak aman dipanggil dari beberapa thread
/// sekaligus pada versi lama. Impor massal berjalan di TaskPool, jadi
/// penjaganya dipasang di sini alih-alih diandalkan pada pemanggil.
void EnsureInitialised() {
    static std::once_flag once;
    std::call_once(once, [] { openvdb::initialize(); });
}

}  // namespace

bool Available() { return true; }

const char* BackendVersion() { return openvdb::getLibraryVersionString(); }

SdfBakeResult BakeMeshSdf(std::span<const Vec3> positions, std::span<const uint32_t> indices,
                          const SdfBakeSettings& settings, SdfGrid& out,
                          std::span<const uint8_t> polygonAttribute) {
    SdfBakeResult result;
    out = SdfGrid{};

    if (!ValidateInput(positions, indices, settings, result.error)) {
        return result;
    }
    if (!polygonAttribute.empty() && polygonAttribute.size() != indices.size() / 3) {
        result.error = "polygon attribute has " + std::to_string(polygonAttribute.size()) +
                       " entries for " + std::to_string(indices.size() / 3) + " triangles";
        return result;
    }

    EnsureInitialised();

    std::vector<openvdb::Vec3s> points;
    points.reserve(positions.size());
    for (const Vec3& p : positions) {
        points.emplace_back(p.x, p.y, p.z);
    }
    std::vector<openvdb::Vec3I> triangles;
    triangles.reserve(indices.size() / 3);
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        triangles.emplace_back(indices[i], indices[i + 1], indices[i + 2]);
    }

    openvdb::FloatGrid::Ptr grid;
    openvdb::Int32Grid::Ptr closestPolygon;
    try {
        const auto transform =
            openvdb::math::Transform::createLinearTransform(settings.voxelSize);
        // Satu `halfWidth`, dan ia berlaku simetris — `bandVoxels` di luar dan
        // di dalam. Yang di dalam ikut dibutuhkan karena sphere tracing bisa
        // bermula di dalam geometri, dan tanda yang hilang di sana membuatnya
        // melangkah keluar menembus dinding.
        if (polygonAttribute.empty()) {
            grid = openvdb::tools::meshToLevelSet<openvdb::FloatGrid>(
                *transform, points, triangles, settings.bandVoxels);
        } else {
            // **`meshToVolume`, bukan `meshToLevelSet`.** Keduanya menjalankan
            // konversi yang sama; yang pertama saja yang bisa mengembalikan
            // indeks poligon terdekat per voxel. Tanpa angka itu, warna sebuah
            // voxel hanya bisa ditebak dari geometri di sekitarnya — dan
            // tebakan sebaik apa pun tidak tahu bahwa batu bertemu kain di
            // sebuah tepi, bukan di sebuah gradien.
            //
            // **Titiknya harus sudah berada di ruang indeks.** `meshToLevelSet`
            // yang mengubahnya lebih dulu; `meshToVolume` menerima adapter apa
            // adanya, dan `QuadAndTriangleDataAdapter` mengembalikan titiknya
            // tanpa menyentuhnya. Menyerahkan titik ruang dunia tidak gagal —
            // ia menghasilkan mesh yang mengecil sebesar satu per sisi voxel,
            // dan yang terlihat hanya grid yang jauh lebih kecil daripada
            // seharusnya. Sponza menyusut dari 315×186×216 menjadi 65×49×52.
            std::vector<openvdb::Vec3s> indexPoints;
            indexPoints.reserve(points.size());
            for (const openvdb::Vec3s& point : points) {
                const openvdb::Vec3d local = transform->worldToIndex(openvdb::Vec3d(point));
                indexPoints.emplace_back(static_cast<float>(local.x()),
                                         static_cast<float>(local.y()),
                                         static_cast<float>(local.z()));
            }
            closestPolygon = openvdb::Int32Grid::create();
            const openvdb::tools::QuadAndTriangleDataAdapter<openvdb::Vec3s, openvdb::Vec3I> mesh(
                indexPoints, triangles);
            grid = openvdb::tools::meshToVolume<openvdb::FloatGrid>(
                mesh, *transform, settings.bandVoxels, settings.bandVoxels, 0,
                closestPolygon.get());
        }
    } catch (const std::exception& error) {
        result.error = std::string("OpenVDB failed to bake the mesh: ") + error.what();
        return result;
    }
    if (grid == nullptr) {
        result.error = "OpenVDB returned no grid";
        return result;
    }

    // Level set OpenVDB jarang; yang dipakai clipmap padat. Menyalinnya di sini
    // — sekali, saat bake — jauh lebih murah daripada membayar penelusuran
    // pohon jarang untuk setiap voxel clipmap di setiap gerakan kamera.
    const openvdb::CoordBBox bounds = grid->evalActiveVoxelBoundingBox();
    if (bounds.empty()) {
        result.error = "bake produced no active voxels; the mesh may be smaller than one voxel";
        return result;
    }

    const openvdb::Coord min = bounds.min();
    const openvdb::Coord dim = bounds.dim();
    out.voxelSize = settings.voxelSize;
    out.band = settings.bandVoxels * settings.voxelSize;
    out.sizeX = static_cast<uint32_t>(dim.x());
    out.sizeY = static_cast<uint32_t>(dim.y());
    out.sizeZ = static_cast<uint32_t>(dim.z());
    out.origin = Vec3(static_cast<float>(min.x()) * settings.voxelSize,
                      static_cast<float>(min.y()) * settings.voxelSize,
                      static_cast<float>(min.z()) * settings.voxelSize);
    out.distances.assign(out.VoxelCount(), out.band);

    if (closestPolygon != nullptr) {
        out.materials.assign(out.VoxelCount(), 0);
    }

    auto accessor = grid->getConstAccessor();
    for (uint32_t z = 0; z < out.sizeZ; ++z) {
        for (uint32_t y = 0; y < out.sizeY; ++y) {
            const std::size_t row =
                (static_cast<std::size_t>(z) * out.sizeY + static_cast<std::size_t>(y)) * out.sizeX;
            for (uint32_t x = 0; x < out.sizeX; ++x) {
                const openvdb::Coord at(min.x() + static_cast<int32_t>(x),
                                        min.y() + static_cast<int32_t>(y),
                                        min.z() + static_cast<int32_t>(z));
                // Voxel tidak aktif mengembalikan latar grid — `+band` di luar,
                // `-band` di dalam — yang persis nilai jenuh yang diinginkan.
                out.distances[row + x] = std::clamp(accessor.getValue(at), -out.band, out.band);
            }
        }
    }

    if (closestPolygon != nullptr) {
        // **Ditelusuri lewat voxel aktifnya, bukan lewat seluruh kotak.** Grid
        // indeks jarang justru di tempat yang penting: hanya voxel di dalam pita
        // yang punya poligon terdekat, dan itu seperempat isi kotaknya. Menyapu
        // seluruh kotak berarti menanyai pohon jarang belasan juta kali untuk
        // jawaban "tidak ada".
        for (auto leaf = closestPolygon->tree().cbeginLeaf(); leaf; ++leaf) {
            for (auto voxel = leaf->cbeginValueOn(); voxel; ++voxel) {
                const openvdb::Coord at = voxel.getCoord();
                const openvdb::Coord local = at - min;
                if (local.x() < 0 || local.y() < 0 || local.z() < 0 ||
                    local.x() >= static_cast<int32_t>(out.sizeX) ||
                    local.y() >= static_cast<int32_t>(out.sizeY) ||
                    local.z() >= static_cast<int32_t>(out.sizeZ)) {
                    continue;
                }
                const int32_t polygon = voxel.getValue();
                if (polygon < 0 ||
                    static_cast<std::size_t>(polygon) >= polygonAttribute.size()) {
                    continue;
                }
                const std::size_t index =
                    (static_cast<std::size_t>(local.z()) * out.sizeY +
                     static_cast<std::size_t>(local.y())) *
                        out.sizeX +
                    static_cast<std::size_t>(local.x());
                out.materials[index] = polygonAttribute[static_cast<std::size_t>(polygon)];
            }
        }
    }

    result.ok = true;
    return result;
}

}  // namespace sim::volume

#else  // SIM_WITH_OPENVDB

namespace sim::volume {

bool Available() { return false; }

const char* BackendVersion() { return ""; }

SdfBakeResult BakeMeshSdf(std::span<const Vec3> positions, std::span<const uint32_t> indices,
                          const SdfBakeSettings& settings, SdfGrid& out,
                          std::span<const uint8_t> polygonAttribute) {
    out = SdfGrid{};
    (void)polygonAttribute;
    SdfBakeResult result;
    // Masukan diperiksa lebih dulu, supaya berkas yang cacat dilaporkan sebagai
    // berkas yang cacat — bukan sebagai pustaka yang kurang.
    if (!ValidateInput(positions, indices, settings, result.error)) {
        return result;
    }
    result.error =
        "this build has no OpenVDB, so mesh SDFs cannot be baked; the GI clipmap falls back to "
        "the oriented-box approximation. See docs/DEPENDENCIES.md.";
    return result;
}

}  // namespace sim::volume

#endif  // SIM_WITH_OPENVDB
