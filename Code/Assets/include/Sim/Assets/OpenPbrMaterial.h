#pragma once

#include "Sim/Core/Math.h"

#include <string>

/// Material OpenPBR Surface v1.1 sebagaimana dibawa berkas sumbernya.
///
/// **Ada karena `MeshMaterial` sengaja tidak lengkap.** Bentuk datar itu adalah
/// pembagi bersama terkecil dari glTF, USD, dan Phong FBX — lima angka yang
/// setiap format bisa jawab. Ia tepat untuk sumber yang memang hanya punya lima
/// angka, dan salah untuk sumber yang punya lebih: material OpenPBR dari 3ds
/// Max harus **diproyeksikan** ke sana, dan proyeksi melipat coat ke roughness,
/// membuang fuzz, dan memaksa IOR menjadi 0,04. Hasilnya bekerja, dan selalu
/// "mirip" — tidak pernah "sama".
///
/// Jadi yang lengkap tinggal di sini, dan `MeshMaterial::openPbr` mengisinya
/// hanya bila sumbernya benar-benar menyebut sebuah material OpenPBR. Yang
/// tidak, tetap datar; jalur lamanya tidak berubah sama sekali.
///
/// **Nama medannya nama pin, bukan nama MaterialX.** Yang tertulis di
/// `open_pbr_surface.mtlx` adalah `base_color`, `specular_roughness`,
/// `coat_weight`; yang tertulis di `output.surface` mesin ini adalah
/// `baseColor`, `specularRoughness`, `coatWeight`. Terjemahannya terjadi tepat
/// sekali, di pembaca `.mtlx`, dan sesudah itu tidak ada lagi dua ejaan untuk
/// satu besaran. Nilai bawaannya pun bawaan pin — sama dengan
/// `OpenPBRSurface::defaults()` di `openpbr.slang` dan dengan nodedef MaterialX
/// 1.39.6, yang ketiganya memang satu angka yang tertulis di tiga tempat.
namespace sim::assets {

/// Satu saluran tekstur beserta apa yang harus dilakukan padanya.
///
/// **Jalurnya relatif terhadap berkas sumbernya**, aturan yang sama dengan
/// `MeshMaterial`: jalur absolut di dalam berkas menunjuk mesin yang
/// mengekspornya.
struct OpenPbrTexture {
    std::string path;

    bool Empty() const { return path.empty(); }
};

struct OpenPbrMaterial {
    /// Nama material di berkas sumbernya. Dipakai memasangkan material FBX
    /// dengan material di dokumen `.mtlx` di sebelahnya.
    std::string name;

    // --- Base ---------------------------------------------------------------
    float baseWeight = 1.0f;
    Vec3 baseColor{0.8f, 0.8f, 0.8f};
    float baseMetalness = 0.0f;
    float baseDiffuseRoughness = 0.0f;

    // --- Specular -----------------------------------------------------------
    float specularWeight = 1.0f;
    Vec3 specularColor{1.0f, 1.0f, 1.0f};
    float specularRoughness = 0.3f;
    float specularRoughnessAnisotropy = 0.0f;
    float specularIor = 1.5f;

    // --- Coat ---------------------------------------------------------------
    float coatWeight = 0.0f;
    Vec3 coatColor{1.0f, 1.0f, 1.0f};
    float coatRoughness = 0.0f;
    float coatRoughnessAnisotropy = 0.0f;
    float coatIor = 1.6f;
    float coatDarkening = 1.0f;

    // --- Fuzz ---------------------------------------------------------------
    float fuzzWeight = 0.0f;
    Vec3 fuzzColor{1.0f, 1.0f, 1.0f};
    float fuzzRoughness = 0.5f;

    // --- Di luar campuran lobe ----------------------------------------------
    //
    // **`emissive` satu warna, bukan `emission_luminance` × `emission_color`.**
    // OpenPBR memisahkan keduanya karena luminansinya bersatuan nit dan bisa
    // menyentuh ribuan; pin mesin ini satu float3 radiansi. Perkaliannya
    // dikerjakan pembacanya, sekali, dan yang sampai ke sini sudah berupa nilai
    // yang dipakai apa adanya — dua medan yang harus selalu dikalikan bersama
    // adalah dua kesempatan untuk lupa mengalikannya.
    Vec3 emissive{0.0f, 0.0f, 0.0f};
    /// `geometry_opacity` di MaterialX.
    float opacity = 1.0f;

    // --- Tekstur -------------------------------------------------------------
    //
    // **Mengalikan, bukan menggantikan.** Induk `Material Impor OpenPBR.simmat`
    // menyusun tiap saluran sebagai `skalar × tekstur`, dan tekstur yang tidak
    // diisi terbaca putih — jadi material tanpa tekstur memakai skalarnya apa
    // adanya, tanpa cabang dan tanpa graph kedua. Untuk input `.mtlx` yang
    // seluruhnya dikemudikan gambar, pembacanya menyetel skalarnya ke 1 supaya
    // `1 × tekstur` benar-benar sama dengan gambarnya.
    OpenPbrTexture baseColorTexture;
    OpenPbrTexture specularRoughnessTexture;
    OpenPbrTexture baseMetalnessTexture;
    OpenPbrTexture emissiveTexture;
    OpenPbrTexture opacityTexture;

    /// Peta normal tangent-space, yaitu `geometry_normal` yang dikemudikan
    /// sebuah node `normalmap`.
    ///
    /// **Ditemani `normalTextureAmount`, dan itu bukan kerapian.** Saluran lain
    /// bisa mengalikan karena putih adalah identitasnya; peta normal tidak —
    /// tekstur kosong terbaca putih, dan `2 × putih − 1` adalah (1,1,1), sebuah
    /// normal miring 45° di setiap piksel. Jadi induknya menyusunnya sebagai
    /// `lerp(datar, terdekode, amount)` dan importir menyalakannya hanya ketika
    /// benar-benar ada petanya.
    OpenPbrTexture normalTexture;

    /// Apakah material ini memakai lapisan di luar base+specular. Menentukan
    /// juga tidaknya lapisan itu ikut dikompilasi — lihat `DetectLobes`.
    bool UsesLayers() const {
        return coatWeight != 0.0f || fuzzWeight != 0.0f || baseDiffuseRoughness != 0.0f ||
               specularRoughnessAnisotropy != 0.0f || coatRoughnessAnisotropy != 0.0f;
    }

    bool HasTexture() const {
        return !baseColorTexture.Empty() || !specularRoughnessTexture.Empty() ||
               !baseMetalnessTexture.Empty() || !emissiveTexture.Empty() ||
               !opacityTexture.Empty() || !normalTexture.Empty();
    }
};

}  // namespace sim::assets
