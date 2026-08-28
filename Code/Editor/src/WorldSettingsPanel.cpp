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

#include "Sim/Core/Log.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/PropertyGrid.h"
#include "Sim/Editor/SceneCommands.h"
#include "Sim/Reflect/TypeRegistry.h"
#include "Sim/Render/Ibl.h"
#include "Sim/Render/ProbeVolume.h"
#include "Sim/SceneView/ProbeBakery.h"
#include "Sim/Render/TimeOfDay.h"
#include "Sim/SceneView/SceneView.h"
#include "Sim/Scene/World.h"
#include "Sim/Scene/ComponentRegistry.h"
#include "Sim/Scene/Serialization.h"
#include "Sim/Scene/WorldSettings.h"

#include <imgui.h>

#include <functional>
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
        DrawTimeOfDayConflict(context);
        DrawBakeStatus(context);
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

    /// Menyatakan Time-of-Day yang bertabrakan dengan tingkat Precomputed (S0).
    ///
    /// **Precomputed berdiri di atas satu andaian: mataharinya diam.** Itu bukan
    /// pembatasan yang dipilih melainkan syarat yang membuat transport bisa
    /// dipanggang sama sekali. Time-of-Day menggerakkannya tiap frame, dan yang
    /// terlihat kemudian bukan galat melainkan bayangan langsung yang bergerak
    /// sementara bayangan tak-langsungnya diam — dan itu terbaca sebagai "GI-nya
    /// rusak", bukan sebagai dua setelan yang tidak boleh menyala bersamaan.
    static void DrawTimeOfDayConflict(EditorContext& context) {
        if (!scene::PrecomputedFightsTimeOfDay(context.world->Settings(),
                                               context.timeOfDayEnabled)) {
            return;
        }
        ImGui::Separator();
        // **Nadanya bergantung pada apa yang benar-benar sudah dipanggang, bukan
        // pada tingkat yang dipilih.** Selama yang dipanggang cuma lingkungannya,
        // iradiansinya ikut bergeser tiap matahari bergerak — kombinasi ini
        // justru yang dibangun dan diuji B1, dan memberi peringatan berwarna di
        // sana berarti mengatakan sebuah susunan yang bekerja itu rusak.
        //
        // Begitu ada kisi yang transportnya ditelusuri (S2), andaian yang
        // menopangnya patah: pantulan dan oklusinya terpanggang pada posisi
        // matahari saat memanggang, dan menggerakkannya membuat bayangan
        // langsung berjalan sementara bayangan tak-langsungnya diam. Tidak ada
        // satu pun galat yang menyebutkannya.
        const bool transportBaked = context.probeVolume != nullptr;
        if (transportBaked) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "Time of Day menggerakkan matahari, dan transport cahaya "
                               "adegan ini sudah dipanggang.");
            ImGui::TextWrapped(
                "Pantulan dan oklusinya terpanggang pada posisi matahari saat memanggang. "
                "Menggerakkannya membuat bayangan langsung berjalan sementara bayangan "
                "tak-langsungnya diam — panggang ulang, atau hentikan Time of Day.");
        } else {
            ImGui::TextWrapped(
                "Time of Day menggerakkan matahari, dan Precomputed dipanggang dengan andaian "
                "matahari yang diam.");
            ImGui::TextDisabled(
                "Selama yang dipanggang baru lingkungannya, keduanya masih boleh: ia ikut "
                "mataharinya. Begitu transport cahaya ikut dipanggang, pantulan dan "
                "oklusinya akan datang dari matahari di tempat lain.");
        }

        if (ImGui::Button("Hentikan Time of Day")) {
            // Bukan lewat perintah: `timeOfDayEnabled` setelan editor, bukan isi
            // level — ia tidak pernah tertulis ke berkas dan karena itu tidak
            // punya tempat di riwayat undo level.
            context.timeOfDayEnabled = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Pakai tingkat RealTime")) {
            // Untuk yang memang menginginkan matahari yang bergerak: RealTime
            // memang kategori untuk itu, dan memilihnya sekarang menghindari
            // memanggang sesuatu yang akan dibuang.
            const scene::WorldSettings before = context.world->Settings();
            scene::WorldSettings dynamic = before;
            dynamic.indirect = scene::IndirectLighting::RealTime;
            context.history->CloseMergeGroup();
            context.history->Execute<SetWorldSettingsCommand>(context.world, before, dynamic);
        }
    }

    /// Mengatakan apa yang sudah dipanggang dan apa yang belum.
    ///
    /// **Ada karena `Precomputed` hari ini belum menepati namanya.** Ia
    /// memanggang lingkungannya — dan itu saja; transport dan oklusinya menyusul
    /// di S1–S5. Tingkat yang menjanjikan lebih daripada yang diberikannya
    /// adalah tingkat yang membuat orang mencari cacat pada adegannya alih-alih
    /// pada rencananya.
    static void DrawBakeStatus(EditorContext& context) {
        if (context.world->Settings().indirect != scene::IndirectLighting::Precomputed) {
            return;
        }
        ImGui::Separator();
        ImGui::TextUnformatted("Sudah dipanggang:");
        ImGui::BulletText("Lingkungan — iradiansi SH dan prafilter spekular");
        ImGui::TextDisabled("Belum dipanggang:");
        ImGui::Indent();
        ImGui::TextDisabled("• Transport cahaya (pantulan antar-permukaan) — S2");
        ImGui::TextDisabled("• Oklusi dan AO — S3");
        ImGui::TextDisabled("• Lightmap permukaan statis — S5");
        ImGui::Unindent();
        ImGui::TextDisabled("Sampai itu ada, langit berlaku sama di ruang tertutup.");

        DrawProbeGrid(context);
        DrawProbeBake(context);
    }

    /// Tombol memanggang transport cahaya statis, beserta kemajuannya (S2).
    ///
    /// **Kemajuannya ditampilkan, dan itu bukan hiasan.** Satu kisi probe adalah
    /// puluhan ribu penelusuran jalur penuh; panggangan yang memakan puluhan
    /// detik tanpa satu angka pun terbaca sebagai editor yang menggantung, dan
    /// yang melihatnya akan menutupnya.
    static void DrawProbeBake(EditorContext& context) {
#if SIM_WITH_PROBE_BAKE
        if (context.world->Settings().indirect != scene::IndirectLighting::Precomputed) {
            return;
        }
        if (context.probeBakery == nullptr) {
            // Disebutkan, bukan didiamkan: tombol yang hilang tanpa penjelasan
            // terbaca sebagai fitur yang belum ada, bukan sebagai build yang
            // dibuat tanpa alatnya.
            ImGui::TextDisabled("Panggangan tidak tersedia: editor ini dibangun tanpa slangc.");
            return;
        }

        ImGui::Separator();
        if (context.probeBakery->Running()) {
            ImGui::ProgressBar(context.probeBakery->Progress());
            ImGui::TextDisabled("%s", context.probeBakery->Status().c_str());
            return;
        }

        const bool ready = context.sceneBounds.valid && !context.bakeItems.empty();
        ImGui::BeginDisabled(!ready);
        if (ImGui::Button("Panggang Cahaya Statis")) {
            StartProbeBake(context);
        }
        ImGui::EndDisabled();
        if (!ready) {
            ImGui::TextDisabled("Menunggu geometri adegan.");
        } else if (context.probeVolume != nullptr) {
            ImGui::TextDisabled("Terpasang: %u brick, %.1f MB di GPU",
                                context.probeVolume->AllocatedBrickCount(),
                                static_cast<double>(context.probeVolume->GpuBytes()) /
                                    (1024.0 * 1024.0));
        } else {
            ImGui::TextDisabled("Belum dipanggang — langit berlaku sama di ruang tertutup.");
        }
        if (!context.probeBakery->Status().empty() && !context.probeBakery->Running()) {
            ImGui::TextDisabled("%s", context.probeBakery->Status().c_str());
        }
#else
        (void)context;
#endif
    }

