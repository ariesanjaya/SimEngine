#pragma once

#include "Sim/Script/Graph.h"

#include <string>
#include <string_view>
#include <vector>

namespace sim::script {

/// Satu jenis node beserta bentuk pin-nya.
struct NodeType {
    /// Kunci yang tertulis di berkas `.simgraph`, mis. "flow.branch".
    /// Mengubahnya memutus graph lama.
    std::string key;
    /// Yang ditampilkan di kanvas dan di pencarian palet.
    std::string label;
    /// Pengelompokan di palet, mis. "Flow", "Math", "Component".
    std::string category;
    /// Node murni tidak punya pin exec: nilainya dihitung saat ada yang
    /// membutuhkannya, seperti pemanggilan fungsi biasa di Lua. Node tak-murni
    /// berjalan menurut urutan pin exec.
    bool pure = false;
    /// Keterangan singkat untuk tooltip di palet.
    std::string tooltip;
    std::vector<GraphPin> pins;

    const GraphPin* FindPin(std::string_view name) const;
    std::vector<const GraphPin*> Inputs() const;
    std::vector<const GraphPin*> Outputs() const;
    /// Pin exec masuk pertama, bila ada. Node event tidak punya.
    const GraphPin* ExecInput() const;
};

/// Seluruh jenis node yang bisa dipakai sebuah graph.
///
/// **Sebagian besar isinya dibangkitkan, bukan didaftarkan tangan.** Setiap
/// komponen yang terdaftar di ComponentRegistry menghasilkan sepasang node
/// get/set, dengan satu pin per field yang tipenya bisa dialirkan lewat graph.
/// Itu berarti komponen baru — atau field baru pada komponen lama — langsung
/// muncul di palet tanpa satu baris tambahan di sini, sumber kebenarannya tetap
/// satu, dan katalognya tidak pernah bisa berbohong tentang apa yang ada.
///
/// Node yang tersisa — event, alur, variabel, matematika, literal — memang
/// tidak punya padanan di reflection dan didaftarkan di sini.
class NodeCatalog {
public:
    /// Katalog bersama, dibangun saat pertama dipakai.
    ///
    /// Dibangun malas dan bukan di startup karena ia membaca ComponentRegistry,
    /// yang diisi RegisterCoreComponents(). Panggilan pertama selalu datang
    /// setelah itu — dari kompiler atau dari panel — sehingga tidak ada urutan
    /// yang harus diingat pemanggil.
    static const NodeCatalog& Get();

    /// Membangun ulang dari ComponentRegistry saat ini. Dipakai test yang
    /// mendaftarkan komponennya sendiri.
    static void Rebuild();

    const NodeType* Find(std::string_view key) const;
    const std::vector<NodeType>& All() const { return types_; }

    /// Kunci node get/set untuk sebuah komponen, mis. "component.get.Transform".
    static std::string ComponentGetKey(std::string_view component);
    static std::string ComponentSetKey(std::string_view component);

private:
    NodeCatalog();
    void AddCoreTypes();
    void AddComponentTypes();

    std::vector<NodeType> types_;
};

/// Pin sebuah node **di dalam konteks graph-nya**.
///
/// Berbeda dari `NodeType::pins` untuk dua jenis node yang bentuknya memang
/// ditentukan isi graph: jumlah keluaran `flow.sequence` mengikuti setelan
/// `count`, dan tipe pin node variabel mengikuti deklarasi variabelnya.
/// Kompiler dan panel memanggil fungsi yang sama supaya yang digambar di kanvas
/// tidak pernah berbeda dari yang dikompilasi.
///
/// Kosong bila tipe node-nya tidak ada di katalog.
std::vector<GraphPin> PinsOf(const Graph& graph, const GraphNode& node);

}  // namespace sim::script
