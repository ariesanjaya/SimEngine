#pragma once

#include "Sim/Vegetation/Vegetation.h"

#include <filesystem>
#include <string>

namespace sim::vegetation {

/// Versi skema `.simveg` yang ditulis sekarang.
inline constexpr int kVegetationSchemaVersion = 1;

/// Isi berkas `.simveg`: aturan, bukan instance.
///
/// **Yang ditulis adalah aturan dan benihnya, bukan satu juta posisi.** Menulis
/// posisinya berarti berkas puluhan megabyte yang tidak bisa di-diff, tidak bisa
/// digabung dua orang, dan kehilangan seluruh artinya begitu sebuah aturan
/// diubah — sementara seluruh isinya bisa dihitung ulang dari beberapa ratus
/// byte. Kriteria terima E7.4 menyebut angkanya: sejuta instance, berkas di
/// bawah 5 MB.
///
/// Yang tidak bisa dihitung ulang tetap ditulis: peta kepadatan yang dicat
/// (berkas PNG pendamping, aturan yang sama dengan peta bobot terrain) dan
/// suntingan tangan per-instance.
struct VegetationDocument {
    std::string name;
    /// Terrain yang ditumbuhi. Vegetasi tanpa permukaan tidak punya tempat.
    ///
    /// Disimpan sebagai GUID, bukan jalur berkas, dengan alasan yang sama
    /// seperti material sebuah layer terrain: memindahkan atau mengganti nama
    /// berkas terrain tidak boleh memutus vegetasinya.
    AssetRef terrain;
    /// Sisi sel kisi kepadatan, meter.
    float densityCellSize = Vegetation::kDefaultDensityCell;
};

struct VegetationIoResult {
    bool ok = false;
    std::string error;
    int sourceVersion = kVegetationSchemaVersion;
};

/// Keluarannya deterministik — urutan field tetap, angka tanpa bergantung locale
/// — supaya menyimpan dokumen yang tidak disunting menghasilkan byte yang sama.
///
/// Deskriptor layer diterima terpisah dari `Vegetation`-nya, dengan alasan yang
/// sama seperti daftar layer terrain: nama berkas pendamping dibangkitkan saat
/// menyimpan, dan membangkitkannya ke dalam dokumen yang sedang dibuka berarti
/// menyimpan mengubah apa yang sedang disunting. Suntingan tangan tetap dibaca
/// dari `vegetation`, yang memilikinya — daftar kedua yang harus dijaga tetap
/// sama adalah daftar yang akan berbeda.
std::string SaveDocumentToString(const VegetationDocument& document,
                                 const Vegetation& vegetation,
                                 const std::vector<VegetationLayer>& layers);
VegetationIoResult LoadDocumentFromString(VegetationDocument& document, Vegetation& vegetation,
                                          const std::string& text);

/// Menyimpan deskriptor beserta peta kepadatan setiap layer yang pernah dicat.
/// Peta yang tidak menyimpan apa pun tidak ditulis dan namanya tidak dicatat —
/// aturan yang sama dengan peta pendamping terrain.
VegetationIoResult SaveVegetation(const Vegetation& vegetation,
                                  const VegetationDocument& document,
                                  const std::filesystem::path& path);
/// Memuat keduanya. Instance **tidak** ikut dibangkitkan: menyebar menuntut
/// terrain, dan terrain-nya baru bisa dicari setelah GUID-nya terbaca dari sini.
/// Pemanggil menyusulnya dengan `Fit` lalu `ScatterAll`.
VegetationIoResult LoadVegetation(Vegetation& vegetation, VegetationDocument& document,
                                  const std::filesystem::path& path);

// --- peta kepadatan -----------------------------------------------------------

/// PNG greyscale 8-bit, format yang sama dengan peta bobot terrain — jadi mask
/// kepadatan bisa disiapkan di penyunting gambar biasa dan langsung dipakai.
VegetationIoResult SaveDensityPng(const Vegetation& vegetation, int layer,
                                  const std::filesystem::path& path);
/// Menuntut ukuran PNG cocok dengan kisi kepadatan yang berlaku, kecuali kalau
/// kisinya belum ditentukan — pada pemuatan berkas, PNG-nyalah yang menentukan
/// ukuran kisinya.
VegetationIoResult LoadDensityPng(Vegetation& vegetation, int layer,
                                  const std::filesystem::path& path);

}  // namespace sim::vegetation
