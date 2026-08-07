#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Render/TraceBackend.h"

#include <cstdint>
#include <span>
#include <vector>

namespace sim::render {

/// Piramida depth hierarkis di atas depth buffer.
///
/// **Reversed-Z, jadi tiap mip menyimpan yang TERBESAR dari texel di bawahnya,
/// bukan yang terkecil.** Nilai depth terbesar adalah permukaan yang paling
/// dekat ke kamera, dan itulah yang harus diketahui penelusur: sebuah sel boleh
/// dilompati hanya kalau sinarnya masih di depan permukaan terdekat di dalamnya.
/// Menyimpan yang terkecil membuat setiap sel melaporkan permukaan terjauhnya
/// sebagai penghalang, dan setiap sinar menembus geometri yang justru paling
/// dekat.
///
/// **Ukuran tiap tingkat dibulatkan ke bawah, mengikuti aturan mip Vulkan.**
/// Bentuk pertama saya membulatkannya ke atas supaya baris terakhir dari ukuran
/// ganjil tidak hilang — dan itu menghasilkan satu tingkat lebih banyak daripada
/// yang boleh dimiliki sebuah image: 1280×768 memberi 12 tingkat sementara
/// Vulkan hanya mengizinkan 11. Barisnya tetap tidak boleh hilang, jadi yang
/// berubah bukan ukurannya melainkan cakupannya: texel terakhir sebuah baris
/// merangkum **tiga** texel sumber, bukan dua, saat ukuran sumbernya ganjil.
class HiZPyramid {
public:
    /// Banyaknya tingkat untuk sebuah ukuran, aturan mip Vulkan.
    static uint32_t LevelsFor(uint32_t width, uint32_t height);

    /// Membangun seluruh mip dari depth buffer beresolusi penuh.
    /// `depth` berisi `width * height` nilai, baris atas lebih dulu.
    void Build(uint32_t width, uint32_t height, std::span<const float> depth);

    void Clear();

    bool IsValid() const { return !levels_.empty(); }
    uint32_t LevelCount() const { return static_cast<uint32_t>(levels_.size()); }
    glm::uvec2 Size(uint32_t level) const { return levels_[level].size; }

    /// Nilai sebuah texel. Koordinat di luar batas dijepit.
    float At(uint32_t level, int32_t x, int32_t y) const;

    /// Nilai pada koordinat tekstur [0,1]², pencuplikan titik.
    float Sample(uint32_t level, const Vec2& uv) const;

    std::span<const float> Texels(uint32_t level) const { return levels_[level].texels; }

private:
    struct Level {
        glm::uvec2 size{0};
        std::vector<float> texels;
    };

    std::vector<Level> levels_;
};

/// Matriks yang dibutuhkan penelusur screen-space.
struct ScreenTraceView {
    /// Dunia → clip.
    Mat4 viewProj{1.0f};
    /// Clip → dunia. Dipakai memulihkan posisi dunia dari sebuah texel depth,
    /// dan itulah yang membuat uji ketebalan bisa diukur dalam meter.
    Mat4 invViewProj{1.0f};
};

struct ScreenTraceSettings {
    /// Anggaran langkah. Rencana GI menyebut 16; angka itu yang membuat lapis
    /// screen-space murah, dan yang membuat fallback ke SDF bukan kemewahan
    /// melainkan keharusan.
    uint32_t maxSteps = 16;

    /// Seberapa jauh di belakang permukaan sebuah perpotongan masih dianggap
    /// mengenainya, dalam meter.
    ///
    /// **Diukur di ruang dunia, bukan dalam satuan depth.** Depth reversed-Z
    /// sangat tidak linear: satu ambang tetap dalam satuan depth berarti
    /// sentimeter di dekat kamera dan ratusan meter di kejauhan. Depth buffer
    /// hanya menyimpan permukaan terdepan, jadi tanpa uji ini setiap sinar yang
    /// lewat di belakang sebuah benda akan melaporkan kena di siluetnya.
    float thickness = 0.5f;

    /// Sinar dimajukan sejauh ini dari titik asalnya sebelum ditelusuri, dalam
    /// meter.
    ///
    /// Sinar GI berangkat dari sebuah permukaan, dan permukaan itu ada di depth
    /// buffer. Tanpa dorongan awal, langkah pertama menemukan permukaan asalnya
    /// sendiri dan setiap sinar mengenai tempat ia berdiri. Dalam meter, bukan
    /// dalam piksel: sinar yang mengarah lurus menjauhi kamera tidak bergerak
    /// satu piksel pun di layar, dan dorongan yang diukur di layar tidak
    /// menggerakkannya sama sekali.
    float originBias = 0.02f;
};

struct ScreenTraceResult {
    bool hit = false;
    /// Sinar meninggalkan layar sebelum anggaran langkahnya habis.
    ///
    /// **Meleset karena keluar layar berbeda dari meleset karena tidak ada apa-
    /// apa**, dan yang membedakan keduanya hanya nilai ini. Yang pertama harus
    /// diteruskan ke SDF; yang kedua sudah merupakan jawaban.
    bool leftScreen = false;
    float distance = 0.0f;
    Vec3 position{0.0f};
    /// Tempat perpotongannya di layar. Diagnostik.
    Vec2 uv{0.0f};
    uint32_t steps = 0;
};

/// Menelusuri depth buffer secara hierarkis.
///
/// `direction` tidak harus ternormalisasi.
ScreenTraceResult TraceScreenSpace(const HiZPyramid& depth, const ScreenTraceView& view,
                                   const Vec3& origin, const Vec3& direction, float tMax,
                                   const ScreenTraceSettings& settings);

}  // namespace sim::render
