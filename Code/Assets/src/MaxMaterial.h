#pragma once

#include "Sim/Assets/MeshData.h"

#include <fbxsdk.h>

#include <string>
#include <vector>

/// Blok properti kustom yang ditulis 3ds Max ke dalam FBX.
///
/// **Header privat, dan sengaja tidak ada di `include/`.** Ia menyebut tipe FBX
/// SDK di tanda tangannya, dan SDK itu berlisensi Autodesk — membocorkannya ke
/// header publik `Sim::Assets` berarti setiap modul yang menyentuh mesh ikut
/// menyeretnya. Aturan yang sama sudah dipegang `Code/Assets/CMakeLists.txt`,
/// yang menautkan `Fbx::Fbx` sebagai PRIVATE.
///
/// **Apa yang sebenarnya ada di dalam FBX dari Max.** FBX tidak punya slot
/// material PBR: `FbxSurfaceMaterial` adalah Lambert/Phong, titik. Yang
/// dilakukan eksportir Max adalah menempelkan blok parameter aslinya sebagai
/// properti kustom bernama hierarkis — `3dsMax|Parameters|base_color`,
/// `3dsMax|Parameters|roughness`, dan seterusnya — di sebelah properti Phong
/// yang sudah ada. Blok itu daftar angka, bukan graph; ia tidak bisa menyatakan
/// node di antara tekstur dan input, dan karena itu selalu kalah dari dokumen
/// `.mtlx` bila keduanya ada.
///
/// **Yang di sini diturunkan dari spesifikasi dan konvensi, belum dari sebuah
/// berkas Max sungguhan.** Nama-nama parameternya adalah nama parameter block
/// Physical Material dan nama input OpenPBR; keduanya terdokumentasi, dan
/// keduanya belum diperiksa terhadap ekspor nyata di mesin ini. Alat
/// pemeriksanya sudah ada dan sengaja dibuat bersamaan:
///
///     SimHeadless --project P --no-render --dump-fbx-material <berkas.fbx>
///
/// Ia mencetak setiap properti tiap material apa adanya, jadi selisih antara
/// tebakan di sini dan kenyataan bisa dilihat alih-alih diperdebatkan.
namespace sim::assets {

/// Membaca blok `3dsMax|Parameters` sebuah material menjadi `MeshMaterial::openPbr`.
///
/// Mengembalikan false bila materialnya tidak membawa blok itu sama sekali —
/// yaitu setiap berkas yang bukan dari Max, dan setiap material Max yang memakai
/// Standard lama. Itu bukan kegagalan: jalur Lambert/Phong tetap berlaku.
///
/// `material` harus sudah terisi jalur teksturnya dari slot FBX baku, karena
/// slot itulah yang dipakai bila blok Max tidak menyebut peta sendiri.
bool ReadMaxMaterial(const FbxSurfaceMaterial& source, MeshMaterial& material);

/// Jalur dokumen `.mtlx` yang disebut sebuah material, bila ada.
///
/// Dikumpulkan dari **setiap** properti bertipe string di material itu, bukan
/// dari satu nama properti yang dipatok: material MaterialX di Max menyimpan
/// jalur dokumennya di parameter block-nya sendiri, dan nama parameter itu
/// berbeda antara MaterialX Map dan material OpenPBR-nya. Yang tidak berbeda
/// adalah ekstensinya.
void CollectMaterialXPaths(const FbxSurfaceMaterial& source, std::vector<std::string>& out);

/// Mencetak seluruh properti sebuah material, satu per baris, apa adanya.
///
/// Dipakai `SimHeadless --dump-fbx-material`. Ada di sini dan bukan di aplikasi
/// itu karena penelusuran propertinya sama persis dengan yang dipakai pembaca
/// di atas — dan dua penelusuran yang harus sepakat lebih baik menjadi satu.
std::vector<std::string> DescribeMaterialProperties(const FbxSurfaceMaterial& source);

}  // namespace sim::assets
