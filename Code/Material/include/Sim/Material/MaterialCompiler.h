#pragma once

#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialValidation.h"

#include <string>
#include <string_view>
#include <vector>

namespace sim::material {

/// Baris Slang yang dihasilkan sebuah node.
struct SourceMapEntry {
    Uuid node;
    /// Nomor baris di dalam `slang`, dihitung dari 1.
    int line = 0;
};

/// Tekstur yang dipakai graph, beserta nama binding yang dihasilkan.
///
/// Renderer memakainya untuk mengikat aset ke slot yang benar tanpa harus
/// membaca ulang graph-nya.
struct TextureBinding {
    /// Nama variabel di kode yang dihasilkan, mis. "tAlbedo".
    std::string name;
    /// Aset tekstur yang dirujuk node. Tidak valid berarti node-nya belum diisi.
    Uuid texture;
    Uuid node;
};

struct MaterialCompileResult {
    bool ok = false;
    /// Sumber Slang yang dihasilkan. Terisi juga saat gagal, supaya panel bisa
    /// menampilkan sejauh mana kompilasi sempat berjalan.
    std::string slang;
    std::vector<MaterialIssue> errors;
    std::vector<SourceMapEntry> sourceMap;
    std::vector<TextureBinding> textures;
    /// Parameter yang benar-benar dipakai graph. Yang dideklarasikan tapi tidak
    /// pernah dibaca tetap ikut ditulis ke cbuffer — tata letaknya harus sama
    /// dengan yang diharapkan material instance, apa pun isi graph-nya.
    std::vector<std::string> usedParameters;

    /// Node yang bertanggung jawab atas sebuah baris. Inilah yang mengubah
    /// "error di baris 42" menjadi node yang menyala merah di kanvas.
    Uuid NodeAtLine(int line) const;
    /// Baris pertama yang dihasilkan sebuah node, atau 0.
    int LineOfNode(const Uuid& node) const;
};

struct MaterialCompileOptions {
    /// Nama modul Slang yang dihasilkan. Dipakai sebagai nama berkas dan
    /// muncul di komentar kepala.
    std::string moduleName = "material";
};

/// Mengompilasi graph material menjadi sumber Slang.
///
/// **Yang dihasilkan mengisi `OpenPBRSurface`, bukan menghitung cahaya.** Model
/// shading-nya sudah ada dan sudah diuji di `openpbr.slang`; tugas graph hanya
/// menjawab "berapa nilai tiap parameter permukaan di titik ini". Memisahkan
/// keduanya berarti mengubah model shading tidak menyentuh satu pun material,
/// dan mengubah material tidak bisa merusak model shading.
///
/// **Hanya pin yang benar-benar dikemudikan yang ditulis.** Fungsinya diawali
/// `OpenPBRSurface::defaults()`, lalu menimpa pin yang tersambung atau yang
/// nilainya diketik pengguna. Akibatnya nilai bawaan runtime tinggal di satu
/// tempat — shader-nya sendiri — dan kode yang dihasilkan tetap pendek untuk
/// material yang hanya menyentuh dua-tiga kanal, yaitu sebagian besarnya.
///
/// **Keluarannya sengaja layak dibaca.** Itu bukan kemewahan: ia yang membuat
/// material bisa di-debug dengan alat shader biasa, dan membuat penulis material
/// bisa lulus dari graph ke Slang tanpa jurang.
///
/// Graph yang tidak lolos `ValidateMaterial` tidak dikompilasi; kesalahannya
/// diteruskan apa adanya.
MaterialCompileResult CompileMaterial(const MaterialGraph& graph,
                                      const MaterialCompileOptions& options = {});

}  // namespace sim::material
