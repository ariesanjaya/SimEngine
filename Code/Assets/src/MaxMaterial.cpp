#include "MaxMaterial.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace sim::assets {
namespace {

/// Akar blok kustom Max. Anaknya `Parameters`, dan cucunya parameter block
/// material itu sendiri.
constexpr const char* kMaxRoot = "3dsMax";
constexpr const char* kMaxParameters = "3dsMax|Parameters";

Vec3 ToVec3(const FbxDouble3& value) {
    return Vec3(static_cast<float>(value[0]), static_cast<float>(value[1]),
                static_cast<float>(value[2]));
}

FbxProperty Parameter(const FbxSurfaceMaterial& source, std::string_view name) {
    const std::string full = std::string(kMaxParameters) + "|" + std::string(name);
    return source.FindPropertyHierarchical(full.c_str());
}

/// Nilai skalar sebuah properti, apa pun bentuk simpannya.
///
/// **Tipenya diperiksa, bukan diandaikan.** `FbxProperty::Get<FbxDouble>()`
/// pada properti bertipe lain mengembalikan nol tanpa berkata apa-apa — dan nol
/// adalah nilai yang sah untuk hampir setiap parameter di sini, jadi
/// kekeliruannya tidak akan pernah terlihat sebagai kekeliruan.
bool Scalar(const FbxProperty& property, float& out) {
    if (!property.IsValid()) {
        return false;
    }
    switch (property.GetPropertyDataType().GetType()) {
        case eFbxFloat:
            out = property.Get<FbxFloat>();
            return true;
        case eFbxDouble:
            out = static_cast<float>(property.Get<FbxDouble>());
            return true;
        case eFbxInt:
            out = static_cast<float>(property.Get<FbxInt>());
            return true;
        case eFbxBool:
            out = property.Get<FbxBool>() ? 1.0f : 0.0f;
            return true;
        default:
            return false;
    }
}

bool Color(const FbxProperty& property, Vec3& out) {
    if (!property.IsValid()) {
        return false;
    }
    switch (property.GetPropertyDataType().GetType()) {
        case eFbxDouble3:
            out = ToVec3(property.Get<FbxDouble3>());
            return true;
        case eFbxDouble4: {
            // Warna beralfa — Max menulis sebagian warnanya begini. Alfanya
            // bukan opacity material dan sengaja tidak ikut.
            const FbxDouble4 value = property.Get<FbxDouble4>();
            out = Vec3(static_cast<float>(value[0]), static_cast<float>(value[1]),
                       static_cast<float>(value[2]));
            return true;
        }
        default:
            return false;
    }
}

bool ScalarParameter(const FbxSurfaceMaterial& source, std::string_view name, float& out) {
    return Scalar(Parameter(source, name), out);
}

bool ColorParameter(const FbxSurfaceMaterial& source, std::string_view name, Vec3& out) {
    return Color(Parameter(source, name), out);
}

/// Jalur tekstur yang tersambung ke sebuah parameter Max, dinormalkan ke `/`.
///
/// Kosong bila tidak ada — dan pemanggilnya lalu memakai jalur yang sudah
/// dibaca dari slot FBX baku, yang untuk sebagian besar berkas berisi peta yang
/// sama.
std::string MapPath(const FbxSurfaceMaterial& source, std::string_view name) {
    const FbxProperty property = Parameter(source, name);
    if (!property.IsValid()) {
        return {};
    }
    const FbxFileTexture* texture = property.GetSrcObject<FbxFileTexture>(0);
    if (texture == nullptr) {
        return {};
    }
    std::string relative =
        texture->GetRelativeFileName() != nullptr ? texture->GetRelativeFileName() : "";
    if (relative.empty() && texture->GetFileName() != nullptr) {
        relative = std::filesystem::path(texture->GetFileName()).filename().string();
    }
    std::replace(relative.begin(), relative.end(), '\\', '/');
    return relative;
}

/// Memasang satu peta: yang disebut blok Max menang, dan yang sudah terbaca
/// dari slot FBX baku menjadi cadangannya.
void BindTexture(const FbxSurfaceMaterial& source, std::string_view mapParameter,
                 const std::string& standardSlot, OpenPbrTexture& target, float& scalar) {
    std::string path = MapPath(source, mapParameter);
    if (path.empty()) {
        path = standardSlot;
    }
    if (path.empty()) {
        return;
    }
    target.path = std::move(path);
    // Alasannya sama dengan di pembaca `.mtlx`: induknya mengalikan skalar
    // dengan teksturnya, jadi skalar yang tidak dikembalikan ke identitas
    // meredam gambarnya.
    scalar = 1.0f;
}

void BindColorTexture(const FbxSurfaceMaterial& source, std::string_view mapParameter,
                      const std::string& standardSlot, OpenPbrTexture& target, Vec3& color) {
    std::string path = MapPath(source, mapParameter);
    if (path.empty()) {
        path = standardSlot;
    }
    if (path.empty()) {
        return;
    }
    target.path = std::move(path);
    color = Vec3(1.0f);
}

/// Membaca blok yang memakai **nama input OpenPBR**.
///
/// Inilah bentuk yang ditulis material OpenPBR Surface, dan pemetaannya
/// satu-ke-satu sepenuhnya — itu memang seluruh alasan mesin ini memilih
/// OpenPBR. Tidak ada satu pun besaran yang perlu dikonversi di sini; yang ada
/// hanya ejaan yang berbeda (`base_color` versus `baseColor`).
void ReadOpenPbrBlock(const FbxSurfaceMaterial& source, OpenPbrMaterial& material,
                      float& emissionScale) {
    ScalarParameter(source, "base_weight", material.baseWeight);
    ColorParameter(source, "base_color", material.baseColor);
    ScalarParameter(source, "base_metalness", material.baseMetalness);
    ScalarParameter(source, "base_diffuse_roughness", material.baseDiffuseRoughness);

    ScalarParameter(source, "specular_weight", material.specularWeight);
    ColorParameter(source, "specular_color", material.specularColor);
    ScalarParameter(source, "specular_roughness", material.specularRoughness);
    ScalarParameter(source, "specular_roughness_anisotropy",
                    material.specularRoughnessAnisotropy);
    ScalarParameter(source, "specular_ior", material.specularIor);

    ScalarParameter(source, "coat_weight", material.coatWeight);
    ColorParameter(source, "coat_color", material.coatColor);
    ScalarParameter(source, "coat_roughness", material.coatRoughness);
    ScalarParameter(source, "coat_roughness_anisotropy", material.coatRoughnessAnisotropy);
    ScalarParameter(source, "coat_ior", material.coatIor);
    ScalarParameter(source, "coat_darkening", material.coatDarkening);

    ScalarParameter(source, "fuzz_weight", material.fuzzWeight);
    ColorParameter(source, "fuzz_color", material.fuzzColor);
    ScalarParameter(source, "fuzz_roughness", material.fuzzRoughness);

    ScalarParameter(source, "emission_luminance", emissionScale);
    Vec3 emissionColor(1.0f);
    ColorParameter(source, "emission_color", emissionColor);
    material.emissive = emissionColor * emissionScale;

    ScalarParameter(source, "geometry_opacity", material.opacity);
}

/// Membaca blok yang memakai **nama parameter Physical Material**.
///
/// Di sini pemetaannya bukan lagi ejaan melainkan terjemahan, dan tiga hal
/// tidak boleh diikutkan begitu saja:
///
/// - **`anisotropy` tidak dipetakan, dan itu keputusan yang diambil sesudah
///   melihat berkasnya.** Ekspor Sponza dari Max menulis `anisotropy = 0` pada
///   seluruh 28 materialnya sementara `anisoangle` tetap 0,25 di semuanya, dan
///   di sebelahnya ada `aniso_mode` serta `aniso_channel` yang ikut menentukan
///   artinya. Nol memang sejajar dengan nol OpenPBR, tetapi skala nilai yang
///   bukan nol tidak bisa disimpulkan dari itu. Dua arah kesalahannya tidak
///   setara: tidak memetakannya membuat material anisotropik masuk sebagai
///   isotropik — satu efek yang hilang; memetakannya dengan skala yang salah
///   membuat material isotropik keluar anisotropik — setiap pantulan di adegan
///   memanjang. Yang kedua jauh lebih mahal, jadi ia menunggu sebuah berkas
///   yang benar-benar memakainya.
/// - **`roughness_inv` menentukan arti `roughness`.** Bila menyala, angka itu
///   glossiness, dan yang halus menjadi kasar tanpa itu.
/// - **`trans_ior` adalah IOR materialnya**, bukan parameter transmisi belaka —
///   Physical Material memakai satu IOR untuk seluruh permukaan.
void ReadPhysicalBlock(const FbxSurfaceMaterial& source, OpenPbrMaterial& material,
                       float& emissionScale) {
    ScalarParameter(source, "base_weight", material.baseWeight);
    ColorParameter(source, "base_color", material.baseColor);
    ScalarParameter(source, "metalness", material.baseMetalness);
    ScalarParameter(source, "diff_roughness", material.baseDiffuseRoughness);

    ScalarParameter(source, "reflectivity", material.specularWeight);
    ColorParameter(source, "refl_color", material.specularColor);
    if (ScalarParameter(source, "roughness", material.specularRoughness)) {
        float inverted = 0.0f;
        if (ScalarParameter(source, "roughness_inv", inverted) && inverted != 0.0f) {
            material.specularRoughness = 1.0f - material.specularRoughness;
        }
    }
    ScalarParameter(source, "trans_ior", material.specularIor);

    ScalarParameter(source, "coating", material.coatWeight);
    ColorParameter(source, "coat_color", material.coatColor);
    if (ScalarParameter(source, "coat_roughness", material.coatRoughness)) {
        float inverted = 0.0f;
        if (ScalarParameter(source, "coat_roughness_inv", inverted) && inverted != 0.0f) {
            material.coatRoughness = 1.0f - material.coatRoughness;
        }
    }
    ScalarParameter(source, "coat_ior", material.coatIor);

    // Sheen Physical Material adalah fuzz OpenPBR: satu lobe, satu makna, dua
    // nama. Diperiksa terhadap ekspor nyata — `sheen`, `sheen_color`, dan
    // `sheen_roughness` memang yang tertulis di dalam berkasnya.
    ScalarParameter(source, "sheen", material.fuzzWeight);
    ColorParameter(source, "sheen_color", material.fuzzColor);
    ScalarParameter(source, "sheen_roughness", material.fuzzRoughness);

    // **Emisi Physical Material: bobot × warna × luminansi (cd/m²), dan
    // bobotnya bawaan 1.** Ekspor nyata menulis `emission = 1` bahkan untuk
    // dinding batu; yang membuatnya gelap adalah `emit_color` hitam. Memakai
    // bobot itu sendirian sebagai emisi menyalakan seluruh adegan.
    float weight = 0.0f;
    float luminance = 0.0f;
    Vec3 emissionColor(1.0f);
    ScalarParameter(source, "emission", weight);
    ScalarParameter(source, "emit_luminance", luminance);
    ColorParameter(source, "emit_color", emissionColor);
    emissionScale = weight * luminance;
    material.emissive = emissionColor * emissionScale;

    // Transparansi, bukan opacity: keduanya kebalikan satu sama lain, dan
    // menyalinnya tanpa membalik menjadikan setiap material buram sepenuhnya
    // tembus pandang.
    float transparency = 0.0f;
    if (ScalarParameter(source, "transparency", transparency)) {
        material.opacity = std::clamp(1.0f - transparency, 0.0f, 1.0f);
    }
}

}  // namespace

