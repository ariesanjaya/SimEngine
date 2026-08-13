#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Core/SdfGrid.h"
#include "Sim/Core/VolumeGrid.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Bake volumetrik di atas OpenVDB.
///
/// **Pengondisi aset, bukan pustaka runtime.** Aturan yang sama yang berlaku
/// untuk OpenImageIO: modul ini boleh dipakai importir, baker, dan editor, dan
/// tidak boleh oleh jalur yang dikirim ke pemain. Yang keluar dari sini adalah
/// `sim::SdfGrid` — tipe biasa di `Sim::Core`, tanpa satu pun tipe OpenVDB —
/// jadi renderer bisa memakainya tanpa pernah menautkan pustakanya.
namespace sim::volume {

/// Apa yang diminta dari sebuah bake.
struct SdfBakeSettings {
    /// Sisi satu voxel di ruang lokal mesh, meter.
    ///
    /// **Dipilih menyamai kaskade clipmap yang akan memakainya.** Grid yang
    /// lebih halus daripada kaskadenya hanya membuang memori pada ketelitian
    /// yang akan hilang saat disampel; yang lebih kasar menghasilkan permukaan
    /// yang bergerigi di kaskade halus.
    float voxelSize = 0.1f;

    /// Setengah lebar pita, dalam voxel.
    ///
    /// Harus **setidaknya** `SdfClipmapSettings::bandVoxels`, karena di luar
    /// pita nilainya jenuh dan clipmap tidak bisa lagi membedakan "sejauh pita"
    /// dari "jauh sekali". Melebihinya sedikit tidak berbahaya; melebihinya
    /// banyak membuat pita hampir sepadat volume penuh.
    float bandVoxels = 4.0f;

    /// Batas jumlah voxel satu grid, sebagai penjaga.
    ///
    /// Bake tumbuh pangkat tiga terhadap ukuran mesh. Sebuah mesh 20 m pada
    /// voxel 10 cm sudah 8 juta voxel — 32 MB untuk satu mesh. Batas ini
    /// membuat mesh raksasa **ditolak dengan pesan** alih-alih menghabiskan
    /// memori mesin sampai tercekik.
    std::size_t maxVoxels = 16u * 1024u * 1024u;
};

struct SdfBakeResult {
    bool ok = false;
    std::string error;

    explicit operator bool() const { return ok; }
};

/// Apakah bake tersedia di build ini.
///
/// Tanpa OpenVDB seluruh fungsi di bawah menolak dengan pesan yang menyebut
/// itu — bukan mengembalikan grid kosong yang lalu terlihat seperti mesh yang
/// tidak punya permukaan.
bool Available();

/// Nama dan versi pustaka yang membakenya, untuk log startup. Kosong bila tidak
/// tersedia.
const char* BackendVersion();

/// Membake segitiga menjadi grid jarak bertanda di ruang lokal mesh.
///
/// `positions` adalah titik di ruang lokal; `indices` adalah tiga indeks per
/// segitiga. Keduanya persis bentuk yang dipegang `Sim::Assets::MeshData`, jadi
/// pemanggil tidak perlu menyalin apa pun.
///
/// **Mesh yang tidak tertutup tetap dibake**, dan tandanya di dalam menjadi
/// tidak berarti — level set dari permukaan terbuka tidak punya "dalam". Yang
/// dijamin tetap benar adalah besarnya jarak di dekat permukaan, dan itulah yang
/// dipakai sphere tracing untuk berhenti.
SdfBakeResult BakeMeshSdf(std::span<const Vec3> positions, std::span<const uint32_t> indices,
                          const SdfBakeSettings& settings, SdfGrid& out);

// --- impor .vdb --------------------------------------------------------------

/// Apa yang diminta saat membaca sebuah berkas volume.
struct VdbLoadSettings {
    /// Nama grid yang diambil. Kosong berarti **grid pertama yang bertipe
    /// float** — bukan sekadar grid pertama, karena berkas asap kerap membawa
    /// grid vektor kecepatan lebih dulu.
    std::string gridName;

    /// Batas jumlah voxel setelah dipadatkan.
    ///
    /// VDB jarang justru karena kebanyakan isinya kosong; memadatkannya
    /// membuang keunggulan itu, dan sebuah sim asap 1024³ akan meminta empat
    /// gigabyte. Batas ini membuatnya **ditolak dengan pesan** alih-alih
    /// menghabiskan memori mesin sampai tercekik.
    std::size_t maxVoxels = 64u * 1024u * 1024u;
};

/// Ekstensi berkas volume yang bisa dibaca build ini.
///
/// Kosong tanpa OpenVDB — dan `AssetTypes` membacanya alih-alih memuat daftar
/// tetap, dengan alasan yang sama seperti daftar format gambar: menawarkan
/// impor untuk format yang tidak bisa dibaca lebih buruk daripada tidak
/// menawarkannya sama sekali.
std::span<const std::string_view> ReadableExtensions();

bool CanRead(std::string_view extension);

/// Nama setiap grid di dalam sebuah berkas, beserta tipenya.
///
/// Dipakai panel impor untuk menawarkan pilihan alih-alih menebak: berkas asap
/// sungguhan membawa `density`, `temperature`, dan `velocity` sekaligus, dan
/// yang dimaksud pengguna tidak bisa disimpulkan dari berkasnya.
SdfBakeResult ListVdbGrids(const std::filesystem::path& path,
                           std::vector<std::string>& outNames);

/// Membaca satu grid dari berkas `.vdb` menjadi grid padat.
SdfBakeResult LoadVdb(const std::filesystem::path& path, const VdbLoadSettings& settings,
                      VolumeGrid& out);

}  // namespace sim::volume
