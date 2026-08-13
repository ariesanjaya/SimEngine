#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Core/VolumeGrid.h"

#include <cstdint>

/// Acuan CPU untuk raymarch volumetrik.
///
/// **Acuan kebenaran, bukan yang dipakai saat menggambar.** Yang menggambar
/// membaca tekstur 3D di GPU; yang di sini memakai rumus yang sama persis,
/// cukup lambat untuk test dan cukup sederhana untuk dibaca. Budaya yang sama
/// yang sudah dipakai `SdfVolume` untuk clipmap — dan alasannya sama: acuan
/// tidak ada gunanya kalau ia ditulis dari pemahaman yang berbeda.
namespace sim::volume {

/// Sifat optis medium, dan bagaimana ia dijejaki.
struct RaymarchSettings {
    /// Penyerapan per satuan kerapatan per meter. Nilai grid dikali ini
    /// menghasilkan koefisien kepunahan.
    float extinction = 1.0f;
    /// Albedo hamburan: berapa bagian dari yang punah kembali sebagai cahaya.
    Vec3 scatterAlbedo{0.8f};
    /// Cahaya yang datang, sudah termasuk fase dan bayangannya. Disederhanakan
    /// menjadi tetapan karena yang diuji di sini adalah **integrasinya**, bukan
    /// model pencahayaannya.
    Vec3 incomingLight{1.0f};
    /// Panjang satu langkah, meter.
    float stepSize = 0.05f;
    /// Batas langkah, supaya ray yang menyerempet volume besar tidak berjalan
    /// tanpa akhir.
    uint32_t maxSteps = 512;
    /// Transmitansi di mana penjejakan berhenti. Di bawah ini sisanya tidak
    /// lagi mengubah piksel mana pun.
    float minTransmittance = 0.003f;
};

struct RaymarchResult {
    /// Cahaya yang terkumpul di sepanjang ray.
    Vec3 scattered{0.0f};
    /// Berapa bagian dari latar yang masih terlihat menembusnya.
    float transmittance = 1.0f;
    /// Berapa langkah yang benar-benar diambil. Dipakai test untuk membuktikan
    /// penghentian dini memang terjadi.
    uint32_t steps = 0;
};

/// Memotong ray dengan kotak sejajar sumbu. False bila tidak berpotongan.
///
/// Terbuka karena raymarch tidak boleh mulai dari kamera: kebanyakan ray tidak
/// menyentuh volumenya sama sekali, dan yang membuatnya murah adalah menolak
/// mereka sebelum langkah pertama.
bool IntersectBox(const Vec3& origin, const Vec3& direction, const Vec3& boxMin,
                  const Vec3& boxMax, float& outNear, float& outFar);

/// Menjejaki sebuah ray melalui grid, di **ruang lokal grid**.
///
/// Integrasinya persis yang dipakai `sky_clouds.frag.slang`: kepunahan
/// eksponensial per langkah, dan cahaya terhambur diintegrasikan secara
/// analitik di dalam langkah alih-alih dikali panjang langkah. Bedanya bukan
/// kerapian — cara yang naif membuat hasilnya bergantung pada besar langkah,
/// sehingga menaikkan kualitas mengubah kecerahan adegan.
RaymarchResult Raymarch(const VolumeGrid& grid, const Vec3& origin, const Vec3& direction,
                        const RaymarchSettings& settings);

}  // namespace sim::volume
