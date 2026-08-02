#pragma once

#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelManager.h"

#include <memory>
#include <vector>

namespace sim::editor {

using PanelFactory = std::unique_ptr<Panel> (*)();

/// Daftar panel yang mendaftarkan diri sendiri lewat SIM_REGISTER_PANEL.
///
/// Ini yang membuat menambah panel berbiaya satu berkas: tidak ada daftar pusat
/// yang harus ikut diedit, tidak ada header yang harus di-include di tempat
/// lain. Kriteria terima E2 nomor 1 persis soal ini.
class PanelRegistry {
public:
    static PanelRegistry& Get();

    /// `order` menentukan urutan pembuatan panel. Diperlukan karena urutan
    /// inisialisasi statik antar-TU tidak ditentukan bahasa; tanpa ini urutan
    /// panel di menu Window bisa berbeda tiap kali proyek di-link ulang.
    void Add(int order, PanelFactory factory);

    void InstantiateAll(PanelManager& panels) const;

private:
    struct Registration {
        int order = 0;
        PanelFactory factory = nullptr;
    };

    mutable std::vector<Registration> registrations_;
};

namespace detail {

struct PanelRegistrar {
    PanelRegistrar(int order, PanelFactory factory) {
        PanelRegistry::Get().Add(order, factory);
    }
};

}  // namespace detail
}  // namespace sim::editor

/// Mendaftarkan sebuah kelas panel. Ditulis sekali di berkas panel itu sendiri.
///
/// Catatan build: berkas objek yang isinya hanya inisialisasi statik seperti ini
/// akan dibuang linker saat mengambil dari static library. Karena itu
/// Apps/SimEditor menautkan Sim::EditorPanels dengan WHOLE_ARCHIVE.
#define SIM_REGISTER_PANEL(Type, order)                                                       \
    namespace {                                                                               \
    const ::sim::editor::detail::PanelRegistrar sim_panel_registrar_for_##Type{                \
        (order), []() -> std::unique_ptr<::sim::editor::Panel> {                              \
            return std::make_unique<Type>();                                                  \
        }};                                                                                   \
    }
