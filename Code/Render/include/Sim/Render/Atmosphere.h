#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>

namespace sim::render {

/// Parameter atmosfer Bruneton-Hillaire. **Seluruhnya dalam kilometer**, sama
/// dengan acuan di `/home/arie/SDK/atmosphere-bac` — jari-jari bumi 6360 dan
/// hamburan berorde 10⁻³ per kilometer. Mencampurnya dengan meter, satuan yang
/// dipakai seluruh sisa engine, tidak menghasilkan galat apa pun: hanya langit
/// yang seluruhnya hitam atau seluruhnya putih, bergantung arah kesalahannya.
struct AtmosphereParameters {
    float bottomRadius = 6360.0f;
    float topRadius = 6460.0f;

    /// Hamburan Rayleigh per kilometer pada permukaan laut, dan tinggi skalanya.
    Vec3 rayleighScattering{0.005802f, 0.013558f, 0.033100f};
    float rayleighScaleHeight = 8.0f;

    /// Mie: yang dihamburkan lebih kecil daripada yang diredam — selisihnya
    /// diserap.
    float mieScattering = 0.003996f;
    float mieExtinction = 0.004440f;
    float mieScaleHeight = 1.2f;
    /// Anisotropi Mie. 0,8 berarti hamburan sangat condong ke depan, yang
    /// menghasilkan lingkar terang di sekitar matahari.
    float miePhaseG = 0.8f;

    /// Serapan ozon. **Tidak menghamburkan apa pun**, hanya menyerap — dan
    /// tanpanya langit siang hari condong ke kuning kehijauan alih-alih biru,
    /// karena tidak ada yang memakan bagian merah-kuning di jalur panjang.
    Vec3 ozoneAbsorption{0.000650f, 0.001881f, 0.000085f};
    /// Profil tenda: nol di bawah `ozoneBottom`, puncak 1 di `ozonePeak`, nol
    /// lagi di `ozoneTop`.
    float ozoneBottom = 10.0f;
    float ozonePeak = 25.0f;
    float ozoneTop = 40.0f;

    /// Iradiansi matahari di puncak atmosfer.
    Vec3 solarIrradiance{1.474000f, 1.850400f, 1.911980f};
    /// Jari-jari sudut matahari, radian.
    float sunAngularRadius = 0.004675f;