bool ReadMaxMaterial(const FbxSurfaceMaterial& source, MeshMaterial& material) {
    if (!source.FindProperty(kMaxRoot).IsValid()) {
        return false;
    }

    // **Max menyebutkan sendiri material apa yang diekspornya.** Ekspor nyata
    // membawa `3dsMax|ORIGINAL_MTL = "PHYSICAL_MTL"` di sebelah blok
    // parameternya, dan itu jawaban yang jauh lebih baik daripada menebak dari
    // nama parameter — nama boleh bertambah di rilis berikutnya, pernyataan
    // eksplisit tidak berubah artinya.
    //
    // Tebakan tetap ada sebagai cadangan, untuk versi Max yang tidak menulis
    // baris itu. Yang membedakan keduanya `base_metalness`: ia ada di OpenPBR
    // Surface dan tidak ada di Physical Material, yang menyebutnya `metalness`.
    std::string original;
    if (const FbxProperty declared = source.FindPropertyHierarchical("3dsMax|ORIGINAL_MTL");
        declared.IsValid() && declared.GetPropertyDataType().GetType() == eFbxString) {
        original = declared.Get<FbxString>().Buffer();
        std::transform(original.begin(), original.end(), original.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    }

    float probe = 0.0f;
    bool openPbrNames = original.find("OPENPBR") != std::string::npos;
    bool physicalNames = original.find("PHYSICAL") != std::string::npos;
    if (!openPbrNames && !physicalNames) {
        openPbrNames = ScalarParameter(source, "base_metalness", probe) ||
                       ScalarParameter(source, "specular_roughness", probe);
        physicalNames = ScalarParameter(source, "metalness", probe) ||
                        ScalarParameter(source, "roughness", probe);
    }
    if (!openPbrNames && !physicalNames) {
        // Blok Max ada, tapi bukan salah satu dari dua material itu — Standard
        // lama, sebuah shader pihak ketiga, atau material yang memang tidak
        // membawa parameter apa pun. Jalur Phong tetap yang berlaku.
        return false;
    }

    OpenPbrMaterial parsed;
    parsed.name = material.name;
    float emissionScale = 0.0f;
    if (openPbrNames) {
        ReadOpenPbrBlock(source, parsed, emissionScale);
    } else {
        ReadPhysicalBlock(source, parsed, emissionScale);
    }

    // Tekstur: nama slot peta di blok Max, dengan slot FBX baku sebagai
    // cadangan. `*_map` adalah konvensi parameter block Max — peta warna dasar
    // tinggal di `base_color_map`, di sebelah `base_color`.
    BindColorTexture(source, "base_color_map", material.baseColorTexture,
                     parsed.baseColorTexture, parsed.baseColor);
    BindTexture(source, openPbrNames ? "specular_roughness_map" : "roughness_map",
                material.roughnessTexture, parsed.specularRoughnessTexture,
                parsed.specularRoughness);
    BindTexture(source, openPbrNames ? "base_metalness_map" : "metalness_map",
                material.metalnessTexture, parsed.baseMetalnessTexture, parsed.baseMetalness);
    // **Peta emisi memakai skalanya, bukan putih.** Saluran lain identitasnya
    // 1, tapi emisi Physical Material sudah memuat luminansi 1500 cd/m² di
    // dalam skalanya — menggantinya dengan putih membuat lampu yang bertekstur
    // 1500 kali lebih redup daripada lampu yang tidak.
    std::string emissivePath =
        MapPath(source, openPbrNames ? "emission_color_map" : "emit_color_map");
    if (emissivePath.empty()) {
        emissivePath = material.emissiveTexture;
    }
    if (!emissivePath.empty()) {
        parsed.emissiveTexture.path = std::move(emissivePath);
        parsed.emissive = Vec3(emissionScale);
    }

    float opacityScalar = parsed.opacity;
    BindTexture(source, openPbrNames ? "geometry_opacity_map" : "cutout_map", std::string{},
                parsed.opacityTexture, opacityScalar);
    parsed.opacity = opacityScalar;

    // Peta normal: `bump_map` di Physical Material, `geometry_normal_map` di
    // OpenPBR. Slot `NormalMap`/`Bump` FBX baku menjadi cadangannya, dan itu
    // yang sungguh terisi di sebagian besar ekspor.
    std::string normal = MapPath(source, openPbrNames ? "geometry_normal_map" : "bump_map");
    if (normal.empty()) {
        normal = material.normalTexture;
    }
    parsed.normalTexture.path = std::move(normal);

    // **Lapisan yang mesin ini belum punya, disebut namanya.** Transmission,
    // subsurface, dan thin film ada di Physical Material maupun OpenPBR dan
    // tidak ada di `openpbr.slang`, jadi material kaca dari Max masuk sebagai
    // permukaan buram. Itu batas yang sah; yang tidak sah adalah membiarkannya
    // lewat tanpa suara, karena yang mengimpornya lalu mencari sebabnya di
    // pencahayaan dan di eksportirnya.
    struct Unsupported {
        const char* openPbr;
        const char* physical;
    };
    static constexpr Unsupported kUnsupported[] = {
        {"transmission_weight", "transparency"},
        {"subsurface_weight", "scattering"},
        {"thin_film_weight", "thin_film"},
    };
    for (const Unsupported& layer : kUnsupported) {
        float weight = 0.0f;
        if (ScalarParameter(source, openPbrNames ? layer.openPbr : layer.physical, weight) &&
            weight != 0.0f) {
            SIM_WARN("Assets",
                     "material \"{}\": {} = {} in the 3ds Max block, and that layer is not in "
                     "openpbr.slang — the material comes in without it",
                     material.name, openPbrNames ? layer.openPbr : layer.physical, weight);
        }
    }

    material.openPbr = std::move(parsed);
    return true;
}

void CollectMaterialXPaths(const FbxSurfaceMaterial& source, std::vector<std::string>& out) {
    const auto consider = [&out](const FbxProperty& property) {
        if (!property.IsValid() || property.GetPropertyDataType().GetType() != eFbxString) {
            return;
        }
        const FbxString value = property.Get<FbxString>();
        if (value.GetLen() < 5) {
            return;
        }
        std::string path(value.Buffer(), static_cast<std::size_t>(value.GetLen()));
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.size() < 5 || lower.compare(lower.size() - 5, 5, ".mtlx") != 0) {
            return;
        }
        std::replace(path.begin(), path.end(), '\\', '/');
        if (std::find(out.begin(), out.end(), path) == out.end()) {
            out.push_back(std::move(path));
        }
    };

    // **`GetNextProperty` sudah menelusuri yang bersarang.** Menambahkan
    // penelusuran keturunan di atasnya menghasilkan setiap properti bersarang
    // tiga kali — sekali dari akar, sekali dari `3dsMax`, sekali dari
    // `3dsMax|Parameters`. Yang terlihat bukan galat melainkan daftar yang tiga
    // kali lebih panjang, dan itu terlihat justru karena daftarnya dicetak.
    for (FbxProperty property = source.GetFirstProperty(); property.IsValid();
         property = source.GetNextProperty(property)) {
        consider(property);
    }
}

