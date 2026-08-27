#include "Sim/Assets/MaterialImport.h"

#include "Sim/Core/Log.h"

#include <system_error>
#include <unordered_map>

namespace sim::assets {
namespace {

material::MaterialValue Scalar(float value) {
    material::MaterialValue out;
    out.kind = material::ValueKind::Float;
    out.components = {value, 0.0f, 0.0f, 0.0f};
    return out;
}

material::MaterialValue Color(const Vec3& value) {
    material::MaterialValue out;
    out.kind = material::ValueKind::Float3;
    out.components = {value.x, value.y, value.z, 0.0f};
    return out;
}

/// Memasang satu tekstur, dan hanya bila pemanggilnya bisa menyelesaikannya.
///
/// Yang tidak terselesaikan **tidak** ditulis sama sekali: parameter yang kosong
/// berarti "pakai bawaan induk", yaitu putih — dan itu persis perilaku saluran
/// tanpa tekstur, karena induknya mengalikan.
bool BindTexture(material::MaterialInstance& instance, std::string_view parameter,
                 const std::string& path, TextureUsage usage,
                 const TextureResolver& resolveTexture) {
    if (path.empty() || !resolveTexture) {
        return false;
    }
    const Uuid texture = resolveTexture(path, usage);
    if (!texture.IsValid()) {
        return false;
    }
    instance.SetTexture(parameter, texture);
    return true;
}

/// Instance di atas induk OpenPBR: seluruh pin, apa adanya.
///
/// **Semuanya ditulis, juga yang kebetulan sama dengan bawaan induknya** —
/// alasan yang sama dengan jalur datar di bawah, dan di sini lebih penting lagi:
/// material ini datang dari berkas yang menyebut setiap angkanya, jadi
/// `coatWeight` 0 adalah pernyataan "tidak ada coat", bukan medan yang
/// kebetulan tidak disentuh.
material::MaterialInstance OpenPbrInstance(const OpenPbrMaterial& source, const Uuid& parent,
                                           const TextureResolver& resolveTexture) {
    material::MaterialInstance instance;
    instance.parent = parent;

    instance.Set("baseWeight", Scalar(source.baseWeight));
    instance.Set("baseColor", Color(source.baseColor));
    instance.Set("baseMetalness", Scalar(source.baseMetalness));
    instance.Set("baseDiffuseRoughness", Scalar(source.baseDiffuseRoughness));

    instance.Set("specularWeight", Scalar(source.specularWeight));
    instance.Set("specularColor", Color(source.specularColor));
    instance.Set("specularRoughness", Scalar(source.specularRoughness));
    instance.Set("specularRoughnessAnisotropy", Scalar(source.specularRoughnessAnisotropy));
    instance.Set("specularIor", Scalar(source.specularIor));

    instance.Set("coatWeight", Scalar(source.coatWeight));
    instance.Set("coatColor", Color(source.coatColor));
    instance.Set("coatRoughness", Scalar(source.coatRoughness));
    instance.Set("coatRoughnessAnisotropy", Scalar(source.coatRoughnessAnisotropy));
    instance.Set("coatIor", Scalar(source.coatIor));
    instance.Set("coatDarkening", Scalar(source.coatDarkening));

    instance.Set("fuzzWeight", Scalar(source.fuzzWeight));
    instance.Set("fuzzColor", Color(source.fuzzColor));
    instance.Set("fuzzRoughness", Scalar(source.fuzzRoughness));

    instance.Set("emissive", Color(source.emissive));
    instance.Set("opacity", Scalar(source.opacity));

    BindTexture(instance, kOpenPbrBaseColorTexture, source.baseColorTexture.path,
                TextureUsage::Color, resolveTexture);
    BindTexture(instance, kOpenPbrMetalnessTexture, source.baseMetalnessTexture.path,
                TextureUsage::Mask, resolveTexture);
    BindTexture(instance, kOpenPbrRoughnessTexture, source.specularRoughnessTexture.path,
                TextureUsage::Mask, resolveTexture);
    BindTexture(instance, kOpenPbrEmissiveTexture, source.emissiveTexture.path,
                TextureUsage::Color, resolveTexture);
    BindTexture(instance, kOpenPbrOpacityTexture, source.opacityTexture.path, TextureUsage::Mask,
                resolveTexture);

    // **Sakelarnya menyala hanya kalau petanya benar-benar terpasang**, bukan
    // kalau materialnya menyebut satu. Peta yang disebut tapi berkasnya hilang
    // menghasilkan tekstur putih, dan sakelar yang terlanjur menyala mengubah
    // putih itu menjadi normal miring 45° di seluruh permukaan — jauh lebih
    // buruk daripada sekadar kehilangan detailnya.
    const bool normalBound =
        BindTexture(instance, kOpenPbrNormalTexture, source.normalTexture.path,
                    TextureUsage::NormalMap, resolveTexture);
    instance.Set(std::string(kNormalTextureAmountParameter), Scalar(normalBound ? 1.0f : 0.0f));
    return instance;
}

}  // namespace

ImportedMaterialParents DefaultImportedMaterialParents() {
    return {Uuid::Parse(kImportedMaterialGuid), Uuid::Parse(kImportedOpenPbrMaterialGuid)};
}

material::MaterialInstance MaterialInstanceFromMesh(const MeshMaterial& source,
                                                    const ImportedMaterialParents& parents,
                                                    const TextureResolver& resolveTexture) {
    if (source.openPbr) {
        return OpenPbrInstance(*source.openPbr, parents.openPbr, resolveTexture);
    }

    material::MaterialInstance instance;
    instance.parent = parents.flat;

    // **Kelimanya selalu ditimpa, juga yang kebetulan sama dengan bawaan
    // induknya.** Aturan umum instance adalah "yang tidak diubah tidak ditulis",
    // supaya memperbaiki induk mengalir ke bawah. Di sini kebalikannya yang
    // benar: nilai-nilai ini dibaca dari berkas mesh-nya, jadi ia pernyataan
    // tentang material itu — bukan medan yang kebetulan dibiarkan. Membiarkan
    // roughness 0,5 tidak tertulis berarti mengubah bawaan induk diam-diam
    // mengubah setiap material impor yang kebetulan ber-roughness 0,5.
    instance.Set("baseColor", Color(source.baseColor));
    instance.Set("metalness", Scalar(source.metalness));
    instance.Set("roughness", Scalar(source.roughness));
    instance.Set("emissive", Color(source.emissive));
    instance.Set("opacity", Scalar(source.opacity));

    // Induk datar hanya punya satu slot tekstur, dan itu warna dasarnya.
    BindTexture(instance, kBaseColorTextureParameter, source.baseColorTexture,
                TextureUsage::Color, resolveTexture);
    return instance;
}

bool WriteMaterialInstances(const MeshData& mesh, const std::filesystem::path& folder,
                            const ImportedMaterialParents& parents,
                            std::vector<std::string>& outWritten, std::string& error,
                            const TextureResolver& resolveTexture) {
    outWritten.clear();
    error.clear();
    if (!parents.IsValid()) {
        error = "the parent material has no GUID";
        return false;
    }
    if (mesh.materials.empty()) {
        return true;  // Mesh tanpa material bukan kegagalan; tidak ada yang ditulis.
    }

    std::error_code created;
    std::filesystem::create_directories(folder, created);
    if (created) {
        error = created.message();
        return false;
    }

    // Nama yang sudah dipakai, supaya dua material bernama sama tidak saling
    // menimpa. Berkas dari DCC kerap memakai "lambert1" berkali-kali.
    std::unordered_map<std::string, int> used;
    outWritten.reserve(mesh.materials.size());

    for (std::size_t i = 0; i < mesh.materials.size(); ++i) {
        const MeshMaterial& source = mesh.materials[i];
        std::string stem = SafeAssetFileName(source.name);
        if (stem.empty()) {
            stem = "Material " + std::to_string(i);
        }
        const int seen = used[stem]++;
        if (seen > 0) {
            stem += " " + std::to_string(seen + 1);
        }

        const std::string name = stem + ".simmatinst";
        const material::MaterialIoResult result =
            material::SaveInstanceToFile(
                MaterialInstanceFromMesh(source, parents, resolveTexture), folder / name);
        if (!result.ok) {
            error = result.error;
            return false;
        }
        outWritten.push_back(name);
    }
    std::size_t layered = 0;
    for (const MeshMaterial& source : mesh.materials) {
        layered += source.openPbr ? 1 : 0;
    }
    SIM_INFO("Assets", "{} material instances written to {} ({} OpenPBR)", outWritten.size(),
             folder.string(), layered);
    return true;
}

}  // namespace sim::assets
