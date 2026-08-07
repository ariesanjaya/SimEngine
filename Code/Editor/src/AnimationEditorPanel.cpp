#include "Sim/Animation/AnimationIo.h"
#include "Sim/Animation/Clip.h"
#include "Sim/Animation/ClipHistory.h"
#include "Sim/Animation/Pose.h"
#include "Sim/Animation/Skeleton.h"
#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/Widgets.h"
#include "Sim/Script/ScriptRuntime.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

namespace sim::editor {
namespace {

using namespace sim::animation;

constexpr ImVec4 kHintColor(0.55f, 0.57f, 0.60f, 1.0f);
constexpr ImVec4 kWarnColor(0.95f, 0.68f, 0.25f, 1.0f);

/// Sumbu yang dipetakan ke layar pada pratinjau.
enum class ViewAxis {
    Front,  // X ke kanan, Y ke atas
    Side,   // Z ke kanan, Y ke atas
    Top,    // X ke kanan, Z ke atas
};

/// Warna baris dope sheet menurut kelompok kanalnya.
constexpr std::array<ImVec4, 3> kGroupColors{{
    ImVec4(0.90f, 0.45f, 0.40f, 1.0f),  // translasi
    ImVec4(0.45f, 0.75f, 0.95f, 1.0f),  // rotasi
    ImVec4(0.60f, 0.85f, 0.45f, 1.0f),  // skala
}};

constexpr std::array<const char*, 3> kGroupNames{"Translation", "Rotation", "Scale"};

/// Nama event yang boleh disalurkan ke Lua.
///
/// **Nama datang dari berkas, dan berkas boleh berisi apa saja.** Penyaluran
/// event menyusun sepotong kode Lua yang memuat namanya, jadi nama yang memuat
/// tanda kutip bisa menutup literal string dan menyambung kode lain — sebuah
/// klip yang dibagikan orang lain menjadi jalan masuk untuk menjalankan apa pun
/// di dalam editor. Nama yang tidak lolos saringan ini tidak disalurkan, dan
/// panel menyebutkannya alih-alih diam.
bool IsDispatchableEventName(const std::string& name) {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    for (const char c : name) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) == 0 && c != '_' && c != '.') {
            return false;
        }
    }
    return true;
}

/// Penyunting animasi.
///
/// **Pratinjaunya rangka yang digambar dua dimensi, bukan mesh yang di-skin.**
/// Alasan yang sama dengan peta 2D penyunting terrain dan vegetasi: belum ada
/// yang bisa menggambar mesh sampai E8, dan alat yang hasilnya tidak terlihat
/// tidak bisa diuji maupun dipakai. Untuk menyunting kurva, rangka bergaris
/// bahkan lebih terbaca daripada mesh — yang perlu dinilai adalah sudut sendi
/// dan jangkauan gerak, dan keduanya justru tertutup oleh kulit.
///
/// **Suntingan tidak melewati `CommandHistory`**, sama seperti Material, Graph,
/// Script, Terrain, dan Vegetation Editor. Riwayat utama menjanjikan pembatalan
/// perubahan *scene*; klip adalah dokumen yang dibuka dan ditutup, dan undo-nya
/// dilayani `ClipHistory`.
class AnimationEditorPanel final : public Panel {
public:
    AnimationEditorPanel()
        : Panel(panel_id::kAnimationEditor,
                std::string(icons::kAnimationEditor) + "  Animation Editor",
                PanelCategory::Authoring) {}

