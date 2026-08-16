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

/// Kelompok alat.
///
/// Tiga tab, bukan satu daftar alat yang panjang: sculpt, paint, dan hole
/// menyunting tiga peta yang berbeda dengan parameter yang berbeda, dan alat
/// yang berbagi satu papan parameter akan berbagi slider yang artinya berganti
/// diam-diam. Tab juga yang membuat "apa yang akan terjadi kalau saya menyeret"
/// bisa dijawab tanpa melihat parameter mana yang terakhir disentuh.
enum class Mode {
    Sculpt,
    Paint,
    Holes,
};

/// Apa yang diwarnai peta 2D.
enum class MapView {
    Relief,
    Layers,
    Weight,
};

/// Warna bawaan layer baru, berputar menurut indeksnya.
///
/// Bukan satu warna bawaan untuk semuanya: layer kedua yang warnanya sama persis
/// dengan layer pertama membuat peta 2D-nya tidak bisa dibaca tepat pada saat
/// warna itu satu-satunya cara membacanya.
constexpr std::array<std::array<float, 3>, 6> kLayerPalette{{
    {0.42f, 0.52f, 0.30f},
    {0.52f, 0.50f, 0.48f},
    {0.72f, 0.66f, 0.48f},
    {0.30f, 0.34f, 0.38f},
    {0.88f, 0.90f, 0.93f},
    {0.45f, 0.35f, 0.28f},
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
                PanelCategory::Authoring) {
        // Alasan yang sama dengan Script Editor: tidak di-dock, jadi jangan
        // terbuka sendiri.
        SetOpen(false);
    }

    void OnDraw(EditorContext& context) override {
        if (context.assets == nullptr || context.terrains == nullptr) {
            ImGui::TextDisabled("No asset database.");
            return;
        }

        // Penunjuk dokumen disegarkan sekali per frame, di sini. Store bisa
        // dikosongkan di antara dua frame — project berganti, terrain ditutup —
        // dan penunjuk yang disimpan lintas frame akan menunjuk memori yang
        // sudah dibebaskan tanpa satu pun tanda.
        store_ = context.terrains;
        terrain_ = openGuid_.IsValid() ? store_->Find(openGuid_) : nullptr;
        document_ = openGuid_.IsValid() ? store_->Document(openGuid_) : nullptr;
        if (terrain_ == nullptr || document_ == nullptr) {
            openGuid_ = Uuid{};
            terrain_ = nullptr;
            document_ = nullptr;
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
        ImGui::BeginDisabled(!store_->Dirty(openGuid_) || !openGuid_.IsValid());
        if (ImGui::Button((std::string(icons::kSave) + "  Save").c_str())) {
            Save(context);
        }
        ImGui::EndDisabled();

        if (!openGuid_.IsValid()) {
            return;
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(terrain_->UndoDepth() == 0);
        if (ImGui::Button(icons::kUndo)) {
            terrain_->Undo();
            Touch();
        }
        ImGui::EndDisabled();
        widgets::Tooltip("Undo stroke (Ctrl+Z)");

        ImGui::SameLine();
        ImGui::BeginDisabled(terrain_->RedoDepth() == 0);
        if (ImGui::Button(icons::kRedo)) {
            terrain_->Redo();
            Touch();
        }
        ImGui::EndDisabled();
        widgets::Tooltip("Redo stroke (Ctrl+Shift+Z)");

        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "%s%s", openName_.c_str(), store_->Dirty(openGuid_) ? " *" : "");

        // Pintasan hanya berlaku saat panel ini yang fokus. Ctrl+Z global milik
        // riwayat scene, dan dua pemilik untuk satu pintasan berarti satu di
        // antaranya diam-diam kalah.
        if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
            ImGui::GetIO().KeyCtrl) {
            if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
                if (ImGui::GetIO().KeyShift) {
                    terrain_->Redo();
                } else {
                    terrain_->Undo();
                }
                Touch();
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
        viewX_ = terrain_->WorldWidth() * 0.5f;
        viewZ_ = terrain_->WorldDepth() * 0.5f;
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
    /// Cahaya dari barat laut — arah baku peta topografi, karena otak membaca
    /// bayangan dari kiri atas sebagai cekung/cembung dengan benar dan dari arah
    /// lain sering terbalik.
    ///
    /// Dipisah dari warnanya supaya ketiga tampilan peta — relief, layer, dan
    /// bobot — memakai bayangan yang sama persis. Peta bobot yang kehilangan
    /// bayangannya menjadi bercak abu-abu yang tidak bisa dicocokkan dengan
    /// bentuk terrain di bawahnya, dan justru itu yang ingin dilihat saat
    /// mengecat.
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

    int SampleIndexX(float worldX) const {
        return std::clamp(static_cast<int>(std::lround(worldX / terrain_->Desc().sampleSpacing)), 0,
                          terrain_->SamplesX() - 1);
    }

    int SampleIndexZ(float worldZ) const {
        return std::clamp(static_cast<int>(std::lround(worldZ / terrain_->Desc().sampleSpacing)), 0,
                          terrain_->SamplesY() - 1);
    }

    /// Campuran warna layer pada satu titik, ditimbang bobotnya.
    ///
    /// Dicampur dengan rumus yang sama dengan yang akan dipakai perender —
    /// jumlah bobot terbagi 255 — jadi apa yang terlihat di peta adalah
    /// perbandingan yang benar-benar tersimpan, bukan perkiraan yang kebetulan
    /// mirip.
    ImU32 ShadeLayers(float worldX, float worldZ, float shade) const {
        const int sx = SampleIndexX(worldX);
        const int sy = SampleIndexZ(worldZ);
        Vec3 albedo(0.0f);
        int sum = 0;
        for (int layer = 1; layer < terrain_->LayerCount(); ++layer) {
            const int weight = terrain_->WeightAt(layer, sx, sy);
            if (weight == 0) {
                continue;
            }
            albedo += terrain_->Layer(layer).color * static_cast<float>(weight);
            sum += weight;
        }
        albedo += terrain_->Layer(0).color *
                  static_cast<float>(std::max(0, static_cast<int>(kWeightMax) - sum));
        return Tint(albedo / static_cast<float>(kWeightMax), shade);
    }

    /// Bobot satu layer saja, dari gelap ke warna layernya.
    ///
    /// Bukan abu-abu ke putih: dengan gradasi netral tidak ada yang membedakan
    /// peta bobot layer satu dari peta bobot layer lain di tangkapan layar mana
    /// pun, termasuk di kepala orang yang baru saja berpindah layer.
    ImU32 ShadeWeight(float worldX, float worldZ, float shade) const {
        const int sx = SampleIndexX(worldX);
        const int sy = SampleIndexZ(worldZ);
        const float weight = static_cast<float>(terrain_->WeightAt(paintLayer_, sx, sy)) /
                             static_cast<float>(kWeightMax);
        const Vec3 albedo =
            Vec3(0.07f, 0.07f, 0.09f) + (terrain_->Layer(paintLayer_).color -
                                         Vec3(0.07f, 0.07f, 0.09f)) *
                                            weight;
        return Tint(albedo, shade);
    }

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

        return Tint(Vec3(r, g, b), Hillshade(slopeX, slopeZ));
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
                heights_[at] = terrain_->HeightAtWorld(wx, wz);
                inside_[at] = (wx >= 0.0f && wz >= 0.0f && wx <= terrain_->WorldWidth() &&
                               wz <= terrain_->WorldDepth())
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
            visibleLow_ = terrain_->Desc().minHeight;
            visibleHigh_ = terrain_->Desc().maxHeight;
        }
        // Rentang minimum supaya terrain yang benar-benar rata tidak diperbesar
        // menjadi derau pembulatan sampel 16-bit.
        const float quantum =
            (terrain_->Desc().maxHeight - terrain_->Desc().minHeight) / 65535.0f;
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
                    if (view_ == MapView::Relief || terrain_->LayerCount() < 2) {
                        color = Shade((heights_[at] - visibleLow_) / range, slopeX, slopeZ);
                    } else {
                        float wx = 0.0f;
                        float wz = 0.0f;
                        ScreenToWorld(screen, wx, wz);
                        color = view_ == MapView::Layers
                                    ? ShadeLayers(wx, wz, Hillshade(slopeX, slopeZ))
                                    : ShadeWeight(wx, wz, Hillshade(slopeX, slopeZ));
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

    void DrawMap(EditorContext& context) {
        const float height = std::max(ImGui::GetContentRegionAvail().y -
                                          ImGui::GetFrameHeightWithSpacing() * 2.0f,
                                      ImGui::GetFontSize() * 8.0f);
        mapOrigin_ = ImGui::GetCursorScreenPos();
        mapSize_ = ImVec2(ImGui::GetContentRegionAvail().x, height);
        if (metersPerPixel_ <= 0.0f) {
            metersPerPixel_ = std::max(terrain_->WorldWidth() / std::max(mapSize_.x, 1.0f),
                                       terrain_->WorldDepth() / std::max(mapSize_.y, 1.0f));
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
        DrawHoles(draw);
        DrawTileGrid(draw);
        DrawCursor(draw, hovered);
        draw->PopClipRect();
        draw->AddRect(mapOrigin_, ImVec2(mapOrigin_.x + mapSize_.x, mapOrigin_.y + mapSize_.y),
                      ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.10f)));
    }

    /// Lubang digambar sebagai persegi quad-nya sendiri, bukan dicuplik di
    /// simpul kisi seperti tingginya.
    ///
    /// Tinggi boleh dicuplik: sebuah puncak yang hilang pada zoom jauh tetap
    /// menyisakan lerengnya, jadi petanya tetap benar walaupun kasar. Lubang
    /// tidak punya lereng — ia ada atau tidak ada — dan lubang yang tidak
    /// tergambar terbaca sebagai lubang yang tidak ada. Karena itu peta hole
    /// dipindai pada kisi quad-nya sendiri, dengan langkah yang melebar mengikuti
    /// zoom supaya ongkosnya tetap terbatas, dan sel-sel sebaris digabung menjadi
    /// satu persegi supaya jumlah perintah gambarnya tidak ikut melebar.
    void DrawHoles(ImDrawList* draw) {
        if (terrain_->HoleCount() == 0) {
            return;
        }
        const float spacing = terrain_->Desc().sampleSpacing;
        float minX = 0.0f;
        float minZ = 0.0f;
        float maxX = 0.0f;
        float maxZ = 0.0f;
        ScreenToWorld(mapOrigin_, minX, minZ);
        ScreenToWorld(ImVec2(mapOrigin_.x + mapSize_.x, mapOrigin_.y + mapSize_.y), maxX, maxZ);

        const int x0 = std::max(0, static_cast<int>(std::floor(minX / spacing)));
        const int y0 = std::max(0, static_cast<int>(std::floor(minZ / spacing)));
        const int x1 = std::min(terrain_->SamplesX() - 1,
                                static_cast<int>(std::ceil(maxX / spacing)) + 1);
        const int y1 = std::min(terrain_->SamplesY() - 1,
                                static_cast<int>(std::ceil(maxZ / spacing)) + 1);
        if (x1 <= x0 || y1 <= y0) {
            return;
        }

        constexpr double kMaxTaps = 32768.0;
        const double area = static_cast<double>(x1 - x0) * static_cast<double>(y1 - y0);
        const int step = std::max(1, static_cast<int>(std::ceil(std::sqrt(area / kMaxTaps))));
        const ImU32 color = ImGui::GetColorU32(ImVec4(0.95f, 0.22f, 0.52f, 0.60f));

        for (int y = y0; y < y1; y += step) {
            int run = -1;
            for (int x = x0; x <= x1; x += step) {
                const bool hole = x < x1 && terrain_->HoleAt(x, y);
                if (hole && run < 0) {
                    run = x;
                }
                if (!hole && run >= 0) {
                    draw->AddRectFilled(
                        WorldToScreen(static_cast<float>(run) * spacing,
                                      static_cast<float>(y) * spacing),
                        WorldToScreen(static_cast<float>(std::min(x, x1)) * spacing,
                                      static_cast<float>(std::min(y + step, y1)) * spacing),
                        color);
                    run = -1;
                }
            }
        }
    }

    void DrawTileGrid(ImDrawList* draw) {
        if (!showTiles_) {
            return;
        }
        // Batas ubin diperlihatkan karena ia menentukan apa yang dialokasikan
        // dan apa yang diekspor per berkas — bukan karena ia terlihat pada
        // terrainnya sendiri. Justru sebaliknya: kalau ia terlihat di sana, ada
        // yang salah.
        const TerrainDesc& desc = terrain_->Desc();
        const float tileMeters = static_cast<float>(desc.tileSamples) * desc.sampleSpacing;
        const ImU32 color = ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.14f));
        for (int i = 0; i <= desc.tilesX; ++i) {
            const float x = WorldToScreen(static_cast<float>(i) * tileMeters, 0.0f).x;
            draw->AddLine(ImVec2(x, WorldToScreen(0.0f, 0.0f).y),
                          ImVec2(x, WorldToScreen(0.0f, terrain_->WorldDepth()).y), color);
        }
        for (int j = 0; j <= desc.tilesY; ++j) {
            const float y = WorldToScreen(0.0f, static_cast<float>(j) * tileMeters).y;
            draw->AddLine(ImVec2(WorldToScreen(0.0f, 0.0f).x, y),
                          ImVec2(WorldToScreen(terrain_->WorldWidth(), 0.0f).x, y), color);
        }
    }

    void DrawCursor(ImDrawList* draw, bool hovered) {
        if (!hovered && !stroke_.Active() && !rampAnchored_) {
            return;
        }
        const ImVec2 centre = WorldToScreen(cursorX_, cursorZ_);
        const float falloff = std::clamp(CursorFalloff(), 0.0f, 1.0f);
        const float radius = CursorRadius() / metersPerPixel_;

        if (mode_ == Mode::Holes) {
            // Batas yang digambar adalah batas bobot setengah, karena di situlah
            // quad mulai dipotong. Menggambar jari-jari penuh untuk alat yang
            // memotong hanya sampai setengahnya adalah kursor yang berbohong
            // tepat pada satu-satunya hal yang ditanyakan orang kepadanya.
            draw->AddCircle(centre, radius, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.20f)));
            draw->AddCircle(centre, radius * (1.0f - falloff * 0.5f),
                            ImGui::GetColorU32(ImVec4(1.0f, 0.45f, 0.65f, 0.9f)), 0, 1.5f);
        } else {
            // Dua lingkaran: tepi luar jari-jari, tepi dalam batas bobot penuh.
            // Falloff tidak bisa dibaca dari satu lingkaran, dan menebaknya
            // berarti menggores lalu membatalkan sampai terasa benar.
            draw->AddCircle(centre, radius, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.85f)), 0,
                            1.5f);
            const float inner = radius * (1.0f - falloff);
            if (inner > 2.0f) {
                draw->AddCircle(centre, inner, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.35f)));
            }
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
                                         terrain_->Desc().sampleSpacing * 0.05f,
                                         std::max(terrain_->WorldWidth(), 1.0f) / 64.0f);
            float afterX = 0.0f;
            float afterZ = 0.0f;
            ScreenToWorld(io.MousePos, afterX, afterZ);
            viewX_ += beforeX - afterX;
            viewZ_ += beforeZ - afterZ;
        }

        if (mode_ == Mode::Sculpt && tool_ == Tool::Ramp) {
            HandleRamp(hovered);
            return;
        }
        if (mode_ == Mode::Paint && terrain_->LayerCount() < 2) {
            return;  // tanpa layer di atas dasar, tidak ada yang bisa dicat
        }

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            stroke_.Begin((*terrain_), cursorX_, cursorZ_);
        }
        if (!stroke_.Active()) {
            return;
        }

        const float dt = context.deltaSeconds > 0.0f ? context.deltaSeconds : 1.0f / 60.0f;
        switch (mode_) {
            case Mode::Sculpt:
                stroke_.Advance((*terrain_), EffectiveBrush(), cursorX_, cursorZ_, dt);
                break;
            case Mode::Paint: {
                // Shift menghapus, sama seperti Shift membalik Raise/Lower.
                // Menghapus cat adalah menariknya ke bobot nol, bukan alat lain.
                PaintBrush brush = paint_;
                if (ImGui::GetIO().KeyShift) {
                    brush.target = 0.0f;
                }
                const int layer = paintLayer_;
                stroke_.Advance(brush.radius, cursorX_, cursorZ_, dt,
                                [&](float x, float z, float step) {
                                    ApplyLayerDab((*terrain_), brush, layer, x, z, step);
                                });
                break;
            }
            case Mode::Holes: {
                const bool cut = !ImGui::GetIO().KeyShift;
                stroke_.Advance(paint_.radius, cursorX_, cursorZ_, dt,
                                [&](float x, float z, float) {
                                    ApplyHoleDab((*terrain_), paint_, cut, x, z);
                                });
                break;
            }
        }
        Touch();
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            stroke_.End((*terrain_));
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
            rampStart_ = Vec3(cursorX_, terrain_->HeightAtWorld(cursorX_, cursorZ_), cursorZ_);
            rampAnchored_ = true;
            return;
        }
        const Vec3 end(cursorX_, terrain_->HeightAtWorld(cursorX_, cursorZ_), cursorZ_);
        terrain_->BeginStroke();
        ApplyRamp((*terrain_), Sculpt(), rampStart_, end);
        terrain_->EndStroke();
        rampAnchored_ = false;
        Touch();
    }

    float CursorRadius() const { return mode_ == Mode::Sculpt ? Sculpt().radius : paint_.radius; }
    float CursorFalloff() const { return mode_ == Mode::Sculpt ? Sculpt().falloff : paint_.falloff; }

    /// Brush yang benar-benar dipakai, setelah pengubah papan ketik.
    Brush EffectiveBrush() const {
        Brush brush = Sculpt();
        switch (tool_) {
            case Tool::Raise: brush.kind = BrushKind::Raise; break;
            case Tool::Lower: brush.kind = BrushKind::Lower; break;
            case Tool::Flatten: brush.kind = BrushKind::Flatten; break;
            case Tool::Smooth: brush.kind = BrushKind::Smooth; break;
            case Tool::Noise: brush.kind = BrushKind::Noise; break;
            case Tool::Ramp: break;
        }
        // Pengubah papan ketiknya dipakai bersama viewport. Ctrl melembutkan
        // dan Shift membalik di keduanya, dan menuliskannya dua kali adalah dua
        // tempat yang suatu saat tidak sepakat tentang apa arti Shift.
        return EffectiveSculptBrush(brush, ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift);
    }

    void DrawStatus() {
        const bool inside = cursorX_ >= 0.0f && cursorZ_ >= 0.0f &&
                            cursorX_ <= terrain_->WorldWidth() &&
                            cursorZ_ <= terrain_->WorldDepth();
        ImGui::TextColored(kHintColor, "%.1f, %.1f m   h=%.2f m%s", cursorX_, cursorZ_,
                           terrain_->HeightAtWorld(cursorX_, cursorZ_),
                           inside ? "" : "   (outside)");
        ImGui::SameLine();
        if (mode_ == Mode::Sculpt || terrain_->LayerCount() < 2) {
            // Legenda: warna peta menyesuaikan rentang yang terlihat, jadi
            // artinya harus disebut angkanya — kalau tidak, "hijau" berarti
            // berbeda setiap kali digeser dan tidak ada yang bisa dibaca
            // darinya.
            ImGui::TextColored(kHintColor, "|  shading %.1f..%.1f m  |", visibleLow_, visibleHigh_);
        } else if (mode_ == Mode::Paint) {
            // Bobot di bawah kursor disebut angkanya karena campuran warna tidak
            // bisa dibaca terbalik: hijau yang sedikit lebih pucat bisa berarti
            // 200 atau 230, dan selisih itu yang menentukan apakah masih perlu
            // disapu sekali lagi.
            ImGui::TextColored(kHintColor, "|  %s %d/255  |",
                               terrain_->Layer(paintLayer_).name.c_str(),
                               terrain_->WeightAt(paintLayer_, SampleIndexX(cursorX_),
                                                 SampleIndexZ(cursorZ_)));
        } else {
            ImGui::TextColored(kHintColor, "|  %zu quads cut  |", terrain_->HoleCount());
        }
        ImGui::SameLine();
        ImGui::TextColored(kHintColor, "%zu/%d tiles   %.1f MB   %zu undo",
                           terrain_->TilesResident(),
                           terrain_->Desc().tilesX * terrain_->Desc().tilesY,
                           static_cast<double>(terrain_->BytesResident()) / (1024.0 * 1024.0),
                           terrain_->UndoDepth());
    }

    // --- alat ----------------------------------------------------------------

    void DrawTools(EditorContext& context) {
        if (ImGui::BeginTabBar("##modes")) {
            if (ImGui::BeginTabItem((std::string(icons::kSculpt) + "  Sculpt").c_str())) {
                SetMode(Mode::Sculpt);
                DrawSculptTools();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem((std::string(icons::kBrush) + "  Paint").c_str())) {
                SetMode(Mode::Paint);
                DrawPaintTools(context);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem((std::string(icons::kHole) + "  Holes").c_str())) {
                SetMode(Mode::Holes);
                DrawHoleTools();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        DrawTransferSection(context);
        ImGui::Separator();
        DrawViewSection();
    }

    /// Tampilan peta mengikuti apa yang sedang dikerjakan, tapi hanya saat
    /// modenya berganti — bukan setiap frame. Berpindah tab adalah tindakan yang
    /// disengaja, jadi pantas mengubah tampilan; memaksakannya terus-menerus akan
    /// membuat combo tampilan di bawah tidak bisa dipakai sama sekali.
    void SetMode(Mode mode) {
        if (mode_ == mode) {
            return;
        }
        if (stroke_.Active()) {
            stroke_.End((*terrain_));
        }
        mode_ = mode;
        rampAnchored_ = false;
        if (mode == Mode::Sculpt) {
            view_ = MapView::Relief;
        } else if (mode == Mode::Paint) {
            view_ = MapView::Layers;
        }
    }

    void DrawSculptTools() {
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
        ImGui::DragFloat("Radius", &Sculpt().radius, 0.25f, 0.5f, 2000.0f, "%.1f m");
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("Strength", &Sculpt().strength, 0.1f, 0.0f, 500.0f, "%.1f m/s");
        ImGui::SetNextItemWidth(width);
        ImGui::SliderFloat("Falloff", &Sculpt().falloff, 0.0f, 1.0f);

        if (tool_ == Tool::Flatten) {
            ImGui::SetNextItemWidth(width);
            ImGui::DragFloat("Target", &Sculpt().targetHeight, 0.25f, terrain_->Desc().minHeight,
                             terrain_->Desc().maxHeight, "%.2f m");
            ImGui::SameLine();
            if (ImGui::SmallButton("Pick")) {
                Sculpt().targetHeight = terrain_->HeightAtWorld(cursorX_, cursorZ_);
            }
            widgets::Tooltip("Take the height under the cursor");
        }
        if (tool_ == Tool::Noise) {
            ImGui::SetNextItemWidth(width);
            ImGui::DragFloat("Frequency", &Sculpt().noiseFrequency, 0.001f, 0.001f, 2.0f, "%.3f /m");
            ImGui::SetNextItemWidth(width);
            int seed = static_cast<int>(Sculpt().seed);
            if (ImGui::InputInt("Seed", &seed, 0)) {
                Sculpt().seed = static_cast<uint32_t>(std::max(0, seed));
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
    }

    // --- layer material -------------------------------------------------------

    void DrawPaintTools(EditorContext& context) {
        DrawLayerList(context);
        ImGui::Separator();

        if (terrain_->LayerCount() < 2) {
            ImGui::TextColored(kHintColor,
                               "Add a layer to paint. The base layer\nshows wherever nothing else "
                               "is painted,\nso it has no weight map of its own.");
            return;
        }

        const float width = ImGui::GetFontSize() * 7.0f;
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("Radius", &paint_.radius, 0.25f, 0.5f, 2000.0f, "%.1f m");
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("Strength", &paint_.strength, 0.05f, 0.05f, 20.0f, "%.2f /s");
        widgets::Tooltip("How fast the weight converges, per second");
        ImGui::SetNextItemWidth(width);
        ImGui::SliderFloat("Falloff", &paint_.falloff, 0.0f, 1.0f);
        ImGui::SetNextItemWidth(width);
        ImGui::SliderFloat("Target", &paint_.target, 0.0f, 1.0f);
        widgets::Tooltip("Weight the brush converges to. Hold Shift to erase.");
        DrawBrushProfile();
    }

    void DrawLayerList(EditorContext& context) {
        ImGui::BeginDisabled(terrain_->LayerCount() >= kMaxLayers);
        if (ImGui::Button(icons::kAdd)) {
            AddLayer();
        }
        ImGui::EndDisabled();
        widgets::Tooltip(terrain_->LayerCount() >= kMaxLayers ? "Layer limit reached"
                                                             : "Add a material layer");

        // Layer dasar tidak bisa dihapus maupun dipindah: bobotnya sisa dari yang
        // lain, jadi ia bukan salah satu dari mereka.
        ImGui::SameLine();
        ImGui::BeginDisabled(paintLayer_ <= 0);
        if (ImGui::Button(icons::kDelete)) {
            terrain_->RemoveLayer(paintLayer_);
            paintLayer_ = std::min(paintLayer_, terrain_->LayerCount() - 1);
            Touch();
        }
        widgets::Tooltip("Remove the layer. Its weight returns to the base layer.");
        ImGui::SameLine();
        if (ImGui::Button(icons::kChevronUp) && terrain_->MoveLayer(paintLayer_, paintLayer_ - 1)) {
            paintLayer_ = std::max(1, paintLayer_ - 1);
            Touch();
        }
        ImGui::SameLine();
        if (ImGui::Button(icons::kChevronDown) && terrain_->MoveLayer(paintLayer_, paintLayer_ + 1)) {
            paintLayer_ = std::min(terrain_->LayerCount() - 1, paintLayer_ + 1);
            Touch();
        }
        ImGui::EndDisabled();

        const float rows = std::clamp(static_cast<float>(terrain_->LayerCount()), 3.0f, 7.0f);
        if (ImGui::BeginChild("##layers", ImVec2(0.0f, ImGui::GetFrameHeight() * rows),
                              ImGuiChildFlags_Borders)) {
            for (int index = 0; index < terrain_->LayerCount(); ++index) {
                ImGui::PushID(index);
                const TerrainLayer& layer = terrain_->Layer(index);
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##row", index == paintLayer_, 0,
                                      ImVec2(0.0f, ImGui::GetFrameHeight()))) {
                    paintLayer_ = index;
                }

                // Contoh warnanya digambar di baris, bukan hanya di properti
                // layer terpilih: warna itulah satu-satunya cara membaca peta di
                // sebelah kiri, jadi ia harus terlihat tanpa memilih apa pun.
                const float swatch = ImGui::GetFrameHeight() * 0.55f;
                const float pad = (ImGui::GetFrameHeight() - swatch) * 0.5f;
                ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(origin.x + pad, origin.y + pad),
                    ImVec2(origin.x + pad + swatch, origin.y + pad + swatch),
                    ImGui::GetColorU32(ImVec4(layer.color.x, layer.color.y, layer.color.z, 1.0f)),
                    2.0f);

                ImGui::SameLine(ImGui::GetFrameHeight());
                ImGui::TextUnformatted(layer.name.c_str());
                if (index == 0) {
                    ImGui::SameLine();
                    ImGui::TextColored(kHintColor, "(base)");
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        TerrainLayer& layer = terrain_->Layer(paintLayer_);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputText("##name", &layer.name)) {
            Touch();
        }
        widgets::Tooltip("Layer name");

        ImGui::TextColored(kHintColor, "Material");
        DrawMaterialPicker(context, layer);

        if (ImGui::ColorEdit3("Colour", &layer.color.x, ImGuiColorEditFlags_NoInputs)) {
            Touch();
        }
        widgets::Tooltip("Stand-in colour for the map above, until the 3D viewport can draw the "
                         "material itself");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        if (ImGui::DragFloat("Tile size", &layer.tileSize, 0.05f, 0.05f, 512.0f, "%.2f m")) {
            Touch();
        }
        widgets::Tooltip("Metres per texture repeat");

        ImGui::BeginDisabled(!terrain_->LayerPainted(paintLayer_));
        if (ImGui::Button("Clear painted weight")) {
            terrain_->BeginStroke();
            terrain_->ClearLayerWeights(paintLayer_);
            terrain_->EndStroke();
            Touch();
        }
        ImGui::EndDisabled();
    }

    void DrawMaterialPicker(EditorContext& context, TerrainLayer& layer) {
        const assets::AssetRecord* current =
            layer.material.IsValid() ? context.assets->Find(layer.material.guid) : nullptr;
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (!ImGui::BeginCombo("##material",
                               current != nullptr ? current->name.c_str() : "(none)")) {
            return;
        }
        if (ImGui::Selectable("(none)", !layer.material.IsValid())) {
            layer.material.Clear();
            Touch();
        }
        for (const assets::AssetRecord& record : context.assets->All()) {
            if (record.type != assets::AssetType::Material) {
                continue;
            }
            if (ImGui::Selectable(record.name.c_str(), record.guid == layer.material.guid)) {
                layer.material = AssetRef{record.guid};
                Touch();
            }
        }
        ImGui::EndCombo();
    }

    // --- hole -----------------------------------------------------------------

    void DrawHoleTools() {
        ImGui::TextColored(kHintColor, "Drag to cut holes.\nHold Shift to fill them back in.");
        ImGui::Spacing();

        const float width = ImGui::GetFontSize() * 7.0f;
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("Radius", &paint_.radius, 0.25f, 0.5f, 2000.0f, "%.1f m");
        ImGui::SetNextItemWidth(width);
        ImGui::SliderFloat("Falloff", &paint_.falloff, 0.0f, 1.0f);
        widgets::Tooltip("A quad is either cut or it is not, so falloff is a threshold here, not a "
                         "gradient: quads past half weight go.");
        DrawBrushProfile();

        ImGui::Spacing();
        ImGui::TextColored(kHintColor, "%zu quads cut", terrain_->HoleCount());
        ImGui::BeginDisabled(terrain_->HoleCount() == 0);
        if (ImGui::Button("Fill every hole")) {
            // Dibungkus goresan supaya satu Ctrl+Z mengembalikannya. Menghapus
            // seluruh pekerjaan tanpa jalan pulang adalah tombol yang tidak berani
            // ditekan siapa pun.
            terrain_->BeginStroke();
            terrain_->ClearHoles();
            terrain_->EndStroke();
            Touch();
        }
        ImGui::EndDisabled();
    }

    // --- berkas dan tampilan --------------------------------------------------

    void DrawTransferSection(EditorContext& context) {
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
    }

    void DrawViewSection() {
        static constexpr std::array<const char*, 3> kViews{"Relief", "Layers", "Weight"};
        int view = static_cast<int>(view_);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
        ImGui::BeginDisabled(terrain_->LayerCount() < 2);
        if (ImGui::Combo("Map", &view, kViews.data(), static_cast<int>(kViews.size()))) {
            view_ = static_cast<MapView>(view);
        }
        ImGui::EndDisabled();
        widgets::Tooltip(terrain_->LayerCount() < 2
                             ? "Relief only, until there is a second layer to show"
                             : "Relief shading, blended layer colours, or the selected layer's "
                               "weight alone");

        ImGui::Checkbox("Show tile bounds", &showTiles_);
        if (ImGui::Button("Fit view")) {
            FitView();
        }
    }

    void AddLayer() {
        TerrainLayer layer;
        const int index = terrain_->LayerCount();
        layer.name = "Layer " + std::to_string(index);
        const std::array<float, 3>& rgb =
            kLayerPalette[static_cast<std::size_t>(index - 1) % kLayerPalette.size()];
        layer.color = Vec3(rgb[0], rgb[1], rgb[2]);
        const int added = terrain_->AddLayer(layer);
        if (added < 0) {
            return;
        }
        paintLayer_ = added;
        Touch();
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
        if (mode_ == Mode::Holes) {
            // Garis ambangnya digambar karena di situlah kurvanya berhenti
            // berarti: di atas garis quad-nya hilang, di bawahnya tidak, dan
            // tidak ada di antaranya.
            const float y = origin.y + size.y * 0.5f;
            draw->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + size.x, y),
                          ImGui::GetColorU32(ImVec4(1.0f, 0.45f, 0.65f, 0.55f)));
        }

        ImVec2 previous(origin.x, origin.y + size.y);
        constexpr int kSteps = 48;
        for (int i = 0; i <= kSteps; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(kSteps);
            const float unit = std::abs(t * 2.0f - 1.0f);
            const float weight = mode_ == Mode::Sculpt
                                     ? BrushWeight(Sculpt(), unit * Sculpt().radius)
                                     : BrushWeight(paint_, unit * paint_.radius);
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
            rect = SampleRect{0, 0, terrain_->SamplesX(), terrain_->SamplesY()};
        } else {
            rect = terrain_->RectForCircle(cursorX_, cursorZ_, Sculpt().radius);
        }
        if (rect.Empty()) {
            return;
        }
        // Dibungkus goresan supaya bisa dibatalkan seperti sapuan brush biasa.
        // Erosi yang tidak bisa dibatalkan berarti setiap percobaan menuntut
        // menyimpan lebih dulu.
        terrain_->BeginStroke();
        ApplyThermalErosion((*terrain_), rect, erosionIterations_, erosionTalus_, erosionRate_);
        terrain_->EndStroke();
        Touch();
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
        const std::filesystem::path path = context.assets->AbsolutePath(*record);
        // Lewat store, bukan dimuat sendiri: viewport menggambar terrain yang
        // sama, dan dua salinan berarti yang tergambar adalah bentuk sebelum
        // goresan terakhir — pada dokumen seukuran ini, juga dua kali memorinya.
        terrain::Terrain* opened = context.terrains->Get(guid, path);
        if (opened == nullptr) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Cannot open " + record->name +
                                             "; see the Console");
            }
            return;
        }
        openGuid_ = guid;
        openName_ = record->name;
        openPath_ = context.assets->AbsolutePath(*record);
        transferFile_ = openPath_.stem().string() + ".png";
        rampAnchored_ = false;
        // Yang baru dibuka dipegang di sini, bukan lewat `terrain_`: penunjuk
        // itu disetel di awal frame, dan frame ini sudah lewat awalnya.
        terrain_ = opened;
        document_ = context.terrains->Document(guid);
        Sculpt().targetHeight = terrain_->Desc().baseHeight;
        // Jari-jari bawaan mengikuti ukuran peta. Brush sepuluh meter di atas
        // terrain dua kilometer adalah titik jarum: goresan pertama tidak
        // terlihat, dan kesan pertamanya adalah alat yang rusak.
        Sculpt().radius = std::clamp(terrain_->WorldWidth() * 0.02f, 1.0f, 500.0f);
        Sculpt().strength = std::max((terrain_->Desc().maxHeight - terrain_->Desc().minHeight) * 0.02f,
                                   0.5f);
        // Kuas cat mengikuti ukuran yang sama, dengan alasan yang sama. Ia tidak
        // ikut menyalin kekuatannya: kekuatan sculpt meter per detik, kekuatan
        // cat laju konvergensi, dan menyalin angka di antara dua satuan yang
        // berbeda adalah cara membuat sapuan pertama selalu salah.
        paint_.radius = Sculpt().radius;
        paintLayer_ = terrain_->LayerCount() > 1 ? 1 : 0;
        FitView();
    }

    /// Kuas sculpt milik store, bukan milik panel: viewport memakai yang sama.
    Brush& Sculpt() { return store_->SculptBrush(); }
    const Brush& Sculpt() const { return store_->SculptBrush(); }

    /// Menandai terrain yang terbuka berubah.
    ///
    /// Satu tempat, dipanggil dari mana-mana: goresan, impor, penyuntingan
    /// layer. Yang menaikkan penanda versinya sendiri-sendiri akan melewatkan
    /// satu jalur, dan yang terlewat adalah goresan yang tidak pernah tergambar
    /// ulang di viewport.
    void Touch() {
        if (store_ != nullptr) {
            store_->MarkDirty(openGuid_);
        }
    }

    void Save(EditorContext& context) {
        std::string error;
        if (!context.terrains->Save(openGuid_, openPath_, error)) {
            if (context.notifications != nullptr) {
                context.notifications->Error("Save failed: " + error);
            }
            return;
        }
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
            result = raw ? SaveHeightmapRaw((*terrain_), path) : SaveHeightmapImage((*terrain_), path);
        } else {
            result = raw ? LoadHeightmapRaw((*terrain_), path) : LoadHeightmapImage((*terrain_), path);
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
            terrain_->ClearHistory();
            Touch();
        }
        context.notifications->Success((exporting ? "Exported " : "Imported ") +
                                       path.filename().string());
    }

    /// Dokumen yang sedang dibuka — **milik store, bukan milik panel**, dan
    /// hanya berlaku selama satu frame. Disetel di awal `OnDraw`, tempat satu
    /// pemeriksaan menggantikan pemeriksaan di setiap jalur di bawahnya.
    Terrain* terrain_ = nullptr;
    TerrainDocument* document_ = nullptr;
    TerrainStore* store_ = nullptr;

    TerrainDesc newDesc_;

    Uuid openGuid_;
    std::string openName_;
    std::filesystem::path openPath_;
    std::string transferFile_;

    Mode mode_ = Mode::Sculpt;
    MapView view_ = MapView::Relief;
    Tool tool_ = Tool::Raise;
    PaintBrush paint_;
    int paintLayer_ = 0;
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
