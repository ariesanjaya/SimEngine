#pragma once

#include "Sim/Terrain/Terrain.h"

#include <filesystem>
#include <string>
#include <vector>

namespace sim::terrain {

/// Versi skema `.simterrain` yang ditulis sekarang.
inline constexpr int kTerrainSchemaVersion = 1;

/// Isi berkas `.simterrain`: deskripsi terrain, bukan tingginya.
///
/// **Heightmap tinggal di berkas pendamping, bukan di dalam JSON-nya.** Sebuah
/// heightmap 4096² adalah 33 MB angka; ditulis sebagai teks JSON ia menjadi
/// ratusan megabyte yang tidak bisa dibaca manusia, tidak bisa di-diff dengan
/// berguna, dan lambat diurai. Yang ada di `.simterrain` adalah yang memang
/// ingin dibaca dan diubah orang — ukuran ubin, skala, rentang tinggi.
struct TerrainDocument {
    std::string name;
    TerrainDesc desc;
    /// Nama berkas heightmap, relatif terhadap `.simterrain`-nya.
    std::string heightmapFile;
};

struct TerrainIoResult {
    bool ok = false;
    std::string error;
    int sourceVersion = kTerrainSchemaVersion;
};

/// Keluarannya deterministik — urutan field tetap, angka tanpa bergantung locale
/// — supaya menyimpan dokumen yang tidak disunting menghasilkan byte yang sama.
std::string SaveDocumentToString(const TerrainDocument& document);
TerrainIoResult LoadDocumentFromString(TerrainDocument& document, const std::string& text);

/// Menyimpan deskriptor beserta heightmap pendampingnya.
TerrainIoResult SaveTerrain(const Terrain& terrain, const TerrainDocument& document,
                            const std::filesystem::path& path);
/// Memuat keduanya. `terrain` dibangun ulang dari deskriptor di berkas.
TerrainIoResult LoadTerrain(Terrain& terrain, TerrainDocument& document,
                            const std::filesystem::path& path);

// --- heightmap mentah --------------------------------------------------------

/// PNG greyscale 16-bit. Formatnya persis yang dipertukarkan World Machine,
/// Gaea, dan sejenisnya, jadi ekspor dari sana bisa langsung dipakai.
TerrainIoResult SaveHeightmapPng(const Terrain& terrain, const std::filesystem::path& path);
/// Menuntut ukuran PNG cocok dengan terrain. Menskala ulang diam-diam adalah
/// cara paling halus untuk merusak peta seseorang: hasilnya terlihat masuk akal
/// dan tetap salah.
TerrainIoResult LoadHeightmapPng(Terrain& terrain, const std::filesystem::path& path);

/// Membaca PNG apa adanya, tanpa menuntut ukurannya cocok. Dipakai panel untuk
/// menawarkan menyesuaikan terrain sebelum mengimpor.
TerrainIoResult ReadHeightmapPng(const std::filesystem::path& path, std::vector<Sample>& samples,
                                 int& width, int& height);

/// RAW: uint16 little-endian, tanpa header. Tidak bisa membawa ukurannya
/// sendiri, jadi ukuran terrain yang menentukan — itu sifat formatnya, bukan
/// pilihan.
TerrainIoResult SaveHeightmapRaw(const Terrain& terrain, const std::filesystem::path& path);
TerrainIoResult LoadHeightmapRaw(Terrain& terrain, const std::filesystem::path& path);

/// Enkode PNG greyscale 16-bit ke memori. Terbuka untuk test.
std::vector<unsigned char> EncodeHeightmapPng(const Sample* samples, int width, int height);

}  // namespace sim::terrain
