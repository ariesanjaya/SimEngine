#pragma once

#include "Sim/Render/SdfClipmap.h"
#include "Sim/Render/TraceBackend.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace sim::render {

/// Isi clipmap SDF di sisi CPU.
///
/// **Ini acuan kebenaran, bukan yang dipakai saat menggambar.** Yang menggambar
/// membaca tekstur volume di GPU; yang di sini menyimpan byte yang sama persis
/// dengan rumus yang sama persis, cukup lambat untuk test dan cukup sederhana
/// untuk dibaca. Kriteria selesai M2 menuntut backend SDF lulus uji ray tunggal
/// terhadap referensi CPU — dan referensi itu tidak ada gunanya kalau ia ditulis
/// dari pemahaman yang berbeda.
class SdfVolume {
public:
    /// Jarak bertanda sebuah titik dunia ke permukaan terdekat.
    using DistanceField = std::function<float(const Vec3&)>;

    void Configure(const SdfClipmapSettings& settings);

    SdfClipmap& Clipmap() { return clipmap_; }
    const SdfClipmap& Clipmap() const { return clipmap_; }

    /// Mengisi wilayah yang dilaporkan `Scroll` dari sebuah medan jarak.
    ///
    /// Hanya wilayah itu — bukan seluruh volume. Itulah yang membuat penghematan
    /// toroidal benar-benar terwujud alih-alih hanya tercatat di komentar.
    void Fill(const SdfScrollResult& scroll, const DistanceField& field);
    /// Mengisi seluruh isi setiap kaskade. Dipakai saat pertama kali dan di test.
    void FillAll(const DistanceField& field);

    /// Nilai texel mentah 0..255.
    uint8_t At(uint32_t cascade, const glm::uvec3& texel) const;

    /// Jarak pada sebuah titik dunia, dibaca trilinear dari kaskade yang
    /// diberikan. Mengembalikan false bila titiknya di luar kaskade itu.
    bool SampleCascade(uint32_t cascade, const Vec3& worldPosition, float& outDistance) const;
    /// Jarak dari kaskade terhalus yang memuat titiknya.
    bool Sample(const Vec3& worldPosition, float& outDistance) const;

    /// Banyaknya voxel yang benar-benar ditulis sejak `ResetWriteCount`.
    /// Dipakai test untuk membuktikan pembaruan parsial memang parsial.
    uint64_t WrittenVoxels() const { return written_; }
    void ResetWriteCount() { written_ = 0; }

private:
    void WriteBox(uint32_t cascade, const SdfClipmap::TexelBox& box, const DistanceField& field);

    SdfClipmap clipmap_;
    std::array<std::vector<uint8_t>, kMaxSdfCascades> data_{};
    std::vector<SdfClipmap::TexelBox> boxes_;
    uint64_t written_ = 0;
};

/// Backend SDF: sphere tracing di atas clipmap.
///
/// **Sphere tracing, bukan langkah tetap.** Langkah tetap menuntut langkah
/// sekecil voxel supaya tidak menembus dinding tipis, dan itu ratusan langkah
/// untuk melintasi satu kaskade. Sphere tracing memakai jarak yang tersimpan
/// sebagai jaminan ruang kosong, jadi ia melompat jauh di ruang terbuka dan
/// merapat hanya di dekat permukaan.
std::unique_ptr<ITraceBackend> CreateSdfTraceBackend(const SdfVolume& volume,
                                                     uint32_t maxSteps = 64);

}  // namespace sim::render
