// Panel Mesh Editor.
//
// Membuka sebuah aset mesh, memperlihatkan rangka dan mesh ber-skin dalam 3D
// sungguhan, dan menandai ruas mana yang kain.
//
// **Instance KEDUA `IViewportRenderer`, bukan perender baru.** Ini yang membuat
// panel ini murah, dan ini pula yang dulu ditolak untuk pratinjau material — di
// sana `ViewportScene` tidak punya tempat untuk sebuah material, jadi instance
// kedua hanya akan menggambar bola abu-abu. Untuk mesh justru sebaliknya:
// `ViewportScene` sudah persis bentuk yang dibutuhkan. Mesh-nya satu
// `MeshInstance` — warna per ruasnya mengalir lewat `partColors` yang sudah ada,
// jadi karakter berlima material tergambar berlima warna tanpa satu baris pun
// kode baru di perender — dan rangkanya `LineSegment`, lewat pass garis yang
// juga sudah ada.

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Assets/MeshData.h"
#include "Sim/Assets/MeshSettings.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Render/IViewportRenderer.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace sim::editor {
namespace {

constexpr Vec4 kBoneColor{0.45f, 0.78f, 1.00f, 1.0f};
constexpr Vec4 kSelectedBoneColor{1.00f, 0.62f, 0.20f, 1.0f};
constexpr Vec4 kClothColor{0.35f, 0.90f, 0.55f, 1.0f};

/// Kamera orbit sederhana: yaw, pitch, dan jarak terhadap sebuah titik pusat.
struct OrbitCamera {
    float yaw = 0.6f;
    float pitch = 0.25f;
    float distance = 3.0f;
    Vec3 target{0.0f, 1.0f, 0.0f};

    /// Membingkai sebuah kotak batas: pusatnya menjadi target, jaraknya diambil
    /// dari diagonalnya. Model setinggi 1,8 m dan model setinggi 180 m harus
    /// sama-sama muat tanpa disetel tangan.
    void Frame(const Vec3& boundsMin, const Vec3& boundsMax) {
        target = (boundsMin + boundsMax) * 0.5f;
        const float radius = glm::length(boundsMax - boundsMin) * 0.5f;
        distance = std::max(radius * 2.4f, 0.1f);
    }

    render::Camera ToCamera() const {
        const float clampedPitch = std::clamp(pitch, -1.5f, 1.5f);
        const Vec3 offset(std::cos(clampedPitch) * std::sin(yaw), std::sin(clampedPitch),
                          std::cos(clampedPitch) * std::cos(yaw));
        render::Camera camera;
        camera.position = target + offset * distance;
        // Rotasi diturunkan dari arah pandang, bukan disimpan terpisah: dua
        // sumber yang harus sepakat adalah dua sumber yang suatu saat tidak.
        // `lookAt` menghasilkan matriks pandang — kebalikannya yang menjadi
        // orientasi kamera.
        const Mat4 view = glm::lookAt(camera.position, target, Vec3(0.0f, 1.0f, 0.0f));
        camera.rotation = glm::quat_cast(glm::transpose(Mat3(view)));
        camera.nearZ = std::max(distance * 0.01f, 0.01f);
        camera.farZ = std::max(distance * 20.0f, 50.0f);
        return camera;
    }
};

class MeshEditorPanel final : public Panel {
public:
    MeshEditorPanel()
        : Panel(panel_id::kMeshEditor, std::string(icons::kMesh) + "  Mesh Editor",
                PanelCategory::Authoring) {}

    bool OpenAsset(const Uuid& guid, EditorContext& context) override {
        if (context.assets == nullptr) {
            return false;
        }
        const assets::AssetRecord* record = context.assets->Find(guid);
        if (record == nullptr || record->type != assets::AssetType::Mesh) {
            return false;
        }
        Open(context, *record);
        return true;
    }

