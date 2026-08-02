#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Core/Uuid.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace sim::script {

/// Versi skema berkas `.simgraph` yang ditulis sekarang.
///
/// Riwayat:
///   1 — bentuk awal: node, link, dan variabel
///   2 — antarmuka subgraph: daftar `inputs` dan `outputs`
///
/// Naikkan setiap kali bentuk data berubah. Berkas lama harus tetap bisa
/// dibuka: graph adalah hasil kerja pengguna, bukan artefak build.
inline constexpr int kGraphSchemaVersion = 2;

/// Jenis data yang mengalir lewat sebuah pin.
///
/// `Exec` bukan data melainkan urutan: ia yang menentukan node mana berjalan
/// setelah node mana. Memisahkannya dari tipe data — alih-alih memperlakukan
/// urutan sebagai "data bertipe void" — adalah yang membuat kompilernya bisa
/// menolak menyambung urutan ke sebuah angka tanpa satu pun kasus khusus.
enum class PinKind {
    Exec,
    Bool,
    Number,
    String,
    Vec3,
    Quat,
    Entity,
    /// Menerima apa saja. Dipakai node yang memang tidak peduli, seperti `log`.
    Any,
};

const char* ToString(PinKind kind);
PinKind PinKindFromString(std::string_view text);

/// Apakah nilai bertipe `from` boleh mengalir ke pin bertipe `to`.
///
/// Sengaja ketat kecuali untuk `Any`. Konversi diam-diam — angka jadi teks,
/// bool jadi angka — membuat graph yang salah tetap terkompilasi lalu gagal
/// dengan cara yang jauh dari penyebabnya.
bool PinAccepts(PinKind to, PinKind from);

enum class PinDirection {
    Input,
    Output,
};

/// Satu pin pada sebuah node. Dibangkitkan katalog dari tipe node, tidak
/// disimpan di berkas — lihat NodeCatalog.h.
struct GraphPin {
    /// Stabil, dan inilah yang dirujuk koneksi. Mengubahnya memutus berkas lama.
    std::string name;
    /// Yang ditampilkan di kanvas. Kosong = pakai `name`.
    std::string label;
    PinKind kind = PinKind::Exec;
    PinDirection direction = PinDirection::Input;
    /// Literal Lua yang dipakai bila pin input data ini tidak tersambung.
    /// Kosong berarti pin ini wajib tersambung.
    std::string defaultValue;
};

struct GraphNode {
    /// Identitas node. Koneksi menyimpan GUID, bukan indeks maupun nama, jadi
    /// memindahkan atau menamai ulang node tidak memutus apa pun.
    Uuid guid;
    /// Kunci di katalog node, mis. "component.get" atau "flow.branch".
    std::string type;
    Vec2 position{0.0f, 0.0f};
    /// Ukuran kotak, hanya berarti untuk node grup — node biasa menentukan
    /// ukurannya sendiri dari isinya. Nol berarti "tidak ditentukan", dan tidak
    /// ikut ditulis ke berkas supaya graph tanpa grup tetap menghasilkan teks
    /// yang sama persis seperti sebelum bidang ini ada.
    Vec2 size{0.0f, 0.0f};
    /// Konfigurasi node; makna kuncinya ditentukan tipe node-nya. std::map,
    /// bukan unordered_map, supaya urutan penulisannya sama setiap kali.
    std::map<std::string, std::string> settings;
    /// Nilai literal untuk pin input data yang tidak tersambung. Kunci = nama
    /// pin. Menimpa `GraphPin::defaultValue` dari katalog.
    std::map<std::string, std::string> pinValues;

    std::string Setting(std::string_view key, std::string_view fallback = {}) const;
};

struct GraphLink {
    Uuid guid;
    Uuid fromNode;
    std::string fromPin;
    Uuid toNode;
    std::string toPin;
};

/// Variabel milik graph. Yang `exposed` ikut muncul di Inspector, lewat jalur
/// yang sama persis dengan `properties` di skrip tulis tangan — kompilernya
/// memang membangkitkan deklarasi `properties`, bukan mekanisme kedua.
struct GraphVariable {
    std::string name;
    PinKind kind = PinKind::Number;
    /// Literal Lua. Kosong diisi nilai netral menurut tipenya.
    std::string defaultValue;
    bool exposed = false;
};

/// Satu pin di **antarmuka** sebuah graph — parameter masuk atau hasil keluar.
///
/// Inilah yang membuat sebuah graph bisa dipakai ulang sebagai satu node di
/// graph lain: yang dilihat pemanggil hanya daftar ini, bukan isi graph-nya.
struct GraphPort {
    std::string name;
    PinKind kind = PinKind::Number;
    /// Literal Lua yang dipakai pemanggil bila pin ini dibiarkan kosong.
    /// Hanya berarti untuk parameter masuk.
    std::string defaultValue;
};

struct Graph {
    /// Parameter dan hasil, bila graph ini dipakai sebagai subgraph. Kosong
    /// berarti ia graph biasa yang berjalan lewat node event.
    std::vector<GraphPort> inputs;
    std::vector<GraphPort> outputs;
    std::vector<GraphNode> nodes;
    std::vector<GraphLink> links;
    std::vector<GraphVariable> variables;

    const GraphNode* FindNode(const Uuid& guid) const;
    const GraphVariable* FindVariable(std::string_view name) const;

    /// Link yang berakhir di pin input tertentu. Sebuah pin input hanya boleh
    /// punya satu sumber, jadi hasilnya paling banyak satu.
    const GraphLink* LinkInto(const Uuid& node, std::string_view pin) const;

    /// Seluruh link yang berangkat dari pin output tertentu. Pin output boleh
    /// bercabang ke banyak tujuan.
    std::vector<const GraphLink*> LinksFrom(const Uuid& node, std::string_view pin) const;

    /// Graph ini dimaksudkan sebagai subgraph — ia punya antarmuka, jadi ia
    /// berjalan ketika dipanggil, bukan ketika sebuah event menyala.
    bool IsSubgraph() const { return !inputs.empty() || !outputs.empty(); }
};

/// Sumber graph lain yang dirujuk node `graph.call`.
///
/// Antarmuka, bukan pointer ke AssetDatabase: modul Script tidak boleh
/// bergantung pada bagaimana editor menyimpan aset, dan test harus bisa
/// menyediakan graph-nya dari memori tanpa satu berkas pun di disk.
class GraphLibrary {
public:
    virtual ~GraphLibrary() = default;

    /// Graph yang dirujuk GUID tertentu, atau null bila tidak ada.
    virtual const Graph* Find(const Uuid& guid) const = 0;

    /// Nama yang bisa dibaca manusia, untuk pesan kesalahan.
    virtual std::string NameOf(const Uuid& guid) const = 0;
};

struct GraphIoResult {
    bool ok = false;
    std::string error;
    int sourceVersion = kGraphSchemaVersion;
};

/// Menulis graph sebagai teks JSON.
///
/// Keluarannya deterministik — node dan link menurut urutan simpannya, setting
/// menurut urutan kunci — supaya dua kali menyimpan graph yang sama
/// menghasilkan byte yang sama dan berkasnya ramah diff.
std::string SaveGraphToString(const Graph& graph);
GraphIoResult SaveGraphToFile(const Graph& graph, const std::filesystem::path& path);

GraphIoResult LoadGraphFromString(Graph& graph, const std::string& text);
GraphIoResult LoadGraphFromFile(Graph& graph, const std::filesystem::path& path);

}  // namespace sim::script
