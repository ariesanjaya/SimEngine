#pragma once

#include "Sim/Core/Math.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

/// Geometri yang bisa ditanyai sinar, dua tingkat.
///
/// **Modul ini tidak tahu apa itu entity, aset, atau mesh renderer.** Ia
/// menerima posisi dan indeks, mengembalikan nomor primitif, dan menyimpan 64
/// bit milik pemanggil apa adanya. Itulah yang membuatnya bisa dipakai
/// `SimHeadless` tanpa display — dan yang membuat aturan CMake "tidak boleh
/// bergantung pada Sim::RHI maupun Sim::Render" bisa ditegakkan alih-alih hanya
/// diminta.
///
/// Pemetaan `(instance, primitif) → entity + segitiga` tinggal di sisi
/// pemanggil, bukan di sini. Lihat `userData`.
namespace sim::raycast {

class SceneBackend;

/// Sebuah mesh yang sudah dibangun BVH-nya. Dipakai berkali-kali oleh instance.
enum class GeometryId : uint32_t { Invalid = 0xFFFFFFFFu };
/// Satu penempatan sebuah geometri di dunia.
enum class InstanceId : uint32_t { Invalid = 0xFFFFFFFFu };

/// Scene dua tingkat: BVH segitiga per geometri, BVH instance di atasnya.
///
/// **Dua tingkat sejak awal, bukan ditambahkan belakangan.** Adegan ini berbentuk
/// sedikit aset mesh dikali banyak penempatan; BVH satu tingkat atas seluruh
/// segitiga dunia berarti memindahkan satu benda membangun ulang segalanya.
/// Dengan dua tingkat, memindahkan benda hanya menulis ulang matriksnya dan
/// membangun ulang tingkat atas — yang isinya sebanyak instance, bukan sebanyak
/// segitiga.
class RayScene {
public:
    RayScene();
    ~RayScene();

    RayScene(RayScene&&) noexcept;
    RayScene& operator=(RayScene&&) noexcept;
    RayScene(const RayScene&) = delete;
    RayScene& operator=(const RayScene&) = delete;

    /// Menambahkan sebuah mesh dari buffer yang **tidak disalin**.
    ///
    /// **Ini kontrak umur, dan melanggarnya bukan galat melainkan pembacaan
    /// memori yang sudah dibebaskan.** `positions` dan `indices` harus tetap
    /// hidup dan tidak berpindah alamat sampai `Clear()` atau sampai scene ini
    /// hancur. Menyalinnya akan menggandakan setiap mesh di adegan — Sponza
    /// sendirian sudah puluhan megabyte — untuk data yang sudah ada di RAM.
    ///
    /// **Menyalinnya tidak dilarang, tetapi mengubahnya di tempat tidak
    /// berlaku.** Buffer dibagi supaya tidak ada simpul yang tersimpan dua
    /// kali; struktur percepatannya tetap dibangun sekali, di sini. Menggeser
    /// simpul sesudahnya menghasilkan jawaban yang tidak ditentukan sampai
    /// geometrinya dibangun ulang lewat `Clear()` dan `AddMesh()` lagi —
    /// `Commit()` hanya menyusun tingkat atas, dan itu memang seluruh gunanya.
    ///
    /// `stride` memungkinkan posisi dibaca langsung dari struct vertex
    /// interleaved: `assets::MeshVertex` menaruh `position` di offset nol dengan
    /// stride 32, jadi ia diserahkan apa adanya tanpa repack.
    ///
    /// Mengembalikan `GeometryId::Invalid` bila indeksnya bukan kelipatan tiga,
    /// menunjuk ke luar batas, atau buffer-nya kosong.
    GeometryId AddMesh(const void* positions, std::size_t stride, std::size_t vertexCount,
                       std::span<const uint32_t> indices);

    /// Bentuk ringkas untuk posisi yang sudah rapat. Kontrak umurnya sama.
    GeometryId AddMesh(std::span<const Vec3> positions, std::span<const uint32_t> indices);

    /// Menempatkan sebuah geometri di dunia.
    ///
    /// `userData` dikembalikan apa adanya di `RayHit`. Modul ini tidak pernah
    /// menafsirkannya — di sanalah pemanggil menaruh nomor entity-nya.
    InstanceId AddInstance(GeometryId geometry, const Mat4& transform, uint64_t userData = 0);

    /// Memindahkan sebuah instance. Menuntut `Commit()` lagi sebelum ditanyai.
    ///
    /// **Tidak membangun ulang BVH geometrinya** — itulah seluruh guna dua
    /// tingkat.
    void SetInstanceTransform(InstanceId instance, const Mat4& transform);

    /// Menyusun tingkat atas. Wajib dipanggil sesudah perubahan apa pun; query
    /// terhadap scene yang belum di-commit mengembalikan "tidak kena".
    void Commit();

    /// Membuang seluruh instance, **mempertahankan geometri beserta BVH-nya.**
    ///
    /// Inilah bentuk yang dituntut pemakaian sungguhan, dan R0 tidak
    /// memilikinya: `SceneView` menyusun ulang daftar isinya tiap frame, dan
    /// `Clear()` di sana berarti setiap BVH mesh dibangun ulang enam puluh kali
    /// per detik untuk geometri yang tidak berubah sama sekali.
    ///
    /// Yang tersisa sesudahnya hanya pembangunan tingkat atas — sebanyak
    /// instance, bukan sebanyak segitiga.
    void ClearInstances();

    void Clear();

    bool IsCommitted() const;
    std::size_t GeometryCount() const;
    std::size_t InstanceCount() const;
    /// Banyaknya segitiga di seluruh geometri, tanpa dikali instance-nya.
    std::size_t TriangleCount() const;
    uint64_t UserDataOf(InstanceId instance) const;

    /// **Bukan bagian dari API modul.** Dipakai fungsi di `Query.h` yang
    /// dikompilasi bersama modul ini. Tipenya sengaja tidak lengkap di sini:
    /// itulah yang menjamin tidak ada tipe backend yang bocor ke pemanggil.
    const SceneBackend* Internal() const { return backend_.get(); }

private:
    std::unique_ptr<SceneBackend> backend_;
};

}  // namespace sim::raycast
