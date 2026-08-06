#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/Widgets.h"
#include "Sim/Terrain/TerrainBrush.h"
#include "Sim/Terrain/TerrainIo.h"
#include "Sim/Vegetation/Vegetation.h"
#include "Sim/Vegetation/VegetationBrush.h"
#include "Sim/Vegetation/VegetationIo.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace sim::editor {
namespace {

using namespace sim::vegetation;

constexpr ImVec4 kHintColor(0.55f, 0.57f, 0.60f, 1.0f);
constexpr ImVec4 kWarnColor(0.95f, 0.68f, 0.25f, 1.0f);

/// Kelompok alat. Tiga tab dengan alasan yang sama seperti tab penyunting
/// terrain: masing-masing menyunting hal yang berbeda dengan parameter yang
/// berbeda, dan satu papan parameter bersama berarti slider yang artinya
/// berganti diam-diam.
enum class Mode {
    Layers,
    Density,
    Instances,
};

/// Alat per-instance. Menanam adalah klik, menghapus adalah seretan — lihat
/// catatan di `HandleInstances`.
enum class InstanceTool {
    Plant,
    Erase,
};

/// Apa yang diwarnai peta 2D.
enum class MapView {
    Relief,
    Density,
};

/// Warna bawaan layer baru, berputar menurut indeksnya — alasannya sama dengan
/// palet layer terrain: dua layer sewarna membuat peta tidak bisa dibaca tepat
/// pada saat warna adalah satu-satunya cara membacanya.
constexpr std::array<std::array<float, 3>, 6> kLayerPalette{{
    {0.35f, 0.62f, 0.30f},
    {0.78f, 0.62f, 0.24f},
    {0.28f, 0.45f, 0.55f},
    {0.66f, 0.34f, 0.42f},
    {0.52f, 0.70f, 0.86f},
    {0.85f, 0.86f, 0.62f},
}};

/// Penyunting vegetasi.
///
/// **Petanya dari atas, dengan alasan yang sama seperti penyunting terrain**:
/// belum ada yang bisa menggambar mesh sampai E8, dan alat yang hasilnya tidak
/// terlihat tidak bisa diuji maupun dipakai. Untuk vegetasi pandangan dari atas
/// bahkan lebih berguna daripada untuk terrain — yang ingin dinilai orang adalah
/// *sebarannya*: di mana rapat, di mana kosong, apakah tepi hutan mengikuti
/// lereng. Semua itu justru hilang dalam perspektif.
///
/// **Reliefnya digambar abu-abu, bukan dengan gradasi topografi seperti di
/// penyunting terrain.** Bukan penyederhanaan: di sini warna sudah punya
/// pekerjaan lain, yaitu membedakan layer vegetasi. Latar berwarna membuat titik
/// hijau di atas lereng hijau tidak terlihat sama sekali, dan itu satu-satunya
/// hal yang panel ini harus perlihatkan.
///
/// **Suntingan tidak melewati CommandHistory**, sama seperti penyunting Material,
/// Graph, Script, dan Terrain — riwayat utama menjanjikan pembatalan perubahan
/// *scene*, sedangkan ini dokumen yang dibuka dan ditutup. Undo goresan dilayani
/// jurnal milik `Vegetation`.
class VegetationEditorPanel final : public Panel {
public:
    VegetationEditorPanel()
        : Panel(panel_id::kVegetationEditor,
                std::string(icons::kVegetationEditor) + "  Vegetation Editor",
                PanelCategory::Authoring) {}

    void OnDraw(EditorContext& context) override {
        if (context.assets == nullptr) {
            ImGui::TextDisabled("No asset database.");
            return;
        }

        DrawToolbar(context);
        ImGui::Separator();

        const float listWidth = ImGui::GetFontSize() * 10.0f;
        if (ImGui::BeginChild("##vegetations", ImVec2(listWidth, 0.0f), ImGuiChildFlags_ResizeX)) {
            DrawVegetationList(context);
        }
        ImGui::EndChild();

        ImGui::SameLine();
        if (!openGuid_.IsValid()) {
            ImGui::TextColored(kHintColor, "Pick a vegetation set on the left, or create one.");
            return;
        }

        const float avail = ImGui::GetContentRegionAvail().x;
        const float handle = ImGui::GetStyle().ItemSpacing.x;
        if (sideWidth_ <= 0.0f) {
            sideWidth_ = avail * 0.36f;
        }
        const float maxSide = std::max(avail - ImGui::GetFontSize() * 12.0f - handle, 0.0f);
        sideWidth_ = std::clamp(sideWidth_, std::min(ImGui::GetFontSize() * 14.0f, maxSide),
                                maxSide);

        if (ImGui::BeginChild("##map",
                              ImVec2(std::max(avail - sideWidth_ - handle, 1.0f), 0.0f))) {
            DrawMap(context);
            DrawStatus();
        }
        ImGui::EndChild();

        ImGui::SameLine(0.0f, 0.0f);
        DrawSplitter(handle);
        ImGui::SameLine(0.0f, 0.0f);

        if (ImGui::BeginChild("##tools", ImVec2(0.0f, 0.0f))) {
            DrawTools(context);
        }
        ImGui::EndChild();
    }

private:
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
        ImGui::BeginDisabled(vegetation_.UndoDepth() == 0);
        if (ImGui::Button(icons::kUndo)) {
            vegetation_.Undo();
            dirty_ = true;
        }
        ImGui::EndDisabled();
        widgets::Tooltip("Undo stroke (Ctrl+Z)");

        ImGui::SameLine();
        ImGui::BeginDisabled(vegetation_.RedoDepth() == 0);
        if (ImGui::Button(icons::kRedo)) {
            vegetation_.Redo();
            dirty_ = true;
        }
        ImGui::EndDisabled();
        widgets::Tooltip("Redo stroke (Ctrl+Shift+Z)");

        ImGui::SameLine();
        ImGui::BeginDisabled(!hasTerrain_ || vegetation_.LayerCount() == 0);
        if (ImGui::Button((std::string(icons::kScatter) + "  Scatter").c_str())) {
            ScatterAll();
        }
        ImGui::EndDisabled();
        widgets::Tooltip("Rebuild every layer from its rules and seed");

        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "%s%s", openName_.c_str(), dirty_ ? " *" : "");