std::vector<std::string> DescribeMaterialProperties(const FbxSurfaceMaterial& source) {
    std::vector<std::string> lines;

    const auto describe = [&lines](const FbxProperty& property) {
        if (!property.IsValid()) {
            return;
        }
        std::string line = property.GetHierarchicalName().Buffer();
        line += " : ";
        line += property.GetPropertyDataType().GetName();
        line += " = ";
        float scalar = 0.0f;
        Vec3 color(0.0f);
        if (Color(property, color)) {
            line += "(" + std::to_string(color.x) + ", " + std::to_string(color.y) + ", " +
                    std::to_string(color.z) + ")";
        } else if (Scalar(property, scalar)) {
            line += std::to_string(scalar);
        } else if (property.GetPropertyDataType().GetType() == eFbxString) {
            line += "\"";
            line += property.Get<FbxString>().Buffer();
            line += "\"";
        } else {
            line += "<tidak dibaca>";
        }
        // Tekstur yang tersambung ke properti itu ikut disebut: pertanyaan
        // "peta ini nyangkut di parameter yang mana" adalah separuh dari apa
        // yang dicari orang saat membuka daftar ini.
        if (const FbxFileTexture* texture = property.GetSrcObject<FbxFileTexture>(0);
            texture != nullptr) {
            line += "  <- ";
            line += texture->GetRelativeFileName() != nullptr ? texture->GetRelativeFileName()
                                                              : texture->GetFileName();
        }
        lines.push_back(std::move(line));
    };

    // Bersarang pun ikut; lihat catatan di `CollectMaterialXPaths`.
    for (FbxProperty property = source.GetFirstProperty(); property.IsValid();
         property = source.GetNextProperty(property)) {
        describe(property);
    }
    return lines;
}

}  // namespace sim::assets
