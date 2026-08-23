#pragma once

#include "Sim/Raycast/RayScene.h"
#include "Sim/Reference/Lights.h"
#include "Sim/Reference/PathTracer.h"

#include <cstdint>
#include <vector>

namespace sim::reference {

/// Adegan acuan: geometri, material per segitiga, dan daftar lampunya.
///
/// **Lampu tinggal di dua tempat, dan itu bukan duplikasi melainkan syarat.**
/// Estimator satu-sampel menemukan cahaya dengan *mengenainya* — sebuah lampu
/// yang hanya ada di `LightList` tidak akan pernah menerangi apa pun, dan
/// sebuah kuad emisif yang tidak ada di daftar tidak akan pernah disampel
/// langsung. `AddQuadLight` menaruhnya di keduanya sekaligus, supaya keadaan
/// setengah itu tidak bisa terjadi karena lupa.
class Scene {
public:
    /// Mendaftarkan material. `emission` bukan nol menjadikannya permukaan yang
    /// menyala sendiri.
    uint32_t AddMaterial(const Surface& surface, const Vec3& emission = Vec3(0.0f));

    /// Kuad, dua segitiga. Normalnya `cross(edgeU, edgeV)` yang dinormalkan.
    void AddQuad(const Vec3& origin, const Vec3& edgeU, const Vec3& edgeV, uint32_t material);

    /// Kuad emisif yang **sekaligus** masuk daftar lampu.
    void AddQuadLight(const Vec3& origin, const Vec3& edgeU, const Vec3& edgeV,
                      const Vec3& radiance, bool doubleSided = false);

    /// Kotak sumbu-sejajar dari dua sudutnya. Enam kuad, normalnya menghadap
    /// keluar.
    void AddBox(const Vec3& minimum, const Vec3& maximum, uint32_t material);

    /// Menyusun scene yang bisa ditanyai sinar. Dipanggil sekali setelah
    /// seluruh geometrinya masuk.
    void Commit(raycast::RayScene& scene) const;

    /// Penerjemah hasil penelusuran menjadi permukaan yang bisa diteduhkan.
    ///
    /// **Menyalin apa yang dibutuhkannya**, bukan menyimpan pointer ke sini:
    /// sebuah resolver yang hidup lebih lama daripada adegannya adalah bug yang
    /// muncul sebagai geometri acak, bukan sebagai crash.
    SurfaceResolver Resolver() const;

    const LightList& Lights() const { return lights_; }
    std::size_t TriangleCount() const { return indices_.size() / 3; }

private:
    struct Triangle {
        uint32_t material = 0;
        Vec3 normal{0.0f, 0.0f, 1.0f};
    };

    std::vector<Vec3> positions_;
    std::vector<uint32_t> indices_;
    std::vector<Triangle> triangles_;
    std::vector<Surface> materials_;
    std::vector<Vec3> emissions_;
    LightList lights_;
};

/// Adegan Cornell box yang sudah tersusun.
struct CornellBox {
    Scene scene;
    Camera camera;
};

/// Cornell box: kotak tertutup, dinding kiri merah, kanan hijau, sisanya putih,
/// dengan lampu bidang di langit-langit.
///
/// **Adegan uji kebocoran cahaya, dan itu gunanya yang paling tajam.** Kotak
/// yang tertutup rapat tidak boleh menerima cahaya dari luar, dan setiap
/// aproksimasi GI yang bekerja dengan jarak — SDF, probe berjarak, screen-space
/// — punya cara sendiri membocorkannya lewat dinding.
///
/// Ukurannya satuan-satu, bukan sentimeter aslinya: yang dibandingkan
/// perbandingan, bukan angka mutlaknya.
CornellBox MakeCornellBox();

/// Kotak tertutup yang **seluruh dindingnya memancar dan memantul**.
///
/// **Jawabannya diketahui persis, dan itu satu-satunya alasan adegan ini ada.**
/// Di dalam rongga tertutup ber-albedo seragam ρ yang setiap permukaannya
/// memancarkan radiansi E, radiansi kesetimbangannya adalah deret geometri
/// `E + ρE + ρ²E + ... = E / (1 - ρ)`.
///
/// Itu menguji **energi pantulan ke-n**, bukan pantulan pertama — dan ia yang
/// membedakan integrator tak-bias dari integrator yang memotong kedalamannya.
/// Pada ρ = 0,8 pantulan kelima masih menyumbang 33% jawabannya.
Scene MakeEnclosedFurnace(float albedo, float emission);

}  // namespace sim::reference
