#include "Sim/Editor/SourceImport.h"

#include "Sim/Animation/AnimationIo.h"
#include "Sim/Animation/ClipImport.h"
#include "Sim/Animation/Skeleton.h"
#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Assets/MaterialImport.h"
#include "Sim/Assets/MeshData.h"
#include "Sim/Core/Log.h"

#include <algorithm>
#include <system_error>

namespace sim::editor {
namespace {

bool ToSkeleton(const assets::SkeletonData& source, animation::Skeleton& out) {
    std::vector<animation::Bone> bones;
    bones.reserve(source.bones.size());
    for (const assets::SkeletonBone& bone : source.bones) {
        animation::Bone converted;
        converted.name = bone.name;
        converted.parent = bone.parent;
        converted.bind.translation = bone.translation;
        converted.bind.rotation = bone.rotation;
        converted.bind.scale = bone.scale;
        bones.push_back(std::move(converted));
    }
    return out.SetBones(bones);
}

/// Jalur yang belum dipakai, dengan akhiran nomor bila perlu.
std::filesystem::path FreePath(const std::filesystem::path& folder, const std::string& stem,
                               const std::string& extension) {
    std::error_code error;
    std::filesystem::path candidate = folder / (stem + extension);
    int suffix = 1;
    while (std::filesystem::exists(candidate, error)) {
        candidate = folder / (stem + " (" + std::to_string(++suffix) + ")" + extension);
    }
    return candidate;
}

}  // namespace

SourceContents InspectSource(const std::filesystem::path& path) {
    SourceContents contents;
    std::error_code exists;
    if (path.empty() || !std::filesystem::exists(path, exists)) {
        contents.error = "file not found";
        return contents;
    }

    std::string error;
    const assets::MeshData mesh = assets::LoadMesh(path, error);
    contents.hasMesh = mesh.IsValid();
    contents.boneCount = static_cast<int>(mesh.skeleton.bones.size());
    contents.materialCount = static_cast<int>(mesh.materials.size());
    if (!contents.hasMesh) {
        // Berkas yang tidak punya geometri belum tentu rusak — berkas Mixamo
        // yang hanya berisi animasi memang begitu. Galatnya baru dilaporkan
        // kalau tidak ada satu pun bagian yang bisa dibaca.
        contents.error = error;
    }

    // Ketiga format ditanya, bukan hanya FBX: `ImportClips` memilih pembacanya
    // sendiri menurut ekstensi, dan berkas yang tidak punya animasi menjawab
    // dengan daftar kosong — bukan galat.
    std::string clipError;
    contents.clipCount = static_cast<int>(animation::ImportClips(path, clipError).size());
    if (contents.hasMesh || contents.boneCount > 0 || contents.clipCount > 0) {
        contents.error.clear();
    }
    return contents;
}

SourceImportResult ImportSource(const std::filesystem::path& source,
                                const std::filesystem::path& destinationFolder,
                                const SourceImportOptions& options) {
    SourceImportResult result;
    std::error_code exists;
    if (source.empty() || !std::filesystem::exists(source, exists)) {
        result.error = "file not found";
        return result;
    }

    std::error_code created;
    std::filesystem::create_directories(destinationFolder, created);
    if (created) {
        result.error = created.message();
        return result;
    }

    // Nama sumbernya disaring lebih dulu: ia datang dari berkas yang dibuat
    // orang lain, dan yang dipakai di sini menjadi nama berkas.
    std::string stem = assets::SafeAssetFileName(source.stem().string());
    if (stem.empty()) {
        stem = "Imported";
    }

    // Diurai sekali, dipakai tiga kali. Rig Mixamo sebelas megabyte, dan
    // membacanya ulang untuk rangka lalu untuk material adalah dua kali kerja
    // yang sama.
    std::string meshError;
    const assets::MeshData mesh =
        (options.mesh || options.skeleton || options.materials)
            ? assets::LoadMesh(source, meshError)
            : assets::MeshData{};

    if (options.mesh) {
        const std::filesystem::path destination =
            FreePath(destinationFolder, stem, source.extension().string());
        std::error_code copyError;
        std::filesystem::copy_file(source, destination, copyError);
        if (copyError) {
            result.error = copyError.message();
            return result;
        }
        result.written.push_back(destination.filename().string());
    }

    if (options.skeleton && !mesh.skeleton.bones.empty()) {
        animation::Skeleton skeleton;
        if (ToSkeleton(mesh.skeleton, skeleton)) {
            animation::SkeletonDocument document;
            document.name = stem;
            const std::filesystem::path destination = FreePath(destinationFolder, stem, ".simskel");
            const animation::AnimationIoResult saved =
                animation::SaveSkeleton(skeleton, document, destination);
            if (!saved.ok) {
                result.error = saved.error;
                return result;
            }
            result.written.push_back(destination.filename().string());
        } else {
            // Urutan bone yang tidak topologis dibuang oleh importirnya, jadi
            // sampai di sini artinya rangkanya memang tidak bisa dipakai. Itu
            // bukan alasan membatalkan impor bagian lainnya.
            SIM_WARN("Editor", "{}: bone order is not topological; the skeleton is skipped",
                     source.string());
        }
    }

    if (options.animation) {
        std::string clipError;
        const std::vector<animation::Clip> clips = animation::ImportClips(source, clipError);
        for (const animation::Clip& clip : clips) {
            // **Dinamai `<berkas> - <klip>`, bukan nama klipnya saja.** Dua
            // berkas Mixamo yang berbeda sama-sama menghasilkan klip bernama
            // menurut berkasnya, dan dua take di satu berkas bernama menurut
            // take-nya; awalan nama berkas membuat keduanya tidak pernah bentrok
            // dan tetap berdampingan saat diurutkan.
            std::string clipStem = assets::SafeAssetFileName(clip.name);
            const std::string name =
                clipStem.empty() || clipStem == stem ? stem : stem + " - " + clipStem;
            animation::ClipDocument document;
            const std::filesystem::path destination = FreePath(destinationFolder, name, ".simanim");
            const animation::AnimationIoResult saved =
                animation::SaveClip(clip, document, destination);
            if (!saved.ok) {
                result.error = saved.error;
                return result;
            }
            result.written.push_back(destination.filename().string());
        }
    }

    if (options.materials && !mesh.materials.empty()) {
        std::vector<std::string> materials;
        std::string materialError;
        const Uuid parent = Uuid::Parse(assets::kImportedMaterialGuid);

        // Tekstur disalin ke dalam project, bukan dirujuk di tempat asalnya.
        //
        // Jalur di dalam berkas mesh relatif terhadap berkas itu dan kerap naik
        // satu tingkat — `..\checkerA.tga` di sebelah `shaderBall.fbx` — jadi
        // merujuknya apa adanya berarti aset yang menunjuk ke luar project, dan
        // hilang begitu project dipindahkan atau dibagikan.
        //
        // GUID-nya dibuat di sini lewat `EnsureAssetGuid`, bukan ditunggu dari
        // pemindaian berikutnya: material yang merujuknya sedang ditulis
        // sekarang.
        std::unordered_map<std::string, Uuid> copied;
        const auto resolveTexture = [&](std::string_view relativePath) -> Uuid {
            const std::string key(relativePath);
            if (const auto found = copied.find(key); found != copied.end()) {
                // Dua material yang memakai tekstur yang sama menyalinnya sekali.
                return found->second;
            }

            std::error_code code;
            const std::filesystem::path from =
                std::filesystem::weakly_canonical(source.parent_path() / relativePath, code);
            if (code || !std::filesystem::is_regular_file(from, code)) {
                SIM_WARN("Editor", "texture {} referenced by {} is missing", key,
                         source.filename().string());
                copied.emplace(key, Uuid{});
                return Uuid{};
            }

            // Namanya diambil dari berkasnya saja: `../` di dalam jalur tidak
            // boleh menjadi folder di dalam project, dan nama yang bentrok
            // diberi akhiran nomor alih-alih menimpa.
            const std::filesystem::path destination =
                FreePath(destinationFolder, assets::SafeAssetFileName(from.stem().string()),
                         from.extension().string());
            std::filesystem::copy_file(from, destination,
                                       std::filesystem::copy_options::overwrite_existing, code);
            if (code) {
                SIM_WARN("Editor", "cannot copy texture {}: {}", key, code.message());
                copied.emplace(key, Uuid{});
                return Uuid{};
            }
            result.written.push_back(destination.filename().string());
            const Uuid guid = assets::EnsureAssetGuid(destination);
            copied.emplace(key, guid);
            return guid;
        };

        if (!assets::WriteMaterialInstances(mesh, destinationFolder, parent, materials,
                                            materialError, resolveTexture)) {
            result.error = materialError;
            return result;
        }
        result.written.insert(result.written.end(), materials.begin(), materials.end());
    }

    if (result.written.empty()) {
        // Tidak ada yang ditulis berarti tidak ada satu pun bagian yang diminta
        // ada di dalam berkasnya. Itu kegagalan yang harus terlihat, bukan
        // keberhasilan yang tidak menghasilkan apa-apa.
        result.error = meshError.empty() ? "nothing in this file matches what was asked for"
                                         : meshError;
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace sim::editor
