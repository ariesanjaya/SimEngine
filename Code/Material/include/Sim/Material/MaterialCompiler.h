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
    /// Nama parameter yang mengisinya, untuk node `param.texture`. Kosong
    /// berarti tekstur ini bagian tetap definisi materialnya dan **tidak** bisa
    /// diisi instance — yang membedakan keduanya bukan renderer melainkan
    /// penulis materialnya.
    std::string parameter;
};

/// Lapisan OpenPBR yang **mungkin** dipakai sebuah material.
///
/// **Mungkin, bukan pasti.** Pin yang tersambung ke sesuatu bisa bernilai apa
/// saja saat digambar, jadi jawabannya "ya" — yang bisa dijawab "tidak" hanya
/// pin yang literalnya nol dan tidak tersambung ke mana pun. Tebakan ke arah
/// sebaliknya akan menghapus lobe yang ternyata dipakai, dan yang terlihat
/// bukan galat melainkan coat yang hilang pada material tertentu saja.
///
/// Gunanya mematikan lobe **saat kompilasi**. Di GPU, cabang yang diambil
/// sebagian lane dalam satu warp membayar kedua sisinya — jadi satu material
/// bercoat di layar membuat tetangganya ikut membayar, dan material yang
/// coat-nya nol tetap membawa kodenya di dalam SPIR-V. Dengan konstanta yang
/// nilainya diketahui saat kompilasi, kodenya tidak pernah ada.
struct SurfaceLobes {
    bool coat = true;
    bool fuzz = true;
    bool anisotropy = true;
    bool diffuseRoughness = true;
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
    /// Lapisan yang mungkin dipakai. Dipakai `AssembleMaterialModule` untuk
    /// mematikan yang tidak mungkin dipakai sebelum `slangc` melihatnya.
    SurfaceLobes lobes;

    /// Node yang bertanggung jawab atas sebuah baris. Inilah yang mengubah
    /// "error di baris 42" menjadi node yang menyala merah di kanvas.
    Uuid NodeAtLine(int line) const;
    /// Baris pertama yang dihasilkan sebuah node, atau 0.
    int LineOfNode(const Uuid& node) const;
};

struct MaterialCompileOptions {
    /// Nama yang muncul di komentar kepala, dipakai apa adanya.
    ///
    /// Pemanggil yang menentukan bentuknya — termasuk ekstensinya. Kompiler yang
    /// menambahkan ".simmat" sendiri akan menghasilkan "Batu.simmat.simmat"
    /// untuk pemanggil yang sudah menyertakannya, dan komentar kepala yang
    /// menyebut berkas yang tidak ada adalah petunjuk yang menyesatkan tepat
    /// ketika seseorang mencarinya.
    std::string moduleName = "material.simmat";

    /// Menulis jalur bindless: satu larik descriptor bersama, bukan satu set
    /// per material.
    ///
    /// **Yang berubah bukan isi material melainkan dari mana datanya diambil.**
    /// Blok parameter tetap `cbuffer` dengan tata letak std140 yang sama persis,
    /// tekstur tetap dirujuk dengan nama yang sama, dan badan `evalMaterial`
    /// tidak berubah satu baris pun — yang bertambah hanya prolog yang menyalin
    /// keduanya dari larik ke nama-nama lokal itu.
    ///
    /// Salah satu dari keduanya harus dipilih sebelum `slangc` dipanggil, karena
    /// yang berbeda adalah bentuk descriptor set layout-nya. Lihat G5 di
    /// docs/PLAN-GPU-OPTIM.md.
    bool bindless = false;
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
