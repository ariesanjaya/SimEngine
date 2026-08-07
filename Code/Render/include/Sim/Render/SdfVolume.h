#pragma once

#include "Sim/Render/SdfClipmap.h"
#include "Sim/Render/TraceBackend.h"
#include "Sim/Render/Types.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace sim::render {

/// Medan jarak sebuah himpunan kotak berorientasi.
///
/// **Kelas, bukan `std::function`, dan berjalan per baris, bukan per titik.**
/// Bentuk pertamanya adalah lambda yang dipanggil sekali per voxel: 48 ns per
/// voxel untuk dua mesh, yaitu 0,8 ms untuk satu gerakan kamera 16 ribu voxel —
/// sudah melewati seluruh anggaran pembaruan clipmap sebelum satu byte pun
/// diunggah. Yang mahal bukan rumus jaraknya melainkan pemanggilan tak langsung
/// yang menghalangi inlining, dan perkalian matriks 4×4 penuh yang diulang untuk
/// tiap voxel di sepanjang baris yang sebenarnya lurus.
///
/// **Jarak kotak yang diskala tak seragam hanyalah pendekatan.** Jarak yang
/// diukur di ruang lokal berpadanan dengan antara d·min(skala) dan d·maks(skala)
/// di dunia; mengalikannya dengan yang **terkecil** membuatnya tidak pernah
/// melebih-lebihkan ruang kosong — dan itu arah yang aman: sphere tracing yang
/// melangkah terlalu pendek hanya membuang langkah, sedangkan yang melangkah
/// terlalu jauh menembus dinding.
class BoxSceneField {
public:
    void Build(std::span<const MeshInstance> meshes);

    /// Jarak pada satu titik. Bentuk yang mudah dibaca, dipakai test.
    float Distance(const Vec3& world) const;

    /// Membuka sebuah kotak texel: titik asalnya di dunia, langkah satu voxel di
    /// sepanjang baris, dan langkah satu baris pada kedua sumbu lainnya —
    /// ditambah panjang barisnya dan jarak di mana penyandian jaraknya jenuh.
    ///
    /// **Basis lokal tiap mesh dihitung di sini, sekali untuk seluruh kotak.**
    /// Bentuk sebelumnya menghitungnya per baris — dua perkalian matriks 4×4 per
    /// mesh — dan itu tidak terbagi ke mana-mana pada baris pendek. Baris pendek
    /// bukan kasus langka: satu gerakan kamera satu voxel menghasilkan lempeng
    /// setebal satu voxel, dan pembagian toroidalnya memecah lempeng itu menjadi
    /// kotak-kotak seperti 1×56×9.
    void BeginBox(const Vec3& origin, const Vec3& rowStep, const Vec3& outerStep,
                  const Vec3& planeStep, uint32_t count, float band);

    /// Jarak sepanjang baris (`outer`, `plane`) kotak yang sedang dibuka.
    void Row(uint32_t outer, uint32_t plane, float* out) const;

    bool Empty() const { return entries_.empty(); }

private:
    struct Entry {
        Mat4 inverse{1.0f};
        /// Skala terkecil di antara tiga sumbu.
        float scale = 1.0f;
        /// min(skala)/maks(skala). Jarak yang disimpan tidak pernah lebih kecil
        /// daripada jarak dunia dikali angka ini, jadi inilah yang membuat
        /// pembuangan berdasarkan kotak batas dunia tetap aman pada mesh yang
        /// diskala tak seragam. Bentuk tanpa faktor ini membuang mesh yang
        /// jaraknya di ruang lokal masih di dalam pita — dan lubang di medan
        /// jarak adalah dinding yang bisa ditembus sphere tracing.
        float anisotropy = 1.0f;
        Vec3 boundsMin{0.0f};
        Vec3 boundsMax{0.0f};
    };

    /// Basis kotak yang sedang dibuka, di ruang lokal sebuah mesh.
    struct Local {
        Vec3 origin{0.0f};
        Vec3 rowStep{0.0f};
        Vec3 outerStep{0.0f};
        Vec3 planeStep{0.0f};
    };

    std::vector<Entry> entries_;
    std::vector<Local> locals_;
    Vec3 boxOrigin_{0.0f};
    Vec3 boxRowStep_{0.0f};
    Vec3 boxOuterStep_{0.0f};
    Vec3 boxPlaneStep_{0.0f};
    uint32_t boxCount_ = 0;
    float boxBand_ = 0.0f;
};

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
    void Fill(const SdfScrollResult& scroll, BoxSceneField& field);
    void Fill(const SdfScrollResult& scroll, const DistanceField& field);
    /// Mengisi seluruh isi setiap kaskade. Dipakai saat pertama kali dan di test.
    void FillAll(BoxSceneField& field);
    void FillAll(const DistanceField& field);

    /// Nilai texel mentah 0..255.
    uint8_t At(uint32_t cascade, const glm::uvec3& texel) const;

    /// Seluruh isi sebuah kaskade, X tercepat lalu Y lalu Z.
    ///
    /// Dipakai pengunggah supaya ia bisa menyalin satu baris sekaligus alih-alih
    /// memanggil `At` per voxel — panggilan lintas unit terjemahan yang, dikali
    /// puluhan ribu voxel per frame, berharga lebih mahal daripada salinannya.
    std::span<const uint8_t> Data(uint32_t cascade) const { return data_[cascade]; }

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
    /// Menulis sebuah kotak texel dengan mengevaluasi medannya baris demi baris.
    /// `field` harus punya `BeginBox` dan `Row` seperti `BoxSceneField`; kedua
    /// bentuk `Fill` hanya berbeda pada kelas itu.
    template <typename Field>
    void WriteBoxRows(uint32_t cascade, const SdfClipmap::TexelBox& box, Field& field);

    /// Setiap kotak texel dari setiap kaskade, sekali.
    template <typename Visit>
    void ForEachCascadeBox(Visit&& visit);

    SdfClipmap clipmap_;
    std::array<std::vector<uint8_t>, kMaxSdfCascades> data_{};
    std::vector<SdfClipmap::TexelBox> boxes_;
    std::vector<float> rowScratch_;
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
