#include "Sim/Editor/PanelRegistry.h"

#include <algorithm>

namespace sim::editor {

PanelRegistry& PanelRegistry::Get() {
    // Fungsi statik, bukan variabel global: registrar dijalankan saat
    // inisialisasi statik, dan variabel global belum tentu sudah dikonstruksi
    // saat itu.
    static PanelRegistry registry;
    return registry;
}

void PanelRegistry::Add(int order, PanelFactory factory) {
    if (factory != nullptr) {
        registrations_.push_back(Registration{order, factory});
    }
}

void PanelRegistry::InstantiateAll(PanelManager& panels) const {
    std::stable_sort(registrations_.begin(), registrations_.end(),
                     [](const Registration& a, const Registration& b) { return a.order < b.order; });
    for (const Registration& registration : registrations_) {
        panels.Add(registration.factory());
    }
}

}  // namespace sim::editor
