#include "Sim/Material/MaterialShaderModule.h"
#include <set>

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

    // **Lapisan yang tidak mungkin dipakai dimatikan di sini**, sebelum
    // `slangc` melihat model shadingnya. Nilainya diketahui saat kompilasi, jadi
    // yang mati bukan cabang yang tidak diambil melainkan kode yang tidak pernah
    // ada — dan di GPU keduanya sangat berbeda: cabang yang diambil sebagian
    // lane dalam satu warp membayar kedua sisinya.
    out << "// --- lapisan yang dipakai material ini -----------------------------\n";
    out << "#define OPENPBR_HAS_COAT " << (options.lobes.coat ? 1 : 0) << "\n";
    out << "#define OPENPBR_HAS_FUZZ " << (options.lobes.fuzz ? 1 : 0) << "\n";
    out << "#define OPENPBR_HAS_ANISOTROPY " << (options.lobes.anisotropy ? 1 : 0) << "\n";
    out << "#define OPENPBR_HAS_DIFFUSE_ROUGHNESS "
        << (options.lobes.diffuseRoughness ? 1 : 0) << "\n\n";

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
    // Preview menggambar satu material saja, dan jalurnya tidak pernah
    // bindless — slotnya karena itu nol dan tidak pernah dibaca.
    out << "    inputs.materialSlot = 0;\n";
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

    // **Bingkai coat dibangun sebelum peta normal dasar dipasang.** Spesifikasi
    // memperlakukan keduanya sebagai dua pelekukan yang berdiri sendiri atas
    // normal yang sama; membangunnya sesudah akan membuat coat mewarisi lekukan
    // dasarnya, yaitu persis yang tidak terjadi pada pernis sungguhan.
    if (options.lobes.coatFrame) {
        out << "    const ShadingFrame coatFrame = makeCoatFrame(frame, m.coatNormal,\n";
        out << "                                                 m.coatTangent);\n\n";
    }

    // Normal map bekerja di ruang tangent, jadi ia dipasang sesudah bingkainya
    // ada — dan bingkainya sendiri tidak diputar ulang: yang berubah hanya arah
    // normal yang dipakai lobe, sedangkan sumbu anisotropi tetap milik permukaan.
    out << "    if (dot(m.normal, m.normal) > 1e-8) {\n";
    out << "        const float3 n = normalize(m.normal);\n";
    out << "        frame.normal = normalize(frame.tangent * n.x + frame.bitangent * n.y +\n";
    out << "                                 inputs.worldNormal * n.z);\n";
    out << "    }\n\n";

    // Sumbu anisotropi diputar sesudah normalnya dipasang, dan hanya kalau
    // materialnya memang mengemudikannya — yang tidak, tidak membayar apa pun.
    if (options.lobes.tangent) {
        out << "    frame.tangent = rotatedTangent(frame, m.tangent);\n";
        out << "    frame.bitangent = cross(frame.normal, frame.tangent) *\n";
        out << "                      (varying.worldTangent.w < 0.0 ? -1.0 : 1.0);\n\n";
    }

    const char* const coatArgument = options.lobes.coatFrame ? "frame, coatFrame" : "frame";
    out << "    float3 lit = evaluateOpenPBR(m.surface, " << coatArgument << ",\n";
    out << "                                 normalize(gLightDirection), gLightRadiance);\n\n";

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
    //
    // Kekasaran dasarnya lewat `coatRoughenedSpecular`, bukan apa adanya —
    // pengasaran oleh coat harus terlihat di bawah lingkungan persis seperti
    // di bawah lampu, dan yang memilih mip di sinilah tempat satu-satunya
    // keputusan itu bisa dipasang.
    out << "    const float3 prefilteredBase = gPrefilteredEnv.SampleLevel(\n";
    out << "        sPrefilteredEnv, reflection,\n";
    out << "        prefilterMipForRoughness(coatRoughenedSpecular(m.surface), "
           "gPrefilteredMips)).rgb;\n";
    out << "    const float3 prefilteredCoat = gPrefilteredEnv.SampleLevel(\n";
    out << "        sPrefilteredEnv, "
        << (options.lobes.coatFrame ? "prefilterDirection(coatFrame)" : "reflection") << ",\n";
    out << "        prefilterMipForRoughness(m.surface.coatRoughness, gPrefilteredMips)).rgb;\n";
    out << "    const float nv = saturate(dot(frame.normal, frame.view));\n";
    out << "    const float2 dfg = gDfgLut.SampleLevel(\n";
    out << "        sDfgLut, float2(nv, saturate(coatRoughenedSpecular(m.surface))), 0.0);\n";
    out << "    lit += evaluateOpenPBR_IBL(m.surface, " << coatArgument << ", irradiance,\n";
    out << "                               prefilteredBase, prefilteredCoat, dfg);\n";
    // Emisi lewat coat, bukan di atasnya — lihat `coatedEmission` di
    // openpbr.slang. Material tanpa coat mendapat nilainya apa adanya.
    out << "    lit += coatedEmission(m.surface, m.emissive, nv);\n";
    out << "    return float4(lit, m.opacity);\n";
    out << "}\n";

    return out.str();
}

