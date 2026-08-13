// Impor berkas `.vdb` menjadi grid padat.
//
// **Memadatkan yang jarang, dan itu keputusan yang layak disebut.** VDB jarang
// justru karena kebanyakan isinya kosong; memadatkannya membuang keunggulan
// formatnya. Yang membeli kembali biayanya adalah tujuannya: tekstur 3D di GPU
// pun padat, jadi pemadatan harus terjadi di suatu tempat — dan sekali saat
// impor jauh lebih murah daripada di setiap frame. Volume yang terlalu besar
// untuk dipadatkan ditolak dengan pesan, bukan dipadatkan sampai kehabisan
// memori.
//
// Berkasnya tetap ikut dibangun tanpa OpenVDB dan menolak dengan pesan yang
// menyebut sebabnya, mengikuti pola `UsdImport.cpp`.

#include "Sim/Volume/SdfBake.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace sim::volume {
namespace {

/// Satu-satunya ekstensi volume yang dikenal, dan daftarnya tetap dibangkitkan
/// alih-alih dipatok di `AssetTypes` — supaya build tanpa OpenVDB tidak
/// menawarkan impor untuk berkas yang tidak bisa dibacanya.
constexpr std::array<std::string_view, 1> kVdbExtensions{".vdb"};

std::string ToLower(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

}  // namespace

bool CanRead(std::string_view extension) {
    const std::string lower = ToLower(extension);
    const auto readable = ReadableExtensions();
    return std::find(readable.begin(), readable.end(), lower) != readable.end();
}

}  // namespace sim::volume

#if SIM_WITH_OPENVDB

#include <openvdb/openvdb.h>

#include <mutex>

