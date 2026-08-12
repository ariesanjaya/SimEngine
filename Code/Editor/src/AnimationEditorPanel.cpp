#include "Sim/Animation/AnimationIo.h"
#include "Sim/Animation/Clip.h"
#include "Sim/Animation/AnimationGraph.h"
#include "Sim/Animation/ClipHistory.h"
#include "Sim/Animation/GraphInstance.h"
#include "Sim/Animation/Pose.h"
#include "Sim/Animation/Skeleton.h"
#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/NodeGraph.h"
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
#include <map>
#include <memory>
#include <set>
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

/// Pustaka klip yang mengambil dari basis data aset, dengan singgahan.
///
/// **Ada di panel, bukan di modul animasi.** `ClipLibrary` sengaja sebuah
/// antarmuka: runtime yang memuat dari paket dan editor yang memuat dari folder
/// proyek harus bisa memutar graph yang sama, dan menjahitkan `AssetDatabase` ke
/// dalam modul akan memaksa runtime memalsukan dirinya sebagai editor.
///
/// Singgahannya menahan klip yang sudah dimuat sepanjang graph terbuka. Tanpa
/// itu, setiap frame pratinjau membaca ulang berkas untuk setiap klip yang
/// dirujuk state yang sedang aktif.
class AssetClipLibrary final : public ClipLibrary {
public:
    void Bind(assets::AssetDatabase* assets) {
        if (assets_ != assets) {
            assets_ = assets;
            cache_.clear();
        }
    }
    void Invalidate() { cache_.clear(); }

    const Clip* Find(const Uuid& guid) const override {
        if (!guid.IsValid() || assets_ == nullptr) {
            return nullptr;
        }
        const auto cached = cache_.find(guid);
        if (cached != cache_.end()) {
            // Entri kosong disimpan juga: klip yang gagal dimuat tidak boleh
            // dicoba ulang enam puluh kali per detik.
            return cached->second ? &cached->second->clip : nullptr;
        }
        const assets::AssetRecord* record = assets_->Find(guid);
        if (record == nullptr) {
            cache_.emplace(guid, nullptr);
            return nullptr;
        }
        auto entry = std::make_unique<Entry>();
        if (!LoadClip(entry->clip, entry->document, assets_->AbsolutePath(*record)).ok) {
            cache_.emplace(guid, nullptr);
            return nullptr;
        }
        const Clip* result = &entry->clip;
        cache_.emplace(guid, std::move(entry));
        return result;
    }

private:
    struct Entry {
        Clip clip;
        ClipDocument document;
    };