        // Pintasan hanya berlaku saat panel ini yang fokus, dengan alasan yang
        // sama seperti di penyunting terrain: Ctrl+Z global milik riwayat scene.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::GetIO().KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                if (ImGui::GetIO().KeyShift) {
                    vegetation_.Redo();
                } else {
                    vegetation_.Undo();
                }
                dirty_ = true;
            }
        }
    }

    void DrawVegetationList(EditorContext& context) {
        if (ImGui::Button("New Vegetation", ImVec2(-FLT_MIN, 0.0f))) {
            newTerrain_ = Uuid{};
            ImGui::OpenPopup("##newveg");
        }
        DrawNewVegetationPopup(context);
        ImGui::Spacing();

        for (const assets::AssetRecord& record : context.assets->All()) {
            if (record.type != assets::AssetType::Vegetation) {
                continue;
            }
            if (ImGui::Selectable(record.name.c_str(), record.guid == openGuid_)) {
                Open(context, record.guid);
            }
        }
    }

    void DrawNewVegetationPopup(EditorContext& context) {
        if (!ImGui::BeginPopup("##newveg")) {
            return;
        }
        ImGui::TextColored(kHintColor, "New vegetation set");
        ImGui::Separator();

        // Terrainnya dipilih sebelum apa pun yang lain, dan tanpa itu tombol
        // Create mati. Vegetasi tanpa permukaan tidak punya tempat berdiri —
        // menawarkan membuatnya dulu lalu memintanya menyusul berarti dokumen
        // yang sah tapi tidak bisa dipakai untuk apa pun.
        const assets::AssetRecord* picked =
            newTerrain_.IsValid() ? context.assets->Find(newTerrain_) : nullptr;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::BeginCombo("Terrain", picked != nullptr ? picked->name.c_str() : "(pick one)")) {
            for (const assets::AssetRecord& record : context.assets->All()) {
                if (record.type != assets::AssetType::Terrain) {
                    continue;
                }
                if (ImGui::Selectable(record.name.c_str(), record.guid == newTerrain_)) {
                    newTerrain_ = record.guid;
                }
            }
            ImGui::EndCombo();
        }

        ImGui::BeginDisabled(!newTerrain_.IsValid());
        if (ImGui::Button("Create", ImVec2(ImGui::GetFontSize() * 6.0f, 0.0f))) {
            CreateVegetation(context);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
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

    // --- peta ----------------------------------------------------------------

    ImVec2 WorldToScreen(float worldX, float worldZ) const {
        return ImVec2(mapOrigin_.x + mapSize_.x * 0.5f + (worldX - viewX_) / metersPerPixel_,
                      mapOrigin_.y + mapSize_.y * 0.5f + (worldZ - viewZ_) / metersPerPixel_);
    }

    void ScreenToWorld(const ImVec2& screen, float& worldX, float& worldZ) const {
        worldX = viewX_ + (screen.x - mapOrigin_.x - mapSize_.x * 0.5f) * metersPerPixel_;
        worldZ = viewZ_ + (screen.y - mapOrigin_.y - mapSize_.y * 0.5f) * metersPerPixel_;
    }

    void FitView() {
        viewX_ = terrain_.WorldWidth() * 0.5f;
        viewZ_ = terrain_.WorldDepth() * 0.5f;
        metersPerPixel_ = 0.0f;  // dihitung saat ukuran peta diketahui
    }

    static float Hillshade(float slopeX, float slopeZ) {
        const Vec3 normal = glm::normalize(Vec3(-slopeX, 1.0f, -slopeZ));
        const Vec3 light = glm::normalize(Vec3(-0.6f, 0.7f, -0.4f));
        return 0.45f + 0.75f * std::clamp(glm::dot(normal, light), 0.0f, 1.0f);
    }

    static ImU32 Tint(const Vec3& albedo, float shade) {
        return ImGui::GetColorU32(ImVec4(std::min(albedo.x * shade, 1.0f),
                                         std::min(albedo.y * shade, 1.0f),
                                         std::min(albedo.z * shade, 1.0f), 1.0f));
    }

    void DrawTerrainRelief(ImDrawList* draw) {
        const int cols = std::clamp(static_cast<int>(mapSize_.x / 7.0f), 8, 96);
        const int rows = std::clamp(static_cast<int>(mapSize_.y / 7.0f), 8, 96);

        heights_.assign(static_cast<std::size_t>(cols + 1) * static_cast<std::size_t>(rows + 1),
                        0.0f);
        inside_.assign(heights_.size(), 0);
        float low = FLT_MAX;
        float high = -FLT_MAX;
        for (int j = 0; j <= rows; ++j) {
            for (int i = 0; i <= cols; ++i) {
                const ImVec2 screen(mapOrigin_.x + mapSize_.x * static_cast<float>(i) /
                                                       static_cast<float>(cols),
                                    mapOrigin_.y + mapSize_.y * static_cast<float>(j) /
                                                       static_cast<float>(rows));
                float wx = 0.0f;
                float wz = 0.0f;
                ScreenToWorld(screen, wx, wz);
                const std::size_t at =
                    static_cast<std::size_t>(j) * static_cast<std::size_t>(cols + 1) +
                    static_cast<std::size_t>(i);
                heights_[at] = terrain_.HeightAtWorld(wx, wz);
                inside_[at] = (wx >= 0.0f && wz >= 0.0f && wx <= terrain_.WorldWidth() &&
                               wz <= terrain_.WorldDepth())
                                  ? 1
                                  : 0;
                if (inside_[at] != 0) {
                    low = std::min(low, heights_[at]);
                    high = std::max(high, heights_[at]);
                }
            }
        }
        if (low > high) {
            low = terrain_.Desc().minHeight;
            high = terrain_.Desc().maxHeight;
        }
        const float quantum = (terrain_.Desc().maxHeight - terrain_.Desc().minHeight) / 65535.0f;
        const float range = std::max(high - low, quantum * 8.0f);

        const float pixelsPerCellX = mapSize_.x / static_cast<float>(cols);
        const float pixelsPerCellZ = mapSize_.y / static_cast<float>(rows);
        const float pixelsPerRange = 0.18f * std::min(mapSize_.x, mapSize_.y);
        const auto heightAt = [&](int i, int j) {
            return heights_[static_cast<std::size_t>(std::clamp(j, 0, rows)) *
                                static_cast<std::size_t>(cols + 1) +
                            static_cast<std::size_t>(std::clamp(i, 0, cols))];
        };

        const int vtxCount = (cols + 1) * (rows + 1);
        const int idxCount = cols * rows * 6;
        draw->PrimReserve(idxCount, vtxCount);
        const auto base = static_cast<ImDrawIdx>(draw->_VtxCurrentIdx);
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();

        for (int j = 0; j <= rows; ++j) {
            for (int i = 0; i <= cols; ++i) {
                const ImVec2 screen(mapOrigin_.x + mapSize_.x * static_cast<float>(i) /
                                                       static_cast<float>(cols),
                                    mapOrigin_.y + mapSize_.y * static_cast<float>(j) /
                                                       static_cast<float>(rows));
                const std::size_t at =
                    static_cast<std::size_t>(j) * static_cast<std::size_t>(cols + 1) +
                    static_cast<std::size_t>(i);
                ImU32 color;
                if (inside_[at] == 0) {
                    color = ImGui::GetColorU32(ImVec4(0.09f, 0.10f, 0.12f, 1.0f));
                } else {
                    const float scale = pixelsPerRange / range;
                    const float slopeX = (heightAt(i + 1, j) - heightAt(i - 1, j)) * scale /
                                         (2.0f * pixelsPerCellX);
                    const float slopeZ = (heightAt(i, j + 1) - heightAt(i, j - 1)) * scale /
                                         (2.0f * pixelsPerCellZ);
                    const float shade = Hillshade(slopeX, slopeZ);
                    if (view_ == MapView::Density && activeLayer_ < vegetation_.LayerCount()) {
                        // Peta kepadatan diwarnai dengan warna layernya, bukan
                        // abu-abu ke putih — sama seperti peta bobot terrain,
                        // dan karena alasan yang sama: gradasi netral membuat
                        // dua layer tidak bisa dibedakan di tangkapan layar mana
                        // pun, termasuk di kepala orang yang baru berpindah
                        // layer.
                        float wx = 0.0f;
                        float wz = 0.0f;
                        ScreenToWorld(screen, wx, wz);
                        const float density =
                            std::clamp(vegetation_.Density(activeLayer_).SampleWorld(wx, wz), 0.0f,
                                       1.0f);
                        const Vec3 dark(0.07f, 0.07f, 0.09f);
                        color = Tint(dark + (vegetation_.Layer(activeLayer_).color - dark) * density,
                                     shade);
                    } else {
                        // Relief abu-abu. Tinggi tetap ikut menaikkan
                        // kecerahannya sedikit, supaya lembah dan punggungan
                        // masih bisa dibedakan ketika lerengnya menghadap
                        // cahaya dari arah yang sama.
                        const float unit = std::clamp((heights_[at] - low) / range, 0.0f, 1.0f);
                        const float grey = 0.26f + 0.20f * unit;
                        color = Tint(Vec3(grey, grey, grey * 1.04f), shade);
                    }
                }
                draw->PrimWriteVtx(screen, uv, color);
            }
        }
        for (int j = 0; j < rows; ++j) {
            for (int i = 0; i < cols; ++i) {
                const auto a = static_cast<ImDrawIdx>(base + j * (cols + 1) + i);
                const auto b = static_cast<ImDrawIdx>(a + 1);
                const auto c = static_cast<ImDrawIdx>(a + cols + 2);
                const auto d = static_cast<ImDrawIdx>(a + cols + 1);
                draw->PrimWriteIdx(a);
                draw->PrimWriteIdx(b);
                draw->PrimWriteIdx(c);
                draw->PrimWriteIdx(a);
                draw->PrimWriteIdx(c);
                draw->PrimWriteIdx(d);
            }
        }
    }

    /// Instance digambar dengan langkah yang melebar mengikuti berapa banyak
    /// yang sedang terlihat.
    ///
    /// Sejuta titik adalah sejuta perintah gambar, dan tidak ada panel yang
    /// bertahan pada angka itu. Langkahnya diambil dari **perkiraan jumlah yang
    /// terlihat**, bukan dari jumlah seluruhnya: dengan langkah tetap, memperbesar
    /// ke satu rumpun akan tetap membuang 49 dari 50 pohonnya walaupun yang
    /// tersisa di layar tinggal seratus. Berapa yang dilewati disebutkan di baris
    /// status, karena peta yang menggambar sebagian dan diam soal itu adalah peta
    /// yang berbohong tentang kerapatan — satu-satunya hal yang dinilai orang di
    /// sini.
    void DrawInstances(ImDrawList* draw) {
        float minX = 0.0f;
        float minZ = 0.0f;
        float maxX = 0.0f;
        float maxZ = 0.0f;
        ScreenToWorld(mapOrigin_, minX, minZ);
        ScreenToWorld(ImVec2(mapOrigin_.x + mapSize_.x, mapOrigin_.y + mapSize_.y), maxX, maxZ);

        const double worldArea = std::max(static_cast<double>(terrain_.WorldWidth()) *
                                              static_cast<double>(terrain_.WorldDepth()),
                                          1.0);
        const double visibleArea = std::max(static_cast<double>(maxX - minX) *
                                                static_cast<double>(maxZ - minZ),
                                            0.0);
        const double fraction = std::clamp(visibleArea / worldArea, 0.0, 1.0);

        drawnStep_ = 1;
        drawnCount_ = 0;
        for (int layer = 0; layer < vegetation_.LayerCount(); ++layer) {
            if (!vegetation_.Layer(layer).visible) {
                continue;
            }
            const std::vector<Instance>& instances = vegetation_.Instances(layer);
            if (instances.empty()) {
                continue;
            }
            const double estimate = static_cast<double>(instances.size()) * fraction;
            const int step = std::max(
                1, static_cast<int>(std::ceil(estimate / static_cast<double>(kMaxDots))));
            drawnStep_ = std::max(drawnStep_, step);

            const Vec3& albedo = vegetation_.Layer(layer).color;
            const float highlight = layer == activeLayer_ ? 1.0f : 0.65f;
            const ImU32 color = ImGui::GetColorU32(ImVec4(albedo.x, albedo.y, albedo.z, highlight));
            const float radius =
                std::clamp(vegetation_.Layer(layer).rules.minDistance * 0.3f / metersPerPixel_,
                           1.0f, 9.0f);

            for (std::size_t i = 0; i < instances.size(); i += static_cast<std::size_t>(step)) {
                const Instance& instance = instances[i];
                if (instance.position.x < minX || instance.position.x > maxX ||
                    instance.position.z < minZ || instance.position.z > maxZ) {
                    continue;
                }
                const ImVec2 centre = WorldToScreen(instance.position.x, instance.position.z);
                if (radius <= 1.5f) {
                    // Persegi satu piksel, bukan lingkaran: pada zoom jauh
                    // sebuah lingkaran adalah selusin vertex untuk menggambar
                    // titik yang sama.
                    draw->AddRectFilled(centre, ImVec2(centre.x + 1.5f, centre.y + 1.5f), color);
                } else {
                    draw->AddCircleFilled(centre, radius * instance.scale, color, 8);
                }
                ++drawnCount_;
            }
        }
    }

    void DrawMap(EditorContext& context) {
        const float height = std::max(ImGui::GetContentRegionAvail().y -
                                          ImGui::GetFrameHeightWithSpacing() * 2.0f,
                                      ImGui::GetFontSize() * 8.0f);
        mapOrigin_ = ImGui::GetCursorScreenPos();
        mapSize_ = ImVec2(ImGui::GetContentRegionAvail().x, height);
        if (metersPerPixel_ <= 0.0f) {
            metersPerPixel_ = std::max(terrain_.WorldWidth() / std::max(mapSize_.x, 1.0f),
                                       terrain_.WorldDepth() / std::max(mapSize_.y, 1.0f));
        }

        ImGui::InvisibleButton("##map", mapSize_,
                               ImGuiButtonFlags_MouseButtonLeft |
                                   ImGuiButtonFlags_MouseButtonMiddle |
                                   ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        HandleInput(context, hovered);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(mapOrigin_,
                           ImVec2(mapOrigin_.x + mapSize_.x, mapOrigin_.y + mapSize_.y), true);
        DrawTerrainRelief(draw);
        DrawInstances(draw);
        DrawCursor(draw, hovered);
        draw->PopClipRect();
        draw->AddRect(mapOrigin_, ImVec2(mapOrigin_.x + mapSize_.x, mapOrigin_.y + mapSize_.y),
                      ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)));
    }

    void DrawCursor(ImDrawList* draw, bool hovered) {
        if (mode_ == Mode::Layers) {
            return;
        }
        if (!hovered && !stroke_.Active()) {
            return;
        }
        const ImVec2 centre = WorldToScreen(cursorX_, cursorZ_);
        const float radius = CursorRadius() / metersPerPixel_;
        draw->AddCircle(centre, radius, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.85f)), 0,
                        1.5f);
        if (mode_ == Mode::Density) {
            const float inner = radius * (1.0f - std::clamp(paint_.falloff, 0.0f, 1.0f));
            if (inner > 2.0f) {
                draw->AddCircle(centre, inner, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.35f)));
            }
        } else if (tool_ == InstanceTool::Plant) {
            // Menanam adalah satu klik, jadi kursornya menandai satu titik, bukan
            // sebuah daerah. Lingkaran seukuran jari-jari kuas di sini akan
            // menjanjikan serumpun pohon yang tidak akan muncul.
            draw->AddCircleFilled(centre, 3.0f,
                                  ImGui::GetColorU32(ImVec4(0.55f, 0.90f, 0.45f, 1.0f)));
        }
    }

    void HandleInput(EditorContext& context, bool hovered) {
        const ImGuiIO& io = ImGui::GetIO();
        if (hovered || stroke_.Active()) {
            ScreenToWorld(io.MousePos, cursorX_, cursorZ_);
        }

        if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                                      ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
            viewX_ -= io.MouseDelta.x * metersPerPixel_;
            viewZ_ -= io.MouseDelta.y * metersPerPixel_;
        }
        if (hovered && io.MouseWheel != 0.0f) {
            float beforeX = 0.0f;
            float beforeZ = 0.0f;
            ScreenToWorld(io.MousePos, beforeX, beforeZ);
            metersPerPixel_ = std::clamp(metersPerPixel_ * (1.0f - io.MouseWheel * 0.12f),
                                         terrain_.Desc().sampleSpacing * 0.05f,
                                         std::max(terrain_.WorldWidth(), 1.0f) / 64.0f);
            float afterX = 0.0f;
            float afterZ = 0.0f;
            ScreenToWorld(io.MousePos, afterX, afterZ);
            viewX_ += beforeX - afterX;
            viewZ_ += beforeZ - afterZ;
        }

        if (mode_ == Mode::Layers || vegetation_.LayerCount() == 0 || !hasTerrain_) {
            return;
        }
        const float dt = context.deltaSeconds > 0.0f ? context.deltaSeconds : 1.0f / 60.0f;
        if (mode_ == Mode::Instances) {
            HandleInstances(hovered, dt);
            return;
        }

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            vegetation_.BeginStroke();
            stroke_.Begin(cursorX_, cursorZ_);
        }
        if (!stroke_.Active()) {
            return;
        }
        // Shift membalik tujuannya dan Ctrl meratakan — pengubah yang sama persis
        // dengan kuas terrain, supaya tidak ada yang perlu dipelajari dua kali.
        PaintBrush brush = paint_;
        if (io.KeyShift) {
            brush.target = 1.0f - brush.target;
        }
        const bool smooth = io.KeyCtrl;
        const int layer = activeLayer_;
        stroke_.Advance(brush.radius, cursorX_, cursorZ_, dt, [&](float x, float z, float step) {
            if (smooth) {
                ApplySmoothDab(vegetation_, brush, layer, x, z, step);
            } else {
                ApplyDensityDab(vegetation_, brush, layer, x, z, step);
            }
        });
        dirty_ = true;
        densityDirty_ = true;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            stroke_.End();
            vegetation_.EndStroke();
        }
    }

    /// Menanam adalah klik, menghapus adalah seretan.
    ///
    /// Bukan ketidaklengkapan: kuas yang menanam sambil diseret harus tahu
    /// apakah sudah ada sesuatu di dekat tempat sentuhannya, dan menjawab itu
    /// pada daftar berisi sejuta instance menuntut indeks spasial yang jalur
    /// prosedural sama sekali tidak membutuhkannya — sebarannya sendiri menjaga
    /// jaraknya lewat kisi yang hanya hidup selama penyebaran. Menanam tanpa
    /// pemeriksaan itu akan menumpuk puluhan pohon di satu titik pada seretan
    /// paling pelan sekalipun. Yang memang dipakai orang untuk menanam banyak
    /// adalah tombol Scatter; yang ditanam tangan adalah beberapa pohon di
    /// tempat tertentu, dan untuk itu klik justru lebih tepat.
    void HandleInstances(bool hovered, float dt) {
        const bool erasing =
            tool_ == InstanceTool::Erase ? !ImGui::GetIO().KeyShift : ImGui::GetIO().KeyShift;

        if (!erasing) {
            if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                vegetation_.BeginStroke();
                ApplyPlantDab(vegetation_, terrain_, activeLayer_, cursorX_, cursorZ_);
                vegetation_.EndStroke();
                dirty_ = true;
            }
            return;
        }

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            vegetation_.BeginStroke();
            stroke_.Begin(cursorX_, cursorZ_);
        }
        if (!stroke_.Active()) {
            return;
        }
        const int layer = activeLayer_;
        const float radius = eraseRadius_;
        stroke_.Advance(radius, cursorX_, cursorZ_, dt, [&](float x, float z, float) {
            ApplyEraseDab(vegetation_, layer, x, z, radius);
        });
        dirty_ = true;
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            stroke_.End();
            vegetation_.EndStroke();
        }
    }

    float CursorRadius() const { return mode_ == Mode::Density ? paint_.radius : eraseRadius_; }

    void DrawStatus() {
        ImGui::TextColored(kHintColor, "%.1f, %.1f m   h=%.2f m", cursorX_, cursorZ_,
                           terrain_.HeightAtWorld(cursorX_, cursorZ_));
        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "|  %zu instances", vegetation_.InstanceCount());
        ImGui::SameLine();
        if (drawnStep_ > 1) {
            ImGui::TextColored(kWarnColor, "(showing 1 in %d)", drawnStep_);
        } else {
            ImGui::TextColored(kHintColor, "(%zu drawn)", drawnCount_);
        }
        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "|  %.1f MB   %zu undo",
                           static_cast<double>(vegetation_.BytesResident()) / (1024.0 * 1024.0),
                           vegetation_.UndoDepth());

        if (densityDirty_) {
            // Mengecat kepadatan tidak langsung menumbuhkan atau mencabut apa
            // pun, dan diam soal itu berarti kuas yang terasa rusak. Sebaran
            // ulang harus utuh — lihat catatan di `Vegetation` — jadi yang bisa
            // dilakukan panel adalah menyebutkannya, bukan menyembunyikannya
            // dengan sebaran sepotong yang diam-diam berbeda.
            ImGui::TextColored(kWarnColor, "%s  Density edited — Scatter to apply",
                               icons::kScatter);
        } else if (lastScatterMs_ > 0.0) {
            ImGui::TextColored(kHintColor, "Last scatter: %zu instances in %.0f ms",
                               lastScatterCount_, lastScatterMs_);
        }
    }

    // --- alat ----------------------------------------------------------------

    void DrawTools(EditorContext& context) {
        if (ImGui::BeginTabBar("##modes")) {
            if (ImGui::BeginTabItem((std::string(icons::kLayers) + "  Layers").c_str())) {
                SetMode(Mode::Layers);
                DrawLayerList(context);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem((std::string(icons::kBrush) + "  Density").c_str())) {
                SetMode(Mode::Density);
                DrawDensityTools();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem((std::string(icons::kPlant) + "  Instances").c_str())) {
                SetMode(Mode::Instances);
                DrawInstanceTools();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Spacing();
        ImGui::Separator();
        DrawTerrainSection(context);
        DrawViewSection();
    }

    void SetMode(Mode mode) {
        if (mode_ == mode) {
            return;
        }
        if (stroke_.Active()) {
            stroke_.End();
            vegetation_.EndStroke();
        }
        mode_ = mode;
        // Peta ikut berpindah ke tampilan yang cocok dengan alatnya, tapi hanya
        // saat tabnya berganti — memaksanya setiap frame berarti pilihan
        // tampilan pengguna dibatalkan sedetik setelah dipilih.
        view_ = mode == Mode::Density ? MapView::Density : MapView::Relief;
    }

    void DrawLayerList(EditorContext& context) {
        ImGui::BeginDisabled(vegetation_.LayerCount() >= kMaxLayers);
        if (ImGui::Button((std::string(icons::kAdd) + "  Add layer").c_str())) {
            AddLayer();
        }
        ImGui::EndDisabled();

        const float rowHeight = ImGui::GetFrameHeightWithSpacing();
        if (ImGui::BeginChild("##layers", ImVec2(0.0f, rowHeight * 5.0f),
                              ImGuiChildFlags_Borders)) {
            for (int index = 0; index < vegetation_.LayerCount(); ++index) {
                ImGui::PushID(index);
                VegetationLayer& layer = vegetation_.Layer(index);

                if (ImGui::Checkbox("##visible", &layer.visible)) {
                    dirty_ = true;
                }
                widgets::Tooltip("Draw this layer on the map");
                ImGui::SameLine();

                const ImVec2 size(ImGui::GetFrameHeight() * 0.6f, ImGui::GetFrameHeight() * 0.6f);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(at.x, at.y + ImGui::GetStyle().FramePadding.y),
                    ImVec2(at.x + size.x, at.y + size.y + ImGui::GetStyle().FramePadding.y),
                    ImGui::GetColorU32(ImVec4(layer.color.x, layer.color.y, layer.color.z, 1.0f)));
                ImGui::Dummy(size);
                ImGui::SameLine();

                const std::string label =
                    layer.name + "  (" + std::to_string(vegetation_.Instances(index).size()) + ")";
                if (ImGui::Selectable(label.c_str(), index == activeLayer_)) {
                    activeLayer_ = index;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        if (vegetation_.LayerCount() == 0) {
            ImGui::TextColored(kHintColor, "Add a layer to start.");
            return;
        }
        activeLayer_ = std::clamp(activeLayer_, 0, vegetation_.LayerCount() - 1);

        ImGui::BeginDisabled(activeLayer_ == 0);
        if (ImGui::Button(icons::kChevronUp)) {
            vegetation_.MoveLayer(activeLayer_, activeLayer_ - 1);
            --activeLayer_;
            dirty_ = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(activeLayer_ + 1 >= vegetation_.LayerCount());
        if (ImGui::Button(icons::kChevronDown)) {
            vegetation_.MoveLayer(activeLayer_, activeLayer_ + 1);
            ++activeLayer_;
            dirty_ = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(icons::kDelete)) {
            vegetation_.RemoveLayer(activeLayer_);
            activeLayer_ = std::max(activeLayer_ - 1, 0);
            dirty_ = true;
            return;
        }
        widgets::Tooltip("Remove this layer, its instances, and its density map");

        ImGui::Separator();
        DrawLayerProperties(context);
    }

    void DrawLayerProperties(EditorContext& context) {
        VegetationLayer& layer = vegetation_.Layer(activeLayer_);
        const float width = ImGui::GetFontSize() * 7.0f;

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##name", &layer.name)) {
            dirty_ = true;
        }
        DrawModelPicker(context, layer);
        if (ImGui::ColorEdit3("Map colour", &layer.color.x, ImGuiColorEditFlags_NoInputs)) {
            dirty_ = true;
        }

        if (ImGui::CollapsingHeader("Placement", ImGuiTreeNodeFlags_DefaultOpen)) {
            PlacementRules& rules = layer.rules;
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloat("Spacing", &rules.minDistance, 0.05f, kMinInstanceDistance,
                                 500.0f, "%.2f m")) {
                dirty_ = true;
            }
            widgets::Tooltip("Minimum distance between instances. Changing this rebuilds the whole "
                             "layout, so hand edits keyed to old positions are lost.");

            ImGui::SetNextItemWidth(width);
            if (ImGui::SliderFloat("Density", &rules.density, 0.0f, 1.0f)) {
                dirty_ = true;
            }
            widgets::Tooltip("Fraction of the packed layout that is actually planted");

            ImGui::SetNextItemWidth(width);
            auto seed = static_cast<int>(rules.seed);
            if (ImGui::InputInt("Seed", &seed, 0)) {
                rules.seed = static_cast<uint32_t>(std::max(seed, 0));
                dirty_ = true;
            }
            ImGui::SameLine();
            if (ImGui::Button(icons::kSeed)) {
                // Benih baru diambil dari benih sekarang, bukan dari jam:
                // dokumen yang sama harus menghasilkan sebaran yang sama di
                // mesin mana pun, dan itu mustahil kalau ada satu tombol yang
                // menyuntikkan waktu ke dalamnya.
                Rng rng(CellSeed(rules.seed, 1, 1));
                rules.seed = rng.NextU32();
                dirty_ = true;
            }
            widgets::Tooltip("Reroll the seed");

            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloatRange2("Height", &rules.minHeight, &rules.maxHeight, 0.5f,
                                       -10000.0f, 10000.0f, "%.0f m", "%.0f m")) {
                dirty_ = true;
            }
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloatRange2("Slope", &rules.minSlopeDegrees, &rules.maxSlopeDegrees,
                                       0.25f, 0.0f, 90.0f, "%.0f deg", "%.0f deg")) {
                dirty_ = true;
            }

            DrawTerrainLayerPicker(rules);
            if (ImGui::Checkbox("Avoid holes", &rules.avoidHoles)) {
                dirty_ = true;
            }
        }

        if (ImGui::CollapsingHeader("Instance", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloatRange2("Scale", &layer.minScale, &layer.maxScale, 0.01f, 0.01f,
                                       20.0f, "%.2f", "%.2f")) {
                dirty_ = true;
            }
            if (ImGui::Checkbox("Random yaw", &layer.randomYaw)) {
                dirty_ = true;
            }
            ImGui::SetNextItemWidth(width);
            if (ImGui::SliderFloat("Align to normal", &layer.alignToNormal, 0.0f, 1.0f)) {
                dirty_ = true;
            }
            widgets::Tooltip("0 keeps instances upright, 1 tips them to follow the surface");
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloat("Ground offset", &layer.offsetY, 0.01f, -10.0f, 10.0f, "%.2f m")) {
                dirty_ = true;
            }
            widgets::Tooltip("Negative sinks the base into the ground so it never floats on a "
                             "slope");
        }

        if (ImGui::CollapsingHeader("Distance")) {
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloat("LOD", &layer.lodDistance, 1.0f, 1.0f, 100000.0f, "%.0f m")) {
                dirty_ = true;
            }
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloat("Billboard", &layer.billboardDistance, 1.0f, 1.0f, 100000.0f,
                                 "%.0f m")) {
                dirty_ = true;
            }
            ImGui::SetNextItemWidth(width);
            if (ImGui::DragFloat("Cull", &layer.cullDistance, 1.0f, 1.0f, 100000.0f, "%.0f m")) {
                dirty_ = true;
            }
            ImGui::TextColored(kHintColor, "Used by the renderer in E8.");
        }

        ImGui::Separator();
        ImGui::TextColored(kHintColor, "%zu planted by hand, %zu erased by hand",
                           vegetation_.AddedCount(activeLayer_),
                           vegetation_.RemovedCount(activeLayer_));
        ImGui::BeginDisabled(vegetation_.AddedCount(activeLayer_) == 0 &&
                             vegetation_.RemovedCount(activeLayer_) == 0);
        if (ImGui::Button("Drop hand edits")) {
            vegetation_.ClearManual(activeLayer_);
            ScatterLayer(activeLayer_);
            dirty_ = true;
        }
        ImGui::EndDisabled();
    }

    void DrawModelPicker(EditorContext& context, VegetationLayer& layer) {
        const assets::AssetRecord* current =
            layer.model.IsValid() ? context.assets->Find(layer.model.guid) : nullptr;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (!ImGui::BeginCombo("##model", current != nullptr ? current->name.c_str() : "(no mesh)")) {
            return;
        }
        if (ImGui::Selectable("(no mesh)", !layer.model.IsValid())) {
            layer.model.Clear();
            dirty_ = true;
        }
        for (const assets::AssetRecord& record : context.assets->All()) {
            if (record.type != assets::AssetType::Mesh &&
                record.type != assets::AssetType::Prefab) {
                continue;
            }
            if (ImGui::Selectable(record.name.c_str(), record.guid == layer.model.guid)) {
                layer.model = AssetRef{record.guid};
                dirty_ = true;
            }
        }
        ImGui::EndCombo();
    }

    void DrawTerrainLayerPicker(PlacementRules& rules) {
        const char* preview = rules.terrainLayer < 0 || rules.terrainLayer >= terrain_.LayerCount()
                                  ? "(any)"
                                  : terrain_.Layer(rules.terrainLayer).name.c_str();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (ImGui::BeginCombo("Terrain layer", preview)) {
            if (ImGui::Selectable("(any)", rules.terrainLayer < 0)) {
                rules.terrainLayer = -1;
                dirty_ = true;
            }
            for (int index = 0; index < terrain_.LayerCount(); ++index) {
                if (ImGui::Selectable(terrain_.Layer(index).name.c_str(),
                                      index == rules.terrainLayer)) {
                    rules.terrainLayer = index;
                    dirty_ = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::BeginDisabled(rules.terrainLayer < 0);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (ImGui::SliderInt("Min weight", &rules.minTerrainWeight, 0, 255)) {
            dirty_ = true;
        }
        ImGui::EndDisabled();
        widgets::Tooltip("How much of that painted terrain layer must be under an instance");
    }

    void DrawDensityTools() {
        if (vegetation_.LayerCount() == 0) {
            ImGui::TextColored(kHintColor, "Add a layer first.");
            return;
        }
        ImGui::TextColored(kHintColor,
                           "Drag to thin out %s.\nHold Shift to fill back in, Ctrl to even out.",
                           vegetation_.Layer(activeLayer_).name.c_str());
        ImGui::Spacing();

        const float width = ImGui::GetFontSize() * 7.0f;
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("Radius", &paint_.radius, 0.25f, 0.5f, 2000.0f, "%.1f m");
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("Strength", &paint_.strength, 0.05f, 0.1f, 60.0f, "%.2f /s");
        ImGui::SetNextItemWidth(width);
        ImGui::SliderFloat("Falloff", &paint_.falloff, 0.0f, 1.0f);
        ImGui::SetNextItemWidth(width);
        ImGui::SliderFloat("Target", &paint_.target, 0.0f, 1.0f);
        widgets::Tooltip("Density the brush converges to at its centre");
        DrawBrushProfile();

        ImGui::Spacing();
        float cellSize = vegetation_.DensityCellSize();
        ImGui::SetNextItemWidth(width);
        if (ImGui::DragFloat("Cell size", &cellSize, 0.05f, 0.25f, 64.0f, "%.2f m")) {
            vegetation_.SetDensityCellSize(cellSize);
            vegetation_.Fit(terrain_);
            dirty_ = true;
        }
        widgets::Tooltip("Resolution of the painted density grid. Changing it drops the paint — "
                         "resampling would move the forest edge with nobody moving it.");
        ImGui::TextColored(kHintColor, "%d x %d cells   %.2f MB", vegetation_.DensityWidth(),
                           vegetation_.DensityHeight(),
                           static_cast<double>(vegetation_.Density(activeLayer_).Bytes()) /
                               (1024.0 * 1024.0));

        ImGui::BeginDisabled(!vegetation_.Density(activeLayer_).Painted());
        if (ImGui::Button("Clear painted density")) {
            vegetation_.BeginStroke();
            vegetation_.ClearDensity(activeLayer_);
            vegetation_.EndStroke();
            dirty_ = true;
            densityDirty_ = true;
        }
        ImGui::EndDisabled();
    }

    void DrawInstanceTools() {
        if (vegetation_.LayerCount() == 0) {
            ImGui::TextColored(kHintColor, "Add a layer first.");
            return;
        }
        int tool = static_cast<int>(tool_);
        if (ImGui::RadioButton("Plant", &tool, 0)) {
            tool_ = InstanceTool::Plant;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Erase", &tool, 1)) {
            tool_ = InstanceTool::Erase;
        }
        ImGui::TextColored(kHintColor,
                           tool_ == InstanceTool::Plant
                               ? "Click to plant one. Hold Shift to erase."
                               : "Drag to erase. Hold Shift to plant with a click.");
        ImGui::Spacing();

        // Jari-jarinya tetap bisa disetel di mode Plant: Shift menghapus dari
        // sana juga, dan slider yang mati tepat pada alat yang sedang dipakai
        // adalah slider yang harus dicari di tab lain.
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        ImGui::DragFloat("Erase radius", &eraseRadius_, 0.25f, 0.5f, 2000.0f, "%.1f m");

        ImGui::Spacing();
        ImGui::TextColored(kHintColor,
                           "Hand edits survive a scatter and every rule change except\n"
                           "spacing and seed, which move every instance.");
    }

    /// Profil kuas, digambar dengan `BrushWeight` — fungsi yang sama persis yang
    /// menyunting peta kepadatan. Profil yang digambar ulang dengan rumus kedua
    /// adalah profil yang akan berbeda dari yang sebenarnya terjadi.
    void DrawBrushProfile() {
        const ImVec2 size(ImGui::GetContentRegionAvail().x - widgets::kPanelRightMargin,
                          ImGui::GetFontSize() * 2.6f);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy(size);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                            ImGui::GetColorU32(ImVec4(0.10f, 0.11f, 0.13f, 1.0f)));
        ImVec2 previous(origin.x, origin.y + size.y);
        constexpr int kSteps = 48;
        for (int i = 0; i <= kSteps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSteps);
            const float unit = std::abs(t * 2.0f - 1.0f);
            const float weight = terrain::BrushWeight(paint_, unit * paint_.radius);
            const ImVec2 point(origin.x + size.x * t, origin.y + size.y * (1.0f - weight));
            if (i > 0) {
                draw->AddLine(previous, point,
                              ImGui::GetColorU32(ImVec4(0.55f, 0.80f, 0.45f, 1.0f)), 2.0f);
            }
            previous = point;
        }
        draw->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)));
    }

    void DrawTerrainSection(EditorContext& context) {
        if (!ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }
        ImGui::TextColored(kHintColor, "%s   %.0f x %.0f m", terrainName_.c_str(),
                           static_cast<double>(terrain_.WorldWidth()),
                           static_cast<double>(terrain_.WorldDepth()));

        // Terrain yang berubah di bawah vegetasi tidak diambil diam-diam. Memuat
        // ulang dokumen orang lain tanpa diminta adalah kejutan; yang bisa
        // dilakukan panel adalah memperlihatkan bahwa berkasnya sudah bergerak.
        const assets::AssetRecord* record =
            document_.terrain.IsValid() ? context.assets->Find(document_.terrain.guid) : nullptr;
        const bool stale = record != nullptr && record->modifiedSeconds != terrainStamp_;
        if (stale) {
            ImGui::TextColored(kWarnColor, "%s  Terrain changed on disk", icons::kRefresh);
        }
        if (ImGui::Button((std::string(icons::kRefresh) + "  Reload & reseat").c_str())) {
            ReloadTerrain(context);
        }
        widgets::Tooltip("Re-read the terrain, then drop every instance back onto the new surface "
                         "without moving it sideways");
        if (reseated_ > 0) {
            ImGui::TextColored(kHintColor, "%zu instances reseated", reseated_);
        }
    }

    void DrawViewSection() {
        static constexpr std::array<const char*, 2> kViews{"Relief", "Density"};
        int view = static_cast<int>(view_);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (ImGui::Combo("Map", &view, kViews.data(), static_cast<int>(kViews.size()))) {
            view_ = static_cast<MapView>(view);
        }
        if (ImGui::Button("Fit view")) {
            FitView();
        }
    }

    void AddLayer() {
        VegetationLayer layer;
        const int index = vegetation_.LayerCount();
        layer.name = "Layer " + std::to_string(index + 1);
        const std::array<float, 3>& rgb =
            kLayerPalette[static_cast<std::size_t>(index) % kLayerPalette.size()];
        layer.color = Vec3(rgb[0], rgb[1], rgb[2]);
        // Benih bawaan mengikuti indeksnya, bukan sama untuk semua layer: dua
        // layer berbenih sama dengan jarak minimum sama akan tumbuh di titik yang
        // sama persis, dan hasilnya adalah dua hutan yang saling menembus.
        layer.rules.seed = 1u + static_cast<uint32_t>(index) * 7919u;
        const int added = vegetation_.AddLayer(layer);
        if (added < 0) {
            return;
        }
        activeLayer_ = added;
        dirty_ = true;
        ScatterLayer(added);
    }

    void ScatterLayer(int layer) {
        if (!hasTerrain_ || layer < 0 || layer >= vegetation_.LayerCount()) {
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        lastScatterCount_ = vegetation_.Scatter(terrain_, layer);
        lastScatterMs_ = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    }

    void ScatterAll() {
        if (!hasTerrain_) {
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        lastScatterCount_ = vegetation_.ScatterAll(terrain_);
        lastScatterMs_ = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
        densityDirty_ = false;
        // Angka "berapa yang ditempelkan ulang" milik pemuatan terrain terakhir.
        // Setelah sebaran ulang ia tidak lagi menggambarkan apa pun yang ada di
        // layar.
        reseated_ = 0;
    }

    // --- berkas ---------------------------------------------------------------

    void CreateVegetation(EditorContext& context) {
        const std::filesystem::path folder =
            std::filesystem::path(context.assets->Root()) / "Vegetation";
        std::filesystem::path path = folder / "NewVegetation.simveg";
        int suffix = 0;
        while (std::filesystem::exists(path)) {
            path = folder / ("NewVegetation" + std::to_string(++suffix) + ".simveg");
        }

        Vegetation fresh;
        VegetationDocument document;
        document.name = path.stem().string();
        document.terrain = AssetRef{newTerrain_};

        const VegetationIoResult result = SaveVegetation(fresh, document, path);
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot create " + path.filename().string() + ": " +
                                             result.error);
            }
            return;
        }
        context.assets->ScanNow();
        if (const assets::AssetRecord* record =
                context.assets->FindByRelativePath("Vegetation/" + path.filename().string())) {
            Open(context, record->guid);
        }
    }

    void Open(EditorContext& context, const Uuid& guid) {
        const assets::AssetRecord* record = context.assets->Find(guid);
        if (record == nullptr) {
            return;
        }
        Vegetation loaded;
        VegetationDocument document;
        const VegetationIoResult result =
            LoadVegetation(loaded, document, context.assets->AbsolutePath(*record));
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot open " + record->name + ": " + result.error);
            }
            return;
        }
        vegetation_ = std::move(loaded);
        document_ = std::move(document);
        openGuid_ = guid;
        openName_ = record->name;
        openPath_ = context.assets->AbsolutePath(*record);
        dirty_ = false;
        densityDirty_ = false;
        reseated_ = 0;
        activeLayer_ = 0;
        lastScatterMs_ = 0.0;

        LoadTerrainFor(context);
        FitView();
    }

    void LoadTerrainFor(EditorContext& context) {
        hasTerrain_ = false;
        terrainName_ = "(no terrain)";
        terrainStamp_ = 0;
        const assets::AssetRecord* record =
            document_.terrain.IsValid() ? context.assets->Find(document_.terrain.guid) : nullptr;
        if (record == nullptr) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Vegetation " + openName_ +
                                             " points at a terrain that is not in the project.");
            }
            return;
        }
        terrain::TerrainDocument terrainDocument;
        const terrain::TerrainIoResult result =
            terrain::LoadTerrain(terrain_, terrainDocument, context.assets->AbsolutePath(*record));
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot open " + record->name + ": " + result.error);
            }
            return;
        }
        hasTerrain_ = true;
        terrainName_ = record->name;
        terrainStamp_ = record->modifiedSeconds;

        // Kisi kepadatan disesuaikan sebelum menyebar. Peta yang ukurannya tidak
        // cocok dengan terrainnya dibuang di sini, bukan diam-diam dicuplik
        // lain — lihat catatan pada `Vegetation::Fit`.
        vegetation_.Fit(terrain_);
        // Muat langsung menyebar: dokumen yang terbuka dengan peta kosong sampai
        // sebuah tombol ditekan terbaca sebagai dokumen yang rusak.
        ScatterAll();
    }

    void ReloadTerrain(EditorContext& context) {
        if (!document_.terrain.IsValid()) {
            return;
        }
        const assets::AssetRecord* record = context.assets->Find(document_.terrain.guid);
        if (record == nullptr) {
            return;
        }
        terrain::TerrainDocument terrainDocument;
        terrain::Terrain reloaded;
        const terrain::TerrainIoResult result =
            terrain::LoadTerrain(reloaded, terrainDocument, context.assets->AbsolutePath(*record));
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot reload " + record->name + ": " + result.error);
            }
            return;
        }
        terrain_ = std::move(reloaded);
        hasTerrain_ = true;
        terrainStamp_ = record->modifiedSeconds;
        vegetation_.Fit(terrain_);
        // **Ditempelkan ulang, bukan disebar ulang.** Menyebar ulang di sini akan
        // mengganti hutan yang sudah dinilai orang dengan hutan lain hanya karena
        // sebuah bukit di ujung peta bergeser. Yang berubah di bawah vegetasi
        // adalah tingginya, dan hanya itu yang ikut berubah.
        reseated_ = vegetation_.RefreshHeights(terrain_);
        dirty_ = true;
    }

    void Save(EditorContext& context) {
        const VegetationIoResult result = SaveVegetation(vegetation_, document_, openPath_);
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Save failed: " + result.error);
            }
            return;
        }
        dirty_ = false;
        if (context.notifications != nullptr) {
            context.notifications->Success("Saved " + openName_);
        }
    }

    /// Titik terbanyak yang digambar peta dalam satu frame.
    static constexpr int kMaxDots = 24000;

    Vegetation vegetation_;
    VegetationDocument document_;
    terrain::Terrain terrain_;
    bool hasTerrain_ = false;
    std::string terrainName_ = "(no terrain)";
    std::int64_t terrainStamp_ = 0;
    std::size_t reseated_ = 0;

    Uuid openGuid_;
    Uuid newTerrain_;
    std::string openName_;
    std::filesystem::path openPath_;
    bool dirty_ = false;
    bool densityDirty_ = false;

    Mode mode_ = Mode::Layers;
    MapView view_ = MapView::Relief;
    InstanceTool tool_ = InstanceTool::Plant;
    int activeLayer_ = 0;
    terrain::PaintBrush paint_{20.0f, 6.0f, 0.5f, 0.0f};
    float eraseRadius_ = 10.0f;
    terrain::BrushStroke stroke_;

    std::size_t lastScatterCount_ = 0;
    double lastScatterMs_ = 0.0;
    int drawnStep_ = 1;
    std::size_t drawnCount_ = 0;

    ImVec2 mapOrigin_{0.0f, 0.0f};
    ImVec2 mapSize_{0.0f, 0.0f};
    float viewX_ = 0.0f;
    float viewZ_ = 0.0f;
    float metersPerPixel_ = 0.0f;
    float cursorX_ = 0.0f;
    float cursorZ_ = 0.0f;
    float sideWidth_ = 0.0f;

    std::vector<float> heights_;
    std::vector<uint8_t> inside_;
};

}  // namespace

SIM_REGISTER_PANEL(VegetationEditorPanel, 30)

}  // namespace sim::editor
