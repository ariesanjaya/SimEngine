#pragma once

#include "Sim/Core/Math.h"

#include <array>
#include <cstdint>
#include <span>

namespace sim::render {

/// Kotak sejajar sumbu.
struct Aabb {
    Vec3 min{0.0f};
    Vec3 max{0.0f};

    Vec3 Centre() const { return (min + max) * 0.5f; }
    Vec3 Extent() const { return (max - min) * 0.5f; }
};

/// Kotak sebuah instance setelah ditransformasikan ke ruang dunia.
///
/// Kotaknya dihitung ulang sebagai kotak sejajar sumbu di ruang dunia, bukan
/// dibawa apa adanya: kotak lokal yang diputar bukan lagi sejajar sumbu, dan
/// mengujinya seolah-olah masih sejajar akan membuang objek yang sebenarnya
/// terlihat. Yang dibayar adalah kotak yang sedikit lebih besar daripada
/// perlunya — dan itu selalu arah yang aman.
Aabb TransformAabb(const Aabb& local, const Mat4& transform);

/// Enam bidang pembatas sebuah frustum, dalam ruang dunia.
///
/// **Diturunkan dari matriks view-proj apa adanya**, jadi ia tidak perlu tahu
/// apakah proyeksinya reversed-Z atau bukan. Yang berubah pada reversed-Z adalah
/// isi matriksnya, dan bidang yang diturunkan darinya tetap membatasi volume
/// yang sama — hanya saja bidang yang tadinya near menjadi far. Uji "di dalam"
/// tidak peduli bidang yang mana; ia hanya menuntut titiknya berada di sisi
/// dalam keenam-enamnya.
class Frustum {
public:
    /// Urutan bidang: kiri, kanan, bawah, atas, dekat, jauh.
    static constexpr int kPlaneCount = 6;

    Frustum() = default;
    explicit Frustum(const Mat4& viewProjection) { Extract(viewProjection); }

    void Extract(const Mat4& viewProjection);

    /// Bidang dalam bentuk `Vec4(normal.xyz, d)`, sudah dinormalkan sehingga
    /// `dot(normal, p) + d` benar-benar jarak bertanda. Jaraknya yang membuat
    /// LOD dan penyusutan bayangan bisa memakai bidang yang sama.
    const std::array<Vec4, kPlaneCount>& Planes() const { return planes_; }

    bool Contains(const Vec3& point) const;
    /// Uji kotak. Salah dalam arah yang aman: kotak yang menyentuh sudut frustum
    /// tanpa benar-benar memotongnya tetap dianggap terlihat. Uji yang tepat
    /// menuntut memeriksa sumbu pemisah dua arah, dan objek yang sesekali lolos
    /// jauh lebih murah daripada objek yang sesekali hilang.
    bool Intersects(const Aabb& box) const;

private:
    std::array<Vec4, kPlaneCount> planes_{};
};

/// Menyaring instance yang terlihat. Mengembalikan jumlah yang lolos.
///
/// Menulis indeks, bukan menyalin kotaknya: pemanggil memegang datanya sendiri,
/// dan menyalin ribuan kotak per frame hanya untuk membuang sebagian besarnya
/// adalah pekerjaan yang hasilnya langsung dibuang.
std::size_t CullAabbs(const Frustum& frustum, std::span<const Aabb> boxes,
                      std::vector<uint32_t>& visible);

/// Proyeksi perspektif reversed-Z untuk Vulkan: near dipetakan ke 1, far ke 0.
///
/// **Bukan selera, dan sudah dikunci sejak fase editor.** Depth buffer float
/// menyimpan angka jauh lebih rapat di dekat nol, sedangkan proyeksi biasa
/// menaruh objek jauh justru di sana — hasilnya z-fighting pada jarak yang persis
/// menjadi masalah begitu ada terrain sepanjang kilometer. Ditukar, presisi
/// terbaik jatuh ke tempat yang paling membutuhkannya.
///
/// Konsekuensi yang harus ikut: depth di-clear ke 0, uji depth memakai
/// `GREATER`, dan matriks ini tidak boleh dicampur dengan `Perspective` di dalam
/// frame yang sama.
Mat4 PerspectiveReversedZ(float fovYRadians, float aspect, float nearZ, float farZ);

/// Reversed-Z dengan bidang jauh tak hingga.
///
/// Menghilangkan satu angka yang harus disetel dan tidak ada yang tahu
/// nilainya, dan justru **menambah** presisi: bidang jauh yang terhingga
/// membuang sebagian rentang depth untuk jarak yang tidak pernah dipakai.
Mat4 PerspectiveReversedZInfinite(float fovYRadians, float aspect, float nearZ);

}  // namespace sim::render
