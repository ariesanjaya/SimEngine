#include "Sim/Material/MaterialShaderModule.h"

#include <fstream>
#include <iterator>
#include <sstream>

namespace sim::material {
namespace {

constexpr std::string_view kImportLine = "import openpbr;";

/// Membuang baris `import openpbr;` dari kode yang dihasilkan graph.
///
/// Kompiler graph menulisnya karena itu bentuk yang benar untuk dibaca manusia
/// — dan kode yang dihasilkan memang sengaja layak dibaca. Yang dikompilasi
/// justru menanam prelude-nya, jadi import itu harus hilang di sini alih-alih
/// tidak pernah ditulis: penulis material yang membuka kode hasil kompilasi
/// tetap melihat berkas yang masuk akal sebagai modul Slang.
std::string StripImport(const std::string& source) {
    const size_t at = source.find(kImportLine);
    if (at == std::string::npos) {
        return source;
    }
    size_t end = source.find('\n', at);
    end = end == std::string::npos ? source.size() : end + 1;
    std::string out = source.substr(0, at);
    out += source.substr(end);
    return out;
}

}  // namespace

std::string LoadOpenPbrPrelude(const std::filesystem::path& shaderDirectory) {
    std::ifstream file(shaderDirectory / "openpbr.slang", std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string AssembleMaterialModule(const std::string& generatedSlang,
                                   const MaterialModuleOptions& options) {
    const std::string vertexEntry =
        options.vertexEntry.empty() ? std::string("vertexMain") : options.vertexEntry;
    const std::string entry =
        options.fragmentEntry.empty() ? std::string("fragmentMain") : options.fragmentEntry;
    std::ostringstream out;

    out << "// Modul yang dikompilasi — dirakit dari kode material dan openpbr.slang.\n";
    out << "// Jangan disunting: yang disunting adalah graph-nya.\n\n";

    out << "// --- konstanta spesialisasi ---------------------------------------\n";
    out << "[vk::constant_id(0)] const bool kSkinned = false;\n";
    out << "[vk::constant_id(1)] const bool kInstanced = false;\n";
    out << "[vk::constant_id(2)] const bool kAlphaTest = false;\n\n";

    out << "// --- model shading (openpbr.slang, ditanam) -----------------------\n";
    out << options.prelude;
    if (!options.prelude.empty() && options.prelude.back() != '\n') {
        out << '\n';
    }
    out << '\n';

    // Set 0 per-frame, set 1 per-objek, set 2 milik material (ditulis kompiler
    // graph). Pembagiannya mengikuti seberapa sering isinya berubah: satu set
    // yang mencampur keduanya memaksa mengikat ulang data per-frame setiap kali
    // objek berganti.
    out << "// --- keadaan per-frame (set 0) -------------------------------------\n";
    out << "[[vk::binding(0, 0)]]\n";
    out << "cbuffer FrameParams\n{\n";
    out << "    float4x4 gViewProjection;\n";
    out << "    float3   gCameraPosition;\n";
    out << "    float    gTime;\n";
    out << "    float3   gLightDirection;  // dari permukaan ke cahaya\n";
    out << "    float    gAlphaCutoff;\n";
    out << "    float3   gLightRadiance;\n";
    out << "    float    gPrefilteredMips;\n";
    // Sembilan koefisien sebagai float4, bukan float3. std140 menjajarkan
    // anggota larik ke 16 byte apa pun tipenya, jadi float3[9] memakan tempat
    // yang sama dengan float4[9] — sementara menulisnya float3 mengundang sisi
    // C++ mengunggah 108 byte yang rapat, dan seluruh koefisien sesudah yang
    // pertama lalu meleset.
    out << "    float4   gIrradianceSh[9];\n";
    out << "}\n\n";

    // Matriks skinning, bukan matriks tulang: `bone world * inverse bind`,
    // sudah digabung di CPU oleh modul Animation. Menggabungnya di shader
    // berarti setiap vertex mengulang perkalian yang sama untuk tulang yang sama.
    out << "[[vk::binding(1, 0)]]\n";
    out << "StructuredBuffer<float4x4> gBoneMatrices;\n";
    out << "[[vk::binding(2, 0)]]\n";
    out << "StructuredBuffer<float4x4> gInstanceTransforms;\n\n";

    // Lingkungan. Tekstur dan sampler terpisah, sama dengan tekstur material —
    // dan cubemap prefilter memakai sampler yang sama dengan LUT DFG tidak bisa,
    // karena yang satu menuntut interpolasi antar-mip dan yang lain tidak punya
    // mip sama sekali.
    out << "[[vk::binding(3, 0)]]\n";
    out << "TextureCube<float4> gPrefilteredEnv;\n";
    out << "[[vk::binding(4, 0)]]\n";
    out << "SamplerState sPrefilteredEnv;\n";
    out << "[[vk::binding(5, 0)]]\n";
    out << "Texture2D<float2> gDfgLut;\n";
    out << "[[vk::binding(6, 0)]]\n";
    out << "SamplerState sDfgLut;\n\n";

    out << "// --- keadaan per-objek (set 1) -------------------------------------\n";
    out << "[[vk::binding(0, 1)]]\n";
    out << "cbuffer ObjectParams\n{\n";
    out << "    float4x4 gWorld;\n";
    out << "}\n\n";

    // Lokasi atribut ditulis eksplisit, tidak diserahkan ke penomoran otomatis.
    //
    // Otomatis menomori menurut urutan atribut *yang bertahan*: dengan kSkinned
    // mati, kedua atribut tulang tidak dibaca siapa pun dan bisa dibuang — lalu
    // seluruh atribut sesudahnya bergeser, sementara `VkVertexInputAttributeDescription`
    // di sisi C++ tidak ikut bergeser. Yang terjadi bukan galat validasi
    // melainkan mesh yang membaca UV sebagai warna.
    out << "struct MaterialVertex\n{\n";
    out << "    [[vk::location(0)]] float3 position     : POSITION;\n";
    out << "    [[vk::location(1)]] float3 normal       : NORMAL;\n";
    out << "    [[vk::location(2)]] float4 tangent      : TANGENT;\n";
    out << "    [[vk::location(3)]] float2 uv0          : TEXCOORD0;\n";
    out << "    [[vk::location(4)]] float4 color        : COLOR0;\n";
    out << "    [[vk::location(5)]] uint4  boneIndices  : BLENDINDICES0;\n";
    out << "    [[vk::location(6)]] float4 boneWeights  : BLENDWEIGHT0;\n";
    out << "};\n\n";

    out << "struct MaterialVarying\n{\n";
    out << "                        float4 clipPosition  : SV_Position;\n";
    out << "    [[vk::location(0)]] float3 worldPosition : TEXCOORD1;\n";
    out << "    [[vk::location(1)]] float3 worldNormal   : NORMAL;\n";
    out << "    [[vk::location(2)]] float4 worldTangent  : TANGENT;  // w = arah bitangent\n";
    out << "    [[vk::location(3)]] float2 uv0           : TEXCOORD0;\n";
    out << "    [[vk::location(4)]] float4 vertexColor   : COLOR0;\n";
    out << "};\n\n";

    out << "// --- kode material -------------------------------------------------\n";
    out << StripImport(generatedSlang);
    out << "\n";

    out << "// --- entry point: vertex -------------------------------------------\n";
    out << "[shader(\"vertex\")]\n";
    out << "MaterialVarying " << vertexEntry
        << "(MaterialVertex v, uint instanceId : SV_InstanceID)\n{\n";
    out << "    float3 position = v.position;\n";
    out << "    float3 normal = v.normal;\n";
    out << "    float3 tangent = v.tangent.xyz;\n\n";

    out << "    if (kSkinned) {\n";
    // Bobot dinormalisasi, bukan dipercaya. Bobot yang jumlahnya 0.98 karena
    // dikuantisasi ke 8 bit di importer menyusutkan mesh 2% — merata, jadi ia
    // tidak terlihat sebagai cacat melainkan sebagai model yang "agak kecil".
    out << "        const float total = dot(v.boneWeights, float4(1.0));\n";
    out << "        const float4 w = total > 1e-6 ? v.boneWeights / total\n";
    out << "                                      : float4(1.0, 0.0, 0.0, 0.0);\n";
    out << "        float3 skinnedPosition = float3(0.0);\n";
    out << "        float3 skinnedNormal = float3(0.0);\n";
    out << "        float3 skinnedTangent = float3(0.0);\n";
    out << "        [unroll]\n";
    out << "        for (int i = 0; i < 4; ++i) {\n";
    out << "            const float4x4 bone = gBoneMatrices[v.boneIndices[i]];\n";
    out << "            skinnedPosition += mul(bone, float4(position, 1.0)).xyz * w[i];\n";
    // Normal dan tangent memakai bagian 3x3-nya saja: translasi tidak boleh ikut
    // pada vektor arah. Ini benar untuk tulang yang rotasi + translasi + skala
    // seragam — yaitu tulang pada umumnya. Skala tak seragam menuntut invers
    // transpose, dan tanpanya normal miring pada tulang yang dipipihkan.
    out << "            skinnedNormal += mul((float3x3)bone, normal) * w[i];\n";
    out << "            skinnedTangent += mul((float3x3)bone, tangent) * w[i];\n";
    out << "        }\n";
    out << "        position = skinnedPosition;\n";
    out << "        normal = skinnedNormal;\n";
    out << "        tangent = skinnedTangent;\n";
    out << "    }\n\n";

    // Instancing membaca transform dari buffer alih-alih dari cbuffer per-objek.
    // Keduanya float4x4 yang sama bentuknya, jadi sisa fungsinya tidak
    // bercabang: yang berbeda hanya dari mana matriksnya datang.
    //
    // `SV_InstanceID` dihitung **relatif terhadap `firstInstance`** — slangc
    // menerjemahkannya jadi `gl_InstanceIndex - gl_BaseInstance`. Jadi draw yang
    // memakai firstInstance bukan nol tetap mengindeks buffer ini dari nol, dan
    // firstInstance tidak bisa dipakai sebagai offset ke dalamnya.
    out << "    const float4x4 world = kInstanced ? gInstanceTransforms[instanceId] : gWorld;\n";
    out << "    const float4 worldPosition = mul(world, float4(position, 1.0));\n\n";

    // Skala seragam saja yang benar di sini. Instance yang diskalakan tak
    // seragam menuntut invers transpose 3x3, yang berarti 64 byte tambahan per
    // instance — dan penabur vegetasi E7.4 hanya menghasilkan yaw + skala
    // seragam, jadi ongkos itu belum dibayar siapa pun.
    out << "    const float3x3 rotation = (float3x3)world;\n\n";

    out << "    MaterialVarying varying;\n";
    out << "    varying.clipPosition = mul(gViewProjection, worldPosition);\n";
    out << "    varying.worldPosition = worldPosition.xyz;\n";
    out << "    varying.worldNormal = mul(rotation, normal);\n";
    out << "    varying.worldTangent = float4(mul(rotation, tangent), v.tangent.w);\n";
    out << "    varying.uv0 = v.uv0;\n";
    out << "    varying.vertexColor = v.color;\n";
    out << "    return varying;\n";
    out << "}\n\n";

    out << "// --- entry point: fragment -----------------------------------------\n";
    out << "[shader(\"fragment\")]\n";
    out << "float4 " << entry << "(MaterialVarying varying) : SV_Target\n{\n";
    out << "    MaterialInputs inputs;\n";
    out << "    inputs.uv0 = varying.uv0;\n";
    out << "    inputs.vertexColor = varying.vertexColor;\n";
    out << "    inputs.worldNormal = normalize(varying.worldNormal);\n";
    out << "    inputs.viewDirection = normalize(gCameraPosition - varying.worldPosition);\n";
    out << "    inputs.time = gTime;\n\n";

    out << "    MaterialSurface m = evalMaterial(inputs);\n\n";

    // Alpha test sebelum apa pun dihitung. Menaruhnya di akhir tetap benar
    // hasilnya, tapi membuat fragmen yang dibuang tetap membayar seluruh
    // evaluasi lobe-nya — dan itu justru material yang paling banyak fragmennya.
    out << "    if (kAlphaTest && m.opacity < gAlphaCutoff) {\n";
    out << "        discard;\n";
    out << "    }\n\n";

    // Bingkai shading dibangun dari tangent kalau ada, dan dari sumbu sembarang
    // kalau tidak. Mesh tanpa tangent tetap harus tergambar; yang hilang hanya
    // arah anisotropi, dan itu tidak terlihat pada material yang isotropik —
    // yaitu hampir semuanya.
    out << "    ShadingFrame frame;\n";
    out << "    frame.normal = inputs.worldNormal;\n";
    out << "    frame.view = inputs.viewDirection;\n";
    out << "    float3 tangent = varying.worldTangent.xyz;\n";
    out << "    if (dot(tangent, tangent) < 1e-8) {\n";
    out << "        const float3 axis = abs(frame.normal.y) < 0.99 ? float3(0, 1, 0)\n";
    out << "                                                      : float3(1, 0, 0);\n";
    out << "        tangent = cross(axis, frame.normal);\n";
    out << "    }\n";
    // Gram-Schmidt: tangent yang diinterpolasi antar-vertex tidak lagi tegak
    // lurus normal, dan bingkai yang miring memutar highlight anisotropik.
    out << "    tangent = normalize(tangent - frame.normal * dot(frame.normal, tangent));\n";
    out << "    frame.tangent = tangent;\n";
    out << "    frame.bitangent = cross(frame.normal, tangent) *\n";
    out << "                      (varying.worldTangent.w < 0.0 ? -1.0 : 1.0);\n\n";

    // Normal map bekerja di ruang tangent, jadi ia dipasang sesudah bingkainya
    // ada — dan bingkainya sendiri tidak diputar ulang: yang berubah hanya arah
    // normal yang dipakai lobe, sedangkan sumbu anisotropi tetap milik permukaan.
    out << "    if (dot(m.normal, m.normal) > 1e-8) {\n";
    out << "        const float3 n = normalize(m.normal);\n";
    out << "        frame.normal = normalize(frame.tangent * n.x + frame.bitangent * n.y +\n";
    out << "                                 inputs.worldNormal * n.z);\n";
    out << "    }\n\n";

    out << "    float3 lit = evaluateOpenPBR(m.surface, frame, normalize(gLightDirection),\n";
    out << "                                 gLightRadiance);\n\n";

    // Lingkungan. Tanpa ini logam hitam di luar sorotannya — benar secara
    // fisika untuk satu cahaya langsung, dan salah sebagai gambar.
    out << "    float3 shCoefficients[9];\n";
    out << "    [unroll]\n";
    out << "    for (int i = 0; i < 9; ++i) {\n";
    out << "        shCoefficients[i] = gIrradianceSh[i].rgb;\n";
    out << "    }\n";
    out << "    const float3 irradiance = evaluateIrradianceSh(shCoefficients, frame.normal);\n";
    out << "    const float3 reflection = prefilterDirection(frame);\n";
    // Dua pengambilan dari peta yang sama pada mip yang berbeda: base dan coat
    // punya kekasarannya sendiri, dan memakai satu mip untuk keduanya membuat
    // coat yang licin tampak sekasar lapisan di bawahnya.
    out << "    const float3 prefilteredBase = gPrefilteredEnv.SampleLevel(\n";
    out << "        sPrefilteredEnv, reflection,\n";
    out << "        prefilterMipForRoughness(m.surface.specularRoughness, gPrefilteredMips)).rgb;\n";
    out << "    const float3 prefilteredCoat = gPrefilteredEnv.SampleLevel(\n";
    out << "        sPrefilteredEnv, reflection,\n";
    out << "        prefilterMipForRoughness(m.surface.coatRoughness, gPrefilteredMips)).rgb;\n";
    out << "    const float nv = saturate(dot(frame.normal, frame.view));\n";
    out << "    const float2 dfg = gDfgLut.SampleLevel(\n";
    out << "        sDfgLut, float2(nv, saturate(m.surface.specularRoughness)), 0.0);\n";
    out << "    lit += evaluateOpenPBR_IBL(m.surface, frame, irradiance, prefilteredBase,\n";
    out << "                               prefilteredCoat, dfg);\n";
    out << "    lit += m.emissive;\n";
    out << "    return float4(lit, m.opacity);\n";
    out << "}\n";

    return out.str();
}

CompileRequest MakeMaterialRequest(const std::string& generatedSlang, ShaderStage stage,
                                   const MaterialModuleOptions& options) {
    CompileRequest request;
    request.source = AssembleMaterialModule(generatedSlang, options);
    request.stage = stage;
    if (stage == ShaderStage::Vertex) {
        request.entryPoint =
            options.vertexEntry.empty() ? std::string("vertexMain") : options.vertexEntry;
    } else {
        request.entryPoint =
            options.fragmentEntry.empty() ? std::string("fragmentMain") : options.fragmentEntry;
    }
    return request;
}

}  // namespace sim::material
