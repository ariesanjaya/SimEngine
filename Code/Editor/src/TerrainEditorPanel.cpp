#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/Widgets.h"
#include "Sim/Terrain/Terrain.h"
#include "Sim/Terrain/TerrainBrush.h"
#include "Sim/Terrain/TerrainIo.h"

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

using namespace sim::terrain;

constexpr ImVec4 kHintColor(0.55f, 0.57f, 0.60f, 1.0f);
constexpr ImVec4 kWarnColor(0.95f, 0.68f, 0.25f, 1.0f);

/// Alat yang bisa dipilih. Ramp bukan `BrushKind` karena ia ditentukan dua
/// titik, bukan satu — jadi ia mode interaksi tersendiri, bukan sekadar rumus
/// lain di bawah kursor.
enum class Tool {
    Raise,
    Lower,
    Flatten,
    Smooth,
    Noise,
    Ramp,
};

struct ToolEntry {
    Tool tool;
    const char* label;
    const char* hint;
};

constexpr std::array<ToolEntry, 6> kTools{{
    {Tool::Raise, "Raise", "Drag to raise. Hold Shift to lower."},
    {Tool::Lower, "Lower", "Drag to lower. Hold Shift to raise."},
    {Tool::Flatten, "Flatten", "Pulls terrain towards the target height."},
    {Tool::Smooth, "Smooth", "Averages with neighbours. Hold Ctrl with any tool."},
    {Tool::Noise, "Noise", "Adds detail. Same seed gives the same detail."},
    {Tool::Ramp, "Ramp", "Click a start point, then an end point."},
}};

/// Penyunting terrain.
///
/// **Preview-nya peta 2D dari atas, bukan viewport 3D.** Bukan karena viewport
/// 3D salah — di situlah tempat memahat yang sebenarnya, dan ke situ panel ini
/// akan tersambung begitu E8 bisa menggambar terrain. Tapi sampai itu ada,
/// kursor brush akan melayang di atas ruang kosong, dan alat yang tidak bisa
/// dilihat hasilnya tidak bisa diuji maupun dipakai.
///
/// Peta dari atas juga bukan sekadar penambal. Untuk membaca *bentuk* terrain —
/// di mana lembahnya, seberapa lebar punggungannya, apakah jalan yang dipahat
/// lurus — pandangan dari atas justru lebih jujur daripada perspektif, dan itu
/// sebabnya alat terrain mana pun tetap menyediakannya berdampingan dengan
/// viewport 3D-nya.
///
/// **Digambar sebagai kisi bervertex warna, bukan tekstur.** Panel tidak punya
/// jalan untuk mengunggah buffer piksel — `EditorContext` tidak membuka
/// `rhi::Device`, dan itu batas yang benar: preview tidak sepadan dengan membuka
/// RHI ke seluruh panel. Kisi ber-shading digambar langsung dari terrain, jadi
/// ia selalu sinkron dan tidak ada yang perlu diunggah ulang setiap sentuhan
/// brush.
///
/// **Suntingan tidak melewati CommandHistory**, sama seperti Material, Graph,
/// dan Script Editor — lihat catatan di `Command.h`. Riwayat undo utama
/// menjanjikan pembatalan perubahan *scene*, sedangkan terrain adalah dokumen
/// yang dibuka dan ditutup. Undo goresan dilayani jurnal blok milik `Terrain`.
class TerrainEditorPanel final : public Panel {
public:
    TerrainEditorPanel()
        : Panel(panel_id::kTerrainEditor, std::string(icons::kTerrainEditor) + "  Terrain Editor",
                PanelCategory::Authoring) {}