namespace {

/// Menanam sebuah berkas beserta `#include`-nya secara rekursif.
void InlineOne(const std::filesystem::path& directory, const std::string& name,
               std::set<std::string>& seen, std::ostringstream& out) {
    if (!seen.insert(name).second) {
        return;
    }
    std::ifstream stream(directory / name);
    if (!stream) {
        // Berkas yang hilang tidak dikarang menjadi teks kosong: modul yang
        // dirakit tanpanya tetap dikompilasi dan gagal jauh dari sebabnya, dengan
        // pesan tentang simbol yang tidak dikenal alih-alih tentang berkas.
        out << "#error \"tidak bisa membaca " << name << "\"\n";
        return;
    }
    std::string line;
    while (std::getline(stream, line)) {
        const std::size_t include = line.find("#include \"");
        if (include != std::string::npos) {
            const std::size_t start = include + 10;
            const std::size_t end = line.find('"', start);
            if (end != std::string::npos) {
                InlineOne(directory, line.substr(start, end - start), seen, out);
                continue;
            }
        }
        out << line << '\n';
    }
}

}  // namespace

std::string InlineShaderIncludes(const std::filesystem::path& shaderDirectory,
                                 const std::vector<std::string>& roots) {
    std::set<std::string> seen;
    std::ostringstream out;
    for (const std::string& root : roots) {
        InlineOne(shaderDirectory, root, seen, out);
    }
    return out.str();
}

