#include "Sim/Editor/PanelManager.h"

#include "Sim/Core/Log.h"

#include <nlohmann/json.hpp>

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>

namespace sim::editor {
namespace {

const char* CategoryLabel(PanelCategory category) {
    switch (category) {
        case PanelCategory::Scene: return "Scene";
        case PanelCategory::Assets: return "Assets";
        case PanelCategory::Authoring: return "Authoring";
        case PanelCategory::Debug: return "Debug";
    }
    return "Other";
}

}  // namespace

Panel* PanelManager::Add(std::unique_ptr<Panel> panel) {
    if (Find(panel->Id()) != nullptr) {
        SIM_WARN("Editor", "Panel id '{}' registered twice", panel->Id());
        return nullptr;
    }
    panels_.push_back(std::move(panel));
    return panels_.back().get();
}

Panel* PanelManager::Find(std::string_view id) {
    auto it = std::find_if(panels_.begin(), panels_.end(),
                           [id](const std::unique_ptr<Panel>& panel) { return panel->Id() == id; });
    return it != panels_.end() ? it->get() : nullptr;
}

void PanelManager::Update(EditorContext& context) {
    for (std::unique_ptr<Panel>& panel : panels_) {
        panel->OnUpdate(context);
    }
}

void PanelManager::Draw(EditorContext& context) {
    for (std::unique_ptr<Panel>& panel : panels_) {
        if (!panel->IsOpen()) {
            panel->SetFocused(false);
            continue;
        }

        if (panel->IsDockLocked()) {
            // NoUndocking berlaku pada node dock yang menampung panel ini,
            // sehingga menyeret tab-nya tidak lagi melepaskannya jadi jendela
            // OS terpisah. NoMove menutup jalur satunya lagi: menyeret badan
            // jendela ketika panel kebetulan sedang mengambang.
            ImGuiWindowClass windowClass;
            windowClass.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoUndocking;
            ImGui::SetNextWindowClass(&windowClass);
        }

        // Ukuran bawaan hanya berlaku saat panel muncul pertama kali sebagai
        // jendela mengambang — panel yang sudah punya tempat di layout dock
        // mengabaikannya. Tanpa ini ImGui menyesuaikan ukuran jendela dengan
        // isinya, dan panel yang justru mengisi ruang yang tersedia (transkrip
        // setinggi sisa jendela) menjadi umpan balik: jendela membesar, isinya
        // ikut membesar, dan begitu seterusnya tiap frame.
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f,
                                        ImGui::GetFontSize() * 20.0f),
                                 ImGuiCond_FirstUseEver);

        const bool zeroPadding = panel->WantsZeroPadding();
        if (zeroPadding) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        }

        // Judul memakai "Name###id": bagian setelah ### jadi identitas jendela
        // yang stabil, sehingga judul boleh berubah (mis. menampilkan nama
        // level) tanpa membuat ImGui menganggapnya jendela baru dan kehilangan
        // posisi dock-nya.
        const std::string label = panel->Title() + "###" + panel->Id();
        int windowFlags = panel->WindowFlags();
        // p_open == nullptr membuat ImGui tidak menggambar tombol X sama sekali,
        // baik di tab maupun di title bar.
        bool* openFlag = panel->OpenFlag();
        if (panel->IsDockLocked()) {
            windowFlags |= ImGuiWindowFlags_NoMove;
            openFlag = nullptr;
        }
        const bool visible = ImGui::Begin(label.c_str(), openFlag, windowFlags);
        EnforceViewportPlacement();

        if (zeroPadding) {
            ImGui::PopStyleVar();
        }

        panel->SetFocused(ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows));
        if (visible) {
            panel->OnDraw(context);
        }
        ImGui::End();
    }
}

bool PanelManager::SaveState(const std::filesystem::path& path) const {
    nlohmann::json root;
    root["version"] = 1;
    nlohmann::json open = nlohmann::json::object();
    for (const std::unique_ptr<Panel>& panel : panels_) {
        open[panel->Id()] = panel->IsOpen();
    }
    root["open"] = std::move(open);

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path);
    if (!file) {
        return false;
    }
    file << root.dump(2) << '\n';
    return true;
}

