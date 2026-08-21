#pragma once

#include "Sim/Render/Types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace sim::render {

/// Sederet instance berurutan yang bisa digambar satu panggilan.
///
/// **Bebas Vulkan dengan sengaja**, sama seperti `Frustum`, `LightCluster`, dan
/// `ShadowAtlas`: yang memutuskan apa yang digambar adalah aritmetika, dan
/// aritmetika yang hanya bisa dijalankan dengan kartu grafis adalah aritmetika
/// yang tidak pernah diuji.
struct DrawRun {
    MeshHandle mesh = kUnitCubeMesh;
    /// Ruas mesh yang digambar — indeks ke `GpuMesh::parts`.
    ///
    /// **Ikut menjadi kunci ruas**, karena ia menentukan `firstIndex` dan
    /// `indexCount` panggilan gambarnya.
    uint32_t part = 0;
    /// Material ruas ini, atau `kInvalidMaterial` untuk jalur mundur.
    ///
    /// **Ikut menjadi kunci**, karena tiap material adalah pipeline-nya sendiri:
    /// satu panggilan gambar tidak bisa mengganti pipeline di tengah jalan.
    MaterialHandle material = kInvalidMaterial;
    /// Tekstur albedo jalur mundur. **Ikut menjadi kunci hanya karena jalur
    /// mundur mengikatnya sebagai descriptor set**; pada jalur bindless ia
    /// tinggal di data instance dan tidak memecah apa pun — tetapi kuncinya
    /// tetap dipegang kedua jalur, karena aturan yang berbeda antar-jalur adalah
    /// dua daftar ruas yang suatu saat tidak sepakat.
    ///
    /// Selalu `kInvalidTexture` bila `material` sah: material membawa teksturnya
    /// sendiri.
    TextureHandle texture = kInvalidTexture;
    /// Digambar lewat pipeline ber-kulit. **Ikut menjadi kunci ruas**, karena ia
    /// menentukan pipeline dan vertex buffer yang diikat: mesh ber-rig yang satu
    /// instance-nya dipasok pose dan satu lagi tidak adalah dua ruas, bukan satu
    /// ruas dengan cabang di dalamnya.
    bool skinned = false;
    /// Indeks permukaan pertama di dalam buffer instance frame ini.
    ///
    /// **Permukaan, bukan entity.** Satu entity bermesh berruas tiga menempati
    /// tiga entri berurutan: warna, material, dan tekstur tiap ruas tinggal di
    /// data instance sejak G6, dan itu satu-satunya cara ketiganya bisa berbeda
    /// di dalam satu panggilan gambar.
    uint32_t first = 0;
    uint32_t count = 0;
};

/// Menyusun ulang daftar ruas menjadi ruas-ruas yang hanya berisi instance yang
/// lolos sebuah uji — **tanpa memindahkan satu pun instance di dalam buffer**.
///
/// Itulah syarat yang membuat penyaringan per pass murah. Instance diunggah
/// sekali per frame sebagai satu larik bersambung, dan `vkCmdDrawIndexed`
/// menggambar `count` instance mulai dari `first` — jadi sebuah himpunan bagian
/// yang berselang-seling cukup dinyatakan sebagai beberapa ruas pendek yang
/// menunjuk larik yang sama. Menyusun ulang isinya per pass akan berarti satu
/// unggahan buffer per muka bayangan: 32 unggahan per frame untuk data yang sama
/// persis.
///
/// Kunci ruas — mesh, skinned, ruas warna — tidak ikut berubah: seluruh instance
/// di dalam satu ruas asal sudah berbagi ketiganya, jadi pecahan mana pun
/// darinya mewarisinya apa adanya.
///
/// `out` dikosongkan lebih dulu dan dipakai ulang antar-pemanggilan; itu yang
/// membuat pemanggilan per muka bayangan tidak mengalokasi apa pun.
template <typename Predicate>
void SplitRuns(std::span<const DrawRun> source, Predicate keep, std::vector<DrawRun>& out) {
    out.clear();
    for (const DrawRun& run : source) {
        uint32_t spanFirst = 0;
        uint32_t spanCount = 0;
        for (uint32_t offset = 0; offset < run.count; ++offset) {
            const uint32_t index = run.first + offset;
            if (keep(index)) {
                if (spanCount == 0) {
                    spanFirst = index;
                }
                ++spanCount;
                continue;
            }
            if (spanCount > 0) {
                DrawRun piece = run;
                piece.first = spanFirst;
                piece.count = spanCount;
                out.push_back(piece);
                spanCount = 0;
            }
        }
        if (spanCount > 0) {
            DrawRun piece = run;
            piece.first = spanFirst;
            piece.count = spanCount;
            out.push_back(piece);
        }
    }
}

}  // namespace sim::render