    Vec3 groundAlbedo{0.0f};
};

/// Jarak ke perpotongan terdekat sebuah sinar dengan bola berpusat di titik asal.
/// Mengembalikan −1 bila tidak ada perpotongan di depan.
float RaySphereNearest(const Vec3& origin, const Vec3& direction, float radius);

/// Kerapatan tiap medium pada sebuah ketinggian (km di atas permukaan laut).
struct MediumDensity {
    float rayleigh = 0.0f;
    float mie = 0.0f;
    float ozone = 0.0f;
};
MediumDensity SampleDensity(const AtmosphereParameters& atmosphere, float heightKm);

/// Koefisien redaman total pada sebuah ketinggian.
Vec3 SampleExtinction(const AtmosphereParameters& atmosphere, float heightKm);
/// Koefisien hamburan (tanpa ozon — ia hanya menyerap).
Vec3 SampleScattering(const AtmosphereParameters& atmosphere, float heightKm);

/// Kedalaman optik sepanjang sebuah sinar sampai ia meninggalkan atmosfer.
Vec3 IntegrateOpticalDepth(const AtmosphereParameters& atmosphere, const Vec3& origin,
                           const Vec3& direction, uint32_t sampleCount = 512);

/// Transmitansi sepanjang sinar itu, yaitu exp(−kedalaman optik).
Vec3 Transmittance(const AtmosphereParameters& atmosphere, const Vec3& origin,
                   const Vec3& direction, uint32_t sampleCount = 512);

/// Kedalaman optik menuju zenit dari permukaan laut, dihitung analitik.
///
/// **Ada supaya integral numeriknya punya lawan bicara.** Untuk lapisan
/// eksponensial, ∫ρ ds sepanjang zenit tepat sama dengan tinggi skalanya; untuk
/// ozon, luas tendanya. Angka yang hanya dihitung satu cara adalah angka yang
/// tidak pernah diperiksa.
Vec3 AnalyticZenithOpticalDepth(const AtmosphereParameters& atmosphere);

// --- Pemetaan LUT ------------------------------------------------------------

/// Parameter LUT transmitansi: ketinggian dari pusat planet, dan kosinus sudut
/// zenit pandangan.
struct TransmittanceParams {
    float radius = 0.0f;
    float cosZenith = 0.0f;
};

/// **Pemetaan uv-nya tidak seragam, dan itu bukan hiasan.** Pemetaan seragam
/// menghabiskan hampir seluruh texel pada sudut yang jarang dipandang, dan
/// menyisakan beberapa texel untuk pita tipis di dekat horizon — yaitu bagian
/// langit yang paling diperhatikan orang. Kedua arah harus saling membalik
/// dengan tepat: pemetaan yang tidak membalik tidak menghasilkan galat apa pun,
/// hanya langit yang warnanya masuk akal di tempat yang salah.
Vec2 TransmittanceParamsToUv(const AtmosphereParameters& atmosphere,
                             const TransmittanceParams& params);
TransmittanceParams UvToTransmittanceParams(const AtmosphereParameters& atmosphere,
                                            const Vec2& uv);

/// Parameter LUT sky-view: sudut zenit pandangan dan sudut antara pandangan dan
/// matahari, keduanya radian.
struct SkyViewParams {
    float viewZenithAngle = 0.0f;
    float lightViewAngle = 0.0f;
};

Vec2 SkyViewParamsToUv(const AtmosphereParameters& atmosphere, const SkyViewParams& params,
                       float viewHeight, bool intersectsGround, const Vec2& dimensions);
SkyViewParams UvToSkyViewParams(const AtmosphereParameters& atmosphere, const Vec2& uv,
                                float viewHeight, const Vec2& dimensions);

// --- Aerial perspective ------------------------------------------------------

/// Satu texel LUT aerial perspective: udara yang berada **di antara** kamera dan
/// sebuah permukaan.
///
/// **Dua besaran, bukan satu.** Kabut yang hanya memadu warna adegan menuju satu
/// warna kabut memaksa memilih satu warna untuk seluruh arah pandang — dan arah
/// yang menghadap matahari serta arah yang membelakanginya berbeda jauh justru
/// pada jarak yang membuat kabut terlihat. Di sini keduanya terpisah: apa yang
/// dimakan udara, dan apa yang ditambahkannya.
struct AerialSample {
    /// Cahaya yang dihamburkan masuk sepanjang ruas kamera→permukaan.
    Vec3 inscatter{0.0f};
    /// Transmitansi sepanjang ruas yang sama.
    ///
    /// **Vektor, bukan skalar.** Udara memakan biru jauh lebih cepat daripada
    /// merah; satu skalar akan memudarkan gunung yang jauh tanpa memerahkan
    /// cahaya yang menembusnya, dan yang hilang persis kebalikan dari yang
    /// membuat matahari terbenam terlihat seperti matahari terbenam.
    Vec3 transmittance{1.0f};
};

/// Jarak ke tengah slice ke-`slice`, kilometer.
///
/// **Sebaran kuadratik, bukan seragam.** Kabut yang berarti berada di beberapa
/// ratus meter pertama; sebaran seragam atas jangkauan penuh menghabiskan hampir
/// seluruh slice-nya pada jarak yang isinya sudah nyaris tak berubah, dan
/// menyisakan satu-dua slice untuk seluruh peralihan yang benar-benar terlihat.
float AerialSliceDistance(uint32_t slice, uint32_t sliceCount, float maxDistanceKm);

/// Kebalikannya: jarak → koordinat tekstur w, 0..1.
///
/// **Harus membalik `AerialSliceDistance` dengan tepat.** Pemetaan yang meleset
/// tidak menghasilkan galat apa pun, hanya kabut yang pekatnya benar pada jarak
/// yang salah — dan "pekat yang benar di tempat yang salah" adalah persis yang
/// tidak terlihat sebagai kesalahan saat memandanginya.
float AerialDistanceToSliceCoord(float distanceKm, uint32_t sliceCount, float maxDistanceKm);

/// Integrasi hamburan masuk dan transmitansi sepanjang ruas sepanjang
/// `distanceKm`, berhenti lebih awal bila sinarnya menembus planet.
///
/// **Hamburan tunggal saja.** Suku multiscattering-nya ada di GPU lewat LUT yang
/// dibangun pass tersendiri, dan yang di sini adalah bagian yang punya lawan
/// bicara analitik — geometri, energi, dan kesinambungannya dengan langit.
AerialSample IntegrateAerialPerspective(const AtmosphereParameters& atmosphere,
                                        const Vec3& origin, const Vec3& direction,
                                        const Vec3& sunDirection, float distanceKm,
                                        uint32_t sampleCount = 64);

/// Komposit ke warna adegan: **diredam lalu ditambah**, bukan dipadu.
///
/// Paduan `lerp(scene, fogColor, k)` adalah bentuk yang sama dengan kesalahan
/// komposit bloom, dan gagal dengan cara yang sama: ia menuntut satu warna kabut
/// yang berdiri sendiri, sementara yang sebenarnya terjadi adalah dua hal
/// berlainan yang kebetulan sama-sama bergantung jarak.
Vec3 ApplyAerialPerspective(const Vec3& sceneColor, const AerialSample& sample);

// --- Fungsi fase -------------------------------------------------------------

/// Fase Rayleigh. Berintegral tepat satu atas bola — sebuah fase yang tidak
/// berintegral satu memindahkan energi masuk atau keluar dari adegan pada setiap
/// peristiwa hamburan.
float RayleighPhase(float cosTheta);

/// Fase Mie Cornette-Shanks.
float MiePhase(float cosTheta, float g);

}  // namespace sim::render
