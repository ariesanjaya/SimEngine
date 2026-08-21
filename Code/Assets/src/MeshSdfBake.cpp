#include "Sim/Assets/MeshSdfBake.h"

#include "Sim/Assets/MeshData.h"
#include "Sim/Core/Log.h"
#include "Sim/Volume/SdfBake.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace sim::assets {
namespace {

/// Dinaikkan setiap kali hasil bake bisa berubah untuk mesh yang sama — lebar
/// pita diubah, pemilihan voxel diubah, pustakanya berganti. Kunci cache ikut
/// berubah, jadi mesh yang sudah pernah dibake dikerjakan ulang.
constexpr uint64_t kBakerVersion = 1;

/// Delapan byte pertama berkas `.simsdf`. Berkas yang tidak dimulai dengan ini
/// ditolak alih-alih diurai — sebuah berkas lain yang kebetulan sepanjang
/// header akan terbaca sebagai grid berukuran acak.
constexpr char kMagic[8] = {'S', 'I', 'M', 'S', 'D', 'F', '1', '\0'};

/// Header berkas. **Setiap medan eksplisit, tanpa satu pun padding tersirat:**
/// yang ditulis `fwrite` atas sebuah struct berpadding adalah byte yang tidak
/// pernah diinisialisasi, dan berkas yang isinya berbeda antar jalan adalah
/// cache yang tidak pernah kena.
struct FileHeader {
    char magic[8];
    uint32_t version;
    uint32_t sizeX;
    uint32_t sizeY;
    uint32_t sizeZ;
    float voxelSize;
    float band;
    float originX;
    float originY;
    float originZ;
};
static_assert(sizeof(FileHeader) == 8 + 4 * 9, "header .simsdf tidak boleh berpadding");

std::atomic<uint64_t> g_bakeCount{0};

constexpr uint64_t kFnvOffset = 1469598103934665603ull;

uint64_t HashInto(uint64_t hash, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {};
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!file) {
        return {};
    }
    return bytes;
}

MeshSdfBakeResult Fail(std::string message) {
    MeshSdfBakeResult result;
    result.error = std::move(message);
    return result;
}

/// Berapa voxel yang dibutuhkan sebuah kotak batas pada sisi voxel tertentu.
/// Rumusnya sama dengan penjaga di `sim::volume::BakeMeshSdf`, dan harus tetap
/// sama: yang menghitung anggaran di sini dan yang menolak di sana tidak boleh
/// berselisih, kalau tidak baker menolak sesuatu yang sudah dinyatakan muat.
double VoxelEstimate(const Vec3& extent, float voxelSize, float band) {
    const float pad = band + voxelSize;
    const Vec3 padded = extent + Vec3(2.0f * pad);
    return static_cast<double>(std::ceil(padded.x / voxelSize) + 1.0) *
           static_cast<double>(std::ceil(padded.y / voxelSize) + 1.0) *
           static_cast<double>(std::ceil(padded.z / voxelSize) + 1.0);
}

}  // namespace

float BandMeters(const Vec3& boundsMin, const Vec3& boundsMax, const MeshSdfSettings& settings,
                 float voxelSize) {
    const Vec3 extent = glm::max(boundsMax - boundsMin, Vec3(0.0f));
    const float longest = std::max({extent.x, extent.y, extent.z});
    return std::max(settings.bandVoxels * voxelSize, settings.bandFraction * longest);
}