    void OnDraw(EditorContext& context) override {
        if (context.assets == nullptr) {
            ImGui::TextDisabled("No asset database.");
            return;
        }

        DrawToolbar(context);
        ImGui::Separator();

        const float listWidth = ImGui::GetFontSize() * 11.0f;
        if (ImGui::BeginChild("##assets", ImVec2(listWidth, 0.0f), ImGuiChildFlags_ResizeX)) {
            DrawAssetList(context);
        }
        ImGui::EndChild();
        ImGui::SameLine();

        if (ImGui::BeginChild("##main", ImVec2(0.0f, 0.0f))) {
            if (ImGui::BeginTabBar("##tabs")) {
                if (ImGui::BeginTabItem((std::string(icons::kBone) + "  Skeleton").c_str())) {
                    DrawSkeletonTab(context);
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem((std::string(icons::kCurve) + "  Clip").c_str())) {
                    DrawClipTab(context);
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::EndChild();
    }

private:
    // --- toolbar & daftar aset -----------------------------------------------

    void DrawToolbar(EditorContext& context) {
        ImGui::BeginDisabled(!skeletonDirty_ && !clipDirty_);
        if (ImGui::Button((std::string(icons::kSave) + "  Save").c_str())) {
            SaveAll(context);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(history_.UndoDepth() == 0);
        if (ImGui::Button(icons::kUndo)) {
            history_.Undo(clip_);
            clipDirty_ = true;
        }
        ImGui::EndDisabled();
        widgets::Tooltip(history_.UndoDepth() == 0
                             ? "Nothing to undo"
                             : ("Undo " + std::string(history_.UndoLabel()) + " (Ctrl+Z)").c_str());

        ImGui::SameLine();
        ImGui::BeginDisabled(history_.RedoDepth() == 0);
        if (ImGui::Button(icons::kRedo)) {
            history_.Redo(clip_);
            clipDirty_ = true;
        }
        ImGui::EndDisabled();
        widgets::Tooltip("Redo (Ctrl+Shift+Z)");

        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "|");
        ImGui::SameLine();

        ImGui::BeginDisabled(!clipGuid_.IsValid());
        if (ImGui::Button(playing_ ? icons::kPause : icons::kPlay)) {
            playing_ = !playing_;
        }
        widgets::Tooltip(playing_ ? "Pause preview" : "Play preview");
        ImGui::SameLine();
        if (ImGui::Button(icons::kStop)) {
            playing_ = false;
            time_ = 0.0f;
            previousTime_ = 0.0f;
            firedLog_.clear();
        }
        widgets::Tooltip("Stop and rewind");
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
        ImGui::DragFloat("##speed", &previewSpeed_, 0.01f, 0.05f, 8.0f, "%.2fx");
        widgets::Tooltip("Preview speed. Does not change the clip.");
        ImGui::SameLine();
        ImGui::Checkbox(icons::kLoop, &previewLoop_);
        widgets::Tooltip("Loop the preview");

        if (skeletonGuid_.IsValid() || clipGuid_.IsValid()) {
            ImGui::SameLine();
            ImGui::TextColored(kHintColor, "|  %s%s   %s%s", skeletonName_.c_str(),
                               skeletonDirty_ ? " *" : "", clipName_.c_str(),
                               clipDirty_ ? " *" : "");
        }

        // Pintasan hanya saat panel ini fokus — Ctrl+Z global milik riwayat
        // scene, dan dua pemilik untuk satu pintasan berarti satu di antaranya
        // diam-diam kalah.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (ImGui::GetIO().KeyShift) {
                history_.Redo(clip_);
            } else {
                history_.Undo(clip_);
            }
            clipDirty_ = true;
        }
    }

    void DrawAssetList(EditorContext& context) {
        if (ImGui::Button("New Skeleton", ImVec2(-FLT_MIN, 0.0f))) {
            CreateSkeleton(context);
        }
        ImGui::BeginDisabled(!skeletonGuid_.IsValid());
        if (ImGui::Button("New Clip", ImVec2(-FLT_MIN, 0.0f))) {
            CreateClip(context);
        }
        ImGui::EndDisabled();
        widgets::Tooltip(skeletonGuid_.IsValid()
                             ? "Create a clip for the open skeleton"
                             : "Open a skeleton first — a clip is written against one");
        ImGui::Spacing();

        ImGui::TextColored(kHintColor, "Skeletons");
        for (const assets::AssetRecord& record : context.assets->All()) {
            if (record.type != assets::AssetType::Skeleton) {
                continue;
            }
            const std::string label = std::string(icons::kBone) + "  " + record.name;
            if (ImGui::Selectable(label.c_str(), record.guid == skeletonGuid_)) {
                OpenSkeleton(context, record.guid);
            }
        }

        ImGui::Spacing();
        ImGui::TextColored(kHintColor, "Clips");
        for (const assets::AssetRecord& record : context.assets->All()) {
            if (record.type != assets::AssetType::AnimationClip) {
                continue;
            }
            const std::string label = std::string(icons::kAssetAnimation) + "  " + record.name;
            if (ImGui::Selectable(label.c_str(), record.guid == clipGuid_)) {
                OpenClip(context, record.guid);
            }
        }
    }

    // --- tab rangka -----------------------------------------------------------

    void DrawSkeletonTab(EditorContext& context) {
        if (!skeletonGuid_.IsValid()) {
            ImGui::TextColored(kHintColor, "Pick a skeleton on the left, or create one.");
            return;
        }

        const float treeWidth = ImGui::GetFontSize() * 14.0f;
        if (ImGui::BeginChild("##bones", ImVec2(treeWidth, 0.0f), ImGuiChildFlags_ResizeX)) {
            DrawBoneTree(context);
        }
        ImGui::EndChild();
        ImGui::SameLine();

        if (ImGui::BeginChild("##boneprops", ImVec2(0.0f, 0.0f))) {
            DrawBoneProperties();
        }
        ImGui::EndChild();
    }

    void DrawBoneTree(EditorContext& context) {
        (void)context;
        if (ImGui::Button((std::string(icons::kAdd) + "  Add bone").c_str())) {
            AddBone();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(selectedBone_ < 0 || skeleton_.BoneCount() <= 1);
        if (ImGui::Button(icons::kDelete)) {
            skeleton_.RemoveBone(selectedBone_);
            selectedBone_ = std::min(selectedBone_, skeleton_.BoneCount() - 1);
            skeletonDirty_ = true;
            RebindClip();
        }
        ImGui::EndDisabled();
        widgets::Tooltip("Remove this bone and everything below it");
        ImGui::Separator();

        // Pohonnya digambar dengan menelusuri anak, bukan dengan indentasi yang
        // dihitung dari kedalaman: ImGui menuntut pasangan push/pop yang benar,
        // dan indentasi manual akan menyimpang begitu ada bone yang dilipat.
        for (int i = 0; i < skeleton_.BoneCount(); ++i) {
            if (skeleton_.Bone(i).parent < 0) {
                DrawBoneNode(i);
            }
        }
    }

    void DrawBoneNode(int index) {
        childScratch_.clear();
        skeleton_.Children(index, childScratch_);
        const std::vector<int> children = childScratch_;

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                   ImGuiTreeNodeFlags_DefaultOpen |
                                   ImGuiTreeNodeFlags_SpanAvailWidth;
        if (children.empty()) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }
        if (index == selectedBone_) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        ImGui::PushID(index);
        const bool open = ImGui::TreeNodeEx(skeleton_.Bone(index).name.c_str(), flags);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            selectedBone_ = index;
        }
        if (open) {
            for (const int child : children) {
                DrawBoneNode(child);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }

    void DrawBoneProperties() {
        if (selectedBone_ < 0 || selectedBone_ >= skeleton_.BoneCount()) {
            ImGui::TextColored(kHintColor, "Pick a bone.");
            return;
        }
        Bone& bone = skeleton_.Bone(selectedBone_);
        const float width = ImGui::GetFontSize() * 12.0f;

        ImGui::SetNextItemWidth(width);
        std::string name = bone.name;
        if (ImGui::InputText("Name", &name)) {
            // Nama unik dijaga rangka; nama kembar ditolak diam-diam di sini
            // supaya mengetik tidak menghasilkan rangka yang tidak bisa dimuat.
            if (!name.empty() && skeleton_.Find(name) < 0) {
                bone.name = name;
                skeletonDirty_ = true;
                RebindClip();
            }
        }

        if (ImGui::CollapsingHeader("Bind pose", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloat3("Translation", &bone.bind.translation.x, 0.01f)) {
                skeletonDirty_ = true;
            }
            // Rotasi disunting sebagai derajat Euler dan disimpan sebagai
            // kuaternion. Yang diketik orang selalu derajat; yang dipakai
            // pencampuran selalu kuaternion.
            Vec3 euler = QuatToEuler(bone.bind.rotation) * kRadToDeg;
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f, -360.0f, 360.0f, "%.1f deg")) {
                bone.bind.rotation = EulerToQuat(euler * kDegToRad);
                skeletonDirty_ = true;
            }
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloat3("Scale", &bone.bind.scale.x, 0.01f, 0.01f, 100.0f)) {
                skeletonDirty_ = true;
            }
        }

