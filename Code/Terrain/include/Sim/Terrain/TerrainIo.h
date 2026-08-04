#pragma once

#include "Sim/Terrain/Terrain.h"

#include <filesystem>
#include <string>
#include <vector>

namespace sim::terrain {

/// Versi skema `.simterrain` yang ditulis sekarang.
///
/// 2 menambahkan daftar layer material dan peta hole. Berkas versi 1 tetap
/// terbaca: ia dimuat sebagai terrain berlayer tunggal tanpa lubang, yang memang
/// persis keadaannya.
inline constexpr int kTerrainSchemaVersion = 2;

/// Isi berkas `.simterrain`: deskripsi terrain, bukan tingginya.
///
/// **Heightmap tinggal di berkas pendamping, bukan di dalam JSON-nya.** Sebuah
/// heightmap 4096² adalah 33 MB angka; ditulis sebagai teks JSON ia menjadi
/// ratusan megabyte yang tidak bisa dibaca manusia, tidak bisa di-diff dengan
/// berguna, dan lambat diurai. Yang ada di `.simterrain` adalah yang memang
/// ingin dibaca dan diubah orang — ukuran ubin, skala, rentang tinggi.
///
/// Peta bobot dan peta hole mengikuti aturan yang sama, dan nama berkasnya ikut
/// tercatat di JSON alih-alih ditebak dari nama dokumennya: pemuat yang menebak
/// nama akan diam-diam mengambil peta milik terrain lain yang kebetulan senama,
/// dan tidak punya cara membedakan "berkasnya belum ada" dari "layernya memang
/// belum pernah dicat".
struct TerrainDocument {
    std::string name;
    TerrainDesc desc;
    /// Nama berkas heightmap, relatif terhadap `.simterrain`-nya.
    std::string heightmapFile;
    /// Nama berkas peta hole. Kosong berarti terrain tanpa lubang.
    std::string holeFile;
};

struct TerrainIoResult {
    bool ok = false;
    std::string error;
    int sourceVersion = kTerrainSchemaVersion;
};

/// Keluarannya deterministik — urutan field tetap, angka tanpa bergantung locale
/// — supaya menyimpan dokumen yang tidak disunting menghasilkan byte yang sama.
///
/// Daftar layer diterima terpisah, bukan sebagai anggota `TerrainDocument`,
/// karena di memori ia tinggal di dalam `Terrain` — bersama peta bobot yang
/// ditunjuknya. Menyalinnya ke dokumen berarti dua daftar yang harus dijaga tetap
/// sama setiap kali sebuah layer ditambah, dihapus, atau dipindah, dan urutan
/// layer yang bergeser di satu salinan saja adalah cat yang berpindah ke material
/// yang salah.
std::string SaveDocumentToString(const TerrainDocument& document,
                                 const std::vector<TerrainLayer>& layers);
TerrainIoResult LoadDocumentFromString(TerrainDocument& document,
                                       std::vector<TerrainLayer>& layers,
                                       const std::string& text);

/// Menyimpan deskriptor beserta seluruh peta pendampingnya: heightmap, satu peta
/// bobot per layer yang pernah dicat, dan peta hole kalau ada lubangnya.
///
/// Peta yang tidak menyimpan apa pun tidak ditulis, dan namanya tidak dicatat.
/// Aturannya satu dan berlaku untuk ketiganya, jadi tidak ada berkas nol byte
/// yang harus dijelaskan dan tidak ada nama yang menunjuk berkas yang tidak ada.
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

// --- peta bobot dan peta hole -------------------------------------------------

/// PNG greyscale 8-bit. 256 tingkat sudah lebih halus daripada yang bisa
/// dibedakan mata pada campuran material, dan sama dengan yang dipakai peta splat
/// di mana pun — jadi peta bobot bisa disiapkan di penyunting gambar biasa.
TerrainIoResult SaveWeightPng(const Terrain& terrain, int layer,
                              const std::filesystem::path& path);
TerrainIoResult LoadWeightPng(Terrain& terrain, int layer, const std::filesystem::path& path);

/// Peta hole ditulis sebagai 0/255, bukan 0/1: berkasnya gambar, dan gambar yang
/// seluruhnya bernilai satu terlihat hitam pekat di penyunting mana pun.
TerrainIoResult SaveHolePng(const Terrain& terrain, const std::filesystem::path& path);
TerrainIoResult LoadHolePng(Terrain& terrain, const std::filesystem::path& path);

/// Enkode PNG greyscale ke memori. Terbuka untuk test.
std::vector<unsigned char> EncodeHeightmapPng(const Sample* samples, int width, int height);
std::vector<unsigned char> EncodeMaskPng(const uint8_t* values, int width, int height);

}  // namespace sim::terrain