float FitVoxelSize(const Vec3& boundsMin, const Vec3& boundsMax,
                   const MeshSdfSettings& settings) {
    float voxel = std::max(settings.voxelSize, 1e-4f);
    if (settings.maxVoxels == 0) {
        return voxel;
    }
    const Vec3 extent = glm::max(boundsMax - boundsMin, Vec3(0.0f));
    // Naik 12,5% sekali putaran, bukan langsung ke tebakan tertutupnya. Jumlah
    // voxel turun pangkat tiga, jadi paling banyak beberapa belas putaran — dan
    // yang didapat sebagai gantinya adalah voxel **sehalus mungkin** yang masih
    // muat, alih-alih hasil pembulatan sebuah rumus yang harus dijaga tetap
    // sepadan dengan penjaga di sisi baker.
    for (int step = 0; step < 64; ++step) {
        if (VoxelEstimate(extent, voxel, BandMeters(boundsMin, boundsMax, settings, voxel)) <=
            static_cast<double>(settings.maxVoxels)) {
            return voxel;
        }
        voxel *= 1.125f;
    }
    return voxel;
}

uint64_t MeshSdfCacheKey(const std::filesystem::path& source, const MeshSdfSettings& settings) {
    const std::vector<uint8_t> bytes = ReadFileBytes(source);
    if (bytes.empty()) {
        return 0;
    }
    uint64_t hash = HashInto(kFnvOffset, bytes.data(), bytes.size());
    // Medannya satu per satu, bukan struct-nya utuh: padding tidak
    // diinisialisasi, dan hash atasnya berbeda antar pemanggilan untuk
    // pengaturan yang sama persis.
    hash = HashInto(hash, &settings.voxelSize, sizeof(settings.voxelSize));
    hash = HashInto(hash, &settings.bandVoxels, sizeof(settings.bandVoxels));
    hash = HashInto(hash, &settings.bandFraction, sizeof(settings.bandFraction));
    hash = HashInto(hash, &settings.maxVoxels, sizeof(settings.maxVoxels));
    hash = HashInto(hash, &kBakerVersion, sizeof(kBakerVersion));
    return hash;
}

std::filesystem::path MeshSdfCachePath(const std::filesystem::path& cacheDir, uint64_t key) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.simsdf", static_cast<unsigned long long>(key));
    return cacheDir / name;
}

bool WriteMeshSdf(const std::filesystem::path& file, const SdfGrid& grid, std::string& error) {
    if (grid.Empty() || grid.distances.size() != grid.VoxelCount()) {
        error = "grid is empty or its size does not match its data";
        return false;
    }
    std::error_code code;
    std::filesystem::create_directories(file.parent_path(), code);

    const std::filesystem::path temporary = file.string() + ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot write " + temporary.string();
            return false;
        }
        FileHeader header{};
        std::memcpy(header.magic, kMagic, sizeof(kMagic));
        header.version = 1;
        header.sizeX = grid.sizeX;
        header.sizeY = grid.sizeY;
        header.sizeZ = grid.sizeZ;
        header.voxelSize = grid.voxelSize;
        header.band = grid.band;
        header.originX = grid.origin.x;
        header.originY = grid.origin.y;
        header.originZ = grid.origin.z;
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(grid.distances.data()),
                  static_cast<std::streamsize>(grid.distances.size() * sizeof(float)));
        if (!out) {
            error = "cannot write " + temporary.string();
            return false;
        }
    }
    std::filesystem::rename(temporary, file, code);
    if (code) {
        std::filesystem::remove(temporary, code);
        error = "cannot move the baked field into " + file.string();
        return false;
    }
    return true;
}

bool ReadMeshSdf(const std::filesystem::path& file, SdfGrid& out, std::string& error) {
    out = SdfGrid{};
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot read " + file.string();
        return false;
    }
    FileHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || std::memcmp(header.magic, kMagic, sizeof(kMagic)) != 0 || header.version != 1) {
        error = file.filename().string() + " is not a .simsdf file this build can read";
        return false;
    }
    const auto voxels = static_cast<std::size_t>(header.sizeX) * header.sizeY * header.sizeZ;
    if (voxels == 0) {
        error = file.filename().string() + " names an empty grid";
        return false;
    }
    out.sizeX = header.sizeX;
    out.sizeY = header.sizeY;
    out.sizeZ = header.sizeZ;
    out.voxelSize = header.voxelSize;
    out.band = header.band;
    out.origin = Vec3(header.originX, header.originY, header.originZ);
    out.distances.resize(voxels);
    in.read(reinterpret_cast<char*>(out.distances.data()),
            static_cast<std::streamsize>(voxels * sizeof(float)));
    // `gcount`, bukan `!in`: sebuah berkas yang terpotong tepat di ujungnya
    // membaca sebagian lalu menyalakan eof, dan eof saja bukan bukti berkasnya
    // utuh.
    if (in.gcount() != static_cast<std::streamsize>(voxels * sizeof(float))) {
        out = SdfGrid{};
        error = file.filename().string() + " is truncated";
        return false;
    }
    return true;
}

