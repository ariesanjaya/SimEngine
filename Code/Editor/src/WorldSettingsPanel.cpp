// Panel World Settings — B0 di docs/PLAN-IBL.md.
//
// **Panelnya sendiri, bukan bagian Inspector.** Inspector menyunting entity yang
// terpilih, dan World Settings tidak dimiliki entity mana pun: menempelkannya di
// keadaan "tidak ada yang terpilih" berarti sebuah setelan level yang hanya bisa
// dicapai dengan lebih dulu membatalkan seleksi, dan hilang lagi begitu ada yang
// diklik.
//
// **Tidak ada satu widget pun di sini yang ditulis tangan**, dan itulah
// pembayaran dari `WorldSettings` yang terdaftar di `reflect::TypeRegistry`:
// `DrawProperties` merender tipe terdaftar apa pun. Menambah setelan baru ke
// World Settings berarti satu baris di pendaftaran tipe — panel ini tidak perlu
// disentuh sama sekali.

#include "Sim/Editor/Command.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/PropertyGrid.h"
#include "Sim/Editor/SceneCommands.h"
#include "Sim/Reflect/TypeRegistry.h"
#include "Sim/Scene/World.h"
#include "Sim/Scene/WorldSettings.h"

#include <imgui.h>

namespace sim::editor {
namespace {

class WorldSettingsPanel final : public Panel {
public:
    WorldSettingsPanel()
        : Panel(panel_id::kWorldSettings,
                std::string(icons::kPanelWorldSettings) + "  World Settings",
                PanelCategory::Scene) {
        // **Terbuka sejak awal, sebagai tab di samping Inspector.** Tingkat
        // pencahayaan adalah keputusan yang diambil saat menata level, bukan
        // sesuatu yang dicari sekali lalu dilupakan — dan yang harus dibuka dari
        // menu View lebih dulu adalah yang tidak pernah dibuka siapa pun.
    }

    void OnDraw(EditorContext& context) override {
        if (context.world == nullptr || context.history == nullptr) {
            ImGui::TextDisabled("No world.");
            return;
        }
        const reflect::TypeDesc* type =
            reflect::TypeRegistry::Get().Find<scene::WorldSettings>();
        if (type == nullptr) {
            ImGui::TextDisabled("WorldSettings is not registered.");
            return;
        }

        ImGui::TextDisabled("Tersimpan di dalam berkas level.");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
            ImGui::SetTooltip(
                "Maksud pengarang, bukan anggaran mesin: level yang dirancang\n"
                "dengan GI real-time memakai tingkat itu di mesin mana pun yang\n"
                "membukanya. Resolusi probe dan pilihan backend tinggal di\n"
                "project, karena mesin lemah tidak boleh dipaksa mengikuti\n"
                "setelan mesin kuat.");
        }
        ImGui::Separator();

        // Disalin lebih dulu: `DrawProperties` menyunting objeknya di tempat,
        // jadi nilai sebelumnya harus sudah dipegang sebelum ia dipanggil —
        // kalau tidak, `before` dan `after` akan menjadi objek yang sama dan
        // undo tidak mengembalikan apa pun.
        const scene::WorldSettings before = context.world->Settings();
        scene::WorldSettings edited = before;

        const PropertyGridResult result = DrawProperties(*type, &edited);
        if (result.edited) {
            context.history->Execute<SetWorldSettingsCommand>(context.world, before, edited);
        }
        if (result.finished) {
            // Menutup kelompok penggabungan: satu seretan slider adalah satu
            // entri undo, dan yang berikutnya entri baru.
            context.history->CloseMergeGroup();
        }

        DrawUnsupportedNotice(context.world->Settings());
    }

private:
    /// **Dinyatakan, bukan didiamkan.** `RealTime` + `File` bukan kombinasi yang
    /// sah: probe GI menelusuri langit yang tergambar, dan sebuah foto tidak bisa
    /// menjadi masukannya tanpa matahari terhitung dua kali. Yang menyatakannya
    /// dengan benar — beserta dua jalan keluarnya, dan tanpa jalur diam-diam yang
    /// menyinari adegan memakai langit yang bukan langit yang tergambar — adalah
    /// B6. Yang di sini baru kalimatnya, supaya kombinasi itu tidak sempat
    /// terlihat seolah didukung selama B1–B5 dikerjakan.
    static void DrawUnsupportedNotice(const scene::WorldSettings& settings) {
        if (!scene::IsUnsupported(settings)) {
            return;
        }
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.72f, 0.35f, 1.0f));
        ImGui::TextWrapped(
            "RealTime + File bukan kombinasi yang didukung. Berkas HDR adalah satu foto "
            "yang sudah berisi mataharinya, jadi ia latar — bukan cahaya. Pilih Baked "
            "untuk disinari berkasnya, atau Sky untuk disinari langit yang tergambar.");
        ImGui::PopStyleColor();
    }
};

}  // namespace

SIM_REGISTER_PANEL(WorldSettingsPanel, 41)

}  // namespace sim::editor
