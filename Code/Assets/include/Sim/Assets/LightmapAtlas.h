#pragma once

#include "Sim/Assets/MeshData.h"

#include <cstdint>
#include <vector>

namespace sim::assets {

/// Petak sebuah objek di dalam atlas lightmap (S5 di docs/PLAN-STATIC-GI.md).
///
/// **Persegi, dan itu turun dari S4.** Unwrapper menormalkan UV ke `[0,1]`
/// dengan skala yang **sama di kedua sumbu** — supaya sebuah ubin persegi
/// panjang tidak mendapat kerapatan cahaya yang berbeda menurut arah — jadi
/// petak yang tidak persegi akan meregangkan texel-nya kembali, membatalkan
/// keputusan itu.
struct LightmapChart {
    uint32_t side = 0;
    /// Tempat **isinya** di dalam atlas, sah sesudah dipaket. Selokannya berada
    /// di luar kotak ini: isinya menempati [x, x+side), dan cincin selebar
    /// `LightmapAtlasLayout::padding` di sekelilingnya dimiliki petak yang sama.
    uint32_t x = 0;
    uint32_t y = 0;
    /// True bila ia benar-benar mendapat tempat.
    bool placed = false;
};

/// Tata letak seluruh atlas.
struct LightmapAtlasLayout {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<LightmapChart> charts;
    /// Berapa texel yang ditempati petak, dan bagian atlas yang terpakai.
    uint64_t usedTexels = 0;
    float utilisation = 0.0f;
    /// Berapa objek yang tidak kebagian tempat. **Bukan nol adalah keadaan yang
    /// harus disebutkan**, bukan didiamkan: objek tanpa petak digambar tanpa
    /// lightmap, dan yang melihatnya akan mengira lightmap-nya rusak.
    uint32_t dropped = 0;
    /// Lebar selokan di sekeliling tiap petak, texel.
    ///
    /// **Ia ada karena atlasnya disampel linier.** Tanpa selokan, cuplikan di
    /// tepi petak jatuh persis di batas texel dan menjadi campuran separuh-separuh
    /// dengan petak sebelahnya, yaitu objek lain. Diukur pada atlas `bench` 256²
    /// sebelum selokan ini ada: 16,5% texel atlas berada di tepi petak, dan
    /// cuplikan di sana meleset rata-rata 0,36 — 30% dari rerata iradiansi
    /// adegan, dan 74% pada persentil 95.
    ///
    /// Satu texel cukup untuk penyaringan bilinear tanpa mip: cuplikan di tepi
    /// menjangkau paling jauh setengah texel ke luar. Isinya replika texel tepi
    /// petaknya sendiri, jadi campurannya menjawab nilai petak itu juga —
    /// setara `CLAMP_TO_EDGE` per petak, yang tidak bisa diminta dari sampler
    /// karena samplernya menjepit di tepi atlas, bukan di tepi petak.
    uint32_t padding = 0;

    bool IsValid() const { return width > 0 && height > 0; }
    /// Byte yang ditempatinya di GPU sebagai RGBA float16.
    uint64_t GpuBytes() const {
        return static_cast<uint64_t>(width) * height * 4 * 2;
    }
};

/// Luas permukaan sebuah mesh di ruang dunia.
///
/// **Diukur, bukan ditaksir dari kotak batasnya.** Sebuah pagar dan sebuah
/// kubus bisa punya kotak batas yang sama sementara luas permukaannya berbeda
/// dua puluh kali, dan yang menaksir dari kotaknya memberi pagar itu texel dua
/// puluh kali lebih sedikit daripada yang dibutuhkannya.
float MeshWorldArea(const MeshData& mesh, const Mat4& transform);

/// Sisi petak untuk sebuah objek, dari luas dunianya dan kerapatan texelnya.
///
/// Dibulatkan ke pangkat dua: petak yang ukurannya sembarang membuat pemaketan
/// meninggalkan celah yang tidak bisa diisi apa pun, dan celah itu texel yang
/// dibayar tanpa pernah dibaca.
uint32_t LightmapChartSide(float worldArea, float texelsPerMeter, uint32_t minSide = 4,
                           uint32_t maxSide = 512);

/// Memaketkan petak ke dalam atlas persegi terkecil yang memuatnya.
///
/// **Pemaketan rak**, bukan yang optimal: petak diurutkan dari yang terbesar
/// lalu ditaruh baris demi baris. Yang optimal adalah masalah NP, dan selisih
/// beberapa persen utilisasi tidak sebanding dengan pemaket yang tidak bisa
/// dibaca ulang.
///
/// `maxSide` membatasi atlasnya; yang tidak muat ditandai `placed == false` dan
/// dihitung di `dropped`.
///
/// `padding` adalah selokan di sekeliling tiap petak — alasannya di
/// `LightmapAtlasLayout::padding`. Tiap petak karena itu memesan
/// `side + 2 * padding` texel per sisi meskipun `chart.x`/`chart.y` tetap
/// menunjuk isinya, sehingga cincin selokan sebuah petak tidak pernah beririsan
/// dengan cincin maupun isi petak lain — yang membuatnya tetap aman ditulis satu
/// tugas per objek.
LightmapAtlasLayout PackLightmapAtlas(std::vector<LightmapChart> charts, uint32_t maxSide = 4096,
                                      uint32_t padding = 1);

}  // namespace sim::assets
