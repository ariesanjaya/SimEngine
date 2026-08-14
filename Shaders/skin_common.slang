// Skinning linear blend: palet matriks kulit dan penerapannya.
//
// **Matriks kulit, bukan transform bone.** Yang ada di palet adalah
// `global × invers bind` — persis yang dihasilkan `Pose::ComputeSkinning`.
// Mengirim transform global saja berarti setiap vertex harus dikembalikan ke
// ruang bind lebih dulu, yaitu satu inversi matriks per vertex per frame untuk
// hasil yang tidak berubah selama rangkanya tidak berubah.
//
// **`SKIN_SET` disebut pemanggil, dan nomornya memang berbeda antar-pass.** Pass
// forward dan prepass memakai set 0 untuk keadaan bayangan per-frame, jadi palet
// menjadi set 1 di sana; pass bayangan tidak boleh mengikat set itu sama sekali
// — ia sedang menulis peta yang ada di dalamnya — jadi paletnya set 0. Satu
// nomor untuk keduanya berarti salah satu pipeline layout menyatakan set yang
// tidak pernah bisa diikat.

#ifndef SKIN_COMMON_SLANG
#define SKIN_COMMON_SLANG

#ifndef SKIN_SET
#error "SKIN_SET harus didefinisikan sebelum meng-include skin_common.slang"
#endif

// Transform instance tinggal di set yang sama dengan palet kulit, dan itu bukan
// kebetulan: pass bayangan hanya mengikat satu set, jadi keduanya harus berada
// di sana bersama-sama atau pass itu tidak bisa menggambar sama sekali.
#define INSTANCE_SET SKIN_SET
#include "instance_common.slang"

/// Konstanta spesialisasi, bukan cabang runtime.
///
/// Nomornya sama dengan `kSkinned` di modul material (`MaterialShaderModule`):
/// keduanya menjawab pertanyaan yang sama, dan dua nomor untuk satu pertanyaan
/// adalah dua nomor yang suatu saat berselisih.
[[vk::constant_id(0)]] const bool kSkinned = false;

[[vk::binding(0, SKIN_SET)]] StructuredBuffer<float4x4> skinPalette;

/// Matriks kulit satu vertex: jumlah berbobot keempat pengaruhnya.
///
/// **Dijumlahkan sebagai matriks, bukan sebagai titik hasil.** Keduanya sama
/// secara matematis, tapi yang ini satu perkalian matriks-vektor untuk posisi
/// dan satu lagi untuk normal — bukan empat masing-masing.
float4x4 skinMatrix(BoxVertex vertex) {
    uint base = vertex.skinBase;
    float4x4 skin = skinPalette[base + vertex.boneIndices.x] * vertex.boneWeights.x;
    skin += skinPalette[base + vertex.boneIndices.y] * vertex.boneWeights.y;
    skin += skinPalette[base + vertex.boneIndices.z] * vertex.boneWeights.z;
    skin += skinPalette[base + vertex.boneIndices.w] * vertex.boneWeights.w;
    return skin;
}

/// Posisi ruang lokal sesudah kulit diterapkan.
///
/// **Terpisah dari versi yang ikut mengurus normal**, dan pemisahannya bukan
/// kerapian: pass bayangan tidak membaca normal sama sekali, dan sebuah fungsi
/// yang menyentuhnya akan menarik atribut itu ke dalam antarmuka entry point-nya
/// — yaitu satu atribut yang harus diambil setiap vertex bayangan untuk hasil
/// yang langsung dibuang.
float3 skinPosition(BoxVertex vertex) {
    if (!kSkinned) {
        return vertex.position;
    }
    return mul(skinMatrix(vertex), float4(vertex.position, 1.0)).xyz;
}

/// Posisi dan normal ruang lokal sesudah kulit diterapkan.
///
/// Arah memakai bagian 3x3 saja — benar untuk bone rotasi + translasi + skala
/// seragam. Skala tak seragam menuntut invers transpose, dan itu belum ada di
/// jalur mana pun di engine ini.
/// Posisi, normal, dan tangent ruang lokal sesudah kulit diterapkan.
///
/// **Terpisah dari `applySkin`, dan pemisahannya bukan kerapian.** Pass bayangan
/// dan prepass tidak membaca tangent, dan sebuah fungsi yang menyentuhnya akan
/// menarik atribut itu ke dalam antarmuka entry point mereka — yaitu satu atribut
/// yang diambil setiap vertex untuk hasil yang langsung dibuang.
void applySkinTangent(BoxVertex vertex, out float3 position, out float3 normal,
                      out float3 tangent) {
    position = vertex.position;
    normal = vertex.normal;
    tangent = vertex.tangent.xyz;
    if (kSkinned) {
        float4x4 skin = skinMatrix(vertex);
        position = mul(skin, float4(vertex.position, 1.0)).xyz;
        normal = mul((float3x3)skin, vertex.normal);
        tangent = mul((float3x3)skin, vertex.tangent.xyz);
    }
}

void applySkin(BoxVertex vertex, out float3 position, out float3 normal) {
    position = vertex.position;
    normal = vertex.normal;
    if (kSkinned) {
        float4x4 skin = skinMatrix(vertex);
        position = mul(skin, float4(vertex.position, 1.0)).xyz;
        normal = mul((float3x3)skin, vertex.normal);
    }
}

#endif
