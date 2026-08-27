// Panel Statistics.
//
// Asset Browser pindah ke AssetBrowserPanel.cpp begitu ia memakai AssetDatabase
// sungguhan; Outliner dan Inspector lebih dulu pindah ke berkasnya masing-masing.

#include "Sim/Core/FrameLimiter.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/SceneView/Selection.h"
#include "Sim/Editor/Widgets.h"

#include "Sim/Editor/SceneCommands.h"
#include "Sim/Scene/World.h"
#include "Sim/Scene/WorldSettings.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace sim::editor {
namespace {
/// Langit pertama di dunia, atau nullptr. Salinan sengaja dari yang ada di
/// ViewportPanel: keduanya cuma menjawab "apakah level ini punya langit", dan
/// menaruhnya di header bersama hanya untuk dua baris ini akan menambah
/// permukaan publik untuk hal yang tidak dipakai siapa pun di luar keduanya.
const scene::SkyComponent* FindSkyComponent(const scene::World& world) {
    for (const auto raw : world.Registry().view<scene::SkyComponent>()) {
        return world.TryGet<scene::SkyComponent>(static_cast<scene::Entity>(raw));
    }
    return nullptr;
}


class StatisticsPanel final : public Panel {
public:
    StatisticsPanel()
        : Panel(panel_id::kStatistics, std::string(icons::kPanelStatistics) + "  Statistics",
                PanelCategory::Debug) {
        SetOpen(false);
    }

    void OnDraw(EditorContext& context) override {
        if (context.frameLimiter != nullptr) {
            ImGui::Text("Frame   : %.2f ms",
                        context.frameLimiter->LastDeltaSeconds() * 1000.0);
            ImGui::Text("FPS     : %.1f", context.frameLimiter->SmoothedFps());
            ImGui::Text("Target  : %.0f Hz", context.frameLimiter->TargetFps());
        }
        ImGui::Separator();
        ImGui::TextWrapped("Frame lock: %s", context.frameLockReason.c_str());
        ImGui::Separator();
        ImGui::Text("Viewport: %ux%u",
                    context.viewportRenderer != nullptr ? context.viewportRenderer->Width() : 0u,
                    context.viewportRenderer != nullptr ? context.viewportRenderer->Height() : 0u);
        ImGui::Text("Renderer: %s", context.viewportRenderer != nullptr
                                        ? context.viewportRenderer->Name()
                                        : "(none)");

        if (context.viewportRenderer != nullptr) {
            const render::RenderStats stats = context.viewportRenderer->Stats();
            ImGui::Text("Opaque  : %u drawn / %u in buffer", stats.opaqueDrawn,
                        stats.opaqueInstances);
            ImGui::Text("Casters : %u over %u shadow faces", stats.shadowCasters,
                        stats.shadowFaces);
            // **Ikatan descriptor di sebelah jumlah draw, dan jalur materialnya
            // di sebelah keduanya.** Ketiganya satu cerita: yang menentukan
            // berapa kali descriptor diikat bukan jumlah benda melainkan cara
            // material diikat, dan angka yang berdiri sendiri tidak
            // mengatakannya. Lihat G5 di docs/PLAN-GPU-OPTIM.md.
            ImGui::Text("Binds   : %u sets / %u draws (%s)", stats.descriptorSetBinds,
                        stats.drawCalls,
                        context.viewportRenderer->UsesBindlessMaterials() ? "bindless"
                                                                         : "per-part sets");
            // **Berwarna, dan hanya saat ada yang terbuang.** Sebuah angka nol
            // yang selalu ada di sana berhenti dibaca dalam sehari; yang muncul
            // hanya ketika ia berarti sesuatu masih terbaca setahun kemudian.
            if (stats.shadowLightsDropped > 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.25f, 1.0f),
                                   "%u shadow lights dropped (atlas full)",
                                   stats.shadowLightsDropped);
            }
        }

