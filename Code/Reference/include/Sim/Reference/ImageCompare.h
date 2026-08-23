#pragma once

#include "Sim/Reference/PathTracer.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sim::reference {

/// Selisih dua gambar, sebagai angka.
///
/// **Keluaran pembanding acuan adalah angka, bukan gambar.** Regresi visual
/// yang dinilai mata bukan regresi yang bisa dijalankan CI — dan sebuah selisih
/// yang hanya bisa dilihat tidak bisa dibandingkan dengan selisih minggu lalu.
struct ImageDifference {
    /// Akar rata-rata kuadrat selisih, atas ketiga kanal.
    float rmse = 0.0f;
    /// Selisih mutlak terbesar di satu piksel satu kanal, dan di mana.
    float maxAbsolute = 0.0f;
    uint32_t maxX = 0;
    uint32_t maxY = 0;
    /// Rata-rata masing-masing gambar. **Selisih rata-rata yang besar berarti
    /// bias, bukan derau** — derau turun ketika sampelnya ditambah, bias tidak.
    Vec3 meanA{0.0f};
    Vec3 meanB{0.0f};

    /// Ringkasan satu baris untuk log dan laporan.
    std::string ToString() const;
};

/// Membandingkan dua gambar berukuran sama.
ImageDifference Compare(const Image& a, const Image& b);

/// Satu daerah persegi di dalam gambar, beserta namanya.
///
/// **RMSE seluruh gambar menyembunyikan justru yang dicari.** Kebocoran cahaya
/// lewat dinding Cornell box menyentuh beberapa persen pikselnya; ditenggelamkan
/// rata-rata seluruh gambar, ia terbaca sebagai selisih kecil yang bisa
/// diabaikan. Daerah bernama membuat "sudut kiri bawah meleset 40%" bisa
/// dikatakan.
struct Region {
    std::string name;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct RegionDifference {
    std::string name;
    ImageDifference difference;
};

std::vector<RegionDifference> CompareRegions(const Image& a, const Image& b,
                                             const std::vector<Region>& regions);

/// Rata-rata sebuah daerah. Dipakai uji yang menilai perbandingan antar-daerah
/// alih-alih nilai mutlaknya.
Vec3 RegionMean(const Image& image, const Region& region);

}  // namespace sim::reference
