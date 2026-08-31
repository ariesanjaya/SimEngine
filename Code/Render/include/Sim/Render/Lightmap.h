#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sim::render {

/// Tempat sebuah objek di dalam atlas lightmap (S5 di docs/PLAN-STATIC-GI.md).
struct LightmapPlacement {
    /// Nomor entity pemiliknya, **buram bagi modul ini**.
    ///
    /// `Sim::Render` tidak boleh melihat `Sim::Scene` — batas yang sama yang
    /// menjaga scene bisa dimuat tanpa satu pun perangkat grafis. Yang
    /// menerjemahkannya kembali menjadi entity adalah yang memiliki keduanya.
    uint64_t owner = 0;
    /// `lightmapUv` mesh → UV atlas: xy skala, zw geseran.
    Vec4 scaleOffset{0.0f};
};

/// Atlas iradiansi untuk permukaan statis.
///
/// **Satu angka per texel, bukan sembilan.** Sebuah texel lightmap *adalah*
/// permukaan: normalnya diketahui saat memanggang, jadi yang perlu disimpan
/// iradiansi pada normal itu. Kisi probe menyimpan SH justru karena ia tidak
/// tahu normal pembacanya. Itu yang membuat lightmap bisa jauh lebih rapat pada
/// anggaran memori yang sama — dan juga yang membuatnya tidak bisa dipakai
/// permukaan yang normalnya berubah, yaitu apa pun yang bergerak.
struct Lightmap {
    uint32_t width = 0;
    uint32_t height = 0;
    /// Iradiansi linier, urutan baris. Kosong berarti belum dipanggang.
    std::vector<Vec3> texels;
    std::vector<LightmapPlacement> placements;

    bool IsValid() const {
        return width > 0 && height > 0 &&
               texels.size() == static_cast<std::size_t>(width) * height;
    }
    /// Byte yang ditempatinya di GPU sebagai RGBA float16 — angka yang
    /// dilaporkan panel.
    uint64_t GpuBytes() const {
        return static_cast<uint64_t>(width) * height * 4 * 2;
    }
    /// Byte yang ditempatinya di dalam artefak.
    uint64_t StoredBytes() const {
        return static_cast<uint64_t>(texels.size()) * sizeof(Vec3) +
               static_cast<uint64_t>(placements.size()) * sizeof(LightmapPlacement);
    }

    /// Penempatan milik sebuah entity, atau nullptr.
    const LightmapPlacement* Find(uint64_t owner) const;
};

// --- artefak masak ----------------------------------------------------------

uint64_t LightmapCacheKey(uint64_t inputKey);
std::filesystem::path LightmapCachePath(const std::filesystem::path& cacheDir, uint64_t key);

/// **Atomik**, dengan alasan yang sama seperti artefak masak lain — dan nama
/// sementaranya unik per penulis; lihat `sim::UniqueTemporaryPath`.
bool WriteLightmap(const std::filesystem::path& file, const Lightmap& lightmap,
                   std::string& error);

/// Gagal membaca bukan galat — yang benar lalu memanggang ulang.
bool ReadLightmap(const std::filesystem::path& file, Lightmap& out, std::string& error);

}  // namespace sim::render