        if (ImGui::CollapsingHeader("Retarget", ImGuiTreeNodeFlags_DefaultOpen)) {
            // Kamus rangka ini memetakan nama bone-nya ke nama rig standar. N rig
            // karena itu menuntut N kamus, bukan N² — lihat `ComposeRetarget`.
            std::string standard(skeletonDocument_.retarget.Resolve(bone.name));
            if (standard == bone.name) {
                standard.clear();
            }
            ImGui::SetNextItemWidth(width);
            if (ImGui::InputText("Standard name", &standard)) {
                if (standard.empty()) {
                    skeletonDocument_.retarget.Remove(bone.name);
                } else {
                    skeletonDocument_.retarget.Set(bone.name, standard);
                }
                skeletonDirty_ = true;
            }
            widgets::Tooltip("Name of this bone in the standard rig, e.g. Hips or LeftUpLeg. "
                             "Empty means this bone takes part in no retarget scheme.");
            ImGui::TextColored(kHintColor, "%zu of %d bones mapped",
                               skeletonDocument_.retarget.Entries().size(),
                               skeleton_.BoneCount());
        }
    }

    // --- tab klip -------------------------------------------------------------

    void DrawClipTab(EditorContext& context) {
        if (!clipGuid_.IsValid()) {
            ImGui::TextColored(kHintColor, "Pick a clip on the left, or create one.");
            return;
        }
        Advance(context);

        const float previewHeight = std::max(ImGui::GetContentRegionAvail().y * 0.42f,
                                             ImGui::GetFontSize() * 8.0f);
        if (ImGui::BeginChild("##preview", ImVec2(0.0f, previewHeight))) {
            DrawPreview();
        }
        ImGui::EndChild();

        DrawTransport();

        const float sideWidth = ImGui::GetFontSize() * 15.0f;
        if (ImGui::BeginChild("##sheet",
                              ImVec2(std::max(ImGui::GetContentRegionAvail().x - sideWidth -
                                                  ImGui::GetStyle().ItemSpacing.x,
                                              1.0f),
                                     0.0f))) {
            DrawDopeSheet();
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##keyprops", ImVec2(0.0f, 0.0f))) {
            DrawKeyProperties();
        }
        ImGui::EndChild();
    }

    /// Memajukan pratinjau dan menyalakan event yang dilewatinya.
    void Advance(EditorContext& context) {
        previousTime_ = time_;
        if (!playing_) {
            return;
        }
        const float dt = context.deltaSeconds > 0.0f ? context.deltaSeconds : 1.0f / 60.0f;
        time_ += dt * previewSpeed_;
        if (time_ >= clip_.duration) {
            if (previewLoop_) {
                time_ = std::fmod(time_, std::max(clip_.duration, 1e-6f));
            } else {
                time_ = clip_.duration;
                playing_ = false;
            }
        }
        clip_.CollectEvents(previousTime_, time_, eventScratch_);
        for (const animation::Event* event : eventScratch_) {
            DispatchEvent(context, *event);
        }
    }

    /// Menyalurkan sebuah event ke Lua.
    ///
    /// **Kontraknya sebuah tabel global `AnimationEvents`**, dan bukan fungsi
    /// global bernama sama dengan event-nya: nama event ditulis animator ("
    /// footstep_left"), dan menjadikan tiap nama itu global akan menabrak apa pun
    /// yang kebetulan sudah bernama begitu. Satu tabel juga membuat "event apa
    /// saja yang ditangani" bisa dijawab dengan membaca satu tempat.
    void DispatchEvent(EditorContext& context, const animation::Event& event) {
        firedLog_.push_back(event.name);
        if (firedLog_.size() > 32) {
            firedLog_.erase(firedLog_.begin());
        }
        if (context.scripts == nullptr) {
            return;
        }
        if (!IsDispatchableEventName(event.name)) {
            if (context.notifications != nullptr) {
                context.notifications->Warning("Event \"" + event.name +
                                            "\" was not sent to Lua: names may only use letters, "
                                            "digits, underscore, and dot.");
            }
            return;
        }
        // Ketiadaan tabel maupun penanganya bukan kesalahan: klip kerap dibuat
        // sebelum skrip yang menanganinya ada, dan editor yang menyalak tiap
        // langkah kaki tidak bisa dipakai.
        const std::string code = "local t = rawget(_G, 'AnimationEvents') "
                                 "if type(t) == 'table' and type(t['" + event.name +
                                 "']) == 'function' then t['" + event.name + "']('" + event.name +
                                 "', " + std::to_string(event.time) + ") end";
        const script::EvalResult result = context.scripts->Evaluate(code);
        if (!result.ok && context.notifications != nullptr) {
            context.notifications->Error("Animation event \"" + event.name + "\": " + result.error);
        }
    }

    void DrawPreview() {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size = ImGui::GetContentRegionAvail();
        ImGui::InvisibleButton("##previewrect", size);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                            ImGui::GetColorU32(ImVec4(0.10f, 0.11f, 0.13f, 1.0f)));

        if (skeleton_.BoneCount() == 0) {
            draw->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                          ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)));
            return;
        }

        SampleClip(clip_, binding_, skeleton_, time_, pose_);
        pose_.ComputeGlobal(skeleton_, globalScratch_);

        // Skala dihitung dari pose yang sedang terlihat, bukan dari bind pose:
        // klip yang mengangkat tangan tinggi-tinggi harus tetap muat, dan rangka
        // yang keluar bingkai di tengah pemutaran adalah pratinjau yang tidak
        // bisa dipakai menilai apa pun.
        const auto project = [&](const Vec3& p) {
            switch (axis_) {
                case ViewAxis::Front: return Vec2(p.x, p.y);
                case ViewAxis::Side: return Vec2(p.z, p.y);
                case ViewAxis::Top: return Vec2(p.x, p.z);
            }
            return Vec2(p.x, p.y);
        };
        Vec2 low(FLT_MAX);
        Vec2 high(-FLT_MAX);
        for (const BoneTransform& global : globalScratch_) {
            const Vec2 flat = project(global.translation);
            low = glm::min(low, flat);
            high = glm::max(high, flat);
        }
        const Vec2 span = glm::max(high - low, Vec2(1e-3f));
        const float margin = ImGui::GetFontSize();
        const float scale = std::min((size.x - margin * 2.0f) / span.x,
                                     (size.y - margin * 2.0f) / span.y);
        const Vec2 centre = (low + high) * 0.5f;
        const auto toScreen = [&](const Vec3& p) {
            const Vec2 flat = (project(p) - centre) * scale;
            // Y layar menghadap ke bawah; tanpa membaliknya, rangka berdiri
            // terbalik dan tidak ada yang bisa membacanya.
            return ImVec2(origin.x + size.x * 0.5f + flat.x, origin.y + size.y * 0.5f - flat.y);
        };

        const ImU32 boneColor = ImGui::GetColorU32(ImVec4(0.75f, 0.80f, 0.90f, 1.0f));
        const ImU32 jointColor = ImGui::GetColorU32(ImVec4(0.95f, 0.75f, 0.35f, 1.0f));
        const ImU32 selectedColor = ImGui::GetColorU32(ImVec4(0.40f, 0.85f, 1.0f, 1.0f));
        for (int i = 0; i < static_cast<int>(globalScratch_.size()); ++i) {
            const int parent = skeleton_.Bone(i).parent;
            const ImVec2 here = toScreen(globalScratch_[static_cast<std::size_t>(i)].translation);
            if (parent >= 0) {
                draw->AddLine(
                    toScreen(globalScratch_[static_cast<std::size_t>(parent)].translation), here,
                    boneColor, 2.0f);
            }
            draw->AddCircleFilled(here, i == selectedBone_ ? 5.0f : 3.0f,
                                  i == selectedBone_ ? selectedColor : jointColor);
        }
        draw->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)));

        // Sudut kiri atas: sumbu tampilan dan event yang baru menyala. Event yang
        // menyala tanpa jejak apa pun di layar tidak bisa diuji tanpa menulis
        // skrip lebih dulu.
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 6.0f, origin.y + 6.0f));
        int axis = static_cast<int>(axis_);
        static constexpr std::array<const char*, 3> kAxisNames{"Front", "Side", "Top"};
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
        if (ImGui::Combo("##axis", &axis, kAxisNames.data(), static_cast<int>(kAxisNames.size()))) {
            axis_ = static_cast<ViewAxis>(axis);
        }
        if (!firedLog_.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + 6.0f, origin.y + 6.0f + ImGui::GetFrameHeightWithSpacing()));
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "%s  %s", icons::kEvent,
                               firedLog_.back().c_str());
        }
    }

    void DrawTransport() {
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 12.0f);
        if (ImGui::SliderFloat("##time", &time_, 0.0f, clip_.duration, "%.3f s")) {
            playing_ = false;
        }
        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "frame %.0f / %.0f", std::round(time_ * clip_.frameRate),
                           std::round(clip_.duration * clip_.frameRate));
    }

    // --- dope sheet -----------------------------------------------------------

    float TimeToX(float time) const {
        const float span = std::max(clip_.duration, 1e-6f);
        return sheetOrigin_.x + sheetLabelWidth_ +
               (time / span) * (sheetSize_.x - sheetLabelWidth_);
    }

    float XToTime(float x) const {
        const float usable = std::max(sheetSize_.x - sheetLabelWidth_, 1.0f);
        return std::clamp((x - sheetOrigin_.x - sheetLabelWidth_) / usable, 0.0f, 1.0f) *
               clip_.duration;
    }

    void DrawDopeSheet() {
        sheetOrigin_ = ImGui::GetCursorScreenPos();
        sheetSize_ = ImGui::GetContentRegionAvail();
        sheetLabelWidth_ = ImGui::GetFontSize() * 10.0f;
        const float rowHeight = ImGui::GetFrameHeight();

        ImGui::InvisibleButton("##sheetrect", sheetSize_,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(sheetOrigin_,
                           ImVec2(sheetOrigin_.x + sheetSize_.x, sheetOrigin_.y + sheetSize_.y),
                           true);
        draw->AddRectFilled(sheetOrigin_,
                            ImVec2(sheetOrigin_.x + sheetSize_.x, sheetOrigin_.y + sheetSize_.y),
                            ImGui::GetColorU32(ImVec4(0.11f, 0.12f, 0.14f, 1.0f)));

        DrawTimeRuler(draw, rowHeight);
        DrawEventRow(draw, rowHeight);
        DrawLegend(draw);

        float y = sheetOrigin_.y + rowHeight * 2.0f;
        HandleKeyDrag(hovered);

        for (int track = 0; track < clip_.TrackCount(); ++track) {
            if (y > sheetOrigin_.y + sheetSize_.y) {
                break;
            }
            DrawTrackRow(draw, track, y, rowHeight, hovered);
            y += rowHeight;
        }

        // Garis waktu digambar terakhir supaya ia berada di atas seluruh kunci —
        // penanda posisi yang tertutup kunci tidak menandai apa pun.
        const float playhead = TimeToX(time_);
        draw->AddLine(ImVec2(playhead, sheetOrigin_.y),
                      ImVec2(playhead, sheetOrigin_.y + sheetSize_.y),
                      ImGui::GetColorU32(ImVec4(0.95f, 0.35f, 0.35f, 0.9f)), 1.5f);
        draw->PopClipRect();

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            ImGui::GetIO().MousePos.y < sheetOrigin_.y + rowHeight) {
            time_ = XToTime(ImGui::GetIO().MousePos.x);
            playing_ = false;
        }
    }

    void DrawTimeRuler(ImDrawList* draw, float rowHeight) {
        const ImU32 tick = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.18f));
        const ImU32 text = ImGui::GetColorU32(kHintColor);
        // Langkah tanda dipilih supaya jaraknya di layar tidak pernah rapat:
        // penggaris yang tanda-tandanya bertumpuk lebih sulit dibaca daripada
        // penggaris tanpa tanda sama sekali.
        const float pixelsPerSecond =
            (sheetSize_.x - sheetLabelWidth_) / std::max(clip_.duration, 1e-6f);
        float step = 0.1f;
        while (step * pixelsPerSecond < 48.0f) {
            step *= step < 0.5f ? 5.0f : 2.0f;
        }
        for (float t = 0.0f; t <= clip_.duration + 1e-4f; t += step) {
            const float x = TimeToX(t);
            draw->AddLine(ImVec2(x, sheetOrigin_.y), ImVec2(x, sheetOrigin_.y + sheetSize_.y), tick);
            char label[32];
            std::snprintf(label, sizeof(label), "%.2fs", static_cast<double>(t));
            draw->AddText(ImVec2(x + 3.0f, sheetOrigin_.y + 2.0f), text, label);
        }
        draw->AddLine(ImVec2(sheetOrigin_.x, sheetOrigin_.y + rowHeight),
                      ImVec2(sheetOrigin_.x + sheetSize_.x, sheetOrigin_.y + rowHeight), tick);
    }

    /// Legenda warna kelompok kanal.
    ///
    /// Warna baris membedakan translasi, rotasi, dan skala — dan warna yang
    /// membedakan sesuatu tanpa menyebutkan apa yang dibedakannya hanya menjadi
    /// hiasan. Digambar di kanan atas supaya tidak menabrak penggaris waktu.
    void DrawLegend(ImDrawList* draw) {
        float x = sheetOrigin_.x + sheetSize_.x - ImGui::GetFontSize() * 0.5f;
        const float y = sheetOrigin_.y + 2.0f;
        for (int group = 2; group >= 0; --group) {
            const char* name = kGroupNames[static_cast<std::size_t>(group)];
            const float width = ImGui::CalcTextSize(name).x;
            x -= width;
            draw->AddText(ImVec2(x, y),
                          ImGui::GetColorU32(kGroupColors[static_cast<std::size_t>(group)]), name);
            x -= ImGui::GetFontSize() * 0.8f;
        }
    }

    void DrawEventRow(ImDrawList* draw, float rowHeight) {
        const float y = sheetOrigin_.y + rowHeight * 1.5f;
        draw->AddText(ImVec2(sheetOrigin_.x + 4.0f, y - ImGui::GetFontSize() * 0.5f),
                      ImGui::GetColorU32(kHintColor), "Events");
        const ImU32 color = ImGui::GetColorU32(ImVec4(0.95f, 0.75f, 0.35f, 1.0f));
        for (const animation::Event& event : clip_.Events()) {
            const float x = TimeToX(event.time);
            draw->AddTriangleFilled(ImVec2(x, y - 5.0f), ImVec2(x - 4.0f, y + 4.0f),
                                    ImVec2(x + 4.0f, y + 4.0f), color);
        }
        for (const SyncMarker& marker : clip_.SyncMarkers()) {
            const float x = TimeToX(marker.time);
            draw->AddLine(ImVec2(x, y - 6.0f), ImVec2(x, y + 5.0f),
                          ImGui::GetColorU32(ImVec4(0.45f, 0.85f, 0.95f, 1.0f)), 2.0f);
        }
    }

    void DrawTrackRow(ImDrawList* draw, int track, float y, float rowHeight, bool hovered) {
        const Track& description = clip_.TrackAt(track);
        const int group = ChannelGroup(description.channel);
        const ImU32 color = ImGui::GetColorU32(kGroupColors[static_cast<std::size_t>(group)]);

        if (track == selectedTrack_) {
            draw->AddRectFilled(ImVec2(sheetOrigin_.x, y),
                                ImVec2(sheetOrigin_.x + sheetSize_.x, y + rowHeight),
                                ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.06f)));
        }
        // Track yang bonenya tidak ada di rangka ini ditandai, bukan didiamkan:
        // ia tetap tersimpan di berkas dan tetap memakan baris, tapi tidak
        // menganimasikan apa pun — dan animasi yang setengah berjalan tanpa
        // sebab yang terlihat adalah yang paling lama dicari orang.
        const bool unresolved = binding_.BoneFor(track) < 0;
        const std::string label = (unresolved ? std::string("? ") : std::string()) +
                                  description.bone + " · " + ToString(description.channel);
        draw->AddText(ImVec2(sheetOrigin_.x + 4.0f, y + 2.0f),
                      unresolved ? ImGui::GetColorU32(kWarnColor) : color, label.c_str());

        const float centre = y + rowHeight * 0.5f;
        const std::vector<CurveKey>& keys = description.curve.Keys();
        for (std::size_t k = 0; k < keys.size(); ++k) {
            const float x = TimeToX(keys[k].time);
            const bool selected = track == selectedTrack_ && static_cast<int>(k) == selectedKey_;
            const float radius = selected ? 6.0f : 4.0f;
            const ImVec2 points[4]{ImVec2(x, centre - radius), ImVec2(x + radius, centre),
                                   ImVec2(x, centre + radius), ImVec2(x - radius, centre)};
            draw->AddConvexPolyFilled(points, 4,
                                      selected ? ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 1.0f))
                                               : color);

            if (!hovered || dragging_) {
                continue;
            }
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            if (std::abs(mouse.x - x) > radius + 2.0f || std::abs(mouse.y - centre) > radius + 2.0f) {
                continue;
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                selectedTrack_ = track;
                selectedKey_ = static_cast<int>(k);
                BeginKeyDrag();
            } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                history_.Begin("Delete key");
                history_.Capture(clip_, track);
                clip_.TrackAt(track).curve.RemoveKey(k);
                history_.End();
                selectedKey_ = -1;
                clipDirty_ = true;
            }
        }
    }

    void BeginKeyDrag() {
        if (selectedTrack_ < 0 || selectedKey_ < 0) {
            return;
        }
        // Langkah undo dibuka saat seretan dimulai dan ditutup saat dilepas —
        // bukan satu langkah per frame. Satu langkah per frame berarti
        // membatalkan satu seretan menuntut puluhan kali Ctrl+Z, dan tidak ada
        // yang bisa memakainya.
        dragging_ = true;
        history_.Begin("Move key");
        history_.Capture(clip_, selectedTrack_);
    }

    void HandleKeyDrag(bool hovered) {
        (void)hovered;
        if (!dragging_) {
            return;
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            history_.End();
            dragging_ = false;
            return;
        }
        if (selectedTrack_ < 0 || selectedKey_ < 0 ||
            selectedTrack_ >= clip_.TrackCount()) {
            return;
        }
        Curve& curve = clip_.TrackAt(selectedTrack_).curve;
        if (selectedKey_ >= static_cast<int>(curve.Keys().size())) {
            return;
        }
        float time = XToTime(ImGui::GetIO().MousePos.x);
        if (!ImGui::GetIO().KeyAlt) {
            // Dikunci ke frame kecuali Alt ditahan. Kunci yang mendarat di antara
            // dua frame terlihat benar di kurva dan meleset saat diputar pada
            // laju frame klipnya.
            const float frame = std::max(clip_.frameRate, 1.0f);
            time = std::round(time * frame) / frame;
        }
        const float value = curve.Keys()[static_cast<std::size_t>(selectedKey_)].value;
        selectedKey_ = static_cast<int>(
            curve.MoveKey(static_cast<std::size_t>(selectedKey_), time, value));
        clipDirty_ = true;
    }

    void DrawKeyProperties() {
        ImGui::TextColored(kHintColor, "%d tracks   %zu events", clip_.TrackCount(),
                           clip_.Events().size());
        ImGui::Separator();

        const float width = ImGui::GetFontSize() * 7.0f;
        ImGui::SetNextItemWidth(width);
        if (ImGui::DragFloat("Duration", &clip_.duration, 0.01f, 0.01f, 3600.0f, "%.2f s")) {
            clipDirty_ = true;
        }
        ImGui::SetNextItemWidth(width);
        if (ImGui::DragFloat("Frame rate", &clip_.frameRate, 1.0f, 1.0f, 240.0f, "%.0f fps")) {
            clipDirty_ = true;
        }
        if (ImGui::Checkbox("Looping", &clip_.looping)) {
            clipDirty_ = true;
        }
        if (ImGui::Checkbox("Root motion", &clip_.extractRootMotion)) {
            clipDirty_ = true;
        }
        widgets::Tooltip("Lift the root bone's movement out of the pose and hand it to the caller");

        ImGui::Separator();
        if (ImGui::Button((std::string(icons::kAdd) + "  Track").c_str())) {
            AddTrackForSelectedBone();
        }
        widgets::Tooltip("Add a track for the bone selected in the Skeleton tab");
        ImGui::SameLine();
        if (ImGui::Button((std::string(icons::kEvent) + "  Event").c_str())) {
            history_.Begin("Add event");
            history_.CaptureEvents(clip_);
            clip_.AddEvent(animation::Event{time_, "event"});
            history_.End();
            clipDirty_ = true;
        }
        widgets::Tooltip("Add an event at the playhead");

        if (selectedTrack_ < 0 || selectedTrack_ >= clip_.TrackCount()) {
            ImGui::TextColored(kHintColor, "Pick a key in the dope sheet.");
            DrawEventList();
            return;
        }

        ImGui::Separator();
        Track& track = clip_.TrackAt(selectedTrack_);
        ImGui::TextColored(kHintColor, "%s · %s", track.bone.c_str(), ToString(track.channel));

        // Penyunting kurva yang sama persis dengan Particle dan Terrain — lihat
        // catatan di `widgets::CurveEditor`. Tangen ditampilkan untuk kunci yang
        // terpilih, dan itu memenuhi "Curve Editor dengan tangent handle".
        float low = FLT_MAX;
        float high = -FLT_MAX;
        for (const CurveKey& key : track.curve.Keys()) {
            low = std::min(low, key.value);
            high = std::max(high, key.value);
        }
        if (low > high) {
            low = 0.0f;
            high = 1.0f;
        }
        const float pad = std::max((high - low) * 0.15f, 0.05f);
        if (widgets::CurveEditor("##curve", track.curve,
                                 ImVec2(-widgets::kPanelRightMargin, ImGui::GetFontSize() * 7.0f),
                                 low - pad, high + pad)) {
            clipDirty_ = true;
        }

        if (selectedKey_ >= 0 && selectedKey_ < static_cast<int>(track.curve.Keys().size())) {
            CurveKey& key = track.curve.Keys()[static_cast<std::size_t>(selectedKey_)];
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloat("Time", &key.time, 0.01f, 0.0f, clip_.duration, "%.3f s")) {
                clipDirty_ = true;
            }
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloat("Value", &key.value, 0.01f)) {
                clipDirty_ = true;
            }
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloat("In tangent", &key.inTangent, 0.05f)) {
                clipDirty_ = true;
            }
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloat("Out tangent", &key.outTangent, 0.05f)) {
                clipDirty_ = true;
            }
            static constexpr std::array<const char*, 3> kModes{"Constant", "Linear", "Bezier"};
            int mode = static_cast<int>(key.interpolation);
            ImGui::SetNextItemWidth(width);
            if (ImGui::Combo("Interp", &mode, kModes.data(), static_cast<int>(kModes.size()))) {
                key.interpolation = static_cast<Interpolation>(mode);
                clipDirty_ = true;
            }
        }
        DrawEventList();
    }

    void DrawEventList() {
        if (!ImGui::CollapsingHeader("Events & sync")) {
            return;
        }
        for (std::size_t i = 0; i < clip_.Events().size(); ++i) {
            ImGui::PushID(static_cast<int>(i));
            std::vector<animation::Event> events = clip_.Events();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
            if (ImGui::DragFloat("##t", &events[i].time, 0.01f, 0.0f, clip_.duration, "%.2fs")) {
                history_.Begin("Move event");
                history_.CaptureEvents(clip_);
                clip_.SetEvents(events);
                history_.End();
                clipDirty_ = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
            if (ImGui::InputText("##n", &events[i].name)) {
                history_.Begin("Rename event");
                history_.CaptureEvents(clip_);
                clip_.SetEvents(events);
                history_.End();
                clipDirty_ = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(icons::kDelete)) {
                history_.Begin("Delete event");
                history_.CaptureEvents(clip_);
                clip_.RemoveEvent(i);
                history_.End();
                clipDirty_ = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (ImGui::Button("Add sync marker")) {
            history_.Begin("Add sync marker");
            history_.CaptureSyncMarkers(clip_);
            clip_.AddSyncMarker(SyncMarker{time_, "Phase"});
            history_.End();
            clipDirty_ = true;
        }
        widgets::Tooltip("Phase markers pair two clips up when they are blended, so a walk/run "
                         "blend keeps its footfalls together");
        if (!firedLog_.empty()) {
            ImGui::TextColored(kHintColor, "Last fired: %s", firedLog_.back().c_str());
        }
        ImGui::TextColored(kHintColor,
                           "Lua: define AnimationEvents[\"name\"] = function(n, t) ... end");
    }

    void AddTrackForSelectedBone() {
        if (selectedBone_ < 0 || selectedBone_ >= skeleton_.BoneCount()) {
            return;
        }
        history_.Begin("Add track");
        history_.CaptureAllTracks(clip_);
        const std::string& bone = skeleton_.Bone(selectedBone_).name;
        const int track = clip_.EnsureTrack(bone, Channel::RotationY);
        if (clip_.TrackAt(track).curve.Keys().empty()) {
            // Kunci pertama bernilai sama dengan bind pose, bukan nol: track skala
            // yang lahir bernilai nol meruntuhkan bone menjadi titik pada frame
            // pertama juga.
            CurveKey key;
            key.time = 0.0f;
            key.value = ChannelValue(skeleton_.Bone(selectedBone_).bind, Channel::RotationY);
            key.interpolation = Interpolation::Bezier;
            clip_.TrackAt(track).curve.AddKey(key);
        }
        history_.End();
        selectedTrack_ = track;
        selectedKey_ = 0;
        clipDirty_ = true;
        RebindClip();
    }

    void AddBone() {
        Bone bone;
        bone.name = "Bone" + std::to_string(skeleton_.BoneCount());
        bone.parent = selectedBone_ >= 0 ? selectedBone_ : -1;
        bone.bind.translation = Vec3(0.0f, 1.0f, 0.0f);
        const int added = skeleton_.AddBone(bone);
        if (added >= 0) {
            selectedBone_ = added;
            skeletonDirty_ = true;
            RebindClip();
        }
    }

    // --- berkas ---------------------------------------------------------------

    void RebindClip() {
        binding_.Bind(clip_, skeleton_);
        pose_.Reset(skeleton_);
    }

    void CreateSkeleton(EditorContext& context) {
        const std::filesystem::path folder =
            std::filesystem::path(context.assets->Root()) / "Animation";
        std::filesystem::path path = folder / "NewSkeleton.simskel";
        int suffix = 0;
        while (std::filesystem::exists(path)) {
            path = folder / ("NewSkeleton" + std::to_string(++suffix) + ".simskel");
        }

        Skeleton fresh;
        Bone root;
        root.name = "Root";
        fresh.AddBone(root);
        SkeletonDocument document;
        document.name = path.stem().string();

        const AnimationIoResult result = SaveSkeleton(fresh, document, path);
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot create " + path.filename().string() + ": " +
                                             result.error);
            }
            return;
        }
        context.assets->ScanNow();
        if (const assets::AssetRecord* record =
                context.assets->FindByRelativePath("Animation/" + path.filename().string())) {
            OpenSkeleton(context, record->guid);
        }
    }

    void CreateClip(EditorContext& context) {
        const std::filesystem::path folder =
            std::filesystem::path(context.assets->Root()) / "Animation";
        std::filesystem::path path = folder / "NewClip.simanim";
        int suffix = 0;
        while (std::filesystem::exists(path)) {
            path = folder / ("NewClip" + std::to_string(++suffix) + ".simanim");
        }

        Clip fresh;
        fresh.name = path.stem().string();
        ClipDocument document;
        document.skeleton = AssetRef{skeletonGuid_};

        const AnimationIoResult result = SaveClip(fresh, document, path);
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot create " + path.filename().string() + ": " +
                                             result.error);
            }
            return;
        }
        context.assets->ScanNow();
        if (const assets::AssetRecord* record =
                context.assets->FindByRelativePath("Animation/" + path.filename().string())) {
            OpenClip(context, record->guid);
        }
    }

    void OpenSkeleton(EditorContext& context, const Uuid& guid) {
        const assets::AssetRecord* record = context.assets->Find(guid);
        if (record == nullptr) {
            return;
        }
        Skeleton loaded;
        SkeletonDocument document;
        const AnimationIoResult result =
            LoadSkeleton(loaded, document, context.assets->AbsolutePath(*record));
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot open " + record->name + ": " + result.error);
            }
            return;
        }
        skeleton_ = std::move(loaded);
        skeletonDocument_ = std::move(document);
        skeletonGuid_ = guid;
        skeletonName_ = record->name;
        skeletonPath_ = context.assets->AbsolutePath(*record);
        skeletonDirty_ = false;
        selectedBone_ = skeleton_.BoneCount() > 0 ? 0 : -1;
        RebindClip();
    }

    void OpenClip(EditorContext& context, const Uuid& guid) {
        const assets::AssetRecord* record = context.assets->Find(guid);
        if (record == nullptr) {
            return;
        }
        Clip loaded;
        ClipDocument document;
        const AnimationIoResult result =
            LoadClip(loaded, document, context.assets->AbsolutePath(*record));
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot open " + record->name + ": " + result.error);
            }
            return;
        }
        clip_ = std::move(loaded);
        clipDocument_ = std::move(document);
        clipGuid_ = guid;
        clipName_ = record->name;
        clipPath_ = context.assets->AbsolutePath(*record);
        clipDirty_ = false;
        history_.Clear();
        time_ = 0.0f;
        previousTime_ = 0.0f;
        playing_ = false;
        selectedTrack_ = clip_.TrackCount() > 0 ? 0 : -1;
        selectedKey_ = -1;
        firedLog_.clear();

        // Rangka klip dibuka sendiri kalau belum ada yang terbuka. Klip tanpa
        // rangka tidak bisa dicuplik menjadi pose, jadi tab Clip akan kosong
        // tanpa sebab yang terlihat.
        if (!skeletonGuid_.IsValid() && clipDocument_.skeleton.IsValid()) {
            OpenSkeleton(context, clipDocument_.skeleton.guid);
        }
        RebindClip();
        if (!binding_.Unresolved().empty() && context.notifications != nullptr) {
            context.notifications->Warning(clipName_ + ": " +
                                        std::to_string(binding_.Unresolved().size()) +
                                        " track(s) name bones this skeleton does not have.");
        }
    }

    void SaveAll(EditorContext& context) {
        if (skeletonDirty_ && skeletonGuid_.IsValid()) {
            const AnimationIoResult result =
                SaveSkeleton(skeleton_, skeletonDocument_, skeletonPath_);
            if (!result.ok) {
                if (context.notifications != nullptr) {
                    context.notifications->Error("Save failed: " + result.error);
                }
                return;
            }
            skeletonDirty_ = false;
        }
        if (clipDirty_ && clipGuid_.IsValid()) {
            const AnimationIoResult result = SaveClip(clip_, clipDocument_, clipPath_);
            if (!result.ok) {
                if (context.notifications != nullptr) {
                    context.notifications->Error("Save failed: " + result.error);
                }
                return;
            }
            clipDirty_ = false;
        }
        if (context.notifications != nullptr) {
            context.notifications->Success("Saved");
        }
    }

    Skeleton skeleton_;
    SkeletonDocument skeletonDocument_;
    Uuid skeletonGuid_;
    std::string skeletonName_;
    std::filesystem::path skeletonPath_;
    bool skeletonDirty_ = false;

    Clip clip_;
    ClipDocument clipDocument_;
    ClipBinding binding_;
    ClipHistory history_;
    Uuid clipGuid_;
    std::string clipName_;
    std::filesystem::path clipPath_;
    bool clipDirty_ = false;

    Pose pose_;
    std::vector<BoneTransform> globalScratch_;
    std::vector<int> childScratch_;
    std::vector<const animation::Event*> eventScratch_;
    std::vector<std::string> firedLog_;

    bool playing_ = false;
    bool previewLoop_ = true;
    float previewSpeed_ = 1.0f;
    float time_ = 0.0f;
    float previousTime_ = 0.0f;
    ViewAxis axis_ = ViewAxis::Front;

    int selectedBone_ = -1;
    int selectedTrack_ = -1;
    int selectedKey_ = -1;
    bool dragging_ = false;

    ImVec2 sheetOrigin_{0.0f, 0.0f};
    ImVec2 sheetSize_{0.0f, 0.0f};
    float sheetLabelWidth_ = 0.0f;
};

}  // namespace

SIM_REGISTER_PANEL(AnimationEditorPanel, 31)

}  // namespace sim::editor