bool PanelManager::LoadState(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return false;
    }
    nlohmann::json root;
    try {
        file >> root;
    } catch (const nlohmann::json::exception& error) {
        SIM_WARN("Editor", "Panel state is not valid JSON ({}): {}", path.string(), error.what());
        return false;
    }
    const auto open = root.find("open");
    if (open == root.end() || !open->is_object()) {
        return false;
    }
    for (const auto& [id, value] : open->items()) {
        Panel* panel = Find(id);
        // Panel terkunci tidak boleh dipulihkan dalam keadaan tertutup: tombol
        // penutupnya sengaja dihilangkan, jadi tidak akan ada cara membukanya.
        if (panel != nullptr && value.is_boolean() && !panel->IsDockLocked()) {
            panel->SetOpen(value.get<bool>());
        }
    }
    return true;
}

void PanelManager::EnforceViewportPlacement() {
    // Menegakkan kembali posisi jendela panel yang punya viewport sendiri.
    //
    // Masalah yang diperbaiki: panel yang tersimpan berada di monitor kedua
    // selalu kembali ke jendela utama saat editor dijalankan lagi. Penyimpanan
    // layout sebenarnya baik-baik saja — ImGui memulihkan posisi dengan benar
    // dan mempertahankannya selama jendela platformnya belum ada. Begitu
    // jendela itu dibuat, window manager X11 menempatkannya di tempat lain
    // (di sini: pojok jendela utama). ImGui kemudian membaca balik posisi hasil
    // WM, memindahkan viewport ke monitor jendela utama, dan karena sekarang
    // bertumpang tindih dengan viewport utama, menggabungkannya kembali.
    //
    // Karena itu posisi yang ImGui inginkan direkam selama jendela platform
    // belum ada, lalu dipaksakan kembali beberapa frame setelah jendela itu
    // muncul. Setelah WM menerima posisi tersebut, entri dibuang dan kita tidak
    // ikut campur lagi.
    constexpr int kEnforceFrames = 8;
    // Ambang cukup besar supaya penyesuaian wajar dari WM (dekorasi, bar
    // panel) tidak dianggap sebagai penolakan posisi.
    constexpr float kToleranceInPixels = 16.0f;

    ImGuiWindow* window = ImGui::GetCurrentWindow();
    auto* viewport = static_cast<ImGuiViewportP*>(window->Viewport);
    if (viewport == nullptr) {
        return;
    }

    if (!window->ViewportOwned) {
        placements_.erase(viewport->ID);
        return;
    }

    ViewportPlacement& placement = placements_[viewport->ID];

    if (!viewport->PlatformWindowCreated) {
        // Belum ada jendela OS: apa pun posisi ImGui sekarang adalah yang
        // diinginkan, entah berasal dari layout tersimpan atau dari seretan.
        placement.wantedX = viewport->Pos.x;
        placement.wantedY = viewport->Pos.y;
        placement.framesRemaining = kEnforceFrames;
        return;
    }

    if (placement.framesRemaining <= 0) {
        placements_.erase(viewport->ID);
        return;
    }
    --placement.framesRemaining;

    // Saat pengguna sedang menyeret jendela, posisinya memang berubah tiap
    // frame dan bukan urusan kita.
    if (ImGui::GetCurrentContext()->MovingWindow != nullptr) {
        placements_.erase(viewport->ID);
        return;
    }

    const float driftX = std::abs(viewport->Pos.x - placement.wantedX);
    const float driftY = std::abs(viewport->Pos.y - placement.wantedY);
    if (driftX > kToleranceInPixels || driftY > kToleranceInPixels) {
        ImGui::SetWindowPos(ImVec2(placement.wantedX, placement.wantedY));
    }
}

void PanelManager::DrawWindowMenu() {
    constexpr std::array<PanelCategory, 4> kOrder{PanelCategory::Scene, PanelCategory::Assets,
                                                  PanelCategory::Authoring, PanelCategory::Debug};

    bool firstSection = true;
    for (PanelCategory category : kOrder) {
        bool sectionHasPanels = false;
        for (const std::unique_ptr<Panel>& panel : panels_) {
            if (panel->Category() != category) {
                continue;
            }
            if (!sectionHasPanels) {
                if (!firstSection) {
                    ImGui::Separator();
                }
                ImGui::SeparatorText(CategoryLabel(category));
                sectionHasPanels = true;
                firstSection = false;
            }
            // Panel terkunci tidak bisa ditutup, jadi entri menunya ditampilkan
            // tercentang tapi nonaktif — kalau tetap bisa diklik, pengguna
            // punya jalan menutup panel yang tombol X-nya sengaja dihilangkan.
            const bool closable = !panel->IsDockLocked();
            if (ImGui::MenuItem(panel->Title().c_str(), nullptr, panel->IsOpen(), closable) &&
                closable) {
                panel->SetOpen(!panel->IsOpen());
            }
        }
    }
}

}  // namespace sim::editor
