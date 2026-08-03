#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/FrameLimiter.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/Widgets.h"
#include "Sim/Particle/ParticleEffect.h"
#include "Sim/Particle/ParticleSystem.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace sim::editor {
namespace {

using namespace sim::particle;

constexpr ImVec4 kHintColor(0.55f, 0.57f, 0.60f, 1.0f);
constexpr ImVec4 kWarnColor(0.95f, 0.68f, 0.25f, 1.0f);

constexpr std::array<ModuleKind, 7> kModules{
    ModuleKind::Spawn,  ModuleKind::Shape,     ModuleKind::Initial, ModuleKind::OverLifetime,
    ModuleKind::Force,  ModuleKind::Collision, ModuleKind::Render};

/// Penyunting efek partikel.
///
/// **Preview-nya digambar dengan draw list ImGui, bukan lewat IViewportRenderer.**
/// Bukan karena renderer tidak cukup, melainkan karena ia satu instance dengan
/// satu target render yang sudah dipakai panel Viewport — preview kedua akan
/// menimpa gambarnya. Dan untuk partikel, gambar 2D justru cukup jujur: sebuah
/// partikel adalah titik dengan ukuran dan warna, jadi yang terlihat di sini
/// benar-benar memperlihatkan bentuk semburan, gerak, kurva ukuran, dan gradient
/// warnanya. Yang belum: billboard bertekstur dan urutan tembus pandang, yang
/// memang milik E8.
///
/// **Simulasinya milik `Sim::Particle`, bukan panel ini.** Panel hanya
/// menentukan waktu mana yang ingin dilihat. Itu yang membuat apa yang terlihat
/// di preview dijamin sama dengan apa yang dijalankan runtime nanti.
class ParticleEditorPanel final : public Panel {
public:
    ParticleEditorPanel()
        : Panel(panel_id::kParticleEditor, std::string(icons::kParticleEditor) + "  Particle Editor",
                PanelCategory::Authoring) {}

    void OnDraw(EditorContext& context) override {
        if (context.assets == nullptr) {
            ImGui::TextDisabled("No asset database.");
            return;
        }
        Advance(context);

        DrawToolbar(context);
        ImGui::Separator();

        const float listWidth = ImGui::GetFontSize() * 10.0f;
        if (ImGui::BeginChild("##effects", ImVec2(listWidth, 0.0f), ImGuiChildFlags_ResizeX)) {
            DrawEffectList(context);
        }
        ImGui::EndChild();

        ImGui::SameLine();
        if (!openGuid_.IsValid()) {
            ImGui::TextColored(kHintColor, "Pick an effect on the left, or create one.");
            return;
        }

        const float avail = ImGui::GetContentRegionAvail().x;
        const float handle = ImGui::GetStyle().ItemSpacing.x;
        if (sideWidth_ <= 0.0f) {
            sideWidth_ = avail * 0.42f;
        }
        const float maxSide = std::max(avail - ImGui::GetFontSize() * 12.0f - handle, 0.0f);
        sideWidth_ = std::clamp(sideWidth_, std::min(ImGui::GetFontSize() * 14.0f, maxSide),
                                maxSide);

        if (ImGui::BeginChild("##preview",
                              ImVec2(std::max(avail - sideWidth_ - handle, 1.0f), 0.0f))) {
            DrawPreview();
            DrawTimeline();
            DrawStats();
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, 0.0f);
        DrawSplitter(handle);
        ImGui::SameLine(0.0f, 0.0f);

        if (ImGui::BeginChild("##modules", ImVec2(0.0f, 0.0f))) {
            DrawModules();
        }
        ImGui::EndChild();
    }

private:
    // --- waktu --------------------------------------------------------------

