#pragma once

#include "Sim/Material/MaterialGraph.h"

#include <string>
#include <string_view>
#include <vector>

namespace sim::material {

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

}  // namespace sim::material