    void OnDraw(EditorContext& context) override {
        render::IViewportRenderer* preview = context.meshPreview;
        if (context.assets == nullptr) {
            ImGui::TextDisabled("No project open.");
            return;
        }

        DrawHeader(context);
        ImGui::Separator();

        if (meshPath_.empty()) {
            // **Tidak ada daftar aset di sini.** Yang menelusuri aset adalah
            // Asset Browser; editor yang punya daftarnya sendiri adalah daftar
            // kedua yang harus dijaga sepakat dengan yang pertama — dan dua
            // tempat untuk menemukan berkas yang sama membuat keduanya
            // setengah dipakai.
            ImGui::TextDisabled("Klik ganda sebuah aset mesh di Asset Browser untuk membukanya.");
            return;
        }
        if (!mesh_.IsValid()) {
            ImGui::TextWrapped("Tidak bisa membaca %s: %s", meshPath_.c_str(), loadError_.c_str());
            return;
        }

        const float available = ImGui::GetContentRegionAvail().x;
        const float sideWidth = std::clamp(available * 0.30f, 220.0f, 380.0f);

        ImGui::BeginChild("##meshview", ImVec2(-sideWidth, 0.0f));
        DrawViewport(context, preview);
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("##meshside", ImVec2(0.0f, 0.0f),
                          ImGuiChildFlags_AlwaysUseWindowPadding);
        DrawInfo();
        DrawParts(context);
        DrawComponentDefaults(context);
        DrawBones();
        ImGui::EndChild();
    }

private:
    void DrawHeader(EditorContext& context) {
        if (meshPath_.empty()) {
            ImGui::TextDisabled("Mesh Editor");
            return;
        }
        ImGui::Text("%s", assetName_.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Muat ulang")) {
            Reload(context);
        }

        // Hanya glTF yang menanam teksturnya; FBX menunjuk berkas di sebelahnya,
        // jadi tombolnya tidak punya arti di sana.
        const bool gltf = meshPath_.size() > 4 &&
                          (meshPath_.rfind(".glb") == meshPath_.size() - 4 ||
                           meshPath_.rfind(".gltf") == meshPath_.size() - 5);
        if (gltf) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Keluarkan tekstur")) {
                ExtractTextures(context);
            }
        }
    }

    /// Menulis tekstur yang tertanam menjadi berkas, di folder yang sama dengan
    /// mesh-nya. **Dipanggil dengan sengaja, bukan saat mesh dimuat** — yang
    /// memuat mesh adalah perender, di jalur daftar gambar.
    void ExtractTextures(EditorContext& context) {
        std::vector<std::string> written;
        std::string error;
        const std::filesystem::path folder = std::filesystem::path(meshPath_).parent_path();
        if (!assets::ExtractGltfTextures(meshPath_, folder, written, error)) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Tidak bisa mengeluarkan tekstur: " + error);
            }
            return;
        }
        context.assets->ScanNow();
        if (context.notifications != nullptr) {
            context.notifications->Success(
                written.empty() ? "Semua teksturnya sudah ada"
                                : std::to_string(written.size()) + " tekstur dikeluarkan");
        }
    }

    void Open(EditorContext& context, const assets::AssetRecord& record) {
        assetGuid_ = record.guid;
        assetName_ = record.relativePath;
        meshPath_ = context.assets->AbsolutePath(record).string();
        Reload(context);
        camera_.Frame(mesh_.boundsMin, mesh_.boundsMax);
    }

    void Reload(EditorContext& context) {
        // Diurai di panel, bukan diminta dari perender. Perender hanya memegang
        // geometri yang sudah di GPU; yang dibutuhkan di sini adalah nama bone,
        // nama material, dan bobot skin — dan ketiganya tinggal di sisi aset.
        mesh_ = assets::LoadMesh(meshPath_, loadError_);
        assets::LoadMeshSettings(settings_, meshPath_);
        selectedBone_ = -1;
        if (!mesh_.IsValid() && context.notifications != nullptr) {
            context.notifications->Error("Mesh tidak terbaca: " + loadError_);
        }
    }

    void DrawViewport(EditorContext& context, render::IViewportRenderer* preview) {
        const ImVec2 size = ImGui::GetContentRegionAvail();
        if (preview == nullptr) {
            ImGui::TextDisabled("Pratinjau mesh tidak tersedia di perangkat ini.");
            return;
        }
        if (size.x < 8.0f || size.y < 8.0f) {
            return;
        }
        preview->Resize(static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y));

        BuildScene(context, *preview);

        render::ViewportDesc desc;
        desc.width = static_cast<uint32_t>(size.x);
        desc.height = static_cast<uint32_t>(size.y);
        desc.camera = camera_.ToCamera();
        desc.showGrid = true;
        // Langit dan GI dimatikan: yang diperiksa di sini bentuk dan bobot, dan
        // atmosfer penuh hanya menambah biaya pada panel yang kerap terbuka
        // berdampingan dengan viewport utama.
        desc.skyEnabled = false;
        desc.gi.enabled = false;
        desc.clearColor = Vec4(0.10f, 0.11f, 0.13f, 1.0f);

        render::ViewportScene scene;
        scene.meshes = meshes_;
        scene.partColors = partColors_;
        scene.lines = lines_;
        preview->Render(desc, scene);

        const render::TextureHandle texture = preview->ColorTarget();
        if (texture == render::kInvalidTexture) {
            ImGui::Dummy(size);
            return;
        }
        const Vec2 uvMax = preview->ColorTargetUvMax();
        ImGui::Image(static_cast<ImTextureID>(texture), size, ImVec2(0.0f, 0.0f),
                     ImVec2(uvMax.x, uvMax.y));

        // Orbit hanya saat gambarnya benar-benar di bawah kursor. Tanpa syarat
        // itu, menyeret di panel lain ikut memutar kamera di sini.
        if (ImGui::IsItemHovered()) {
            const ImGuiIO& io = ImGui::GetIO();
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                camera_.yaw -= io.MouseDelta.x * 0.01f;
                camera_.pitch = std::clamp(camera_.pitch + io.MouseDelta.y * 0.01f, -1.5f, 1.5f);
            }
            if (io.MouseWheel != 0.0f) {
                camera_.distance =
                    std::max(camera_.distance * std::pow(0.9f, io.MouseWheel), 0.05f);
            }
        }
    }

    /// Menyusun isi yang digambar: satu mesh dan rangkanya sebagai garis.
    void BuildScene(EditorContext& context, render::IViewportRenderer& preview) {
        meshes_.clear();
        partColors_.clear();
        lines_.clear();

        const render::MeshAsset asset = preview.AcquireMesh(meshPath_);
        render::MeshInstance instance;
        instance.mesh = asset.handle;
        instance.boundsMin = asset.boundsMin;
        instance.boundsMax = asset.boundsMax;
        instance.partFirst = 0;
        instance.partCount = asset.partCount;
        instance.castShadows = false;

        // Ruas yang ditandai kain diwarnai, sisanya dibiarkan memakai warna
        // material berkasnya — itu yang membuat penandaan terlihat sekaligus
        // membiarkan model tetap tampil seperti aslinya.
        for (uint32_t part = 0; part < asset.partCount; ++part) {
            const bool cloth =
                part < mesh_.parts.size() && IsClothPart(static_cast<std::size_t>(part));
            partColors_.push_back(cloth ? kClothColor : Vec4(0.0f));
        }
        meshes_.push_back(instance);

        AppendSkeletonLines();
        (void)context;
    }

    /// Rangka sebagai garis dari tiap bone ke induknya, dalam bind pose.
    void AppendSkeletonLines() {
        const std::vector<assets::SkeletonBone>& bones = mesh_.skeleton.bones;
        if (bones.empty()) {
            return;
        }
        std::vector<Mat4> global(bones.size(), Mat4(1.0f));
        for (std::size_t i = 0; i < bones.size(); ++i) {
            const assets::SkeletonBone& bone = bones[i];
            const Mat4 local = glm::translate(Mat4(1.0f), bone.translation) *
                               glm::mat4_cast(bone.rotation) *
                               glm::scale(Mat4(1.0f), bone.scale);
            global[i] = bone.parent >= 0 ? global[static_cast<std::size_t>(bone.parent)] * local
                                         : local;
            if (bone.parent < 0) {
                continue;
            }
            const Vec3 from(global[static_cast<std::size_t>(bone.parent)][3]);
            const Vec3 to(global[i][3]);
            const bool highlighted =
                selectedBone_ == static_cast<int>(i) || selectedBone_ == bone.parent;
            lines_.push_back(
                render::LineSegment{from, to, highlighted ? kSelectedBoneColor : kBoneColor});
        }
    }

    bool IsClothPart(std::size_t part) const {
        const int material = mesh_.parts[part].material;
        if (material < 0 || static_cast<std::size_t>(material) >= mesh_.materials.size()) {
            return false;
        }
        return settings_.ClothFor(mesh_.materials[static_cast<std::size_t>(material)].name);
    }

    void DrawInfo() {
        ImGui::SeparatorText("Mesh");
        ImGui::Text("%zu segitiga, %zu vertex", mesh_.TriangleCount(), mesh_.vertices.size());
        ImGui::Text("%zu ruas, %zu material", mesh_.parts.size(), mesh_.materials.size());
        ImGui::Text("%zu bone%s", mesh_.skeleton.bones.size(),
                    mesh_.IsSkinned() ? " (ber-skin)" : "");
        const Vec3 size = mesh_.boundsMax - mesh_.boundsMin;
        ImGui::Text("%.2f x %.2f x %.2f m", size.x, size.y, size.z);
    }

    void DrawParts(EditorContext& context) {
        ImGui::SeparatorText("Ruas & material");
        if (mesh_.parts.empty()) {
            ImGui::TextDisabled("Tidak ada ruas.");
            return;
        }
        ImGui::TextDisabled("Centang ruas yang berupa kain.");
        for (std::size_t part = 0; part < mesh_.parts.size(); ++part) {
            const int material = mesh_.parts[part].material;
            const std::string name =
                material >= 0 && static_cast<std::size_t>(material) < mesh_.materials.size()
                    ? mesh_.materials[static_cast<std::size_t>(material)].name
                    : std::string("(tanpa material)");
            ImGui::PushID(static_cast<int>(part));
            bool cloth = IsClothPart(part);
            if (ImGui::Checkbox("##cloth", &cloth) && !name.empty()) {
                settings_.SetCloth(name, cloth);
                Save(context);
            }
            ImGui::SameLine();
            ImGui::Text("%s", name.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%u tri", mesh_.parts[part].indexCount / 3u);
            DrawMaterialSlot(context, name);
            ImGui::PopID();
        }
    }

    /// Pemilih material bawaan untuk sebuah ruas.
    ///
    /// **Yang disetel di sini bukan yang berlaku saat menggambar** melainkan
    /// titik awal sebuah entity: ia disalin ke `MeshRendererComponent` saat aset
    /// mesh ini ditetapkan, dan sesudah itu entity yang memilikinya.
    void DrawMaterialSlot(EditorContext& context, const std::string& materialName) {
        if (materialName.empty() || context.assets == nullptr) {
            return;
        }
        const Uuid current = settings_.MaterialFor(materialName);
        std::string label = "(bawaan editor)";
        if (const assets::AssetRecord* record = context.assets->Find(current)) {
            label = record->name;
        }

        ImGui::SetNextItemWidth(-1.0f);
        if (!ImGui::BeginCombo("##material", label.c_str())) {
            return;
        }
        if (ImGui::Selectable("(bawaan editor)", !current.IsValid())) {
            settings_.SetMaterial(materialName, Uuid{});
            Save(context);
        }
        for (const assets::AssetRecord& record : context.assets->All()) {
            if (record.type != assets::AssetType::Material) {
                continue;
            }
            const bool selected = record.guid == current;
            ImGui::PushID(record.guid.ToString().c_str());
            if (ImGui::Selectable(record.relativePath.c_str(), selected)) {
                settings_.SetMaterial(materialName, record.guid);
                Save(context);
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    /// Nilai bawaan yang ikut terisi ke komponen saat mesh ini dipasang.
    void DrawComponentDefaults(EditorContext& context) {
        ImGui::SeparatorText("Bawaan MeshRenderer");
        ImGui::TextDisabled("Mengisi slot yang masih kosong saat mesh ini dipasang.");

        bool changed = false;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        changed |= ImGui::SliderFloat("LOD Bias", &settings_.lodBias, -4.0f, 4.0f);
        changed |= ImGui::Checkbox("Cast Shadows", &settings_.castShadows);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Receive Shadows", &settings_.receiveShadows);
        if (changed) {
            Save(context);
        }
    }

    void Save(EditorContext& context) {
        if (!assets::SaveMeshSettings(settings_, meshPath_) && context.notifications != nullptr) {
            context.notifications->Error("Tidak bisa menyimpan pengaturan mesh");
        }
    }

    void DrawBones() {
        ImGui::SeparatorText("Rangka");
        if (mesh_.skeleton.bones.empty()) {
            ImGui::TextDisabled("Mesh ini tidak punya rangka.");
            return;
        }
        // Daftar rata, bukan pohon: urutannya sudah topologis, jadi indentasi
        // menurut kedalaman sudah memperlihatkan hierarkinya tanpa satu pun
        // simpul yang harus dibuka.
        ImGui::BeginChild("##bones", ImVec2(0.0f, 0.0f));
        for (std::size_t i = 0; i < mesh_.skeleton.bones.size(); ++i) {
            const assets::SkeletonBone& bone = mesh_.skeleton.bones[i];
            int depth = 0;
            for (int parent = bone.parent; parent >= 0;
                 parent = mesh_.skeleton.bones[static_cast<std::size_t>(parent)].parent) {
                ++depth;
            }
            ImGui::Indent(static_cast<float>(depth) * 8.0f);
            const bool selected = selectedBone_ == static_cast<int>(i);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::Selectable(bone.name.c_str(), selected)) {
                selectedBone_ = selected ? -1 : static_cast<int>(i);
            }
            ImGui::PopID();
            ImGui::Unindent(static_cast<float>(depth) * 8.0f);
        }
        ImGui::EndChild();
    }

    Uuid assetGuid_;
    std::string assetName_;
    std::string meshPath_;
    std::string loadError_;
    assets::MeshData mesh_;
    assets::MeshSettings settings_;
    int selectedBone_ = -1;
    OrbitCamera camera_;

    std::vector<render::MeshInstance> meshes_;
    std::vector<Vec4> partColors_;
    std::vector<render::LineSegment> lines_;
};

}  // namespace

SIM_REGISTER_PANEL(MeshEditorPanel, 55)

}  // namespace sim::editor