    /// Memajukan waktu preview. Dipisah dari penggambaran supaya simulasi tetap
    /// berjalan pada laju yang sama walau panel sedang tidak terlihat penuh.
    void Advance(EditorContext& context) {
        if (!openGuid_.IsValid()) {
            return;
        }
        if (playing_) {
            const float dt = context.frameLimiter != nullptr
                                 ? static_cast<float>(context.frameLimiter->LastDeltaSeconds())
                                 : 1.0f / 60.0f;
            time_ += dt * speed_;
            const float loopLength = LoopLength();
            if (loop_ && loopLength > 0.0f && time_ > loopLength) {
                time_ = std::fmod(time_, loopLength);
            }
        }
        system_.SimulateTo(time_);
    }

    /// Panjang satu putaran untuk timeline: durasi emitter ditambah umur
    /// terpanjang, sehingga partikel terakhir sempat mati sebelum diulang.
    ///
    /// Diambil dari emitter yang paling panjang, bukan dari emitter terpilih.
    /// Timeline milik efeknya: memutar ulang begitu emitter terpendek selesai
    /// akan memotong asap yang masih membubung dari emitter di sebelahnya.
    float LoopLength() const {
        float longest = 0.1f;
        for (const ParticleEmitter& emitter : system_.Effect().emitters) {
            if (!emitter.enabled) {
                continue;
            }
            const float life =
                emitter.initial.lifetime + std::abs(emitter.initial.lifetimeJitter);
            const float duration =
                emitter.spawn.duration > 0.0f ? emitter.spawn.duration : 2.0f;
            longest = std::max(longest, duration + life);
        }
        return longest;
    }

    // --- toolbar & daftar ----------------------------------------------------

    void DrawToolbar(EditorContext& context) {
        ImGui::BeginDisabled(!dirty_ || !openGuid_.IsValid());
        if (ImGui::Button((std::string(icons::kSave) + "  Save").c_str())) {
            Save(context);
        }
        ImGui::EndDisabled();

        if (!openGuid_.IsValid()) {
            return;
        }
        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "%s%s", openName_.c_str(), dirty_ ? " *" : "");
    }

    void DrawEffectList(EditorContext& context) {
        if (ImGui::Button("New Effect", ImVec2(-FLT_MIN, 0.0f))) {
            CreateEffect(context);
        }
        ImGui::Spacing();

        for (const assets::AssetRecord& record : context.assets->All()) {
            if (record.type != assets::AssetType::Particle) {
                continue;
            }
            if (ImGui::Selectable(record.name.c_str(), record.guid == openGuid_)) {
                Open(context, record.guid);
            }
        }
    }

    void DrawSplitter(float width) {
        ImGui::InvisibleButton("##split", ImVec2(width, -FLT_MIN));
        const bool active = ImGui::IsItemActive();
        const bool touched = active || ImGui::IsItemHovered();
        if (touched) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (active) {
            sideWidth_ -= ImGui::GetIO().MouseDelta.x;
        }
        if (touched) {
            ImGui::GetWindowDrawList()->AddRectFilled(
                ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                ImGui::GetColorU32(active ? ImGuiCol_SeparatorActive
                                          : ImGuiCol_SeparatorHovered));
        }
    }

    // --- preview -------------------------------------------------------------

    void DrawPreview() {
        const float height = std::max(ImGui::GetContentRegionAvail().y -
                                          ImGui::GetFrameHeightWithSpacing() * 4.0f,
                                      ImGui::GetFontSize() * 8.0f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const ImVec2 size(ImGui::GetContentRegionAvail().x, height);
        ImGui::InvisibleButton("##view", size);

        // Orbit dengan seretan kiri, zoom dengan roda — konvensi yang sama
        // dengan viewport 3D, supaya tidak ada yang perlu dipelajari ulang.
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            yaw_ += ImGui::GetIO().MouseDelta.x * 0.01f;
            pitch_ = std::clamp(pitch_ + ImGui::GetIO().MouseDelta.y * 0.01f, -1.4f, 1.4f);
        }
        if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
            distance_ = std::clamp(distance_ * (1.0f - ImGui::GetIO().MouseWheel * 0.1f), 1.0f,
                                   60.0f);
        }

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 end(origin.x + size.x, origin.y + size.y);
        draw->AddRectFilled(origin, end, ImGui::GetColorU32(ImVec4(0.10f, 0.11f, 0.13f, 1.0f)));
        draw->PushClipRect(origin, end, true);

