#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Core/Uuid.h"

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace sim::material {

/// Versi skema berkas `.simmat` yang ditulis sekarang.
inline constexpr int kMaterialSchemaVersion = 1;

/// Tipe nilai yang mengalir lewat kabel.
///
/// Sengaja cerminan tipe skalar/vektor Slang, bukan abstraksi di atasnya:
/// keluaran graph ini adalah kode shader, dan setiap tipe yang tidak punya
/// padanan langsung di sana harus diterjemahkan — pekerjaan yang hanya
/// menambah tempat untuk salah.
enum class ValueKind {
    Float,
    Float2,
    Float3,
    Float4,
    /// Rujukan tekstur, bukan hasil sampling. Yang mengalir ke node Sample.
    Texture,
    Bool,
    /// "Skalar atau vektor float apa pun". Dipakai pin node matematika, yang
    /// memang tidak peduli lebarnya — `a * b` sama masuk akalnya untuk float
    /// maupun float3.
    ///
    /// Bukan "apa saja": tekstur dan bool tetap ditolak. Dan sebuah pin
    /// KELUARAN yang bertipe ini selalu diselesaikan menjadi tipe konkret oleh
    /// penyimpulan (lihat MaterialTypes) sebelum ada yang memeriksanya — kalau
    /// tidak, menyambungkan hasil `float3 * float3` ke sebuah pin float akan
    /// lolos di kanvas lalu gagal di kompilasi shader.
    Numeric,
};

const char* ToString(ValueKind kind);
ValueKind ValueKindFromString(std::string_view text);

/// Banyak komponen sebuah tipe. Texture dan Bool bernilai 1; Numeric bernilai
/// 0, artinya "belum ditentukan" — dengan begitu ia selalu kalah saat dua tipe
/// dibandingkan untuk mencari yang paling lebar.
int ComponentCount(ValueKind kind);

/// Apakah nilai bertipe `from` boleh mengalir ke pin bertipe `to`.
///
/// Skalar melebar ke vektor apa pun — `0.5` sah untuk base color, dan itu
/// aturan yang sama dengan yang berlaku di Slang, jadi kode yang dihasilkan
/// tidak perlu menyisipkan konversi yang tidak ditulis siapa pun.
///
/// Selain itu ketat. Float3 ke Float2 akan diam-diam membuang satu komponen,
/// dan Texture ke Float sama sekali tidak punya arti; keduanya lebih baik
/// ditolak di kanvas daripada menghasilkan shader yang berhasil dikompilasi
/// tapi salah.
bool Accepts(ValueKind to, ValueKind from);

/// Tipe yang paling lebar di antara dua masukan — aturan pelebaran Slang:
/// `float3 * float` menghasilkan float3.
ValueKind Widest(ValueKind a, ValueKind b);

enum class PinDirection {
    Input,
    Output,
};

/// Satu pin pada sebuah node. Dibangkitkan katalog dari tipe node, tidak
/// disimpan di berkas — lihat MaterialNodeCatalog.h.
struct MaterialPin {
    /// Stabil, dan inilah yang dirujuk koneksi. Mengubahnya memutus berkas lama.
    std::string name;
    /// Yang ditampilkan di kanvas. Kosong = pakai `name`.
    std::string label;
    ValueKind kind = ValueKind::Float;
    PinDirection direction = PinDirection::Input;
    /// Nilai yang dipakai bila pin input ini tidak tersambung, ditulis sebagai
    /// literal Slang. Kosong berarti pin ini **wajib** tersambung.
    std::string defaultValue;
};

struct MaterialNode {
    /// Identitas node. Koneksi menyimpan GUID, bukan indeks maupun nama, jadi
    /// memindahkan atau menamai ulang node tidak memutus apa pun.
    Uuid guid;
    /// Kunci di katalog node, mis. "math.lerp" atau "input.texture".
    std::string type;
    Vec2 position{0.0f, 0.0f};
    /// Ukuran kotak, hanya berarti untuk node grup dan komentar — node biasa
    /// menentukan ukurannya sendiri dari isinya. Nol berarti "tidak ditentukan"
    /// dan tidak ikut ditulis.
    Vec2 size{0.0f, 0.0f};
    /// Konfigurasi node; makna kuncinya ditentukan tipe node-nya. std::map,
    /// bukan unordered_map, supaya urutan penulisannya sama setiap kali.
    std::map<std::string, std::string> settings;
    /// Nilai literal untuk pin input yang tidak tersambung. Kunci = nama pin.
    /// Menimpa `MaterialPin::defaultValue` dari katalog.
    std::map<std::string, std::string> pinValues;

    std::string Setting(std::string_view key, std::string_view fallback = {}) const;
};

struct MaterialLink {
    Uuid guid;
    Uuid fromNode;
    std::string fromPin;
    Uuid toNode;
    std::string toPin;
};

/// Parameter yang diekspos graph ke instance material.
///
/// Inilah yang membuat `.simmatinst` mungkin: instance tidak menyalin graph,
/// ia hanya menimpa nilai-nilai ini. Node `param.*` di kanvas membaca dari
/// daftar yang sama, sehingga tidak ada dua sumber kebenaran untuk "parameter
/// apa saja yang dimiliki material ini".
struct MaterialParameter {
    std::string name;
    ValueKind kind = ValueKind::Float;
    /// Literal Slang. Kosong diisi nilai netral menurut tipenya.
    std::string defaultValue;
    /// Keterangan untuk tooltip di panel parameter.
    std::string tooltip;
    /// Rentang slider di panel parameter. Sama berarti "tanpa batas".
    float minValue = 0.0f;
    float maxValue = 0.0f;
    /// Digambar sebagai pemilih warna, bukan tiga kotak angka.
    ///
    /// Ditandai penulis material, bukan ditebak dari namanya. Menebak dari nama
    /// bekerja untuk "tint" lalu gagal diam-diam untuk "rimColour" atau untuk
    /// sebuah float3 yang memang arah, dan tebakan yang salah pada pemilih warna
    /// jauh lebih menyulitkan daripada tiga kotak angka yang polos.
    bool isColor = false;
};

struct MaterialGraph {
    std::vector<MaterialNode> nodes;
    std::vector<MaterialLink> links;
    std::vector<MaterialParameter> parameters;

    const MaterialNode* FindNode(const Uuid& guid) const;
    const MaterialParameter* FindParameter(std::string_view name) const;

    /// Link yang berakhir di pin input tertentu. Sebuah pin input hanya boleh
    /// punya satu sumber, jadi hasilnya paling banyak satu.
    const MaterialLink* LinkInto(const Uuid& node, std::string_view pin) const;

    /// Seluruh link yang berangkat dari pin output tertentu. Pin output boleh
    /// bercabang ke banyak tujuan — di graph material itu justru hal biasa,
    /// karena satu tekstur kerap memberi makan beberapa kanal sekaligus.
    std::vector<const MaterialLink*> LinksFrom(const Uuid& node, std::string_view pin) const;

    /// Menghapus sebuah node beserta seluruh koneksi yang menyentuhnya.
    ///
    /// Membuang node tanpa membuang link-nya meninggalkan koneksi yang menunjuk
    /// GUID yang tidak ada — bentuk kerusakan yang tidak terlihat di kanvas
    /// (kabelnya memang hilang) tapi tertulis ke berkas dan baru meledak saat
    /// dikompilasi.
    void RemoveNode(const Uuid& guid);
};

struct MaterialIoResult {
    bool ok = false;
    std::string error;
    int sourceVersion = kMaterialSchemaVersion;
};

/// Menulis graph sebagai teks JSON.
///
/// Keluarannya deterministik — node dan link menurut urutan simpannya, setting
/// menurut urutan kunci — supaya dua kali menyimpan graph yang sama
/// menghasilkan byte yang sama dan berkasnya ramah diff.
std::string SaveMaterialToString(const MaterialGraph& graph);
MaterialIoResult SaveMaterialToFile(const MaterialGraph& graph,
                                    const std::filesystem::path& path);

MaterialIoResult LoadMaterialFromString(MaterialGraph& graph, const std::string& text);
MaterialIoResult LoadMaterialFromFile(MaterialGraph& graph, const std::filesystem::path& path);

}  // namespace sim::material