std::string AssembleForwardMaterialModule(const std::string& generatedSlang,
                                          const ForwardMaterialOptions& options) {
    const std::string entry =
        options.fragmentEntry.empty() ? std::string("main") : options.fragmentEntry;

    std::ostringstream out;
    out << "// Dihasilkan AssembleForwardMaterialModule. Jangan disunting tangan.\n\n";

    out << "// --- deklarasi milik renderer (set 0 dan varying) ------------------\n";
    out << options.frameDeclarations;
    if (!options.frameDeclarations.empty() && options.frameDeclarations.back() != '\n') {
        out << '\n';
    }
    out << '\n';

    out << "// --- lapisan yang dipakai material ini -----------------------------\n";
    out << "#define OPENPBR_HAS_COAT " << (options.lobes.coat ? 1 : 0) << "\n";
    out << "#define OPENPBR_HAS_FUZZ " << (options.lobes.fuzz ? 1 : 0) << "\n";
    out << "#define OPENPBR_HAS_ANISOTROPY " << (options.lobes.anisotropy ? 1 : 0) << "\n";
    out << "#define OPENPBR_HAS_DIFFUSE_ROUGHNESS "
        << (options.lobes.diffuseRoughness ? 1 : 0) << "\n\n";

    out << "// --- model shading (openpbr.slang, ditanam) -----------------------\n";
    out << options.prelude;
    if (!options.prelude.empty() && options.prelude.back() != '\n') {
        out << '\n';
    }
    out << '\n';

    out << "// --- kode material -------------------------------------------------\n";
    out << StripImport(generatedSlang);
    out << "\n";

    out << "// --- entry point: fragment -----------------------------------------\n";
    out << "[shader(\"fragment\")]\n";
    out << "float4 " << entry << "(BoxVarying input) : SV_Target\n{\n";
    out << "    MaterialInputs inputs;\n";
    out << "    inputs.materialSlot = input.slots.x;\n";
    out << "    inputs.uv0 = input.uv;\n";
    // Warna instance dan warna simpul sudah dikalikan tahap vertex, jadi yang
    // sampai ke sini satu warna. Ia masuk sebagai warna simpul material — yang
    // memakainya lewat node `input.vertexColor`, dan yang tidak memakainya tidak
    // terpengaruh sama sekali.
    out << "    inputs.vertexColor = input.color;\n";
    out << "    inputs.worldNormal = normalize(input.normal);\n";
    out << "    inputs.viewDirection =\n";
    out << "        normalize(shadowParams.cameraPosition.xyz - input.worldPosition);\n";
    // **Set 0 renderer membawa jam sekarang.** Sampai R3 medan ini nol, dan
    // akibatnya material yang menganimasikan dirinya diam di viewport sementara
    // pratinjaunya bergerak — dua jawaban untuk satu material, dan yang salah
    // justru yang dipakai menggambar.
    //
    // Jamnya sama dengan yang menggerakkan awan, jadi ia maju menurut delta yang
    // bisa dipaksakan mode ukur: material beranimasi tetap menghasilkan gambar
    // yang sama persis di dua kali jalan.
    out << "    inputs.time = sceneTime();\n\n";

    out << "    MaterialSurface m = evalMaterial(inputs);\n\n";

    if (options.alphaTest) {
        // Sebelum apa pun dihitung — lihat catatan di `ForwardMaterialOptions`.
        // Ambangnya ditanam sebagai literal, bukan dibaca dari blok parameter:
        // ia milik material, bukan milik instance, dan menaruhnya di blok
        // berarti setiap material membayar empat byte untuk angka yang hampir
        // selalu 0,5.
        out << "    if (m.opacity < " << options.alphaCutoff << ") {\n";
        out << "        discard;\n";
        out << "    }\n\n";
    }

    // Mode clay: material diganti seluruhnya, cahayanya tidak disentuh.
    //
    // **Sesudah uji alfa, dan itu bukan kerapian.** Yang menentukan bentuk
    // material bertopeng adalah fragmen yang dibuangnya; mengganti permukaan
    // lebih dulu berarti daun pohon menjadi kuad penuh — dan bayangan yang
    // sedang diperiksa jatuh dari siluet yang salah.
    //
    // Yang diganti bukan hanya `baseColor`. Logam memantulkan lingkungannya
    // alih-alih menaunginya, sorotan spekular terbaca seperti sinar yang jatuh
    // di tempat yang bukan tempatnya, dan peta normal melekukkan cahaya pada
    // permukaan yang sebenarnya datar — ketiganya persis hal yang membuat
    // sebaran bayangan tidak bisa dinilai. `defaults()` sudah menjawab logam,
    // coat, dan fuzz sekaligus; spekular dimatikan lewat bobotnya karena ia
    // bukan lobe pilihan, sama seperti yang dilakukan jalur pratinjau node.
    //
    // `emissive` ikut nol: permukaan yang menyala sendiri adalah permukaan yang
    // tidak bisa dibaca bayangannya.
    out << "    if (clayView()) {\n";
    out << "        m.surface = OpenPBRSurface::defaults();\n";
    out << "        m.surface.baseColor = kClayAlbedo;\n";
    out << "        m.surface.specularWeight = 0.0;\n";
    // Nol berarti "pakai normal geometri" — sentinel yang sama yang dipakai
    // cabang normal map di bawah.
    //
    // **Detail lighting justru mempertahankannya, dan di situlah seluruh
    // bedanya dari clay.** Clay membuang peta normal karena lekuk palsu
    // mengaburkan bentuk bayangan; detail lighting membuang segalanya *kecuali*
    // peta normal, karena yang ditanyakannya persis lekuk itu — datang dari
    // geometri atau dari tekstur. Dua cabang untuk satu pertanyaan yang
    // berlawanan, dengan satu baris yang membedakannya.
    out << "        if (!detailLightingView()) {\n";
    out << "            m.normal = float3(0.0);\n";
    out << "            m.tangent = float3(1.0, 0.0, 0.0);\n";
    out << "        }\n";
    out << "        m.coatNormal = float3(0.0, 0.0, 1.0);\n";
    out << "        m.coatTangent = float3(1.0, 0.0, 0.0);\n";
    out << "        m.emissive = float3(0.0);\n";
    out << "    }\n\n";

    out << "    ShadingFrame frame;\n";
    out << "    frame.normal = inputs.worldNormal;\n";
    out << "    frame.view = inputs.viewDirection;\n";
    out << "    float3 tangent = input.worldTangent.xyz;\n";
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
    out << "                      (input.worldTangent.w < 0.0 ? -1.0 : 1.0);\n\n";

    // Bingkai coat dibangun sebelum peta normal dasar dipasang — lihat catatan
    // yang sama di `AssembleMaterialModule`.
    if (options.lobes.coatFrame) {
        out << "    const ShadingFrame coatFrame = makeCoatFrame(frame, m.coatNormal,\n";
        out << "                                                 m.coatTangent);\n\n";
    }

    out << "    if (dot(m.normal, m.normal) > 1e-8) {\n";
    out << "        const float3 n = normalize(m.normal);\n";
    out << "        frame.normal = normalize(frame.tangent * n.x + frame.bitangent * n.y +\n";
    out << "                                 inputs.worldNormal * n.z);\n";
    out << "    }\n\n";

    if (options.lobes.tangent) {
        out << "    frame.tangent = rotatedTangent(frame, m.tangent);\n";
        out << "    frame.bitangent = cross(frame.normal, frame.tangent) *\n";
        out << "                      (input.worldTangent.w < 0.0 ? -1.0 : 1.0);\n\n";
    }
    const char* const forwardFrames = options.lobes.coatFrame ? "frame, coatFrame" : "frame";

    // Bayangan memakai normal **geometri**, bukan normal peta: bias normalnya
    // menggeser titik sampel keluar permukaan, dan menggesernya menurut normal
    // peta membuat bias itu ikut berlekuk — yang terlihat sebagai acne pada
    // permukaan yang normal map-nya kasar.
    // **Adegan tanpa lampu digambar diffuse-nya apa adanya** — alasan lengkapnya
    // di `box_shading.slang`, dan keduanya harus sepakat supaya ruas bermaterial
    // dan ruas jalur-mundur tidak berbeda perlakuan di adegan yang sama.
    //
    // `baseWeight` ikut karena ia memang pengali albedo di OpenPBR; emissive ikut
    // karena ia tidak menuntut cahaya untuk terlihat.
    // **Mode Unlit masuk lewat cabang yang sama**, dan itu pula yang membuatnya
    // tidak menuntut varian pipeline kedua per material. Lihat `unlitView()` di
    // `cluster_common.slang`.
    // **Sesudah bingkai dibangun, bukan sebelum.** Keduanya membaca
    // `frame.normal`, yaitu normal yang sudah melewati peta normal — dan mode
    // Normal yang menampilkan normal geometri adalah mode yang menjawab
    // pertanyaan yang tidak diajukan siapa pun.
    //
    // Cermin sempurna: logam mulus tanpa warna dasar, dengan peta normalnya
    // dipertahankan. Peta normal justru yang membuat mode ini berguna — ia yang
    // menunjukkan lingkungan tergeser di tempat permukaannya berlekuk.
    out << "    if (reflectionsView()) {\n";
    out << "        m.surface = OpenPBRSurface::defaults();\n";
    out << "        m.surface.baseColor = float3(1.0);\n";
    out << "        m.surface.baseMetalness = 1.0;\n";
    out << "        m.surface.specularRoughness = 0.0;\n";
    out << "        m.emissive = float3(0.0);\n";
    out << "    }\n\n";

    // Kanal permukaan apa adanya. Pada perender deferred ini menuntut G-buffer
    // lebih dulu; di sini nilainya sedang berada di tangan, jadi yang dibutuhkan
    // hanya keluar lebih awal.
    out << "    switch (viewMode()) {\n";
    out << "        case kViewBaseColor:\n";
    out << "            return float4(m.surface.baseColor * m.surface.baseWeight,\n";
    out << "                          m.opacity * input.color.a);\n";
    out << "        case kViewNormal:\n";
    out << "            return float4(frame.normal * 0.5 + 0.5, 1.0);\n";
    out << "        case kViewRoughness:\n";
    out << "            return float4(float3(m.surface.specularRoughness), 1.0);\n";
    out << "        case kViewMetallic:\n";
    out << "            return float4(float3(m.surface.baseMetalness), 1.0);\n";
    out << "        default:\n";
    out << "            break;\n";
    out << "    }\n\n";

    out << "    if (unlitView() || !anyLightInScene()) {\n";
    out << "        return float4(m.surface.baseColor * m.surface.baseWeight + m.emissive,\n";
    out << "                      m.opacity * input.color.a);\n";
    out << "    }\n\n";

    out << "    const float shadow = (input.flags & kFlagReceiveShadows) != 0u\n";
    out << "                             ? sampleShadow(input.worldPosition, inputs.worldNormal)\n";
    out << "                             : 1.0;\n";
    out << "    float3 lit = evaluateOpenPBR(m.surface, frame,\n";
    out << "                                 normalize(shadowParams.lightDirection.xyz),\n";
    out << "                                 shadowParams.sunRadiance.rgb * shadow);\n\n";

    // Lampu cluster dinilai **satu per satu lewat model shading**, bukan
    // dijumlahkan menjadi iradiansi lalu dikalikan albedo. Yang kedua itu
    // pendekatan Lambert yang dipakai `box.frag`, dan ia tidak punya spekular
    // sama sekali — lampu titik di sebelah bola logam tidak menghasilkan sorotan.
    out << "    const float viewDepth = dot(input.worldPosition - shadowParams.cameraPosition.xyz,\n";
    out << "                                shadowParams.cameraForward.xyz);\n";
    out << "    const uint2 lightRange = clusterLightRange(input.position.xy, viewDepth);\n";
    out << "    for (uint i = 0u; i < lightRange.y; ++i) {\n";
    out << "        ClusterLightSample light;\n";
    out << "        if (!clusterLightAt(lightRange, i, input.worldPosition, inputs.worldNormal,\n";
    out << "                            light)) {\n";
    out << "            continue;\n";
    out << "        }\n";
    out << "        lit += evaluateOpenPBR(m.surface, " << forwardFrames
        << ", light.direction, light.radiance);\n";
    out << "    }\n\n";

    // Iradiansi tak-langsung dari langit panggang, sama sumbernya dengan
    // `box.frag`. Yang berbeda: ia dimasukkan lewat model shading alih-alih
    // dikalikan albedo langsung, jadi logam tidak ikut menerima difus.
    //
    // **Konstanta 0,25 yang dulu di sini bukan cuma tetap, ia juga tidak
    // sesatuan dengan cabang di bawahnya.** `giIrradianceAt` mengembalikan
    // iradiansi E, sedangkan 0,25 diperlakukan sebagai E di sini dan sebagai
    // E/π di `box_shading.slang` — dua arti untuk satu angka, di dua berkas.
    // Keduanya sekarang membaca `skyIrradiance`, yang selalu E, dan selisih pi
    // itu hilang bersama konstantanya.
    out << "    float3 irradiance = skyIrradiance(frame.normal);\n";
    // **Kisi probe menggantikan SH panggang di sini juga, bukan hanya di
    // `box_shading.slang`.** Jalur inilah yang menggambar setiap mesh
    // bermaterial — yaitu hampir seluruh adegan — dan menambahkan pembacaan
    // probe hanya di jalur kotak menghasilkan kisi yang tidak pernah dibaca satu
    // piksel pun. Yang terlihat kemudian bukan galat melainkan gambar yang
    // *identik* dengan sebelum kisinya ada, dan itu terbaca sebagai "kisinya
    // benar" alih-alih sebagai "kisinya mati".
    out << "    if (shadowParams.probeCounts.w > 0.5) {\n";
    out << "        irradiance = probeIrradiance(input.worldPosition, frame.normal);\n";
    out << "    }\n";
    // **Lightmap menang untuk permukaan yang punya petak (S5).** Alasannya di
    // `box_shading.slang`; yang penting di sini adalah ia dipasang di **kedua**
    // jalur — lupa yang satu berarti fitur yang mati untuk hampir seluruh
    // adegan, dan gambar yang identik dengan sebelumnya terbaca sebagai
    // "lightmapnya benar" alih-alih "lightmapnya tidak pernah dibaca". Itu
    // persis yang terjadi pada kisi probe di S1.
    out << "    if (input.hasLightmap != 0u) {\n";
    out << "        irradiance = lightmapIrradiance(input.lightmapUv);\n";
    out << "    }\n";
    out << "    if (shadowParams.giParams.x > 0.5) {\n";
    out << "        irradiance = giIrradianceAt(input.position.xy, input.worldPosition,\n";
    out << "                                    frame.normal, shadowParams.giParams.y);\n";
    out << "    }\n";
    // **Prafilter dan DFG datang dari lingkungan panggang (B2).** Sebelum ini
    // ketiganya nol, dan akibatnya logam gelap di luar sorotan langsungnya:
    // suku spekular lingkungan sepenuhnya hilang, dan tidak ada penyetelan
    // material yang bisa mengembalikannya.
    //
    // **Nol tetap jawabannya sebelum ada panggangan yang selesai**, dan itu
    // bukan penambal melainkan yang benar: lingkungan yang belum ada tidak
    // memantulkan apa pun. `prefilteredEnvMips()` yang nol menjaga cubemap
    // pengganti tidak pernah terbaca sebagai pantulan.
    //
    // Coat dibaca pada bingkainya sendiri: lapisan bening punya normal dan
    // kekasaran yang berbeda dari permukaan di bawahnya, dan membacanya pada
    // satu arah membuat pantulan coat mengikuti permukaan yang seharusnya ia
    // tutupi.
    out << "    const float envMips = prefilteredEnvMips();\n";
    out << "    float3 prefilteredBase = float3(0.0);\n";
    out << "    float3 prefilteredCoat = float3(0.0);\n";
    out << "    float2 envDfg = float2(0.0);\n";
    out << "    if (envMips >= 0.5) {\n";
    out << "        prefilteredBase = prefilteredEnv.SampleLevel(\n";
    out << "            environmentDirection(prefilterDirection(frame)),\n";
    out << "            prefilterMipForRoughness(m.surface.specularRoughness, envMips)).rgb;\n";
    // Bingkai coat hanya ada bila materialnya memang punya lapisan itu; tanpa
    // coat, `forwardFrames` cuma "frame" dan overload lain yang dipanggil.
    if (options.lobes.coatFrame) {
        out << "        prefilteredCoat = prefilteredEnv.SampleLevel(\n";
        out << "            environmentDirection(prefilterDirection(coatFrame)),\n";
        out << "            prefilterMipForRoughness(m.surface.coatRoughness, envMips)).rgb;\n";
    }
    out << "        const float envNv = saturate(dot(frame.normal, frame.view));\n";
    out << "        envDfg = dfgLut.SampleLevel(\n";
    out << "            float2(envNv, saturate(m.surface.specularRoughness)), 0.0).rg;\n";
    out << "    }\n";
    // Satu bentuk panggilan untuk keduanya: overload tanpa bingkai coat tetap
    // menerima `prefilteredCoat`, dan ia yang meneruskan `frame` sebagai
    // bingkai coat-nya sendiri.
    out << "    lit += evaluateOpenPBR_IBL(m.surface, " << forwardFrames
        << ", irradiance, prefilteredBase,\n";
    out << "                               prefilteredCoat, envDfg);\n";
    out << "    const float nv = saturate(dot(frame.normal, frame.view));\n";
    out << "    lit += coatedEmission(m.surface, m.emissive, nv);\n";
    out << "    return float4(lit, m.opacity * input.color.a);\n";
    out << "}\n";

    return out.str();
}

CompileRequest MakeForwardMaterialRequest(const std::string& generatedSlang,
                                          const ForwardMaterialOptions& options) {
    CompileRequest request;
    request.source = AssembleForwardMaterialModule(generatedSlang, options);
    request.stage = ShaderStage::Fragment;
    request.entryPoint = options.fragmentEntry.empty() ? std::string("main") : options.fragmentEntry;
    return request;
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
