#pragma once

#include "Sim/Material/MaterialGraph.h"

#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sim::material {

/// Apakah pin node keluaran ini berada **di luar** `OpenPBRSurface`.
///
/// **Satu daftar, bukan tiga.** Yang di luar struct itu tidak punya
/// `defaults()` untuk bersandar, jadi kompiler harus menulisnya walau tidak
/// dikemudikan, dan uji kesamaan nilai bawaan harus melewatinya. Ketika
/// daftarnya tersebar, menambah satu pin geometri berarti dua tempat yang
/// diam-diam berselisih — dan yang muncul adalah float3 tak berisi, bukan galat.
bool SurfacePinIsExtra(std::string_view pin);

/// Kunci node keluaran. Tepat satu node bertipe ini boleh ada di sebuah graph —
/// ia yang menentukan apa arti graph itu.
inline constexpr std::string_view kSurfaceOutputType = "output.surface";

/// Satu jenis node beserta bentuk pin-nya.
struct MaterialNodeType {
    /// Kunci yang tertulis di berkas `.simmat`, mis. "math.lerp".
    /// Mengubahnya memutus material lama.
    std::string key;
    /// Yang ditampilkan di kanvas dan di pencarian palet.
    std::string label;
    /// Pengelompokan di palet, mis. "Input", "Math", "Utility", "Output".
    std::string category;
    std::string tooltip;
    std::vector<MaterialPin> pins;

    const MaterialPin* FindPin(std::string_view name) const;
    std::vector<const MaterialPin*> Inputs() const;
    std::vector<const MaterialPin*> Outputs() const;
};

/// Seluruh jenis node yang bisa dipakai sebuah graph material.
///
/// **Node keluarannya mengikuti OpenPBR Surface v1.1**, bukan himpunan
/// "base color / metallic / roughness" yang lazim. Alasannya bukan kelengkapan
/// demi kelengkapan: shader yang akan menjalankannya sudah ditulis terhadap
/// spesifikasi itu (`openpbr.slang`), dan graph yang mengekspos himpunan
/// parameter berbeda akan menuntut lapisan penerjemah di antara keduanya —
/// tempat yang paling mungkin membuat material terlihat berbeda antara preview
/// dan hasil akhir.
///
/// Pin yang tidak ada di `OpenPBRSurface` — normal, emissive, opacity — memang
/// bukan bagian BRDF-nya: ketiganya bekerja di luar lobe, pada geometri dan
/// pada penggabungan akhir.
class MaterialNodeCatalog {
public:
    /// Katalog bersama, dibangun saat pertama dipakai.
    static const MaterialNodeCatalog& Get();

    const MaterialNodeType* Find(std::string_view key) const;
    const std::vector<MaterialNodeType>& All() const { return types_; }

private:
    MaterialNodeCatalog();

    std::vector<MaterialNodeType> types_;
};

/// Pin sebuah node **di dalam konteks graph-nya**.
///
/// Berbeda dari `MaterialNodeType::pins` untuk node yang bentuknya ditentukan
/// isi graph: tipe pin node `param.get` mengikuti deklarasi parameternya.
/// Kompiler dan panel memanggil fungsi yang sama supaya yang digambar di kanvas
/// tidak pernah berbeda dari yang dikompilasi.
///
/// Kosong bila tipe node-nya tidak ada di katalog.
std::vector<MaterialPin> PinsOf(const MaterialGraph& graph, const MaterialNode& node);

/// Apakah sebuah pin node keluaran permukaan berlaku untuk domain ini.
///
/// **Dipakai kanvas, bukan kompiler.** Yang dijawabnya "boleh ditawarkan kepada
/// pengguna", bukan "boleh ada di kode yang dihasilkan" — kode itu tetap wajib
/// menginisialisasi setiap medan `MaterialSurface`, berapa pun yang ditawarkan.
bool SurfacePinApplies(std::string_view pin, MaterialDomain domain);

/// Tipe konkret setiap pin keluaran, setelah penyimpulan.
///
/// Node matematika dideklarasikan `Numeric` — ia tidak peduli lebar masukannya.
/// Tipe hasilnya baru diketahui setelah melihat apa yang tersambung: `float3 *
/// float` menghasilkan float3, aturan pelebaran yang sama dengan Slang.
///
/// **Satu penyimpulan, dipakai bersama.** Validasi, kompiler, dan panel
/// memanggil yang ini. Kalau masing-masing menyimpulkan sendiri, kanvas bisa
/// menerima sambungan yang kemudian ditolak kompiler — dan pengguna tidak punya
/// cara menebak siapa yang benar.
class MaterialTypes {
public:
    static MaterialTypes Infer(const MaterialGraph& graph);

    /// Tipe sebuah pin keluaran. Untuk pin masukan, atau pin yang tidak
    /// dikenali, mengembalikan tipe yang dideklarasikan katalog.
    ValueKind Of(const MaterialGraph& graph, const MaterialNode& node,
                 std::string_view pin) const;

    /// Tipe yang mengalir MASUK ke sebuah pin input: tipe sumbernya bila
    /// tersambung, atau tipe pin itu sendiri bila tidak.
    ValueKind Into(const MaterialGraph& graph, const MaterialNode& node,
                   std::string_view pin) const;

private:
    void Resolve(const MaterialGraph& graph, const Uuid& node);

    std::map<std::pair<Uuid, std::string>, ValueKind> outputs_;
    std::set<Uuid> resolving_;
};

}  // namespace sim::material