        // Kamera orbit sederhana. Ditulis di sini dan bukan lewat Sim::Render
        // karena yang dibutuhkan hanya proyeksi titik — bukan sebuah renderer.
        const Vec3 eye(std::cos(pitch_) * std::sin(yaw_) * distance_,
                       std::sin(pitch_) * distance_ + 1.0f,
                       std::cos(pitch_) * std::cos(yaw_) * distance_);
        const Vec3 target(0.0f, 1.0f, 0.0f);
        const Vec3 forward = glm::normalize(target - eye);
        const Vec3 right = glm::normalize(glm::cross(forward, kUp));
        const Vec3 up = glm::cross(right, forward);
        const float focal = size.y * 0.9f;
        const ImVec2 center(origin.x + size.x * 0.5f, origin.y + size.y * 0.5f);

        const auto project = [&](const Vec3& world, ImVec2& outScreen, float& outDepth) {
            const Vec3 rel = world - eye;
            outDepth = glm::dot(rel, forward);
            if (outDepth < 0.05f) {
                return false;
            }
            outScreen = ImVec2(center.x + glm::dot(rel, right) / outDepth * focal,
                               center.y - glm::dot(rel, up) / outDepth * focal);
            return true;
        };

        // Bidang tanah sebagai acuan: tanpa itu, gerak partikel di ruang kosong
        // tidak punya skala maupun arah yang bisa dibaca.
        for (int i = -5; i <= 5; ++i) {
            const float f = static_cast<float>(i);
            ImVec2 a;
            ImVec2 b;
            float depthA = 0.0f;
            float depthB = 0.0f;
            if (project(Vec3(f, 0.0f, -5.0f), a, depthA) &&
                project(Vec3(f, 0.0f, 5.0f), b, depthB)) {
                draw->AddLine(a, b, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.06f)));
            }
            if (project(Vec3(-5.0f, 0.0f, f), a, depthA) &&
                project(Vec3(5.0f, 0.0f, f), b, depthB)) {
                draw->AddLine(a, b, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.06f)));
            }
        }

        // Jauh digambar lebih dulu. Tanpa pengurutan, partikel di belakang bisa
        // menutupi yang di depan dan gumpalannya terlihat berlubang.
        order_.clear();
        order_.reserve(system_.Particles().size());
        for (std::size_t i = 0; i < system_.Particles().size(); ++i) {
            ImVec2 screen;
            float depth = 0.0f;
            if (project(system_.Particles()[i].position, screen, depth)) {
                order_.push_back({depth, screen, i});
            }
        }
        std::sort(order_.begin(), order_.end(),
                  [](const Projected& a, const Projected& b) { return a.depth > b.depth; });

        for (const Projected& item : order_) {
            const Particle& particle = system_.Particles()[item.index];
            const float radius =
                std::clamp(particle.size * focal / item.depth * 0.5f, 1.0f, 200.0f);
            draw->AddCircleFilled(item.screen, radius,
                                  ImGui::GetColorU32(ImVec4(particle.color.x, particle.color.y,
                                                            particle.color.z, particle.color.w)));
        }

        draw->PopClipRect();
        draw->AddRect(origin, end, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)));
    }

    void DrawTimeline() {
        const char* playLabel = playing_ ? icons::kPause : icons::kPlay;
        if (ImGui::Button(playLabel, ImVec2(ImGui::GetFrameHeight() * 1.6f, 0.0f))) {
            playing_ = !playing_;
        }
        widgets::Tooltip(playing_ ? "Pause" : "Play");

        ImGui::SameLine();
        if (ImGui::Button(icons::kStop, ImVec2(ImGui::GetFrameHeight() * 1.6f, 0.0f))) {
            playing_ = false;
            time_ = 0.0f;
        }
        widgets::Tooltip("Restart");

        ImGui::SameLine();
        if (ImGui::Button("Step")) {
            // Satu langkah simulasi, bukan satu frame editor: yang ingin dilihat
            // penulis efek adalah kemajuan simulasinya sendiri.
            playing_ = false;
            time_ += ParticleSystem::kFixedStep;
        }

        ImGui::SameLine();
        ImGui::Checkbox("Loop", &loop_);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
        ImGui::DragFloat("Speed", &speed_, 0.01f, 0.05f, 4.0f, "%.2fx");

        const float length = std::max(LoopLength(), 0.1f);
        ImGui::SetNextItemWidth(-FLT_MIN);
        float scrub = std::min(time_, length);
        if (ImGui::SliderFloat("##time", &scrub, 0.0f, length, "%.2f s")) {
            // Menyeret penanda menghentikan pemutaran: melanjutkan berputar dari
            // tempat yang baru saja dilepas membuat penanda terlihat melompat.
            playing_ = false;
            time_ = scrub;
        }
    }

    void DrawStats() {
        const ParticleEffect& effect = system_.Effect();
        float rate = 0.0f;
        int live = 0;
        for (const ParticleEmitter& emitter : effect.emitters) {
            if (emitter.enabled && emitter.spawn.enabled) {
                rate += emitter.spawn.rate;
                ++live;
            }
        }
        ImGui::TextColored(kHintColor, "%zu alive   %.0f spawn/s   %d/%zu emitters   t=%.2fs",
                           system_.Particles().size(), rate, live, effect.emitters.size(),
                           system_.Time());
        if (system_.AtBudget()) {
            ImGui::SameLine();
            ImGui::TextColored(kWarnColor, "%s  budget reached", icons::kLogWarn);
        }
    }

    // --- modul ---------------------------------------------------------------

    void DrawModules() {
        ParticleEffect effect = system_.Effect();
        bool changed = false;

        changed |= DrawEmitterList(effect);
        if (effect.emitters.empty()) {
            ImGui::TextColored(kHintColor, "Add an emitter to start.");
        } else {
            emitterIndex_ =
                std::min(emitterIndex_, static_cast<int>(effect.emitters.size()) - 1);
            ImGui::Separator();
            changed |= DrawEmitter(effect.emitters[static_cast<std::size_t>(emitterIndex_)]);
        }

        if (changed) {
            // Menyetel efek me-reset simulasi, jadi waktunya dipulihkan: penulis
            // efek yang menggeser satu slider tidak ingin timeline-nya melompat
            // kembali ke nol.
            system_.SetEffect(effect);
            system_.SimulateTo(time_);
            dirty_ = true;
        }
    }

    /// Daftar emitter efek ini, dan tombol untuk menyusunnya.
    ///
    /// Disusun sebagai baris bertumpuk ke bawah, bukan kolom berdampingan.
    /// Alasannya bukan selera: tumpukan modul di bawahnya panjang, dan daftar
    /// yang tumbuh ke arah yang sama dengan isinya bisa berbagi satu batang
    /// gulir — sementara kolom berdampingan memaksa memilih antara nama emitter
    /// yang terpotong atau lebar panel yang habis sebelum modulnya terlihat.
    bool DrawEmitterList(ParticleEffect& effect) {
        bool changed = false;
        const auto selected = static_cast<std::size_t>(std::max(emitterIndex_, 0));

        if (ImGui::Button(icons::kAdd)) {
            ParticleEmitter& added = effect.emitters.emplace_back();
            added.name = "Emitter " + std::to_string(effect.emitters.size());
            added.seed = effect.NextSeed();
            SeedDefaultLook(added);
            emitterIndex_ = static_cast<int>(effect.emitters.size()) - 1;
            changed = true;
        }
        widgets::Tooltip("Add emitter");

        ImGui::SameLine();
        ImGui::BeginDisabled(effect.emitters.empty());
        if (ImGui::Button(icons::kDuplicate)) {
            ParticleEmitter copy = effect.emitters[selected];
            copy.name += " Copy";
            // Benihnya diganti. Salinan yang berbenih sama menghasilkan partikel
            // yang bertumpuk tepat di atas aslinya — dan yang terlihat hanyalah
            // satu emitter yang lebih terang, bukan dua.
            copy.seed = effect.NextSeed();
            effect.emitters.insert(effect.emitters.begin() +
                                       static_cast<std::ptrdiff_t>(selected) + 1,
                                   std::move(copy));
            emitterIndex_ = static_cast<int>(selected) + 1;
            changed = true;
        }
        widgets::Tooltip("Duplicate emitter");

        ImGui::SameLine();
        if (ImGui::Button(icons::kDelete)) {
            effect.emitters.erase(effect.emitters.begin() +
                                  static_cast<std::ptrdiff_t>(selected));
            emitterIndex_ = std::max(0, static_cast<int>(selected) - 1);
            changed = true;
        }
        widgets::Tooltip("Remove emitter");

        ImGui::SameLine();
        ImGui::BeginDisabled(selected == 0);
        if (ImGui::ArrowButton("##up", ImGuiDir_Up)) {
            std::swap(effect.emitters[selected], effect.emitters[selected - 1]);
            emitterIndex_ = static_cast<int>(selected) - 1;
            changed = true;
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(selected + 1 >= effect.emitters.size());
        if (ImGui::ArrowButton("##down", ImGuiDir_Down)) {
            std::swap(effect.emitters[selected], effect.emitters[selected + 1]);
            emitterIndex_ = static_cast<int>(selected) + 1;
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();

        // Tinggi daftar mengikuti isinya sampai enam baris, lalu menggulir.
        // Daftar setinggi tetap membuang ruang pada efek satu emitter — yaitu
        // bentuk yang paling sering disunting.
        const float rows = std::clamp(static_cast<float>(effect.emitters.size()), 1.0f, 6.0f);
        const ImVec2 listSize(0.0f, rows * ImGui::GetFrameHeightWithSpacing() +
                                        ImGui::GetStyle().FramePadding.y * 2.0f);
        if (ImGui::BeginChild("##emitters", listSize, ImGuiChildFlags_Borders)) {
            for (std::size_t i = 0; i < effect.emitters.size(); ++i) {
                ParticleEmitter& emitter = effect.emitters[i];
                ImGui::PushID(static_cast<int>(i));

                if (ImGui::Checkbox("##on", &emitter.enabled)) {
                    changed = true;
                }
                widgets::Tooltip("Disabling an emitter keeps its settings");
                ImGui::SameLine();

                const ParticleSystem::EmitterStats stats = system_.StatsFor(i);
                if (ImGui::Selectable(emitter.name.c_str(),
                                      i == static_cast<std::size_t>(emitterIndex_),
                                      ImGuiSelectableFlags_AllowOverlap)) {
                    emitterIndex_ = static_cast<int>(i);
                }

                // Jumlah partikel per emitter, rata kanan. Pada efek berlapis,
                // "sudah mentok" tanpa menyebut emitter mana tidak menolong.
                char count[32];
                std::snprintf(count, sizeof(count), "%u", stats.alive);
                const float countWidth = ImGui::CalcTextSize(count).x;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - countWidth);
                ImGui::TextColored(stats.atBudget ? kWarnColor : kHintColor, "%s", count);

                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        return changed;
    }

    bool DrawEmitter(ParticleEmitter& emitter) {
        bool changed = false;

        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        changed |= ImGui::InputText("Name", &emitter.name);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
        changed |= ImGui::InputInt("Max", &emitter.maxParticles, 0);
        widgets::Tooltip("Particle budget for this emitter");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.0f);
        int seed = static_cast<int>(emitter.seed);
        if (ImGui::InputInt("Seed", &seed, 0)) {
            emitter.seed = static_cast<uint32_t>(std::max(0, seed));
            changed = true;
        }
        ImGui::Separator();

        for (const ModuleKind kind : kModules) {
            ImGui::PushID(static_cast<int>(kind));
            bool enabled = emitter.IsEnabled(kind);
            if (ImGui::Checkbox("##enabled", &enabled)) {
                emitter.SetEnabled(kind, enabled);
                changed = true;
            }
            widgets::Tooltip("Disabling a module keeps its settings");
            ImGui::SameLine();
            // Modul yang dimatikan tetap bisa dibuka dan disunting: itu bagian
            // dari janji bahwa mematikannya tidak menghapus apa pun.
            const bool open = ImGui::CollapsingHeader(ToString(kind));
            if (open) {
                // Isi modul mendapat lingkup ID sendiri. Tanpa ini, sebuah widget
                // yang labelnya sama dengan nama modulnya — combo "Shape" di
                // dalam modul "Shape" — berbagi ID dengan header di atasnya, dan
                // ImGui mengirim kliknya ke salah satu saja: comboonya tidak
                // pernah terbuka, headernya yang menutup.
                ImGui::PushID("body");
                ImGui::Indent();
                changed |= DrawModuleBody(kind, emitter);
                ImGui::Unindent();
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        return changed;
    }

    bool DrawModuleBody(ModuleKind kind, ParticleEmitter& effect) {
        bool changed = false;
        const float width = ImGui::GetFontSize() * 8.0f;
        switch (kind) {
            case ModuleKind::Spawn:
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat("Rate", &effect.spawn.rate, 0.5f, 0.0f, 100000.0f);
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragInt("Burst", &effect.spawn.burstCount, 1.0f, 0, 100000);
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat("Burst time", &effect.spawn.burstTime, 0.01f, 0.0f,
                                            60.0f);
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat("Duration", &effect.spawn.duration, 0.05f, 0.0f,
                                            60.0f);
                changed |= ImGui::Checkbox("Looping", &effect.spawn.looping);
                break;
            case ModuleKind::Shape: {
                int shape = static_cast<int>(effect.shape.shape);
                ImGui::SetNextItemWidth(width);
                if (ImGui::Combo("Shape", &shape, "Point\0Sphere\0Box\0Cone\0")) {
                    effect.shape.shape = static_cast<EmitterShape>(shape);
                    changed = true;
                }
                changed |= widgets::DragVec3("size", &effect.shape.size.x).edited;
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::SliderAngle("Cone angle", &effect.shape.coneAngle, 0.0f, 89.0f);
                changed |= ImGui::Checkbox("Emit from shell", &effect.shape.emitFromShell);
                break;
            }
            case ModuleKind::Initial:
                ImGui::TextColored(kHintColor, "Velocity");
                changed |= widgets::DragVec3("vel", &effect.initial.velocity.x).edited;
                ImGui::TextColored(kHintColor, "Velocity jitter");
                changed |= widgets::DragVec3("veljit", &effect.initial.velocityJitter.x).edited;
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat("Size", &effect.initial.size, 0.005f, 0.0f, 100.0f);
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat("Size jitter", &effect.initial.sizeJitter, 0.005f,
                                            0.0f, 100.0f);
                changed |= ImGui::ColorEdit3("Color", &effect.initial.color.x);
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat("Lifetime", &effect.initial.lifetime, 0.02f, 0.01f,
                                            60.0f);
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat("Lifetime jitter", &effect.initial.lifetimeJitter,
                                            0.02f, 0.0f, 60.0f);
                break;
            case ModuleKind::OverLifetime: {
                const ImVec2 curveSize(ImGui::GetContentRegionAvail().x - 8.0f,
                                       ImGui::GetFontSize() * 5.0f);
                changed |= DrawCurveRow("Size", effect.overLifetime.sizeOverLife, curveSize);
                changed |= DrawCurveRow("Velocity", effect.overLifetime.velocityScale, curveSize);
                changed |= DrawCurveRow("Rotation", effect.overLifetime.rotationRate, curveSize);
                ImGui::TextColored(kHintColor, "Color over life");
                changed |= widgets::GradientEditor("colorlife", effect.overLifetime.colorOverLife,
                                                   ImVec2(curveSize.x,
                                                          ImGui::GetFontSize() * 3.0f));
                break;
            }
            case ModuleKind::Force:
                ImGui::TextColored(kHintColor, "Gravity");
                changed |= widgets::DragVec3("grav", &effect.force.gravity.x).edited;
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat("Drag", &effect.force.drag, 0.01f, 0.0f, 20.0f);
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat("Vortex", &effect.force.vortexStrength, 0.05f, -50.0f,
                                            50.0f);
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat("Attractor", &effect.force.attractorStrength, 0.05f,
                                            -50.0f, 50.0f);
                ImGui::TextColored(kHintColor, "Attractor position");
                changed |= widgets::DragVec3("attr", &effect.force.attractorPosition.x).edited;
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat("Noise", &effect.force.noiseStrength, 0.05f, 0.0f,
                                            50.0f);
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat("Noise freq", &effect.force.noiseFrequency, 0.05f,
                                            0.01f, 20.0f);
                break;
            case ModuleKind::Collision:
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat("Plane Y", &effect.collision.planeHeight, 0.05f,
                                            -50.0f, 50.0f);
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::SliderFloat("Bounce", &effect.collision.bounce, 0.0f, 1.0f);
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::SliderFloat("Friction", &effect.collision.friction, 0.0f, 1.0f);
                changed |= ImGui::Checkbox("Kill on contact", &effect.collision.killOnContact);
                break;
            case ModuleKind::Render: {
                int mode = static_cast<int>(effect.render.mode);
                ImGui::SetNextItemWidth(width);
                if (ImGui::Combo("Mode", &mode, "Billboard\0Stretched\0Mesh\0")) {
                    effect.render.mode = static_cast<RenderMode>(mode);
                    changed = true;
                }
                ImGui::SetNextItemWidth(width);
                changed |= ImGui::DragFloat("Stretch", &effect.render.stretchScale, 0.01f, 0.0f,
                                            20.0f);
                changed |= ImGui::Checkbox("Sort by distance", &effect.render.sortByDistance);
                ImGui::TextColored(kHintColor,
                                   "Material and mesh are assigned by dropping assets here"
                                   " once E8 can draw them.");
                break;
            }
        }
        return changed;
    }

    bool DrawCurveRow(const char* label, Curve& curve, const ImVec2& size) {
        ImGui::PushID(label);
        ImGui::TextColored(kHintColor, "%s over life", label);
        ImGui::SameLine();
        bool changed = false;
        if (ImGui::SmallButton("presets")) {
            ImGui::OpenPopup("##presets");
        }
        if (ImGui::BeginPopup("##presets")) {
            for (const widgets::CurvePreset preset :
                 {widgets::CurvePreset::Constant, widgets::CurvePreset::Linear,
                  widgets::CurvePreset::EaseIn, widgets::CurvePreset::EaseOut,
                  widgets::CurvePreset::EaseInOut, widgets::CurvePreset::Spike}) {
                if (ImGui::Selectable(widgets::ToString(preset))) {
                    widgets::ApplyPreset(curve, preset);
                    changed = true;
                }
            }
            ImGui::EndPopup();
        }
        float low = 0.0f;
        float high = 1.0f;
        curve.ValueRange(low, high);
        changed |= widgets::CurveEditor("curve", curve, size, low, high);
        ImGui::PopID();
        return changed;
    }

    // --- berkas ---------------------------------------------------------------

    /// Rupa bawaan emitter baru: nyala hangat yang memudar dan mengecil.
    ///
    /// Emitter baru harus langsung terlihat. Nilai bawaan yang menghasilkan
    /// partikel tak kasatmata membuat orang mengira editornya rusak, dan
    /// menghabiskan menit pertama untuk mencari tahu kenapa layarnya kosong
    /// alih-alih menyusun efeknya.
    static void SeedDefaultLook(ParticleEmitter& emitter) {
        emitter.overLifetime.colorOverLife.AddColorStop(0.0f, Vec3(1.0f, 0.95f, 0.8f));
        emitter.overLifetime.colorOverLife.AddColorStop(1.0f, Vec3(1.0f, 0.4f, 0.1f));
        emitter.overLifetime.colorOverLife.AddAlphaStop(0.0f, 1.0f);
        emitter.overLifetime.colorOverLife.AddAlphaStop(1.0f, 0.0f);
        widgets::ApplyPreset(emitter.overLifetime.sizeOverLife, widgets::CurvePreset::Spike);
    }

    void CreateEffect(EditorContext& context) {
        const std::filesystem::path folder =
            std::filesystem::path(context.assets->Root()) / "Effects";
        std::filesystem::path path = folder / "NewEffect.simfx";
        int suffix = 0;
        while (std::filesystem::exists(path)) {
            path = folder / ("NewEffect" + std::to_string(++suffix) + ".simfx");
        }

        ParticleEffect fresh;
        fresh.name = path.stem().string();
        SeedDefaultLook(fresh.emitters.emplace_back());

        if (!SaveEffectToFile(fresh, path).ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot create " + path.filename().string());
            }
            return;
        }
        context.assets->ScanNow();
        if (const assets::AssetRecord* record =
                context.assets->FindByRelativePath("Effects/" + path.filename().string())) {
            Open(context, record->guid);
        }
    }

    void Open(EditorContext& context, const Uuid& guid) {
        const assets::AssetRecord* record = context.assets->Find(guid);
        if (record == nullptr) {
            return;
        }
        ParticleEffect loaded;
        const EffectIoResult result =
            LoadEffectFromFile(loaded, context.assets->AbsolutePath(*record));
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot open " + record->name + ": " + result.error);
            }
            return;
        }
        system_.SetEffect(loaded);
        emitterIndex_ = 0;
        openGuid_ = guid;
        openName_ = record->name;
        openPath_ = context.assets->AbsolutePath(*record);
        dirty_ = false;
        time_ = 0.0f;
        playing_ = true;
    }

    void Save(EditorContext& context) {
        if (!SaveEffectToFile(system_.Effect(), openPath_).ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Save failed: " + openName_);
            }
            return;
        }
        dirty_ = false;
        if (context.notifications != nullptr) {
            context.notifications->Success("Saved " + openName_);
        }
    }

    struct Projected {
        float depth = 0.0f;
        ImVec2 screen{0.0f, 0.0f};
        std::size_t index = 0;
    };

    ParticleSystem system_;
    std::vector<Projected> order_;

    Uuid openGuid_;
    std::string openName_;
    std::filesystem::path openPath_;
    bool dirty_ = false;

    float time_ = 0.0f;
    float speed_ = 1.0f;
    bool playing_ = true;
    bool loop_ = true;
    float sideWidth_ = 0.0f;
    int emitterIndex_ = 0;

    float yaw_ = 0.6f;
    float pitch_ = 0.25f;
    float distance_ = 8.0f;
};

}  // namespace

SIM_REGISTER_PANEL(ParticleEditorPanel, 28)

}  // namespace sim::editor
