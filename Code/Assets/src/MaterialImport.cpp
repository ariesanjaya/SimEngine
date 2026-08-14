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

}  // namespace

material::MaterialInstance MaterialInstanceFromMesh(const MeshMaterial& source,
                                                    const Uuid& parent,
                                                    const TextureResolver& resolveTexture) {
    material::MaterialInstance instance;
    instance.parent = parent;

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

    // Tekstur warna dasar, bila ada dan bila pemanggil bisa menyelesaikannya.
    // Yang tidak terselesaikan **tidak** ditulis sama sekali: parameter yang
    // kosong berarti "pakai bawaan induk", yaitu putih — dan itu persis
    // perilaku material tanpa tekstur.
    if (!source.baseColorTexture.empty() && resolveTexture) {
        const Uuid texture = resolveTexture(source.baseColorTexture);
        if (texture.IsValid()) {
            instance.SetTexture(kBaseColorTextureParameter, texture);
        }
    }
    return instance;
}

bool WriteMaterialInstances(const MeshData& mesh, const std::filesystem::path& folder,
                            const Uuid& parent, std::vector<std::string>& outWritten,
                            std::string& error, const TextureResolver& resolveTexture) {
    outWritten.clear();
    error.clear();
    if (!parent.IsValid()) {
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
                MaterialInstanceFromMesh(source, parent, resolveTexture), folder / name);
        if (!result.ok) {
            error = result.error;
            return false;
        }
        outWritten.push_back(name);
    }
    SIM_INFO("Assets", "{} material instances written to {}", outWritten.size(), folder.string());
    return true;
}

}  // namespace sim::assets