#if SIM_WITH_PROBE_BAKE
    /// Menyusun masukan panggangan dari adegan, lalu memulainya.
    static void StartProbeBake(EditorContext& context) {
        const scene::WorldSettings& settings = context.world->Settings();
        const render::ProbeVolumeLayout layout = render::MakeProbeLayout(
            context.sceneBounds.minimum, context.sceneBounds.maximum,
            scene::ProbeSpacingOf(settings));

        // **Langitnya disalin ke dalam lambda, bukan dirujuk.** Panggangan
        // berjalan di `TaskPool` dan bisa hidup lebih lama daripada level yang
        // memulainya; sebuah lambda yang menangkap pointer ke `SkyComponent`
        // akan membaca memori yang sudah dibebaskan begitu levelnya ditutup di
        // tengah panggangan — dan yang keluar bukan galat melainkan kisi berisi
        // sampah.
        const scene::SkyComponent* sky = FindSky(*context.world);
        Vec3 sunDirection(0.0f, -1.0f, 0.0f);
        Vec3 sunIrradiance(0.0f);
        const scene::Entity sun = FindDirectionalLight(*context.world);
        if (sun != scene::kNullEntity) {
            const auto* light = context.world->TryGet<scene::LightComponent>(sun);
            const Mat4 matrix = context.world->WorldMatrix(sun);
            // Lampu directional menyinari sepanjang sumbu -Z lokalnya, konvensi
            // yang sama dengan renderer.
            sunDirection = glm::normalize(-Vec3(matrix[2]));
            if (light != nullptr) {
                // **Sebelum eksposur.** Yang dikirim renderer ke shader sudah
                // dikalikan eksposur; sebuah panggangan yang ikut memuatnya akan
                // berubah setiap slider eksposur digeser, dan itu panggangan yang
                // artinya bergantung pada cara melihatnya.
                sunIrradiance = light->color * light->intensity;
            }
        }

        std::function<Vec3(const Vec3&)> sampler;
        if (sky != nullptr && sky->source == scene::SkySourceKind::Atmosphere) {
            render::AtmosphereSky atmosphere;
            atmosphere.intensity = sky->intensity;
            atmosphere.cameraHeightKm = sky->cameraHeightKm;
            atmosphere.sunDirection = -sunDirection;
            atmosphere.Prepare();
            sampler = [atmosphere](const Vec3& direction) { return atmosphere.Sample(direction); };
        } else {
            // **Bukan langit atmosferik berarti belum ada langit yang bisa
            // dipanggang di sini.** Berkas HDR menuntut membacanya di sisi ini
            // juga, dan itu pekerjaan tersendiri; sampai ada, yang dipanggang
            // hitam — dan adegan yang tiba-tiba gelap adalah pertanyaan yang
            // diajukan alih-alih kesalahan yang disembunyikan.
            sampler = [](const Vec3&) { return Vec3(0.0f); };
        }

        view::ProbeBakery::Settings bake;
        bake.layout = layout;
        bake.sunIrradiance = sunIrradiance;
        bake.sunDirection = sunDirection;
        bake.cacheDir = context.configDir / "ProbeCache";
        // **Sidik jari langitnya, karena bakery tidak bisa melihat ke dalam
        // lambda.** Yang tidak masuk ke sini akan terbaca kembali sebagai
        // panggangan yang benar: artefak matahari sore dipakai ulang untuk
        // matahari pagi, dan berkasnya memang sah.
        uint64_t skyKey = 1469598103934665603ull;
        const auto mix = [&skyKey](const void* data, std::size_t length) {
            const auto* bytes = static_cast<const uint8_t*>(data);
            for (std::size_t i = 0; i < length; ++i) {
                skyKey = (skyKey ^ bytes[i]) * 1099511628211ull;
            }
        };
        if (sky != nullptr) {
            mix(&sky->source, sizeof(sky->source));
            mix(&sky->intensity, sizeof(sky->intensity));
            mix(&sky->cameraHeightKm, sizeof(sky->cameraHeightKm));
        }
        mix(&sunDirection.x, sizeof(float) * 3);
        bake.skyKey = skyKey;
        if (!context.probeBakery->Bake(context.bakeItems, std::move(sampler), bake)) {
            SIM_WARN("Editor", "probe bake could not start");
        }
    }