        // Waktu GPU per pass. **Ini alat diagnostik yang paling sering dipakai**
        // begitu ada pass yang anggarannya harus dijaga — dan yang pertama
        // menuntutnya adalah GI, yang seluruh rencananya disusun sekitar angka
        // 3,0 ms. Angka yang tidak bisa diukur bukan anggaran melainkan harapan.
        if (context.viewportRenderer != nullptr) {
            const std::span<const render::PassTiming> timings =
                context.viewportRenderer->PassTimings();
            ImGui::Separator();
            if (timings.empty()) {
                ImGui::TextDisabled("GPU timing unavailable");
            } else {
                float total = 0.0f;
                for (const render::PassTiming& timing : timings) {
                    total += timing.milliseconds;
                }
                ImGui::Text("GPU     : %.3f ms", static_cast<double>(total));
                if (ImGui::BeginTable("##passes", 2,
                                      ImGuiTableFlags_SizingStretchProp |
                                          ImGuiTableFlags_RowBg)) {
                    for (const render::PassTiming& timing : timings) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(timing.name.data(),
                                               timing.name.data() + timing.name.size());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f ms", static_cast<double>(timing.milliseconds));
                    }
                    ImGui::EndTable();
                }
            }

            // **Tabel kedua, bukan baris tambahan di tabel pertama.** Angka GPU
            // datang beberapa frame terlambat karena pemungutannya tidak pernah
            // menunggu, sementara angka CPU adalah frame yang baru saja lewat.
            // Menjumlahkan keduanya menghasilkan total yang tidak pernah menjadi
            // waktu frame mana pun.
            const std::span<const render::PassTiming> cpuTimings =
                context.viewportRenderer->CpuTimings();
            if (!cpuTimings.empty()) {
                ImGui::Separator();
                if (ImGui::BeginTable("##cpupasses", 2,
                                      ImGuiTableFlags_SizingStretchProp |
                                          ImGuiTableFlags_RowBg)) {
                    for (const render::PassTiming& timing : cpuTimings) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(timing.name.data(),
                                               timing.name.data() + timing.name.size());
                        ImGui::TableNextColumn();
                        ImGui::Text("%.3f ms", static_cast<double>(timing.milliseconds));
                    }
                    ImGui::EndTable();
                }
            }
        }


        // --- Langit (E8.8) ---
        ImGui::Separator();
        // Langit tidak lagi punya sakelar di sini: yang menyalakannya adalah
        // entity ber-komponen Sky di dalam level, dan yang menyuntingnya adalah
        // Inspector lewat refleksi seperti komponen lain. Panel ini hanya
        // mengatakan keadaannya — dua tempat menyunting satu hal adalah dua
        // tempat yang suatu saat tidak sepakat.
        {
            const scene::SkyComponent* sky =
                context.world != nullptr ? FindSkyComponent(*context.world) : nullptr;
            if (sky == nullptr) {
                ImGui::TextDisabled("No sky in this level.");
                ImGui::TextDisabled("Place a Sky Dome prefab to add one.");
            } else {
                ImGui::Text("Sky: %s", sky->source == scene::SkySourceKind::HdrMap
                                           ? "HDR map"
                                           : "atmosphere");
                ImGui::TextDisabled("Select the Sky Dome entity to edit it.");
            }
        }

        static bool volumeEnabled = false;
        volumeEnabled = volumeEnabled || !context.volume.path.empty();

        // --- Volume .vdb (V2) ---
        //
        // Di panel yang sama dengan langit karena ia pass adegan yang sama
        // sifatnya: biayanya muncul sebagai barisnya sendiri di tabel GPU di
        // atas, dan yang tidak bisa dimatikan tidak bisa diukur.
        ImGui::Separator();
        if (ImGui::Checkbox("Volume (.vdb)", &volumeEnabled)) {
            if (!volumeEnabled) {
                context.volume.path.clear();
            }
        }
        if (volumeEnabled) {
            context.volume.path.resize(512);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
            ImGui::InputText("VDB file", context.volume.path.data(), context.volume.path.size());
            context.volume.path.resize(std::strlen(context.volume.path.c_str()));

            context.volume.gridName.resize(128);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
            ImGui::InputText("Grid", context.volume.gridName.data(),
                             context.volume.gridName.size());
            context.volume.gridName.resize(std::strlen(context.volume.gridName.c_str()));
            ImGui::SameLine();
            ImGui::TextDisabled("(empty = first float grid)");

            // Status pemuatan ditampilkan apa adanya. Berkas volume gagal dimuat
            // karena banyak sebab yang bisa diperbaiki — nama grid yang salah,
            // voxel tak seragam, terlalu besar — dan pesan yang disembunyikan
            // membuat semuanya terlihat sama: "tidak muncul apa-apa".
            if (!context.volume.status.empty()) {
                ImGui::TextWrapped("%s", context.volume.status.c_str());
            }

            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
            ImGui::DragFloat3("Position", &context.volume.position.x, 0.05f);
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            ImGui::SliderFloat("Size", &context.volume.scale, 0.1f, 20.0f, "%.2f×");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            ImGui::SliderFloat("Density", &context.volume.extinction, 0.0f, 40.0f, "%.1f");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            ImGui::SliderFloat("Light", &context.volume.lightIntensity, 0.0f, 20.0f, "%.1f");
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
            ImGui::ColorEdit3("Scatter", &context.volume.albedo.x);
            // Langkah lebih kecil = lebih halus dan lebih mahal. **Kecerahannya
            // tidak ikut berubah** — integrasinya analitik di dalam langkah, dan
            // itu yang diuji `SimVolumeTests`.
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
            ImGui::SliderFloat("Step", &context.volume.stepSize, 0.01f, 0.5f, "%.3f m");
        }

        // --- Post-process (E8.8) ---
        //
        // Sakelar mode ada di sini, bukan tersembunyi, karena eksposur otomatis
        // punya satu kasus di mana ia justru menghalangi: menyetel material dan
        // lampu. Setiap kali sebuah lampu dicerahkan, pengukur mengimbanginya —
        // dan yang terlihat adalah lampu yang "tidak berpengaruh apa-apa".
        ImGui::Separator();
        ImGui::Checkbox("Post-process", &context.post.enabled);
        if (context.post.enabled) {
            // **Mode eksposur dan kompensasinya milik level, bukan panel ini.**
            // Keduanya maksud pengarang — "adegan ini dinilai pada eksposur
            // tetap" — dan maksud yang berubah saat berpindah mesin adalah
            // pengarangan yang rusak. Sakelarnya tetap di sini, dan itu
            // disengaja: eksposur otomatis justru menghalangi saat menyetel
            // material dan lampu, jadi memindahkannya ke panel lain berarti
            // alur kerja yang menjadi lebih jauh tanpa alasan. Dua widget yang
            // merender satu nilai tidak apa-apa; dua tempat yang **menyimpannya**
            // yang tidak.
            DrawWorldExposure(context);
            if (WorldExposureMode(context) == scene::ExposureModeKind::Manual) {
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
                ImGui::SliderFloat("EV100", &context.post.manualEv100, -8.0f, 8.0f, "%.1f");
            } else {
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
                ImGui::SliderFloat("Adapt bright", &context.post.adaptationBrightenSeconds, 0.0f,
                                   4.0f, "%.2f s");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
                ImGui::SliderFloat("Adapt dark", &context.post.adaptationDarkenSeconds, 0.0f,
                                   4.0f, "%.2f s");
            }
            ImGui::Checkbox("Bloom", &context.post.bloom.enabled);
            if (context.post.bloom.enabled) {
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
                ImGui::SliderFloat("Threshold", &context.post.bloom.threshold, 0.0f, 8.0f,
                                   "%.2f");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
                ImGui::SliderFloat("Knee", &context.post.bloom.knee, 0.0f, 2.0f, "%.2f");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
                ImGui::SliderFloat("Scatter", &context.post.bloom.scatter, 0.0f, 1.0f, "%.2f");
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
                ImGui::SliderFloat("Strength", &context.post.bloom.strength, 0.0f, 1.0f, "%.3f");
            }
        }

        // --- Global illumination (M0) ---
        //
        // Pemilih backend terlihat sejak sekarang, bukan nanti bersama backend
        // kedua. Mesin ini punya RT core, jadi pemilihan otomatis tidak akan
        // pernah menjalankan jalur SDF — jalur yang justru harus bekerja di
        // setiap GPU. Tanpa sakelar ini, ia berhenti diuji tanpa ada yang
        // menyadarinya.
        ImGui::Separator();
        // **Tingkat pencahayaan tidak lagi punya sakelar di sini.** Yang
        // menentukannya adalah `indirect` di World Settings, yaitu di dalam
        // berkas levelnya — alasan yang sama persis dengan langit dua bagian di
        // atas: dua tempat menyunting satu hal adalah dua tempat yang suatu saat
        // tidak sepakat. Yang tersisa di panel ini semuanya alat ukur, dan alat
        // ukur memang tidak disimpan ke mana pun.
        {
            const char* tier = "Baked";
            if (context.world != nullptr) {
                switch (context.world->Settings().indirect) {
                    case scene::IndirectLighting::None: tier = "None"; break;
                    case scene::IndirectLighting::Baked: tier = "Baked"; break;
                    case scene::IndirectLighting::RealTime: tier = "RealTime"; break;
                }
            }
            ImGui::Text("Indirect: %s", tier);
            ImGui::TextDisabled("Edit it in World Settings.");
        }

        const char* backends[] = {"Auto", "Force SDF", "Force ray query"};
        int backend = static_cast<int>(context.gi.backend);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        if (ImGui::Combo("Backend", &backend, backends, IM_ARRAYSIZE(backends))) {
            context.gi.backend = static_cast<render::TraceBackendPreference>(backend);
        }

        ImGui::Checkbox("Screen-space layer", &context.gi.screenTrace);

        const char* views[] = {"Off",       "Albedo",      "Normal",     "Irradiance",
                               "Ray count", "March steps", "Trace layer"};
        int view = static_cast<int>(context.gi.debugView);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        if (ImGui::Combo("Debug view", &view, views, IM_ARRAYSIZE(views))) {
            context.gi.debugView = static_cast<render::GiDebugView>(view);
        }

        // --- Jalur compute (G3) ---
        //
        // **Sakelar diagnostik, bukan sebuah tampilan.** Yang dijawabnya satu
        // pertanyaan saja: apakah compute berjalan di mesin ini. Pertanyaan itu
        // tidak punya jawaban lain yang murah — pipeline yang gagal dibuat,
        // shader yang ditolak driver, dan barrier yang salah semuanya terlihat
        // sama dari luar, yaitu sebagai fitur yang "tidak muncul". Barisnya di
        // tabel waktu GPU di atas — `compute-gradient` — adalah separuh lain
        // jawabannya.
        ImGui::Separator();
        ImGui::Checkbox("Compute path (gradient)", &context.computeGradient);
        // **Sakelar pembanding, bukan sakelar kualitas.** Kedua jalur harus
        // menghasilkan gambar yang sama persis; kalau tidak, salah satunya
        // salah — dan menemukan yang mana jauh lebih murah dengan sakelar ini
        // daripada dengan membaca dua implementasi berdampingan. Selisih
        // biayanya terbaca sebagai `cpu-clusters` di tabel CPU melawan
        // `cluster-assign` di tabel GPU.
        ImGui::Checkbox("GPU cluster assignment", &context.gpuClusters);
        // Sakelar yang paling terasa di seluruh tabel. Mematikannya
        // mengembalikan evaluasi medan jarak per voxel ke satu core, dan
        // `cpu-sdf` melompat dari puluhan mikrodetik ke beberapa milidetik.
        ImGui::Checkbox("GPU SDF composite", &context.gpuSdf);

        if (context.viewportRenderer != nullptr) {
            const render::TraceBackendSelection selection =
                context.viewportRenderer->GiBackend();
            // Yang dipakai, bukan yang diminta — keduanya bisa berbeda, dan
            // perbedaan yang tidak ditampilkan adalah perbedaan yang tidak
            // diketahui siapa pun.
            ImGui::Text("Active  : %s", render::ToString(selection.kind));
            // Biaya komposit clipmap. Di CPU, jadi ia tidak muncul di tabel
            // pass GPU di atas — dan justru angka inilah yang dibatasi anggaran
            // 0,4 ms rencana GI.
            const uint64_t voxels = context.viewportRenderer->SdfVoxelsWritten();
            const float updateMs = context.viewportRenderer->SdfUpdateMilliseconds();
            if (context.gi.enabled) {
                // **Puncak jendela, bukan hanya angka frame ini.** Yang dibatasi
                // anggaran adalah frame terberat — yaitu frame saat kamera
                // melintasi batas voxel — dan frame itu satu di antara belasan.
                // Angka sesaat memperlihatkannya hanya kalau kebetulan terbaca
                // pada frame yang tepat.
                sdfHistory_[sdfCursor_] = {updateMs, voxels};
                sdfCursor_ = (sdfCursor_ + 1) % sdfHistory_.size();
                const SdfSample peak = *std::max_element(
                    sdfHistory_.begin(), sdfHistory_.end(),
                    [](const SdfSample& a, const SdfSample& b) { return a.ms < b.ms; });
                ImGui::Text("SDF CPU : %.3f ms  (%llu voxels)", static_cast<double>(updateMs),
                            static_cast<unsigned long long>(voxels));
                ImGui::Text("SDF peak: %.3f ms  (%llu voxels)", static_cast<double>(peak.ms),
                            static_cast<unsigned long long>(peak.voxels));
            } else {
                sdfHistory_.fill({});
            }
            if (selection.fellBack) {
                ImGui::TextColored(ImVec4(0.94f, 0.72f, 0.35f, 1.0f), "%s",
                                   selection.reason.c_str());
            } else {
                ImGui::TextDisabled("%s", selection.reason.c_str());
            }
        }

        ImGui::Separator();
        ImGui::Text("Selected: %zu",
                    context.selection != nullptr ? context.selection->Count() : 0u);
        ImGui::Text("History : %zu steps",
                    context.history != nullptr ? context.history->Entries().size() : 0u);
    }

