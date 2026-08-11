#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <vector>

namespace sim::render {

/// Derau untuk awan volumetrik: Worley dan Perlin 3D yang **menyambung di
/// tepinya**.
///
/// **Menyambung adalah syarat, bukan kemewahan.** Lapisan awan membentang
/// puluhan kilometer sementara volume deraunya beberapa puluh texel; ia
/// karena itu diulang berkali-kali di setiap arah. Derau yang tidak menyambung
/// menaruh sebuah tepi tajam pada setiap batas pengulangan — dan yang terlihat
/// bukan derau yang salah melainkan kisi garis lurus di langit, teratur sempurna,
/// yang tidak mungkin dikira awan oleh siapa pun.
///
/// **Dibangkitkan di CPU, bukan di compute shader.** Acuan
/// `/home/arie/SDK/atmosphere-bac` memakai compute; engine ini sengaja belum
/// punya satu pun compute pipeline. Yang didapat dari memindahkannya ke CPU
/// bukan sekadar menghindari jalur baru: derau menjadi kode biasa yang punya
/// nilai balik, jadi sifat yang menentukan benar-tidaknya — bahwa ia menyambung —
/// bisa dinyatakan sebagai uji alih-alih ditatap di layar.
class CloudNoise {
public:
    /// Derau Worley 3D pada `position` dalam kubus satuan.
    ///
    /// `cellsPerAxis` menentukan kerapatan titik fitur; pencarian tetangga
    /// dibungkus modulo bilangan itu, dan itulah yang membuatnya menyambung.
    ///
    /// Mengembalikan jarak ke titik fitur terdekat, dinormalkan ke 0..1.
    /// **Belum dibalik**: nol berarti tepat di sebuah titik fitur.
    static float Worley(const Vec3& position, uint32_t cellsPerAxis, uint32_t seed);

    /// Derau Perlin 3D pada `position` dalam kubus satuan, dibungkus dengan cara
    /// yang sama. Mengembalikan 0..1.
    static float Perlin(const Vec3& position, uint32_t cellsPerAxis, uint32_t seed);

    /// FBM Worley terbalik: beberapa oktaf yang makin rapat dan makin lemah,
    /// lalu `1 - w` supaya gumpalan berada di nilai **tinggi**.
    ///
    /// Terbalik, karena Worley mentah bernilai nol di titik fiturnya — dipakai
    /// apa adanya, ia menghasilkan awan yang berlubang tepat di tempat yang
    /// seharusnya paling padat.
    static float WorleyFbm(const Vec3& position, uint32_t baseCells, uint32_t octaves,
                           uint32_t seed);

    /// FBM Perlin: oktaf yang makin rapat dan makin lemah, dinormalkan ke 0..1.
    static float PerlinFbm(const Vec3& position, uint32_t baseCells, uint32_t octaves,
                           uint32_t seed);

    /// Campuran Perlin-Worley yang menjadi kanal dasar bentuk awan.
    ///
    /// Perlin sendirian terlalu berkabut untuk menjadi awan, dan Worley
    /// sendirian terlalu bergumpal-gumpal seragam. Yang dipakai adalah Perlin
    /// yang **dipetakan ulang** oleh Worley: gumpalan Worley memberi bentuk
    /// kumulus, kesinambungan Perlin memberi bagian dalamnya isi.
    static float PerlinWorley(const Vec3& position, uint32_t baseCells, uint32_t seed);
};

/// Volume derau RGBA8 yang siap diunggah ke `rhi::Texture3D`.
struct CloudNoiseVolume {
    uint32_t size = 0;
    /// RGBA8, `size³` texel.
    std::vector<uint8_t> texels;

    /// Satu kanal sebuah texel, 0..1.
    float At(uint32_t x, uint32_t y, uint32_t z, uint32_t channel) const;
};

/// Volume bentuk: kanal R adalah Perlin-Worley, kanal G/B/A adalah Worley yang
/// makin rapat. Shader memadu keempatnya dengan bobot yang bisa disetel — itulah
/// yang membuat "lebih bergumpal" dan "lebih halus" menjadi slider alih-alih
/// menjadi pembangkitan ulang.
CloudNoiseVolume BuildCloudShapeVolume(uint32_t size, uint32_t seed);

/// Volume rincian: tiga oktaf Worley yang jauh lebih rapat, dipakai mengikis
/// tepi bentuk dasarnya.
CloudNoiseVolume BuildCloudDetailVolume(uint32_t size, uint32_t seed);

/// Gradien ketinggian di dalam lapisan awan, 0..1.
///
/// **Nol di kedua ujungnya, bukan hanya di atas.** Lapisan yang dipotong rata di
/// bawah memperlihatkan alasnya sebagai bidang datar sempurna yang membentang
/// sampai horizon — dan tidak ada yang lebih cepat memberi tahu mata bahwa
/// langitnya palsu daripada sebuah bidang datar di udara.
float CloudHeightGradient(float heightKm, float bottomKm, float topKm);

}  // namespace sim::render