    void OnDraw(EditorContext& context) override {
        if (context.assets == nullptr) {
            ImGui::TextDisabled("No asset database.");
            return;
        }

        DrawToolbar(context);
        ImGui::Separator();

        const float listWidth = ImGui::GetFontSize() * 10.0f;
        if (ImGui::BeginChild("##terrains", ImVec2(listWidth, 0.0f), ImGuiChildFlags_ResizeX)) {
            DrawTerrainList(context);
        }
        ImGui::EndChild();

        ImGui::SameLine();
        if (!openGuid_.IsValid()) {
            ImGui::TextColored(kHintColor, "Pick a terrain on the left, or create one.");
            return;
        }

        const float avail = ImGui::GetContentRegionAvail().x;
        const float handle = ImGui::GetStyle().ItemSpacing.x;
        if (sideWidth_ <= 0.0f) {
            sideWidth_ = avail * 0.34f;
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
        ImGui::BeginDisabled(terrain_.UndoDepth() == 0);
        if (ImGui::Button(icons::kUndo)) {
            terrain_.Undo();
            dirty_ = true;
        }
        ImGui::EndDisabled();
        widgets::Tooltip("Undo stroke (Ctrl+Z)");

        ImGui::SameLine();
        ImGui::BeginDisabled(terrain_.RedoDepth() == 0);
        if (ImGui::Button(icons::kRedo)) {
            terrain_.Redo();
            dirty_ = true;
        }
        ImGui::EndDisabled();
        widgets::Tooltip("Redo stroke (Ctrl+Shift+Z)");

        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "%s%s", openName_.c_str(), dirty_ ? " *" : "");

        // Pintasan hanya berlaku saat panel ini yang fokus. Ctrl+Z global milik
        // riwayat scene, dan dua pemilik untuk satu pintasan berarti satu di
        // antaranya diam-diam kalah.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::GetIO().KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                if (ImGui::GetIO().KeyShift) {
                    terrain_.Redo();
                } else {
                    terrain_.Undo();
                }
                dirty_ = true;
            }
        }
    }

    void DrawTerrainList(EditorContext& context) {
        if (ImGui::Button("New Terrain", ImVec2(-FLT_MIN, 0.0f))) {
            newDesc_ = TerrainDesc{};
            ImGui::OpenPopup("##newterrain");
        }
        DrawNewTerrainPopup(context);
        ImGui::Spacing();

        for (const assets::AssetRecord& record : context.assets->All()) {
            if (record.type != assets::AssetType::Terrain) {
                continue;
            }
            if (ImGui::Selectable(record.name.c_str(), record.guid == openGuid_)) {
                Open(context, record.guid);
            }
        }
    }

    void DrawNewTerrainPopup(EditorContext& context) {
        if (!ImGui::BeginPopup("##newterrain")) {
            return;
        }
        const float width = ImGui::GetFontSize() * 7.0f;
        ImGui::TextColored(kHintColor, "New terrain");
        ImGui::Separator();

        ImGui::SetNextItemWidth(width);
        ImGui::InputInt("Samples per tile", &newDesc_.tileSamples, 0);
        ImGui::SetNextItemWidth(width);
        ImGui::InputInt("Tiles X", &newDesc_.tilesX, 0);
        ImGui::SetNextItemWidth(width);
        ImGui::InputInt("Tiles Y", &newDesc_.tilesY, 0);
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("Sample spacing", &newDesc_.sampleSpacing, 0.01f, 0.05f, 64.0f, "%.2f m");
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("Min height", &newDesc_.minHeight, 1.0f, -10000.0f, 10000.0f, "%.0f m");
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("Max height", &newDesc_.maxHeight, 1.0f, -10000.0f, 10000.0f, "%.0f m");
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("Base height", &newDesc_.baseHeight, 1.0f, -10000.0f, 10000.0f, "%.0f m");

        newDesc_.tileSamples = std::clamp(newDesc_.tileSamples, 16, 4096);
        newDesc_.tilesX = std::clamp(newDesc_.tilesX, 1, 32);
        newDesc_.tilesY = std::clamp(newDesc_.tilesY, 1, 32);

        // Ukuran dunia dan anggaran memorinya disebut sebelum dibuat, bukan
        // setelahnya. Terrain 16×16 ubin 4096² adalah 8 GB — angka yang harus
        // terlihat sebelum tombolnya ditekan, bukan sesudah mesin tersendat.
        const double samples = static_cast<double>(newDesc_.tileSamples);
        const double bytes = samples * samples * 2.0 * newDesc_.tilesX * newDesc_.tilesY;
        ImGui::Separator();
        ImGui::TextColored(kHintColor, "%.0f x %.0f m   %d x %d samples",
                           static_cast<double>(newDesc_.tileSamples * newDesc_.tilesX - 1) *
                               static_cast<double>(newDesc_.sampleSpacing),
                           static_cast<double>(newDesc_.tileSamples * newDesc_.tilesY - 1) *
                               static_cast<double>(newDesc_.sampleSpacing),
                           newDesc_.tileSamples * newDesc_.tilesX,
                           newDesc_.tileSamples * newDesc_.tilesY);
        ImGui::TextColored(bytes > 2e9 ? kWarnColor : kHintColor,
                           "%.0f MB if every tile is edited", bytes / (1024.0 * 1024.0));

        if (ImGui::Button("Create", ImVec2(ImGui::GetFontSize() * 6.0f, 0.0f))) {
            CreateTerrain(context);
            ImGui::CloseCurrentPopup();
        }
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

    /// Warna sebuah titik: gradasi topografi menurut tinggi, dikalikan bayangan
    /// lereng.
    ///
    /// Bayangan lerengnya yang membuat peta bisa dibaca. Gradasi tinggi saja
    /// memberitahu *seberapa tinggi* sebuah titik, tapi bukan *bentuknya* —
    /// punggungan dan lembah pada ketinggian yang sama menjadi satu bidang warna
    /// rata, dan hasil goresan brush tidak terlihat sama sekali.
    ///
    /// **Keduanya diskalakan ke rentang tinggi yang benar-benar terlihat, bukan
    /// ke `minHeight`/`maxHeight` yang dikonfigurasi.** Terrain baru boleh
    /// berentang seribu meter sementara isinya baru setinggi dua meter; diukur
    /// terhadap rentang konfigurasi, seluruh peta menjadi satu warna rata dan
    /// setiap goresan tampak tidak melakukan apa-apa. Yang dibayar adalah arti
    /// warnanya berubah saat digeser — dan itu dibayar balik dengan legenda yang
    /// menyebutkan meternya, bukan dengan menebak.
    ImU32 Shade(float unitHeight, float slopeX, float slopeZ) const {
        const float t = std::clamp(unitHeight, 0.0f, 1.0f);

        // Gradasi topografi baku: air, dataran, lereng, batu, salju.
        static constexpr std::array<std::array<float, 4>, 5> kRamp{{
            {0.00f, 0.16f, 0.28f, 0.40f},
            {0.15f, 0.30f, 0.45f, 0.25f},
            {0.35f, 0.45f, 0.36f, 0.22f},
            {0.65f, 0.52f, 0.50f, 0.48f},
            {1.00f, 0.92f, 0.93f, 0.96f},
        }};
        float r = kRamp.back()[1];
        float g = kRamp.back()[2];
        float b = kRamp.back()[3];
        for (std::size_t i = 1; i < kRamp.size(); ++i) {
            if (t <= kRamp[i][0]) {
                const float span = kRamp[i][0] - kRamp[i - 1][0];
                const float k = span > 0.0f ? (t - kRamp[i - 1][0]) / span : 0.0f;
                r = kRamp[i - 1][1] + (kRamp[i][1] - kRamp[i - 1][1]) * k;
                g = kRamp[i - 1][2] + (kRamp[i][2] - kRamp[i - 1][2]) * k;
                b = kRamp[i - 1][3] + (kRamp[i][3] - kRamp[i - 1][3]) * k;
                break;
            }
        }

        // Cahaya dari barat laut — arah baku peta topografi, karena otak membaca
        // bayangan dari kiri atas sebagai cekung/cembung dengan benar dan dari
        // arah lain sering terbalik.
        const Vec3 normal = glm::normalize(Vec3(-slopeX, 1.0f, -slopeZ));
        const Vec3 light = glm::normalize(Vec3(-0.6f, 0.7f, -0.4f));
        const float lambert = std::clamp(glm::dot(normal, light), 0.0f, 1.0f);
        const float shade = 0.45f + 0.75f * lambert;
        return ImGui::GetColorU32(ImVec4(std::min(r * shade, 1.0f), std::min(g * shade, 1.0f),
                                         std::min(b * shade, 1.0f), 1.0f));
    }

    void DrawHeightfield(ImDrawList* draw) {
        // Resolusi kisi mengikuti ukuran panel, dibatasi supaya jumlah vertex
        // tetap jauh di bawah batas indeks 16-bit ImGui.
        const int cols = std::clamp(static_cast<int>(mapSize_.x / 7.0f), 8, 96);
        const int rows = std::clamp(static_cast<int>(mapSize_.y / 7.0f), 8, 96);

        // Tinggi dicuplik sekali per simpul, lalu lerengnya diturunkan dari
        // simpul tetangga. Mencuplik ulang untuk normal berarti lima kali kerja
        // yang sama.
        heights_.assign(static_cast<std::size_t>(cols + 1) * static_cast<std::size_t>(rows + 1),
                        0.0f);
        inside_.assign(heights_.size(), 0);
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
            }
        }

        // Rentang yang benar-benar terlihat, dipakai gradasi warna maupun
        // bayangannya. Dihitung dari simpul yang barusan dicuplik — bukan dari
        // seluruh peta, yang berarti membaca delapan megabyte tiap frame.
        visibleLow_ = FLT_MAX;
        visibleHigh_ = -FLT_MAX;
        for (std::size_t i = 0; i < heights_.size(); ++i) {
            if (inside_[i] == 0) {
                continue;
            }
            visibleLow_ = std::min(visibleLow_, heights_[i]);
            visibleHigh_ = std::max(visibleHigh_, heights_[i]);
        }
        if (visibleLow_ > visibleHigh_) {
            visibleLow_ = terrain_.Desc().minHeight;
            visibleHigh_ = terrain_.Desc().maxHeight;
        }
        // Rentang minimum supaya terrain yang benar-benar rata tidak diperbesar
        // menjadi derau pembulatan sampel 16-bit.
        const float quantum =
            (terrain_.Desc().maxHeight - terrain_.Desc().minHeight) / 65535.0f;
        const float range = std::max(visibleHigh_ - visibleLow_, quantum * 8.0f);

        // Lereng dinyatakan dalam piksel layar, bukan meter: tinggi yang terlihat
        // dipetakan ke sebagian tetap dari lebar peta. Itu yang membuat relief
        // tetap terbaca pada peta dua kilometer maupun pada zoom sepuluh meter,
        // tanpa slider "vertical exaggeration" yang harus disetel ulang setiap
        // kali berpindah skala.
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
                    // Di luar peta digambar berbeda, bukan sebagai perpanjangan
                    // tepi. Tanpa itu, batas dunia tidak terlihat dan brush di
                    // pinggir terasa seperti mengenai sesuatu yang sebetulnya
                    // tidak ada.
                    color = ImGui::GetColorU32(ImVec4(0.09f, 0.10f, 0.12f, 1.0f));
                } else {
                    const float scale = pixelsPerRange / range;
                    const float slopeX = (heightAt(i + 1, j) - heightAt(i - 1, j)) * scale /
                                         (2.0f * pixelsPerCellX);
                    const float slopeZ = (heightAt(i, j + 1) - heightAt(i, j - 1)) * scale /
                                         (2.0f * pixelsPerCellZ);
                    color = Shade((heights_[at] - visibleLow_) / range, slopeX, slopeZ);
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
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                                   ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        HandleInput(context, hovered);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(mapOrigin_, ImVec2(mapOrigin_.x + mapSize_.x, mapOrigin_.y + mapSize_.y),
                           true);
        DrawHeightfield(draw);
        DrawTileGrid(draw);
        DrawCursor(draw, hovered);
        draw->PopClipRect();
        draw->AddRect(mapOrigin_, ImVec2(mapOrigin_.x + mapSize_.x, mapOrigin_.y + mapSize_.y),
                      ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)));
    }

    void DrawTileGrid(ImDrawList* draw) {
        if (!showTiles_) {
            return;
        }
        // Batas ubin diperlihatkan karena ia menentukan apa yang dialokasikan
        // dan apa yang diekspor per berkas — bukan karena ia terlihat pada
        // terrainnya sendiri. Justru sebaliknya: kalau ia terlihat di sana, ada
        // yang salah.
        const TerrainDesc& desc = terrain_.Desc();
        const float tileMeters = static_cast<float>(desc.tileSamples) * desc.sampleSpacing;
        const ImU32 color = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.14f));
        for (int i = 0; i <= desc.tilesX; ++i) {
            const float x = WorldToScreen(static_cast<float>(i) * tileMeters, 0.0f).x;
            draw->AddLine(ImVec2(x, WorldToScreen(0.0f, 0.0f).y),
                          ImVec2(x, WorldToScreen(0.0f, terrain_.WorldDepth()).y), color);
        }
        for (int j = 0; j <= desc.tilesY; ++j) {
            const float y = WorldToScreen(0.0f, static_cast<float>(j) * tileMeters).y;
            draw->AddLine(ImVec2(WorldToScreen(0.0f, 0.0f).x, y),
                          ImVec2(WorldToScreen(terrain_.WorldWidth(), 0.0f).x, y), color);
        }
    }

    void DrawCursor(ImDrawList* draw, bool hovered) {
        if (!hovered && !stroke_.Active() && !rampAnchored_) {
            return;
        }
        const ImVec2 centre = WorldToScreen(cursorX_, cursorZ_);
        const float radius = brush_.radius / metersPerPixel_;

        // Dua lingkaran: tepi luar jari-jari, tepi dalam batas bobot penuh.
        // Falloff tidak bisa dibaca dari satu lingkaran, dan menebaknya berarti
        // menggores lalu membatalkan sampai terasa benar.
        draw->AddCircle(centre, radius, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.85f)), 0,
                        1.5f);
        const float inner = radius * (1.0f - std::clamp(brush_.falloff, 0.0f, 1.0f));
        if (inner > 2.0f) {
            draw->AddCircle(centre, inner, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.35f)));
        }

        if (rampAnchored_) {
            const ImVec2 from = WorldToScreen(rampStart_.x, rampStart_.z);
            draw->AddLine(from, centre, ImGui::GetColorU32(ImVec4(1.0f, 0.82f, 0.35f, 0.9f)), 2.0f);
            draw->AddCircleFilled(from, 4.0f, ImGui::GetColorU32(ImVec4(1.0f, 0.82f, 0.35f, 1.0f)));
        }
    }

    void HandleInput(EditorContext& context, bool hovered) {
        const ImGuiIO& io = ImGui::GetIO();
        if (hovered || stroke_.Active()) {
            ScreenToWorld(io.MousePos, cursorX_, cursorZ_);
        }

        // Geser dan zoom dengan tombol tengah dan roda — konvensi yang sama
        // dengan kanvas node dan viewport, supaya tidak ada yang perlu
        // dipelajari ulang.
        if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
                                      ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
            viewX_ -= io.MouseDelta.x * metersPerPixel_;
            viewZ_ -= io.MouseDelta.y * metersPerPixel_;
        }
        if (hovered && io.MouseWheel != 0.0f) {
            // Zoom di sekitar kursor, bukan di sekitar tengah panel: memperbesar
            // ke arah tempat yang sedang dilihat adalah yang diharapkan, dan
            // zoom ke tengah memaksa menggeser lagi setiap kali.
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

        if (tool_ == Tool::Ramp) {
            HandleRamp(hovered);
            return;
        }

        const Brush brush = EffectiveBrush();
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            stroke_.Begin(terrain_, cursorX_, cursorZ_);
        }
        if (stroke_.Active()) {
            const float dt = context.deltaSeconds > 0.0f ? context.deltaSeconds : 1.0f / 60.0f;
            stroke_.Advance(terrain_, brush, cursorX_, cursorZ_, dt);
            dirty_ = true;
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                stroke_.End(terrain_);
            }
        }
    }

    void HandleRamp(bool hovered) {
        if (!hovered || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            return;
        }
        if (!rampAnchored_) {
            // Tingginya diambil dari terrain di titik yang diklik. Ramp yang
            // paling sering dibutuhkan adalah jalan antara dua ketinggian yang
            // sudah ada, dan mengetikkan keduanya berarti menebak angka yang
            // sudah ada di bawah kursor.
            rampStart_ = Vec3(cursorX_, terrain_.HeightAtWorld(cursorX_, cursorZ_), cursorZ_);
            rampAnchored_ = true;
            return;
        }
        const Vec3 end(cursorX_, terrain_.HeightAtWorld(cursorX_, cursorZ_), cursorZ_);
        terrain_.BeginStroke();
        ApplyRamp(terrain_, brush_, rampStart_, end);
        terrain_.EndStroke();
        rampAnchored_ = false;
        dirty_ = true;
    }

    /// Brush yang benar-benar dipakai, setelah pengubah papan ketik.
    Brush EffectiveBrush() const {
        Brush brush = brush_;
        switch (tool_) {
            case Tool::Raise: brush.kind = BrushKind::Raise; break;
            case Tool::Lower: brush.kind = BrushKind::Lower; break;
            case Tool::Flatten: brush.kind = BrushKind::Flatten; break;
            case Tool::Smooth: brush.kind = BrushKind::Smooth; break;
            case Tool::Noise: brush.kind = BrushKind::Noise; break;
            case Tool::Ramp: break;
        }
        // Ctrl melembutkan dan Shift membalik, apa pun alat yang terpilih.
        // Keduanya konvensi yang sama di setiap alat terrain, dan berpindah alat
        // untuk satu sapuan pelembut memutus alur memahat.
        if (ImGui::GetIO().KeyCtrl) {
            brush.kind = BrushKind::Smooth;
        } else if (ImGui::GetIO().KeyShift) {
            if (brush.kind == BrushKind::Raise) {
                brush.kind = BrushKind::Lower;
            } else if (brush.kind == BrushKind::Lower) {
                brush.kind = BrushKind::Raise;
            }
        }
        return brush;
    }

    void DrawStatus() {
        const bool inside = cursorX_ >= 0.0f && cursorZ_ >= 0.0f &&
                            cursorX_ <= terrain_.WorldWidth() &&
                            cursorZ_ <= terrain_.WorldDepth();
        ImGui::TextColored(kHintColor, "%.1f, %.1f m   h=%.2f m%s", cursorX_, cursorZ_,
                           terrain_.HeightAtWorld(cursorX_, cursorZ_),
                           inside ? "" : "   (outside)");
        ImGui::SameLine();
        // Legenda: warna peta menyesuaikan rentang yang terlihat, jadi artinya
        // harus disebut angkanya — kalau tidak, "hijau" berarti berbeda setiap
        // kali digeser dan tidak ada yang bisa dibaca darinya.
        ImGui::TextColored(kHintColor, "|  shading %.1f..%.1f m  |", visibleLow_, visibleHigh_);
        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "%zu/%d tiles   %.1f MB   %zu undo",
                           terrain_.TilesResident(),
                           terrain_.Desc().tilesX * terrain_.Desc().tilesY,
                           static_cast<double>(terrain_.BytesResident()) / (1024.0 * 1024.0),
                           terrain_.UndoDepth());
    }

    // --- alat ----------------------------------------------------------------

    void DrawTools(EditorContext& context) {
        const float width = ImGui::GetFontSize() * 7.0f;

        for (const ToolEntry& entry : kTools) {
            const bool selected = tool_ == entry.tool;
            if (ImGui::RadioButton(entry.label, selected)) {
                tool_ = entry.tool;
                rampAnchored_ = false;
            }
            widgets::Tooltip(entry.hint);
        }
        ImGui::Separator();

        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("Radius", &brush_.radius, 0.25f, 0.5f, 2000.0f, "%.1f m");
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("Strength", &brush_.strength, 0.1f, 0.0f, 500.0f, "%.1f m/s");
        ImGui::SetNextItemWidth(width);
        ImGui::SliderFloat("Falloff", &brush_.falloff, 0.0f, 1.0f);

        if (tool_ == Tool::Flatten) {
            ImGui::SetNextItemWidth(width);
            ImGui::DragFloat("Target", &brush_.targetHeight, 0.25f, terrain_.Desc().minHeight,
                             terrain_.Desc().maxHeight, "%.2f m");
            ImGui::SameLine();
            if (ImGui::SmallButton("Pick")) {
                brush_.targetHeight = terrain_.HeightAtWorld(cursorX_, cursorZ_);
            }
            widgets::Tooltip("Take the height under the cursor");
        }
        if (tool_ == Tool::Noise) {
            ImGui::SetNextItemWidth(width);
            ImGui::DragFloat("Frequency", &brush_.noiseFrequency, 0.001f, 0.001f, 2.0f, "%.3f /m");
            ImGui::SetNextItemWidth(width);
            int seed = static_cast<int>(brush_.seed);
            if (ImGui::InputInt("Seed", &seed, 0)) {
                brush_.seed = static_cast<uint32_t>(std::max(0, seed));
            }
        }

        DrawBrushProfile();

        ImGui::Separator();
        if (ImGui::CollapsingHeader("Erosion")) {
            ImGui::SetNextItemWidth(width);
            ImGui::DragInt("Iterations", &erosionIterations_, 1.0f, 1, 500);
            ImGui::SetNextItemWidth(width);
            ImGui::DragFloat("Talus angle", &erosionTalus_, 0.5f, 1.0f, 89.0f, "%.0f deg");
            ImGui::SetNextItemWidth(width);
            ImGui::SliderFloat("Rate", &erosionRate_, 0.0f, 0.5f);
            if (ImGui::Button("Erode brush area")) {
                ApplyErosion(false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Erode all")) {
                ApplyErosion(true);
            }
            widgets::Tooltip("Whole terrain — this materialises every tile");
        }

        ImGui::Separator();
        if (ImGui::CollapsingHeader("Heightmap", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##file", &transferFile_);
            widgets::Tooltip("File name, next to the .simterrain");

            if (ImGui::Button((std::string(icons::kExport) + "  Export").c_str())) {
                Transfer(context, true);
            }
            ImGui::SameLine();
            if (ImGui::Button((std::string(icons::kImport) + "  Import").c_str())) {
                Transfer(context, false);
            }
            ImGui::TextColored(kHintColor, ".png is 16-bit greyscale, .raw is uint16 LE");
        }

        ImGui::Separator();
        ImGui::Checkbox("Show tile bounds", &showTiles_);
        if (ImGui::Button("Fit view")) {
            FitView();
        }
    }

    /// Profil brush, digambar dengan `BrushWeight` — fungsi yang sama persis
    /// yang menyunting terrain. Profil yang digambar ulang dengan rumus kedua
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
            const float weight = BrushWeight(brush_, (t * 2.0f - 1.0f < 0.0f ? -(t * 2.0f - 1.0f)
                                                                            : (t * 2.0f - 1.0f)) *
                                                         brush_.radius);
            const ImVec2 point(origin.x + size.x * t, origin.y + size.y * (1.0f - weight));
            if (i > 0) {
                draw->AddLine(previous, point, ImGui::GetColorU32(ImVec4(0.55f, 0.80f, 0.45f, 1.0f)),
                              2.0f);
            }
            previous = point;
        }
        draw->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)));
    }

    void ApplyErosion(bool whole) {
        SampleRect rect;
        if (whole) {
            rect = SampleRect{0, 0, terrain_.SamplesX(), terrain_.SamplesY()};
        } else {
            rect = terrain_.RectForCircle(cursorX_, cursorZ_, brush_.radius);
        }
        if (rect.Empty()) {
            return;
        }
        // Dibungkus goresan supaya bisa dibatalkan seperti sapuan brush biasa.
        // Erosi yang tidak bisa dibatalkan berarti setiap percobaan menuntut
        // menyimpan lebih dulu.
        terrain_.BeginStroke();
        ApplyThermalErosion(terrain_, rect, erosionIterations_, erosionTalus_, erosionRate_);
        terrain_.EndStroke();
        dirty_ = true;
    }

    // --- berkas ---------------------------------------------------------------

    void CreateTerrain(EditorContext& context) {
        const std::filesystem::path folder =
            std::filesystem::path(context.assets->Root()) / "Terrain";
        std::filesystem::path path = folder / "NewTerrain.simterrain";
        int suffix = 0;
        while (std::filesystem::exists(path)) {
            path = folder / ("NewTerrain" + std::to_string(++suffix) + ".simterrain");
        }

        Terrain fresh(newDesc_);
        TerrainDocument document;
        document.name = path.stem().string();
        document.desc = newDesc_;

        const TerrainIoResult result = SaveTerrain(fresh, document, path);
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot create " + path.filename().string() + ": " +
                                             result.error);
            }
            return;
        }
        context.assets->ScanNow();
        if (const assets::AssetRecord* record =
                context.assets->FindByRelativePath("Terrain/" + path.filename().string())) {
            Open(context, record->guid);
        }
    }

    void Open(EditorContext& context, const Uuid& guid) {
        const assets::AssetRecord* record = context.assets->Find(guid);
        if (record == nullptr) {
            return;
        }
        Terrain loaded;
        TerrainDocument document;
        const TerrainIoResult result =
            LoadTerrain(loaded, document, context.assets->AbsolutePath(*record));
        if (!result.ok) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot open " + record->name + ": " + result.error);
            }
            return;
        }
        terrain_ = std::move(loaded);
        document_ = std::move(document);
        openGuid_ = guid;
        openName_ = record->name;
        openPath_ = context.assets->AbsolutePath(*record);
        transferFile_ = openPath_.stem().string() + ".png";
        dirty_ = false;
        rampAnchored_ = false;
        brush_.targetHeight = terrain_.Desc().baseHeight;
        // Jari-jari bawaan mengikuti ukuran peta. Brush sepuluh meter di atas
        // terrain dua kilometer adalah titik jarum: goresan pertama tidak
        // terlihat, dan kesan pertamanya adalah alat yang rusak.
        brush_.radius = std::clamp(terrain_.WorldWidth() * 0.02f, 1.0f, 500.0f);
        brush_.strength = std::max((terrain_.Desc().maxHeight - terrain_.Desc().minHeight) * 0.02f,
                                   0.5f);
        FitView();
    }

    void Save(EditorContext& context) {
        const TerrainIoResult result = SaveTerrain(terrain_, document_, openPath_);
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

    void Transfer(EditorContext& context, bool exporting) {
        if (transferFile_.empty()) {
            return;
        }
        const std::filesystem::path path = openPath_.parent_path() / transferFile_;
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool raw = extension == ".raw";

        TerrainIoResult result;
        if (exporting) {
            result = raw ? SaveHeightmapRaw(terrain_, path) : SaveHeightmapPng(terrain_, path);
        } else {
            result = raw ? LoadHeightmapRaw(terrain_, path) : LoadHeightmapPng(terrain_, path);
        }

        if (context.notifications == nullptr) {
            return;
        }
        if (!result.ok) {
            context.notifications->Error(result.error);
            return;
        }
        if (!exporting) {
            // Impor bukan goresan, jadi riwayat goresan tidak lagi cocok dengan
            // apa yang ada di peta. Membiarkannya berarti satu Ctrl+Z memasang
            // kembali potongan heightmap lama di atas yang baru diimpor.
            terrain_.ClearHistory();
            dirty_ = true;
        }
        context.notifications->Success((exporting ? "Exported " : "Imported ") +
                                       path.filename().string());
    }

    Terrain terrain_;
    TerrainDocument document_;
    TerrainDesc newDesc_;

    Uuid openGuid_;
    std::string openName_;
    std::filesystem::path openPath_;
    std::string transferFile_;
    bool dirty_ = false;

    Tool tool_ = Tool::Raise;
    Brush brush_;
    BrushStroke stroke_;
    bool rampAnchored_ = false;
    Vec3 rampStart_{0.0f};

    int erosionIterations_ = 20;
    float erosionTalus_ = 35.0f;
    float erosionRate_ = 0.3f;

    ImVec2 mapOrigin_{0.0f, 0.0f};
    ImVec2 mapSize_{0.0f, 0.0f};
    float viewX_ = 0.0f;
    float viewZ_ = 0.0f;
    float metersPerPixel_ = 0.0f;
    float visibleLow_ = 0.0f;
    float visibleHigh_ = 1.0f;
    float cursorX_ = 0.0f;
    float cursorZ_ = 0.0f;
    float sideWidth_ = 0.0f;
    bool showTiles_ = true;

    std::vector<float> heights_;
    std::vector<uint8_t> inside_;
};

}  // namespace

SIM_REGISTER_PANEL(TerrainEditorPanel, 29)

}  // namespace sim::editor