private:
    /// Mode eksposur yang tertulis di level. Bawaan saat belum ada dunia, supaya
    /// panel tetap menggambar sesuatu yang benar alih-alih membaca pointer null.
    static scene::ExposureModeKind WorldExposureMode(const EditorContext& context) {
        return context.world != nullptr ? context.world->Settings().exposureMode
                                        : scene::ExposureModeKind::Automatic;
    }

    /// Mode eksposur dan kompensasinya, disunting lewat perintah supaya keduanya
    /// ikut undo/redo seperti setiap suntingan level yang lain.
    static void DrawWorldExposure(EditorContext& context) {
        if (context.world == nullptr || context.history == nullptr) {
            ImGui::TextDisabled("Exposure: no world.");
            return;
        }
        const scene::WorldSettings before = context.world->Settings();
        scene::WorldSettings edited = before;

        const char* modes[] = {"Automatic", "Manual"};
        int mode = static_cast<int>(edited.exposureMode);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        bool changed = false;
        if (ImGui::Combo("Exposure", &mode, modes, IM_ARRAYSIZE(modes))) {
            edited.exposureMode = static_cast<scene::ExposureModeKind>(mode);
            changed = true;
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        if (ImGui::SliderFloat("Compensation", &edited.exposureCompensation, -5.0f, 5.0f,
                               "%.2f EV")) {
            changed = true;
        }
        if (changed) {
            context.history->Execute<SetWorldSettingsCommand>(context.world, before, edited);
        }
        // Satu seretan slider adalah satu entri undo; yang berikutnya entri baru.
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            context.history->CloseMergeGroup();
        }
    }

    struct SdfSample {
        float ms = 0.0f;
        uint64_t voxels = 0;
    };

    // Dua detik pada 60 Hz: cukup panjang untuk memuat lintasan batas voxel,
    // cukup pendek untuk melupakan pembangunan penuh saat GI baru dinyalakan.
    std::array<SdfSample, 120> sdfHistory_{};
    std::size_t sdfCursor_ = 0;
};

}  // namespace

SIM_REGISTER_PANEL(StatisticsPanel, 55)

}  // namespace sim::editor