    assets::AssetDatabase* assets_ = nullptr;
    mutable std::map<Uuid, std::unique_ptr<Entry>> cache_;
};

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

    ~AnimationEditorPanel() override { canvas_.Shutdown(); }

    void OnDraw(EditorContext& context) override {
        if (context.assets == nullptr) {
            ImGui::TextDisabled("No asset database.");
            return;
        }
        if (!canvasReady_) {
            // Diinisialisasi saat pertama digambar, bukan di konstruktor: panel
            // dibuat sebelum konteks ImGui ada, dan kanvasnya menuntut konteks
            // itu hidup.
            canvas_.Initialize();
            canvasReady_ = true;
        }

        DrawToolbar(context);
        ImGui::Separator();

        // Penelusuran aset milik Asset Browser; lihat catatan yang sama di
        // Mesh, Material, dan Particle Editor.
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
                if (ImGui::BeginTabItem((std::string(icons::kNodeGraph) + "  Graph").c_str())) {
                    DrawGraphTab(context);
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
        ImGui::BeginDisabled(!skeletonDirty_ && !clipDirty_ && !graphDirty_);
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
        DrawSkeleton(draw, origin, size, pose_);
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
            ImGui::SetCursorScreenPos(
                ImVec2(origin.x + 6.0f, origin.y + 6.0f + ImGui::GetFrameHeightWithSpacing()));
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "%s  %s", icons::kEvent,
                               firedLog_.back().c_str());
        }
    }

    /// Menggambar sebuah pose sebagai rangka bergaris.
    ///
    /// Dipakai dua tab — pratinjau klip dan pratinjau graph — jadi ia tidak boleh
    /// tahu apa pun tentang klip maupun graph. Yang masuk hanya pose dan kotak
    /// tempat menggambarnya.
    void DrawSkeleton(ImDrawList* draw, const ImVec2& origin, const ImVec2& size,
                      const Pose& pose) {
        pose.ComputeGlobal(skeleton_, globalScratch_);
        if (globalScratch_.empty()) {
            return;
        }

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

        // Legenda lebih dulu: ia menentukan sampai mana label penggaris boleh
        // ditulis. Digambar sesudahnya, keduanya bertumpuk di sudut kanan dan
        // dua-duanya tidak terbaca.
        DrawLegend(draw);
        DrawTimeRuler(draw, rowHeight);
        DrawEventRow(draw, rowHeight);

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
            // Label yang akan menabrak legenda tidak ditulis. Garis tanda-nya
            // tetap digambar — yang hilang hanya angkanya, dan angka yang
            // bertumpuk dengan teks lain lebih buruk daripada tidak ada angka.
            if (x + 3.0f + ImGui::CalcTextSize(label).x < legendLeft_) {
                draw->AddText(ImVec2(x + 3.0f, sheetOrigin_.y + 2.0f), text, label);
            }
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
        legendLeft_ = x;
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

    // --- tab graph ------------------------------------------------------------

    /// Id kanvas.
    ///
    /// Dipisah per jenis lewat rentang yang tidak bertumpang tindih, dan tidak
    /// satu pun boleh nol — pustaka di balik kanvas memakai nol sebagai "tidak
    /// ada". State ditunjuk lewat indeksnya: graph dibaca ulang dari model tiap
    /// frame, jadi id yang bergeser saat sebuah state dihapus tidak menyimpan
    /// keadaan basi di mana pun.
    static uint64_t StateNodeId(int state) { return static_cast<uint64_t>(state) + 1u; }
    static uint64_t AnyStateNodeId() { return 0x300000u; }
    static uint64_t InputPinId(uint64_t node) { return 0x100000u + node * 2u; }
    static uint64_t OutputPinId(uint64_t node) { return 0x100000u + node * 2u + 1u; }
    static uint64_t TransitionLinkId(int index) { return 0x200000u + static_cast<uint64_t>(index); }

    void DrawGraphTab(EditorContext& context) {
        if (!graphGuid_.IsValid()) {
            ImGui::TextColored(kHintColor, "Pick a graph on the left, or create one.");
            return;
        }
        library_.Bind(context.assets);
        DrawGraphToolbar(context);

        Layer& layer = graph_.LayerAt(activeLayer_);
        const float sideWidth = ImGui::GetFontSize() * 16.0f;
        if (ImGui::BeginChild("##graphcanvas",
                              ImVec2(std::max(ImGui::GetContentRegionAvail().x - sideWidth -
                                                  ImGui::GetStyle().ItemSpacing.x,
                                              1.0f),
                                     0.0f))) {
            DrawGraphCanvas(layer);
        }
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##graphside", ImVec2(0.0f, 0.0f))) {
            DrawGraphSide(context, layer);
        }
        ImGui::EndChild();
    }

    void DrawGraphToolbar(EditorContext& context) {
        (void)context;
        if (ImGui::Button((std::string(icons::kAdd) + "  State").c_str())) {
            AddState();
        }
        ImGui::SameLine();
        if (ImGui::Button((std::string(icons::kAdd) + "  Layer").c_str())) {
            Layer layer;
            layer.name = "Layer " + std::to_string(graph_.LayerCount());
            activeLayer_ = graph_.AddLayer(layer);
            graphDirty_ = true;
            RebindGraph();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        const std::string preview = graph_.LayerAt(activeLayer_).name;
        if (ImGui::BeginCombo("##layer", preview.c_str())) {
            for (int i = 0; i < graph_.LayerCount(); ++i) {
                if (ImGui::Selectable(graph_.LayerAt(i).name.c_str(), i == activeLayer_)) {
                    activeLayer_ = i;
                    placedStates_.clear();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::Button("Fit")) {
            canvas_.FitToContent();
        }
        ImGui::SameLine();
        if (ImGui::Button(graphPlaying_ ? icons::kPause : icons::kPlay)) {
            graphPlaying_ = !graphPlaying_;
            if (graphPlaying_) {
                RebindGraph();
            }
        }
        widgets::Tooltip("Run the graph on the open skeleton");
    }

    void DrawGraphCanvas(Layer& layer) {
        canvas_.Begin("##animgraph", Vec2(0.0f, 0.0f));

        // Simpul "Any State" digambar walaupun ia bukan state: transisi
        // ber-`from` -1 berlaku dari mana pun, dan menggambarnya tanpa pangkal
        // membuat kabelnya seolah muncul dari ketiadaan.
        canvas_.BeginNode(AnyStateNodeId());
        ImGui::TextColored(kHintColor, "Any State");
        canvas_.BeginOutputPin(OutputPinId(AnyStateNodeId()));
        ImGui::TextUnformatted(" >");
        canvas_.EndPin();
        canvas_.EndNode();

        for (int i = 0; i < static_cast<int>(layer.states.size()); ++i) {
            DrawStateNode(layer, i);
        }
        for (int i = 0; i < static_cast<int>(layer.transitions.size()); ++i) {
            const Transition& transition = layer.transitions[static_cast<std::size_t>(i)];
            const uint64_t fromNode =
                transition.from < 0 ? AnyStateNodeId() : StateNodeId(transition.from);
            if (transition.to < 0 || transition.to >= static_cast<int>(layer.states.size())) {
                continue;
            }
            const bool selected = i == selectedTransition_;
            canvas_.Link(TransitionLinkId(i), OutputPinId(fromNode),
                         InputPinId(StateNodeId(transition.to)),
                         selected ? Vec4(1.0f, 0.85f, 0.35f, 1.0f) : Vec4(0.55f, 0.60f, 0.68f, 1.0f),
                         selected ? 3.0f : 2.0f);
        }

        HandleGraphCreate(layer);
        HandleGraphDelete(layer);
        canvas_.End();

        // Seleksi dibaca sesudah End(): sebelum itu pustaka belum memutakhirkan
        // apa yang diklik pada frame ini.
        const std::vector<uint64_t> links = canvas_.SelectedLinks();
        if (!links.empty()) {
            const auto index = static_cast<int>(links.front() - 0x200000u);
            if (index >= 0 && index < static_cast<int>(layer.transitions.size())) {
                selectedTransition_ = index;
            }
        }
        // **Pemasangan otomatis hanya berlaku kalau ada yang perlu dibingkai.**
        // Ditemukan saat menjalankan panelnya: graph baru hanya berisi simpul
        // semu "Any State", dan memasangnya ke layar memperbesar sampai satu
        // simpul memenuhi seluruh kanvas — huruf setinggi seratus piksel dan
        // tidak ada yang bisa dibaca. Membingkai satu simpul pun tidak ada
        // gunanya; yang butuh dibingkai adalah graph yang isinya sudah tersebar.
        // Tombol Fit tetap berlaku kapan pun, karena itu permintaan pengguna
        // sendiri.
        if (pendingFit_) {
            if (layer.states.size() >= 2) {
                canvas_.FitToContent();
            }
            pendingFit_ = false;
        }
    }

    void DrawStateNode(Layer& layer, int index) {
        State& state = layer.states[static_cast<std::size_t>(index)];
        const uint64_t id = StateNodeId(index);

        // Posisi berpindah dua arah — dari berkas ke kanvas saat dibuka, dan
        // kembali begitu node diseret. `placedStates_` yang menentukan arahnya;
        // tanpa itu, menyeret node langsung ditimpa balik posisi tersimpan.
        if (!placedStates_.contains(index)) {
            canvas_.SetNodePosition(id, state.canvas);
            placedStates_.insert(index);
        } else {
            const Vec2 current = canvas_.GetNodePosition(id);
            if (std::abs(current.x - state.canvas.x) > 0.5f ||
                std::abs(current.y - state.canvas.y) > 0.5f) {
                state.canvas = current;
                graphDirty_ = true;
            }
        }

        canvas_.BeginNode(id);
        const bool isDefault = index == layer.defaultState;
        const bool isActive = graphPlaying_ && instance_.Bound() &&
                              instance_.CurrentState(activeLayer_) == index;
        if (isActive) {
            ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1.0f), "%s", state.name.c_str());
        } else if (isDefault) {
            ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.45f, 1.0f), "%s", state.name.c_str());
        } else {
            ImGui::TextUnformatted(state.name.c_str());
        }
        ImGui::TextColored(kHintColor, "%s", ToString(state.motion.kind));

        canvas_.BeginInputPin(InputPinId(id));
        ImGui::TextUnformatted("> ");
        canvas_.EndPin();
        ImGui::SameLine();
        canvas_.BeginOutputPin(OutputPinId(id));
        ImGui::TextUnformatted(" >");
        canvas_.EndPin();
        canvas_.EndNode();
    }

    void HandleGraphCreate(Layer& layer) {
        if (!canvas_.BeginCreate()) {
            canvas_.EndCreate();
            return;
        }
        uint64_t fromPin = 0;
        uint64_t toPin = 0;
        // Sekali, bukan di dalam while — lihat catatan di NodeGraph.h.
        if (canvas_.QueryNewLink(fromPin, toPin)) {
            const int from = StateOfPin(layer, fromPin, true);
            const int to = StateOfPin(layer, toPin, false);
            if (from == -2 || to < 0 || from == to) {
                canvas_.RejectLink();
            } else if (canvas_.AcceptLink()) {
                Transition transition;
                transition.from = from;
                transition.to = to;
                layer.transitions.push_back(transition);
                selectedTransition_ = static_cast<int>(layer.transitions.size()) - 1;
                graphDirty_ = true;
            }
        }
        canvas_.EndCreate();
    }

    /// Indeks state pemilik sebuah pin. -1 berarti "Any State", -2 berarti pin
    /// itu bukan pin yang diminta (arahnya salah, atau tidak dikenal).
    int StateOfPin(const Layer& layer, uint64_t pin, bool wantOutput) const {
        if (pin < 0x100000u) {
            return -2;
        }
        const uint64_t offset = pin - 0x100000u;
        const bool isOutput = (offset & 1u) != 0u;
        if (isOutput != wantOutput) {
            return -2;
        }
        const uint64_t node = offset / 2u;
        if (node == AnyStateNodeId()) {
            // "Any State" hanya punya keluaran: tidak ada arti untuk transisi
            // yang menuju ke mana pun sekaligus.
            return wantOutput ? -1 : -2;
        }
        const auto index = static_cast<int>(node) - 1;
        return index >= 0 && index < static_cast<int>(layer.states.size()) ? index : -2;
    }

    void HandleGraphDelete(Layer& layer) {
        if (!canvas_.BeginDelete()) {
            canvas_.EndDelete();
            return;
        }
        uint64_t id = 0;
        while (canvas_.QueryDeletedLink(id)) {
            if (!canvas_.AcceptDeletion()) {
                continue;
            }
            const auto index = static_cast<int>(id - 0x200000u);
            if (index >= 0 && index < static_cast<int>(layer.transitions.size())) {
                layer.transitions.erase(layer.transitions.begin() + index);
                selectedTransition_ = -1;
                graphDirty_ = true;
            }
        }
        while (canvas_.QueryDeletedNode(id)) {
            if (id == AnyStateNodeId()) {
                // "Any State" bukan state; ia tidak bisa dihapus.
                canvas_.RejectDeletion();
                continue;
            }
            if (!canvas_.AcceptDeletion()) {
                continue;
            }
            RemoveState(layer, static_cast<int>(id) - 1);
        }
        canvas_.EndDelete();
    }

    void RemoveState(Layer& layer, int index) {
        if (index < 0 || index >= static_cast<int>(layer.states.size())) {
            return;
        }
        layer.states.erase(layer.states.begin() + index);
        // Transisi yang menunjuk state yang hilang ikut dibuang, dan indeks yang
        // di atasnya digeser. Membiarkannya berarti transisi yang menunjuk state
        // lain — bukan transisi yang mati, melainkan transisi yang salah.
        auto tail = std::remove_if(layer.transitions.begin(), layer.transitions.end(),
                                   [index](const Transition& transition) {
                                       return transition.from == index || transition.to == index;
                                   });
        layer.transitions.erase(tail, layer.transitions.end());
        for (Transition& transition : layer.transitions) {
            if (transition.from > index) {
                --transition.from;
            }
            if (transition.to > index) {
                --transition.to;
            }
        }
        if (layer.defaultState >= static_cast<int>(layer.states.size())) {
            layer.defaultState = std::max(0, static_cast<int>(layer.states.size()) - 1);
        }
        placedStates_.clear();
        selectedTransition_ = -1;
        graphDirty_ = true;
        RebindGraph();
    }

    void AddState() {
        Layer& layer = graph_.LayerAt(activeLayer_);
        State state;
        state.name = "State " + std::to_string(layer.states.size());
        state.canvas = Vec2(40.0f + static_cast<float>(layer.states.size() % 4) * 190.0f,
                            40.0f + static_cast<float>(layer.states.size() / 4) * 120.0f);
        layer.states.push_back(state);
        graphDirty_ = true;
        RebindGraph();
    }

    void DrawGraphSide(EditorContext& context, Layer& layer) {
        DrawGraphPreview(context);
        ImGui::Separator();

        const float width = ImGui::GetFontSize() * 7.0f;
        if (ImGui::CollapsingHeader("Layer", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##layername", &layer.name)) {
                graphDirty_ = true;
            }
            ImGui::SetNextItemWidth(width);
            if (ImGui::SliderFloat("Weight", &layer.weight, 0.0f, 1.0f)) {
                graphDirty_ = true;
            }
            if (ImGui::Checkbox("Additive", &layer.additive)) {
                graphDirty_ = true;
            }
            ImGui::SetNextItemWidth(width);
            if (ImGui::InputText("Mask root", &layer.maskRootBone)) {
                graphDirty_ = true;
            }
            widgets::Tooltip("Bone this layer starts from. Empty means the whole skeleton.");
            if (!layer.states.empty()) {
                ImGui::SetNextItemWidth(width);
                if (ImGui::BeginCombo("Default",
                                      layer.states[static_cast<std::size_t>(std::clamp(
                                                       layer.defaultState, 0,
                                                       static_cast<int>(layer.states.size()) - 1))]
                                          .name.c_str())) {
                    for (int i = 0; i < static_cast<int>(layer.states.size()); ++i) {
                        if (ImGui::Selectable(layer.states[static_cast<std::size_t>(i)].name.c_str(),
                                              i == layer.defaultState)) {
                            layer.defaultState = i;
                            graphDirty_ = true;
                        }
                    }
                    ImGui::EndCombo();
                }
            }
        }

        DrawParameterList();
        DrawStateProperties(context, layer);
        DrawTransitionProperties(layer);
    }

    void DrawGraphPreview(EditorContext& context) {
        if (graphPlaying_ && instance_.Bound()) {
            const float dt = context.deltaSeconds > 0.0f ? context.deltaSeconds : 1.0f / 60.0f;
            instance_.Update(dt);
            for (const FiredEvent& event : instance_.Events()) {
                firedLog_.push_back(event.name);
                if (firedLog_.size() > 32) {
                    firedLog_.erase(firedLog_.begin());
                }
            }
        }
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size(ImGui::GetContentRegionAvail().x - widgets::kPanelRightMargin,
                          ImGui::GetFontSize() * 8.0f);
        ImGui::Dummy(size);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                            ImGui::GetColorU32(ImVec4(0.10f, 0.11f, 0.13f, 1.0f)));
        if (instance_.Bound() && skeleton_.BoneCount() > 0) {
            DrawSkeleton(draw, origin, size, instance_.Result());
        }
        draw->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)));

        if (instance_.Bound()) {
            const int state = instance_.CurrentState(activeLayer_);
            const Layer& layer = graph_.LayerAt(activeLayer_);
            const char* name = state >= 0 && state < static_cast<int>(layer.states.size())
                                   ? layer.states[static_cast<std::size_t>(state)].name.c_str()
                                   : "(none)";
            ImGui::TextColored(kHintColor, "%s   %.0f%%", name,
                               static_cast<double>(instance_.TransitionProgress(activeLayer_)) *
                                   100.0);
        }
    }

    void DrawParameterList() {
        if (!ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }
        static constexpr std::array<const char*, 3> kTypes{"Bool", "Float", "Trigger"};
        for (int i = 0; i < graph_.parameters.Count(); ++i) {
            Parameter& parameter = graph_.parameters.At(i);
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
            if (ImGui::InputText("##name", &parameter.name)) {
                graphDirty_ = true;
            }
            ImGui::SameLine();
            // Nilainya disunting pada instance saat pratinjau berjalan, dan pada
            // graph saat tidak: yang pertama menggerakkan pemutaran, yang kedua
            // menetapkan nilai awal yang tersimpan ke berkas. Menyunting satu
            // tempat untuk keduanya berarti menggeser slider saat mencoba
            // sesuatu diam-diam mengubah dokumennya.
            const bool live = graphPlaying_ && instance_.Bound();
            ParameterSet& target = live ? instance_.parameters : graph_.parameters;
            const int index = live ? target.Find(parameter.name) : i;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
            switch (parameter.type) {
                case ParameterType::Float: {
                    float value = target.Float(index);
                    if (ImGui::DragFloat("##v", &value, 0.01f)) {
                        target.SetFloat(index, value);
                        graphDirty_ = graphDirty_ || !live;
                    }
                    break;
                }
                case ParameterType::Bool: {
                    bool value = target.Bool(index);
                    if (ImGui::Checkbox("##v", &value)) {
                        target.SetBool(index, value);
                        graphDirty_ = graphDirty_ || !live;
                    }
                    break;
                }
                case ParameterType::Trigger:
                    if (ImGui::Button("Fire")) {
                        target.Fire(index);
                    }
                    break;
            }
            ImGui::SameLine();
            int type = static_cast<int>(parameter.type);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
            if (ImGui::Combo("##type", &type, kTypes.data(), static_cast<int>(kTypes.size()))) {
                parameter.type = static_cast<ParameterType>(type);
                graphDirty_ = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(icons::kDelete)) {
                graph_.parameters.Remove(i);
                graphDirty_ = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (ImGui::Button((std::string(icons::kAdd) + "  Parameter").c_str())) {
            Parameter parameter;
            parameter.name = "Param" + std::to_string(graph_.parameters.Count());
            graph_.parameters.Add(parameter);
            graphDirty_ = true;
        }
    }

    void DrawStateProperties(EditorContext& context, Layer& layer) {
        const std::vector<uint64_t> selected = canvas_.SelectedNodes();
        if (selected.empty() || selected.front() == AnyStateNodeId()) {
            return;
        }
        const auto index = static_cast<int>(selected.front()) - 1;
        if (index < 0 || index >= static_cast<int>(layer.states.size())) {
            return;
        }
        if (!ImGui::CollapsingHeader("State", ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }
        State& state = layer.states[static_cast<std::size_t>(index)];
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##statename", &state.name)) {
            graphDirty_ = true;
        }

        static constexpr std::array<const char*, 3> kKinds{"Clip", "Blend1D", "Blend2D"};
        int kind = static_cast<int>(state.motion.kind);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (ImGui::Combo("Motion", &kind, kKinds.data(), static_cast<int>(kKinds.size()))) {
            state.motion.kind = static_cast<MotionKind>(kind);
            graphDirty_ = true;
            RebindGraph();
        }

        if (state.motion.kind == MotionKind::Clip) {
            DrawClipPicker(context, "##clip", state.motion.clip);
        } else {
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
            if (ImGui::InputText("Param X", &state.motion.parameterX)) {
                graphDirty_ = true;
            }
            if (state.motion.kind == MotionKind::Blend2D) {
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
                if (ImGui::InputText("Param Y", &state.motion.parameterY)) {
                    graphDirty_ = true;
                }
            }
            for (std::size_t i = 0; i < state.motion.children.size(); ++i) {
                MotionChild& child = state.motion.children[i];
                ImGui::PushID(static_cast<int>(i));
                DrawClipPicker(context, "##childclip", child.clip);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
                if (state.motion.kind == MotionKind::Blend2D) {
                    if (ImGui::DragFloat2("Pos", &child.position.x, 0.05f)) {
                        graphDirty_ = true;
                    }
                } else if (ImGui::DragFloat("Pos", &child.position.x, 0.05f)) {
                    graphDirty_ = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(icons::kDelete)) {
                    state.motion.children.erase(state.motion.children.begin() +
                                                static_cast<std::ptrdiff_t>(i));
                    graphDirty_ = true;
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            if (ImGui::Button("Add motion")) {
                state.motion.children.push_back(MotionChild{});
                graphDirty_ = true;
            }
            if (ImGui::Checkbox("Sync phase", &state.motion.syncPhase)) {
                graphDirty_ = true;
            }
            widgets::Tooltip("Pair the clips up by their phase markers, so a walk/run blend keeps "
                             "its footfalls together");
        }
    }

    void DrawClipPicker(EditorContext& context, const char* id, AssetRef& ref) {
        const assets::AssetRecord* current =
            ref.IsValid() ? context.assets->Find(ref.guid) : nullptr;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (!ImGui::BeginCombo(id, current != nullptr ? current->name.c_str() : "(no clip)")) {
            return;
        }
        if (ImGui::Selectable("(no clip)", !ref.IsValid())) {
            ref.Clear();
            graphDirty_ = true;
            RebindGraph();
        }
        for (const assets::AssetRecord& record : context.assets->All()) {
            if (record.type != assets::AssetType::AnimationClip) {
                continue;
            }
            if (ImGui::Selectable(record.name.c_str(), record.guid == ref.guid)) {
                ref = AssetRef{record.guid};
                graphDirty_ = true;
                RebindGraph();
            }
        }
        ImGui::EndCombo();
    }

    void DrawTransitionProperties(Layer& layer) {
        if (selectedTransition_ < 0 ||
            selectedTransition_ >= static_cast<int>(layer.transitions.size())) {
            return;
        }
        if (!ImGui::CollapsingHeader("Transition", ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }
        Transition& transition = layer.transitions[static_cast<std::size_t>(selectedTransition_)];
        const char* from = transition.from < 0
                               ? "Any State"
                               : layer.states[static_cast<std::size_t>(transition.from)].name.c_str();
        ImGui::TextColored(kHintColor, "%s  \xe2\x86\x92  %s", from,
                           layer.states[static_cast<std::size_t>(transition.to)].name.c_str());

        const float width = ImGui::GetFontSize() * 6.0f;
        ImGui::SetNextItemWidth(width);
        if (ImGui::DragFloat("Duration", &transition.duration, 0.01f, 0.0f, 10.0f, "%.2f s")) {
            graphDirty_ = true;
        }
        if (ImGui::Checkbox("Exit time", &transition.hasExitTime)) {
            graphDirty_ = true;
        }
        if (transition.hasExitTime) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(width);
            if (ImGui::SliderFloat("##exit", &transition.exitTime, 0.0f, 1.0f)) {
                graphDirty_ = true;
            }
        }

        static constexpr std::array<const char*, 6> kComparisons{
            "Greater", "Less", "GreaterEqual", "LessEqual", "Equal", "NotEqual"};
        ImGui::TextColored(kHintColor, "Conditions (all must hold)");
        for (std::size_t i = 0; i < transition.conditions.size(); ++i) {
            Condition& condition = transition.conditions[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
            if (ImGui::BeginCombo("##param", condition.parameter.empty()
                                                 ? "(parameter)"
                                                 : condition.parameter.c_str())) {
                for (int p = 0; p < graph_.parameters.Count(); ++p) {
                    const Parameter& parameter = graph_.parameters.At(p);
                    if (ImGui::Selectable(parameter.name.c_str(),
                                          parameter.name == condition.parameter)) {
                        condition.parameter = parameter.name;
                        graphDirty_ = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            int comparison = static_cast<int>(condition.comparison);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
            if (ImGui::Combo("##cmp", &comparison, kComparisons.data(),
                             static_cast<int>(kComparisons.size()))) {
                condition.comparison = static_cast<Comparison>(comparison);
                graphDirty_ = true;
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 3.5f);
            if (ImGui::DragFloat("##value", &condition.value, 0.01f)) {
                graphDirty_ = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(icons::kDelete)) {
                transition.conditions.erase(transition.conditions.begin() +
                                            static_cast<std::ptrdiff_t>(i));
                graphDirty_ = true;
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        if (ImGui::Button((std::string(icons::kAdd) + "  Condition").c_str())) {
            transition.conditions.push_back(Condition{});
            graphDirty_ = true;
        }
    }

    void RebindGraph() {
        library_.Invalidate();
        if (skeleton_.BoneCount() > 0) {
            instance_.Bind(graph_, skeleton_, library_);
        }
        activeLayer_ = std::clamp(activeLayer_, 0, std::max(graph_.LayerCount() - 1, 0));
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

    void CreateGraph(EditorContext& context) {
        const std::filesystem::path folder =
            std::filesystem::path(context.assets->Root()) / "Animation";
        std::filesystem::path path = folder / "NewGraph.simanimgraph";
        int suffix = 0;
        while (std::filesystem::exists(path)) {
            path = folder / ("NewGraph" + std::to_string(++suffix) + ".simanimgraph");
        }

        AnimationGraph fresh;
        fresh.name = path.stem().string();
        fresh.skeleton = AssetRef{skeletonGuid_};
        Layer layer;
        layer.name = "Base";
        fresh.AddLayer(layer);

        const AnimationIoResult result = SaveGraph(fresh, path);
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
            OpenGraph(context, record->guid);
        }
    }

    bool OpenAsset(const Uuid& guid, EditorContext& context) override {
        if (context.assets == nullptr) {
            return false;
        }
        const assets::AssetRecord* record = context.assets->Find(guid);
        if (record == nullptr) {
            return false;
        }
        // Tiga jenis aset, tiga jalur buka — dan tipe asetnya yang membedakan,
        // bukan tebakan dari namanya.
        switch (record->type) {
            case assets::AssetType::AnimationClip:
                OpenClip(context, guid);
                return true;
            case assets::AssetType::Skeleton:
                OpenSkeleton(context, guid);
                return true;
            case assets::AssetType::AnimationGraph:
                OpenGraph(context, guid);
                return true;
            default:
                return false;
        }
    }

    void OpenGraph(EditorContext& context, const Uuid& guid) {
        const assets::AssetRecord* record = context.assets->Find(guid);
        if (record == nullptr) {
            return;
        }
        AnimationGraph loaded;
        const AnimationIoResult result =
            LoadGraph(loaded, context.assets->AbsolutePath(*record));
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot open " + record->name + ": " + result.error);
            }
            return;
        }
        graph_ = std::move(loaded);
        graphGuid_ = guid;
        graphName_ = record->name;
        graphPath_ = context.assets->AbsolutePath(*record);
        graphDirty_ = false;
        graphPlaying_ = false;
        activeLayer_ = 0;
        selectedTransition_ = -1;
        placedStates_.clear();
        pendingFit_ = true;
        if (graph_.LayerCount() == 0) {
            // Graph tanpa lapis tidak punya tempat menaruh state; satu lapis
            // dasar selalu ada, sama seperti layer dasar terrain.
            Layer layer;
            layer.name = "Base";
            graph_.AddLayer(layer);
        }
        if (!skeletonGuid_.IsValid() && graph_.skeleton.IsValid()) {
            OpenSkeleton(context, graph_.skeleton.guid);
        }
        library_.Bind(context.assets);
        RebindGraph();
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
        if (graphDirty_ && graphGuid_.IsValid()) {
            const AnimationIoResult result = SaveGraph(graph_, graphPath_);
            if (!result.ok) {
                if (context.notifications != nullptr) {
                    context.notifications->Error("Save failed: " + result.error);
                }
                return;
            }
            graphDirty_ = false;
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

    AnimationGraph graph_;
    AssetClipLibrary library_;
    GraphInstance instance_;
    NodeCanvas canvas_;
    bool canvasReady_ = false;
    Uuid graphGuid_;
    std::string graphName_;
    std::filesystem::path graphPath_;
    bool graphDirty_ = false;
    bool graphPlaying_ = false;
    int activeLayer_ = 0;
    int selectedTransition_ = -1;
    bool pendingFit_ = false;
    std::set<int> placedStates_;

    ImVec2 sheetOrigin_{0.0f, 0.0f};
    ImVec2 sheetSize_{0.0f, 0.0f};
    float sheetLabelWidth_ = 0.0f;
    float legendLeft_ = FLT_MAX;
};

}  // namespace

SIM_REGISTER_PANEL(AnimationEditorPanel, 31)

}  // namespace sim::editor
