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
#include "Sim/Render/Ibl.h"
#include "Sim/Render/TimeOfDay.h"
#include "Sim/SceneView/SceneView.h"
#include "Sim/Scene/World.h"
#include "Sim/Scene/ComponentRegistry.h"
#include "Sim/Scene/Serialization.h"
#include "Sim/Scene/WorldSettings.h"

#include <imgui.h>

#include <string>
#include <vector>

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

        DrawUnsupportedNotice(context);
        DrawSunExtraction(context);
    }

private:
    /// Langit pertama di dunia, atau nullptr.
    static const scene::SkyComponent* FindSky(const scene::World& world) {
        for (const auto raw : world.Registry().view<scene::SkyComponent>()) {
            return world.TryGet<scene::SkyComponent>(static_cast<scene::Entity>(raw));
        }
        return nullptr;
    }

    /// Lampu directional pertama di dunia, atau entity null.
    static scene::Entity FindDirectionalLight(scene::World& world) {
        for (const auto raw : world.Registry().view<scene::LightComponent>()) {
            const auto entity = static_cast<scene::Entity>(raw);
            const auto* light = world.TryGet<scene::LightComponent>(entity);
            if (light != nullptr && light->type == scene::LightType::Directional) {
                return entity;
            }
        }
        return scene::kNullEntity;
    }

    /// Menawarkan memindahkan matahari dari berkas lingkungan ke lampu
    /// directional adegan (B4).
    ///
    /// **Sebuah tawaran, bukan sesuatu yang terjadi sendiri.** Menyalakan
    /// ekstraksi tanpa mengisi lampunya menghasilkan adegan yang kehilangan
    /// mataharinya sama sekali; mengisi lampunya tanpa menyalakan ekstraksi
    /// menghasilkan dua matahari. Tombol ini mengerjakan keduanya dalam satu
    /// transaksi, jadi tidak ada keadaan setengah jalan yang bisa ditinggalkan
    /// siapa pun.
    void DrawSunExtraction(EditorContext& context) {
        const scene::WorldSettings settings = context.world->Settings();
        if (settings.environment != scene::EnvironmentSource::File) {
            return;
        }
        const scene::SkyComponent* sky = FindSky(*context.world);
        if (sky == nullptr || sky->hdriPath.empty()) {
            return;
        }

        ImGui::Separator();
        ImGui::TextWrapped(
            "Berkas HDR sudah berisi mataharinya. Kalau level ini juga punya lampu "
            "directional, ada dua — dan yang terlihat cuma bayangan yang terlalu tegas.");

        const scene::Entity light = FindDirectionalLight(*context.world);
        if (!scene::IsValid(light)) {
            ImGui::TextDisabled("Tidak ada lampu directional untuk diisi.");
            return;
        }

        // **Time-of-Day menimpa setiap lampu directional tiap frame** — arah
        // dan warnanya sekaligus. Menekan tombol ini selagi ia menyala membuat
        // hasilnya hilang pada frame berikutnya, sementara `extractSun` tetap
        // menyala: adegannya lalu kehilangan matahari HDRI-nya dan penggantinya
        // tidak cocok dengan fotonya. Yang gagal diam-diam lebih buruk daripada
        // tombol yang jujur tidak tersedia.
        if (context.timeOfDayEnabled) {
            ImGui::TextColored(ImVec4(0.94f, 0.72f, 0.35f, 1.0f), "%s",
                               "Time of Day sedang menggerakkan matahari, dan ia menimpa "
                               "lampu directional tiap frame. Matikan 'Drive the sun' dulu.");
            ImGui::BeginDisabled();
            ImGui::Button("Pindahkan matahari ke lampu directional");
            ImGui::EndDisabled();
            return;
        }

        if (!ImGui::Button("Pindahkan matahari ke lampu directional")) {
            if (!extractionStatus_.empty()) {
                ImGui::TextWrapped("%s", extractionStatus_.c_str());
            }
            return;
        }

        // **Di main thread, dan itu diterima di sini.** Memuat HDR 4K memakan
        // sekitar sedetik — mahal untuk sebuah frame, murah untuk sebuah klik
        // yang memang diminta orangnya dan yang hasilnya ia tunggu. Yang tidak
        // boleh menahan frame adalah panggangan yang berjalan sendiri, dan itu
        // memang sudah di kolam tugas.
        const std::string resolved = ResolveHdriPath(sky->hdriPath, context.builtinDir);
        render::EquirectEnvironment map = render::LoadHdrEquirect(resolved);
        if (!map.IsValid()) {
            extractionStatus_ = "Berkas lingkungannya tidak bisa dibaca — lihat Console.";
            return;
        }
        map.intensity = sky->hdriIntensity;

        const render::ExtractedSun sun = render::ExtractSun(map);
        if (!sun.found) {
            extractionStatus_ =
                "Tidak ada matahari yang menonjol di berkas ini. Langit mendung tidak punya "
                "yang bisa dipindahkan, dan melubanginya hanya menambah bercak.";
            return;
        }

        ApplyExtractedSun(context, light, sun);
        extractionStatus_ = "Matahari dipindahkan ke lampu directional.";
    }

    /// Menulis hasil ekstraksi ke lampu dan ke World Settings, sekali jalan.
    static void ApplyExtractedSun(EditorContext& context, scene::Entity light,
                                  const render::ExtractedSun& sun) {
        scene::World& world = *context.world;
        const auto* transform = world.TryGet<scene::TransformComponent>(light);
        const auto* component = world.TryGet<scene::LightComponent>(light);
        if (transform == nullptr || component == nullptr) {
            return;
        }

        scene::TransformComponent orientedTransform = *transform;
        // Rotasi yang membuat sumbu −Z entity menunjuk **menjauhi** matahari:
        // lampu directional memancar ke arah hadapnya, sedangkan arah hasil
        // ekstraksi menunjuk dari permukaan ke matahari. Aturan yang sama persis
        // dengan yang dipakai Time-of-Day.
        orientedTransform.rotation = render::LookRotation(-sun.direction);

        scene::LightComponent litComponent = *component;
        // Iradiansinya masuk seluruhnya ke warna, dan intensitasnya satu:
        // renderer memakai `color * intensity` sebagai iradiansi, jadi
        // menyimpannya dua kali berarti dua angka yang bisa berselisih — aturan
        // yang sama yang sudah dipegang Time-of-Day.
        litComponent.color = sun.irradiance;
        litComponent.intensity = 1.0f;

        scene::WorldSettings settings = context.world->Settings();
        const scene::WorldSettings before = settings;
        settings.extractSun = true;

        const scene::ComponentOps* ops = scene::ComponentRegistry::Get().Find("Light");
        if (ops == nullptr) {
            return;
        }

        // Satu transaksi: undo mengembalikan ketiganya sekaligus. Yang terpisah
        // bisa dibatalkan setengah, dan setengahnya adalah dua matahari atau
        // tidak satu pun.
        context.history->CloseMergeGroup();
        context.history->BeginTransaction("Extract sun from environment");
        context.history->Execute<SetWorldSettingsCommand>(&world, before, settings);
        context.history->Execute<SetTransformsCommand>(
            &world,
            std::vector<SetTransformsCommand::Item>{
                {world.GuidOf(light), *transform, orientedTransform}},
            "Aim sun");
        context.history->Execute<SetComponentsCommand>(
            &world, ops,
            std::vector<SetComponentsCommand::Item>{
                {world.GuidOf(light), scene::SerializeComponent(*ops->type, component),
                 scene::SerializeComponent(*ops->type, &litComponent)}});
        context.history->EndTransaction();
    }

    std::string extractionStatus_;

    /// **Dinyatakan, bukan didiamkan.** `RealTime` + `File` bukan kombinasi yang
    /// sah: probe GI menelusuri langit yang tergambar, dan sebuah foto tidak bisa
    /// menjadi masukannya tanpa matahari terhitung dua kali. Yang menyatakannya
    /// dengan benar — beserta dua jalan keluarnya, dan tanpa jalur diam-diam yang
    /// menyinari adegan memakai langit yang bukan langit yang tergambar — adalah
    /// B6. Yang di sini baru kalimatnya, supaya kombinasi itu tidak sempat
    /// terlihat seolah didukung selama B1–B5 dikerjakan.
    static void DrawUnsupportedNotice(EditorContext& context) {
        const scene::WorldSettings settings = context.world->Settings();
        const scene::SkyComponent* sky = FindSky(*context.world);
        const bool proceduralSky = sky != nullptr && sky->source == scene::SkySourceKind::Atmosphere;

        const bool contradictory = scene::IsUnsupported(settings);
        const bool blind = scene::RealTimeHasNoSky(settings, proceduralSky);
        if (!contradictory && !blind) {
            return;
        }

        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.72f, 0.35f, 1.0f));
        if (blind) {
            // **Yang dikatakan apa yang sedang terjadi, bukan apa yang salah
            // secara abstrak.** Sampai B6, keadaan ini menyinari adegan dengan
            // gradien analitik yang tidak ada hubungannya dengan langit yang
            // tergambar; sekarang ia tidak menyinarinya sama sekali. Adegan yang
            // tiba-tiba gelap tanpa penjelasan adalah cacat yang sama buruknya
            // dengan adegan yang diam-diam salah warna.
            ImGui::TextWrapped(
                "Probe GI mencuplik langit atmosferik, dan level ini tidak punya satu pun. "
                "Cahaya tak-langsungnya karena itu nol — berkas HDR adalah latar, bukan "
                "cahaya (keputusan 1 di docs/PLAN-IBL.md).");
        } else {
            ImGui::TextWrapped(
                "RealTime + File bukan kombinasi yang didukung. Berkas HDR adalah satu foto "
                "yang sudah berisi mataharinya, jadi ia latar — bukan cahaya.");
        }
        ImGui::PopStyleColor();

        // **Dua jalan keluar, dan keduanya sekali klik.** Sebuah peringatan yang
        // hanya menyebutkan masalahnya menyerahkan pekerjaannya kembali kepada
        // yang membacanya — dan yang membacanya belum tentu tahu setelan mana
        // yang harus digeser.
        if (ImGui::Button("Pakai tingkat Baked")) {
            scene::WorldSettings baked = settings;
            baked.indirect = scene::IndirectLighting::Baked;
            context.history->CloseMergeGroup();
            context.history->Execute<SetWorldSettingsCommand>(context.world, settings, baked);
        }
        ImGui::SameLine();
        const bool canSwitchSky = sky != nullptr && !proceduralSky;
        ImGui::BeginDisabled(!canSwitchSky);
        if (ImGui::Button("Pakai langit atmosfer")) {
            SwitchSkyToAtmosphere(context);
        }
        ImGui::EndDisabled();
        if (sky == nullptr) {
            ImGui::TextDisabled("Level ini belum punya langit — tempatkan prefab Sky Dome.");
        }
    }

    /// Menukar sumber langit adegan menjadi atmosfer, lewat perintah.
    static void SwitchSkyToAtmosphere(EditorContext& context) {
        scene::World& world = *context.world;
        for (const auto raw : world.Registry().view<scene::SkyComponent>()) {
            const auto entity = static_cast<scene::Entity>(raw);
            const auto* component = world.TryGet<scene::SkyComponent>(entity);
            if (component == nullptr || component->source == scene::SkySourceKind::Atmosphere) {
                continue;
            }
            const scene::ComponentOps* ops = scene::ComponentRegistry::Get().Find("Sky");
            if (ops == nullptr) {
                return;
            }
            scene::SkyComponent switched = *component;
            switched.source = scene::SkySourceKind::Atmosphere;

            context.history->CloseMergeGroup();
            context.history->Execute<SetComponentsCommand>(
                &world, ops,
                std::vector<SetComponentsCommand::Item>{
                    {world.GuidOf(entity), scene::SerializeComponent(*ops->type, component),
                     scene::SerializeComponent(*ops->type, &switched)}});
            return;
        }
    }
};

}  // namespace

SIM_REGISTER_PANEL(WorldSettingsPanel, 41)

}  // namespace sim::editor
