#pragma once

#include "Sim/Core/SdfGrid.h"

#include <cstdint>
#include <filesystem>
#include <string>

/// Bake medan jarak sebuah mesh menjadi berkas cache, dan pembacaannya kembali.
///
/// **Di `Sim::Assets` dan bukan di `Sim::Render`, dan itu bukan pilihan
/// kerapian.** Yang membakenya OpenVDB — pengondisi aset yang tidak pernah ikut
/// ke jalur yang dikirim ke pemain — sementara renderer adalah jalur itu. Yang
/// menyeberang di antara keduanya hanya `sim::SdfGrid`: larik float dan sembilan
/// angka, tanpa satu pun tipe pustaka.
namespace sim::assets {

/// Apa yang diminta dari sebuah bake mesh.
struct MeshSdfSettings {
    /// Sisi voxel yang **diminta**, meter di ruang lokal mesh.
    ///
    /// Disamakan dengan kaskade terhalus clipmap yang akan memakainya: grid yang
    /// lebih halus daripada kaskadenya membuang memori pada ketelitian yang
    /// hilang saat disampel.
    float voxelSize = 0.1f;
    /// Setengah lebar pita **terkecil**, dalam voxel. Harus setidaknya sama
    /// dengan `SdfClipmapSettings::bandVoxels`.
    float bandVoxels = 4.0f;

    /// Setengah lebar pita sebagai pecahan sisi terpanjang mesh.
    ///
    /// **Pita adalah jangkauan langkah sphere tracing, bukan sekadar
    /// ketelitian.** Di luar pita nilainya jenuh, jadi sebuah medan berpita 45
    /// cm menjawab "paling jauh 45 cm" untuk setiap titik di ruang terbuka —
    /// dan penelusur yang percaya itu melangkah 45 cm pada tiap langkah,
    /// menghabiskan anggarannya sebelum menyeberangi satu halaman. Diukur pada
    /// Sponza: pita 4 voxel meninggalkan 68% voxel jenuh dan menjawab 0,45 m di
    /// posisi kamera; pita 16 voxel meninggalkan 25% dan menjawab 1,58 m, yaitu
    /// jarak sebenarnya. Harganya hampir tidak ada — 6,7 s terhadap 6,8 s —
    /// karena yang mahal adalah menelusuri segitiganya, bukan melebarkan
    /// pitanya.
    ///
    /// **Pecahan, bukan jumlah voxel tetap.** Pita ikut menumbuhkan kotak yang
    /// dibake: 16 voxel pada mesh sebesar kursi berarti grid yang lima kali
    /// lebih lebar daripada kursinya di tiap sumbu — 125 kali voxelnya untuk
    /// ruang kosong yang tidak ada isinya. Yang berguna adalah pita yang
    /// sepadan dengan bendanya sendiri.
    float bandFraction = 0.05f;
    /// Batas voxel satu grid. **Yang melewatinya diperbesar voxelnya, bukan
    /// ditolak** — lihat `FitVoxelSize`.
    std::size_t maxVoxels = 16u * 1024u * 1024u;
};

/// Sisi voxel terkecil yang membuat sebuah kotak batas muat dalam anggaran.
///
/// **Mesh sebesar gedung adalah kasus normal, bukan kasus salah.** Sponza satu
/// mesh berukuran 36×20×24 m: pada voxel 10 cm ia 17,3 juta voxel, di atas
/// anggaran, dan baker menolaknya. Menolak berarti gedung itu kembali menjadi
/// kotak pejal — persis keadaan yang M1 ada untuk mengakhiri. Memperbesar
/// voxelnya kehilangan rincian; kehilangan rincian jauh lebih baik daripada
/// kehilangan bentuk.
///
/// Mengembalikan `settings.voxelSize` bila sudah muat.
float FitVoxelSize(const Vec3& boundsMin, const Vec3& boundsMax, const MeshSdfSettings& settings);

/// Setengah lebar pita dalam meter untuk sebuah mesh pada sisi voxel tertentu.
float BandMeters(const Vec3& boundsMin, const Vec3& boundsMax, const MeshSdfSettings& settings,
                 float voxelSize);

struct MeshSdfBakeResult {
    bool ok = false;
    std::string error;

    /// Berkas `.simsdf` di dalam cache. Terisi hanya bila `ok`.
    std::filesystem::path path;
    /// True bila berkasnya sudah ada dan tidak ada yang dikerjakan ulang.
    bool fromCache = false;

    /// Sisi voxel yang **benar-benar** dipakai, sesudah `FitVoxelSize`.
    float voxelSize = 0.0f;
    uint32_t sizeX = 0;
    uint32_t sizeY = 0;
    uint32_t sizeZ = 0;
    double milliseconds = 0.0;

    explicit operator bool() const { return ok; }
};

/// Kunci cache sebuah mesh: isi berkasnya dikali pengaturannya.
///
/// Nol berarti berkasnya tidak bisa dibaca.
///
/// **Yang di-hash isi berkas sumbernya, dan untuk glTF itu hanya `.gltf`-nya.**
/// Geometrinya ada di `.bin` di sebelahnya, jadi mengubah `.bin` saja — tanpa
/// menyentuh `.gltf` — meninggalkan cache yang basi. Dalam praktiknya keduanya
/// ditulis bersama oleh eksportir yang sama; yang tidak begitu bisa menghapus
/// isi cache-nya. Menghash `.bin` ikut berarti pembaca cache harus tahu format
/// setiap berkas mesh sebelum boleh menjawab "sudah ada" — dan itu justru
/// pekerjaan yang dihindari cache ini.
uint64_t MeshSdfCacheKey(const std::filesystem::path& source, const MeshSdfSettings& settings);

/// Letak berkas cache untuk sebuah kunci.
std::filesystem::path MeshSdfCachePath(const std::filesystem::path& cacheDir, uint64_t key);

/// Memuat mesh, membakenya, dan menuliskan hasilnya ke cache — atau menjawab
/// dari cache tanpa menyentuh mesh-nya sama sekali.
///
/// **Jalan kedua tidak pernah memuat mesh-nya.** Untuk Sponza itu 140 MB
/// geometri dan lima detik yang tidak dibayar dua kali.
MeshSdfBakeResult BakeMeshSdfFile(const std::filesystem::path& source,
                                  const MeshSdfSettings& settings,
                                  const std::filesystem::path& cacheDir);

/// Menulis sebuah grid ke berkas `.simsdf`.
///
/// **Lewat berkas sementara lalu `rename`.** Proses yang mati di tengah
/// penulisan 69 MB meninggalkan berkas yang panjangnya benar di header dan
/// pendek di isinya, dan jalan berikutnya menerimanya sebagai cache yang sah.
bool WriteMeshSdf(const std::filesystem::path& file, const SdfGrid& grid, std::string& error);

/// Membaca berkas `.simsdf`. Berkas yang tidak utuh ditolak, bukan dipotong.
bool ReadMeshSdf(const std::filesystem::path& file, SdfGrid& out, std::string& error);

/// Berapa bake yang benar-benar dikerjakan sejak proses dimulai — bukan yang
/// dijawab cache. Dipakai uji untuk membuktikan cache-nya kena.
uint64_t MeshSdfBakeCount();

}  // namespace sim::assets
