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
    const std::string entry = options.entryPoint.empty() ? std::string("fragmentMain")
                                                         : options.entryPoint;
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

    out << "// --- keadaan per-frame --------------------------------------------\n";
    out << "cbuffer FrameParams\n{\n";
    out << "    float3 gCameraPosition;\n";
    out << "    float  gTime;\n";
    out << "    float3 gLightDirection;  // dari permukaan ke cahaya\n";
    out << "    float  gAlphaCutoff;\n";
    out << "    float3 gLightRadiance;\n";
    out << "    float  gPad0;\n";
    out << "}\n\n";

    out << "struct MaterialVarying\n{\n";
    out << "    float4 clipPosition   : SV_Position;\n";
    out << "    float3 worldPosition  : TEXCOORD1;\n";
    out << "    float3 worldNormal    : NORMAL;\n";
    out << "    float4 worldTangent   : TANGENT;  // w = arah bitangent\n";
    out << "    float2 uv0            : TEXCOORD0;\n";
    out << "    float4 vertexColor    : COLOR0;\n";
    out << "};\n\n";

    out << "// --- kode material -------------------------------------------------\n";
    out << StripImport(generatedSlang);
    out << "\n";

    out << "// --- entry point ---------------------------------------------------\n";
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
    out << "                                 gLightRadiance);\n";
    out << "    lit += m.emissive;\n";
    out << "    return float4(lit, m.opacity);\n";
    out << "}\n";

    return out.str();
}

CompileRequest MakeMaterialRequest(const std::string& generatedSlang,
                                   const MaterialModuleOptions& options) {
    CompileRequest request;
    request.source = AssembleMaterialModule(generatedSlang, options);
    request.stage = ShaderStage::Fragment;
    request.entryPoint = options.entryPoint.empty() ? std::string("fragmentMain")
                                                    : options.entryPoint;
    return request;
}

}  // namespace sim::material