namespace sim::volume {
namespace {

void EnsureInitialisedForIo() {
    static std::once_flag once;
    std::call_once(once, [] { openvdb::initialize(); });
}

}  // namespace

std::span<const std::string_view> ReadableExtensions() { return kVdbExtensions; }

SdfBakeResult ListVdbGrids(const std::filesystem::path& path, std::vector<std::string>& outNames) {
    SdfBakeResult result;
    outNames.clear();

    std::error_code code;
    if (!std::filesystem::exists(path, code)) {
        result.error = "file does not exist: " + path.string();
        return result;
    }

    EnsureInitialisedForIo();
    try {
        openvdb::io::File file(path.string());
        file.open(/*delayLoad=*/false);
        for (auto it = file.beginName(); it != file.endName(); ++it) {
            outNames.push_back(*it);
        }
        file.close();
    } catch (const std::exception& error) {
        result.error = "cannot read " + path.string() + ": " + error.what();
        return result;
    }

    if (outNames.empty()) {
        result.error = "no grids in " + path.string();
        return result;
    }
    result.ok = true;
    return result;
}

SdfBakeResult LoadVdb(const std::filesystem::path& path, const VdbLoadSettings& settings,
                      VolumeGrid& out) {
    SdfBakeResult result;
    out = VolumeGrid{};

    std::error_code code;
    if (!std::filesystem::exists(path, code)) {
        result.error = "file does not exist: " + path.string();
        return result;
    }

    EnsureInitialisedForIo();
    openvdb::FloatGrid::Ptr grid;
    try {
        openvdb::io::File file(path.string());
        file.open(/*delayLoad=*/false);

        if (!settings.gridName.empty()) {
            if (!file.hasGrid(settings.gridName)) {
                // Menyebutkan yang ada, bukan hanya yang tidak ada: berkas asap
                // membawa beberapa grid, dan yang dicari biasanya salah satu
                // dari daftar itu dengan nama yang sedikit berbeda.
                std::string available;
                for (auto it = file.beginName(); it != file.endName(); ++it) {
                    available += available.empty() ? *it : ", " + *it;
                }
                file.close();
                result.error = "no grid named '" + settings.gridName + "' in " + path.string() +
                               "; this file has: " + available;
                return result;
            }
            grid = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid(settings.gridName));
            if (grid == nullptr) {
                file.close();
                result.error = "grid '" + settings.gridName + "' in " + path.string() +
                               " is not a scalar float grid";
                return result;
            }
        } else {
            // Grid float **pertama**, bukan grid pertama: berkas asap kerap
            // menyimpan grid vektor kecepatan lebih dulu, dan mengambilnya
            // menghasilkan penolakan yang membingungkan pada berkas yang
            // sebenarnya baik-baik saja.
            for (auto it = file.beginName(); it != file.endName() && grid == nullptr; ++it) {
                grid = openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid(*it));
            }
            if (grid == nullptr) {
                file.close();
                result.error = "no scalar float grid in " + path.string();
                return result;
            }
        }
        file.close();
    } catch (const std::exception& error) {
        result.error = "cannot read " + path.string() + ": " + error.what();
        return result;
    }

    const openvdb::CoordBBox bounds = grid->evalActiveVoxelBoundingBox();
    if (bounds.empty()) {
        result.error = "grid '" + grid->getName() + "' in " + path.string() + " has no active voxels";
        return result;
    }

    const openvdb::Coord dim = bounds.dim();
    const auto estimate = static_cast<double>(dim.x()) * static_cast<double>(dim.y()) *
                          static_cast<double>(dim.z());
    if (estimate > static_cast<double>(settings.maxVoxels)) {
        result.error = "grid is " + std::to_string(dim.x()) + "x" + std::to_string(dim.y()) + "x" +
                       std::to_string(dim.z()) + " = " +
                       std::to_string(static_cast<uint64_t>(estimate)) +
                       " voxels once made dense, over the limit of " +
                       std::to_string(settings.maxVoxels);
        return result;
    }

    // Transformasinya harus linear dan seragam: tekstur 3D tidak bisa membawa
    // frustum atau taper, dan menerimanya diam-diam menghasilkan asap yang
    // bentuknya salah tanpa satu pun galat.
    const openvdb::math::Transform& transform = grid->transform();
    if (!transform.isLinear()) {
        result.error = "grid '" + grid->getName() + "' uses a non-linear transform, which cannot "
                       "be represented as a 3D texture";
        return result;
    }
    const openvdb::Vec3d voxel = transform.voxelSize();
    if (std::abs(voxel.x() - voxel.y()) > 1e-6 || std::abs(voxel.x() - voxel.z()) > 1e-6) {
        result.error = "grid '" + grid->getName() + "' has non-uniform voxels (" +
                       std::to_string(voxel.x()) + ", " + std::to_string(voxel.y()) + ", " +
                       std::to_string(voxel.z()) + ")";
        return result;
    }

    const openvdb::Coord min = bounds.min();
    out.name = grid->getName();
    out.voxelSize = static_cast<float>(voxel.x());
    out.background = grid->background();
    out.sizeX = static_cast<uint32_t>(dim.x());
    out.sizeY = static_cast<uint32_t>(dim.y());
    out.sizeZ = static_cast<uint32_t>(dim.z());
    // Titik asal diambil lewat transformasinya sendiri, bukan dihitung dari
    // indeks dikali ukuran voxel: transformasi VDB boleh punya translasi, dan
    // mengabaikannya menggeser seluruh asap.
    const openvdb::Vec3d world = transform.indexToWorld(min);
    out.origin = Vec3(static_cast<float>(world.x()), static_cast<float>(world.y()),
                      static_cast<float>(world.z()));
    out.values.assign(out.VoxelCount(), out.background);

    float low = std::numeric_limits<float>::max();
    float high = std::numeric_limits<float>::lowest();
    auto accessor = grid->getConstAccessor();
    for (uint32_t z = 0; z < out.sizeZ; ++z) {
        for (uint32_t y = 0; y < out.sizeY; ++y) {
            const std::size_t row =
                (static_cast<std::size_t>(z) * out.sizeY + static_cast<std::size_t>(y)) * out.sizeX;
            for (uint32_t x = 0; x < out.sizeX; ++x) {
                const openvdb::Coord at(min.x() + static_cast<int32_t>(x),
                                        min.y() + static_cast<int32_t>(y),
                                        min.z() + static_cast<int32_t>(z));
                const float value = accessor.getValue(at);
                out.values[row + x] = value;
                low = std::min(low, value);
                high = std::max(high, value);
            }
        }
    }
    out.minValue = low;
    out.maxValue = high;

    result.ok = true;
    return result;
}

}  // namespace sim::volume

#else  // SIM_WITH_OPENVDB

namespace sim::volume {

std::span<const std::string_view> ReadableExtensions() {
    // Kosong: build ini tidak bisa membaca satu pun berkas volume, dan
    // `AssetTypes` membacanya dari sini alih-alih memuat daftar tetap.
    return {};
}

SdfBakeResult ListVdbGrids(const std::filesystem::path& path, std::vector<std::string>& outNames) {
    outNames.clear();
    SdfBakeResult result;
    result.error = "this build has no OpenVDB, so " + path.string() +
                   " cannot be read. See docs/DEPENDENCIES.md.";
    return result;
}

SdfBakeResult LoadVdb(const std::filesystem::path& path, const VdbLoadSettings& /*settings*/,
                      VolumeGrid& out) {
    out = VolumeGrid{};
    SdfBakeResult result;
    result.error = "this build has no OpenVDB, so " + path.string() +
                   " cannot be read. See docs/DEPENDENCIES.md.";
    return result;
}

}  // namespace sim::volume

#endif  // SIM_WITH_OPENVDB
