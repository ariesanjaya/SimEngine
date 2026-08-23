#pragma once

#include "Sim/Core/Math.h"

namespace sim::reference {

/// Parameter permukaan OpenPBR, dalam bentuk yang enak dipakai C++.
///
/// **Nilai bawaannya sama dengan `OpenPBRSurface::defaults()` di
/// `openpbr.slang`**, dan kesamaan itu diuji — bukan diandaikan. Yang di sana
/// yang berlaku saat sebuah pin dibiarkan tidak tersambung.
struct Surface {
    float baseWeight = 1.0f;
    Vec3 baseColor{0.8f};
    float baseMetalness = 0.0f;
    float baseDiffuseRoughness = 0.0f;

    float specularWeight = 1.0f;
    Vec3 specularColor{1.0f};
    float specularRoughness = 0.3f;
    float specularRoughnessAnisotropy = 0.0f;
    float specularIor = 1.5f;

    float coatWeight = 0.0f;
    Vec3 coatColor{1.0f};
    float coatRoughness = 0.0f;
    float coatRoughnessAnisotropy = 0.0f;
    float coatIor = 1.6f;
    float coatDarkening = 1.0f;

    float fuzzWeight = 0.0f;
    Vec3 fuzzColor{1.0f};
    float fuzzRoughness = 0.5f;
};

/// Bingkai shading di satu titik. Sumbunya diserahkan utuh, bukan diturunkan
/// dari normal saja: anisotropi menuntut tangent yang ditentukan, dan acuan
/// harus bisa menaruhnya di tempat yang dipilihnya sendiri.
struct Frame {
    Vec3 normal{0.0f, 0.0f, 1.0f};
    Vec3 tangent{1.0f, 0.0f, 0.0f};
    Vec3 bitangent{0.0f, 1.0f, 0.0f};
    Vec3 view{0.0f, 0.0f, 1.0f};

    /// Bingkai ortonormal dari sebuah normal dan arah pandang. Tangent-nya
    /// sembarang — yang benar hanya untuk permukaan isotropik, dan itu keadaan
    /// yang disebutkan pemanggil dengan memilih fungsi ini.
    static Frame FromNormal(const Vec3& normal, const Vec3& view);
};

/// Suku lingkungan yang diserahkan sudah jadi.
///
/// **Path tracer acuan tidak memakainya.** Ia menelusuri cahaya tak-langsung
/// dengan sinar, bukan dengan probe — dan justru itu yang membuatnya bisa
/// menilai probe. Struktur ini ada supaya jalur lingkungan `openpbr.slang`
/// tetap bisa diuji dari sisi CPU dengan masukan yang ditentukan.
struct Environment {
    Vec3 irradiance{0.0f};
    Vec3 prefilteredBase{0.0f};
    Vec3 prefilteredCoat{0.0f};
    float dfgScale = 0.0f;
    float dfgBias = 0.0f;
};

/// **Ini `openpbr.slang` yang sedang berjalan, bukan tiruannya.**
///
/// Isinya C++ yang dipancarkan `slangc -target cpp` dari sumber yang sama yang
/// dikompilasi ke SPIR-V untuk GPU. Sebuah acuan yang model shading-nya
/// disalin dengan tangan hanya menguji ketelitian penyalinnya; yang ini tidak
/// bisa berselisih dengan yang digambar, karena keduanya satu berkas.
///
/// `lightDirection` menunjuk **dari permukaan ke cahaya**. Arah yang terbalik
/// tidak menghasilkan galat apa pun — hanya permukaan yang gelap di sisi yang
/// salah.
Vec3 EvaluateDirect(const Surface& surface, const Frame& frame, const Frame& coatFrame,
                    const Vec3& lightDirection, const Vec3& radiance);

/// Bentuk ringkas: coat memakai bingkai yang sama dengan dasarnya. Benar untuk
/// material yang tidak mengemudikan `coatNormal`, dan itu hampir semuanya.
Vec3 EvaluateDirect(const Surface& surface, const Frame& frame, const Vec3& lightDirection,
                    const Vec3& radiance);

/// Jalur lingkungan `openpbr.slang`, untuk diuji dari sisi CPU.
Vec3 EvaluateEnvironment(const Surface& surface, const Frame& frame, const Frame& coatFrame,
                         const Environment& environment);
Vec3 EvaluateEnvironment(const Surface& surface, const Frame& frame,
                         const Environment& environment);

}  // namespace sim::reference