#endif

    /// Ukuran kisi probe untuk jarak yang sedang disetel — sebelum ada yang
    /// menekan Bake (S1).
    ///
    /// **Angkanya ditampilkan, bukan ditebak, dan itu inti keputusan 8.** Jumlah
    /// probe tumbuh kubik terhadap jaraknya: memotong jarak jadi separuh
    /// melipatgandakan biayanya delapan kali. Tanpa angka di layar itu ditemukan
    /// setelah menunggu panggangan yang tidak muat, dan yang terlihat kemudian
    /// cuma editor yang diam.
    static void DrawProbeGrid(EditorContext& context) {
        ImGui::Separator();
        if (!context.sceneBounds.valid) {
            // Bukan nol, dan bukan kisi kosong: yang belum diukur berbeda dari
            // yang terukur kosong, dan menampilkan "0 probe" untuk keduanya
            // membuat adegan tanpa geometri terlihat sama seperti viewport yang
            // belum sempat menggambar.
            ImGui::TextDisabled("Kisi probe: menunggu viewport mengukur adegan.");
            return;
        }

        const float spacing = scene::ProbeSpacingOf(context.world->Settings());
        const render::ProbeVolumeLayout layout = render::MakeProbeLayout(
            context.sceneBounds.minimum, context.sceneBounds.maximum, spacing);
        if (!layout.IsValid()) {
            ImGui::TextDisabled("Kisi probe: adegan tanpa geometri.");
            return;
        }

        const glm::uvec3 bricks = layout.BrickCounts();
        const uint32_t probes = layout.FullProbeCount();

        // **Ukurannya dihitung `ProbeVolume`, bukan ditulis ulang di sini.** Dua
        // rumus untuk satu hal adalah dua rumus yang suatu saat tidak sepakat —
        // dan yang tidak sepakat di panel adalah angka yang menyesatkan justru
        // orang yang sedang memutuskan berapa jarak yang sanggup ia bayar.
        //
        // Kisi penuh, karena S1 belum membuang satu brick pun; S2 yang membuat
        // angka ini turun.
        render::ProbeVolume sized;
        sized.layout = layout;
        sized.brickSlots.assign(layout.BrickCount(), 0u);
        sized.probes.resize(probes);
        constexpr double kMegabyte = 1024.0 * 1024.0;
        const double gpuMegabytes = static_cast<double>(sized.GpuBytes()) / kMegabyte;
        const double fileMegabytes = static_cast<double>(sized.StoredBytes()) / kMegabyte;

        ImGui::Text("Kisi probe: %u × %u × %u", layout.counts.x, layout.counts.y,
                    layout.counts.z);
        // **Angka GPU yang disebut lebih dulu**, karena itulah yang dibayar tiap
        // frame — dan itulah yang ditanyakan orang yang menyetel jarak demi
        // performance. Ukuran berkasnya menyusul, dan keduanya memang berbeda:
        // std430 menaikkan tiap koefisien ke 16 byte.
        ImGui::Text("%u probe dalam %u brick — %.1f MB di GPU", probes, layout.BrickCount(),
                    gpuMegabytes);
        ImGui::TextDisabled("Artefaknya %.1f MB", fileMegabytes);
        ImGui::TextDisabled("Brick %u³, %u × %u × %u", render::ProbeVolumeLayout::kBrickSize,
                            bricks.x, bricks.y, bricks.z);

        const Vec3 size = context.sceneBounds.maximum - context.sceneBounds.minimum;
        ImGui::TextDisabled("Adegan %.1f × %.1f × %.1f m, jarak %.2f m", size.x, size.y, size.z,
                            spacing);

        // **Peringatannya soal apa yang akan terjadi, bukan soal angka yang
        // besar.** Yang menyakitkan bukan megabyte-nya melainkan waktu
        // panggangnya di S2, dan itu sebanding dengan jumlah probe.
        if (probes > 2'000'000u) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                               "Jaraknya rapat untuk adegan sebesar ini.");
            ImGui::TextDisabled("Menggandakan jaraknya memotong keduanya jadi seperdelapan.");
        }
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
        // **Dinilai dari adegan, bukan dari renderer.** Syarat renderer punya
        // satu suku lagi yang tidak terlihat dari sini — LUT atmosfernya harus
        // berhasil dibuat — jadi pada mesin yang gagal membuatnya, panel ini
        // mengatakan semuanya baik sementara GI-nya nol. Itu kegagalan sumber
        // daya yang sudah punya barisnya sendiri di Console, bukan kombinasi
        // yang salah dipilih pengarang.
        const bool proceduralSky =
            sky != nullptr && sky->source == scene::SkySourceKind::Atmosphere;

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
        if (ImGui::Button("Pakai tingkat Precomputed")) {
            scene::WorldSettings baked = settings;
            baked.indirect = scene::IndirectLighting::Precomputed;
            context.history->CloseMergeGroup();
            context.history->Execute<SetWorldSettingsCommand>(context.world, settings, baked);
        }
        ImGui::SameLine();
        // **Jalan keluar kedua bergantung pada keadaan mana yang berlaku**, dan
        // itu bukan kerapian: menawarkan "pakai langit atmosfer" pada level yang
        // langitnya sudah atmosfer adalah tombol mati, dan yang di sana justru
        // butuh `environment: Sky` — yang tanpa itu tidak ditawarkan sama
        // sekali.
        if (blind) {
            ImGui::BeginDisabled(sky == nullptr);
            if (ImGui::Button("Pakai langit atmosfer")) {
                SwitchSkyToAtmosphere(context);
            }
            ImGui::EndDisabled();
            if (sky == nullptr) {
                ImGui::TextDisabled("Level ini belum punya langit — tempatkan prefab Sky Dome.");
            }
        } else {
            // `RealTime` + `File`: yang bertentangan maksudnya, bukan langitnya.
            // Menyinari dari langit menyelesaikannya tanpa melepas GI real-time.
            if (ImGui::Button("Sinari dengan langit")) {
                scene::WorldSettings lit = settings;
                lit.environment = scene::EnvironmentSource::Sky;
                context.history->CloseMergeGroup();
                context.history->Execute<SetWorldSettingsCommand>(context.world, settings, lit);
            }
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