uint64_t MeshSdfBakeCount() { return g_bakeCount.load(std::memory_order_relaxed); }

MeshSdfBakeResult BakeMeshSdfFile(const std::filesystem::path& source,
                                  const MeshSdfSettings& settings,
                                  const std::filesystem::path& cacheDir) {
    const uint64_t key = MeshSdfCacheKey(source, settings);
    if (key == 0) {
        return Fail("cannot read " + source.string());
    }
    const std::filesystem::path cached = MeshSdfCachePath(cacheDir, key);

    std::error_code code;
    if (std::filesystem::exists(cached, code)) {
        // Isinya **tidak** dibaca di sini. Yang membacanya adalah pemanggil yang
        // benar-benar memerlukan gridnya; sebuah jawaban "sudah ada" yang
        // membaca 69 MB lebih dulu adalah persis pekerjaan yang dihindari cache.
        MeshSdfBakeResult result;
        result.ok = true;
        result.path = cached;
        result.fromCache = true;
        return result;
    }

    if (!volume::Available()) {
        return Fail("this build has no OpenVDB, so mesh SDFs cannot be baked");
    }

    const auto started = std::chrono::steady_clock::now();

    std::string error;
    const MeshData mesh = LoadMesh(source, error);
    if (!mesh.IsValid()) {
        return Fail("cannot load " + source.filename().string() + ": " + error);
    }

    // Posisinya disalin ke larik `Vec3` yang rapat: `MeshVertex` membawa normal,
    // tangent, dan uv di sebelah posisinya, dan baker menerima span posisi.
    std::vector<Vec3> positions;
    positions.reserve(mesh.vertices.size());
    for (const MeshVertex& vertex : mesh.vertices) {
        positions.push_back(vertex.position);
    }

    volume::SdfBakeSettings bake;
    bake.voxelSize = FitVoxelSize(mesh.boundsMin, mesh.boundsMax, settings);
    // Pita diserahkan dalam voxel karena itulah satuan yang diminta baker,
    // tetapi yang menentukan lebarnya adalah ukuran mesh-nya.
    bake.bandVoxels =
        BandMeters(mesh.boundsMin, mesh.boundsMax, settings, bake.voxelSize) / bake.voxelSize;
    bake.maxVoxels = settings.maxVoxels;

    SdfGrid grid;
    const volume::SdfBakeResult baked =
        volume::BakeMeshSdf(positions, mesh.indices, bake, grid);
    if (!baked) {
        return Fail(source.filename().string() + ": " + baked.error);
    }

    if (!WriteMeshSdf(cached, grid, error)) {
        return Fail(error);
    }

    g_bakeCount.fetch_add(1, std::memory_order_relaxed);

    MeshSdfBakeResult result;
    result.ok = true;
    result.path = cached;
    result.voxelSize = grid.voxelSize;
    result.sizeX = grid.sizeX;
    result.sizeY = grid.sizeY;
    result.sizeZ = grid.sizeZ;
    result.milliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    SIM_INFO("Assets", "Baked SDF for {} ({}x{}x{} at {:.3f} m, {} tris) in {:.1f} ms",
             source.filename().string(), grid.sizeX, grid.sizeY, grid.sizeZ, grid.voxelSize,
             mesh.TriangleCount(), result.milliseconds);
    return result;
}

}  // namespace sim::assets
