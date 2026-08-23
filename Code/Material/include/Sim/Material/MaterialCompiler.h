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
    /// Material memutar sumbu anisotropinya lewat pin `tangent`.
    bool tangent = true;
    /// Material memberi coat bingkai sendiri lewat `coatNormal`/`coatTangent`.
    ///
    /// **Mati berarti coat memakai bingkai dasar apa adanya**, dan itu bukan
    /// sekadar penghematan: bingkai coat yang dibangun dari normal geometri
    /// membuat coat berhenti mengikuti peta normal dasarnya. Untuk material
    /// yang memang tidak menyatakan apa-apa soal coat-nya, yang benar adalah
    /// mengikuti — bukan diam-diam menjadi rata.
    bool coatFrame = true;
};

struct MaterialCompileResult {
    bool ok = false;
    /// Domain yang dinyatakan asetnya. `alphaTest` dan `alphaBlend` di bawah
    /// **diturunkan darinya**, bukan sebaliknya — keduanya dipertahankan karena
    /// itulah yang dibaca pembangun pipeline, dan mengubah tanda tangannya
    /// bukan bagian dari perubahan ini.
    MaterialDomain domain = MaterialDomain::Opaque;
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

    /// Material bertopeng: fragmen yang opasitasnya di bawah `alphaCutoff`
    /// dibuang, bukan dipadu.
    ///
    /// **Topeng, bukan transparansi.** Yang dipadu menuntut urutan gambar dari
    /// belakang ke depan dan karena itu diputuskan per objek; yang dibuang tidak
    /// menuntut apa pun dan karena itu bisa diputuskan per material. Decal
    /// kotoran, dedaunan, dan pagar kawat semuanya jenis yang kedua — dan
    /// tanpa jalur ini mereka digambar sebagai kuad pejal berwarna apa pun yang
    /// kebetulan ada di bagian tekstur yang seharusnya tak terlihat. Di Sponza
    /// itu hitam pekat: 20,9% tekstur decal-nya beralfa nol, dan RGB di sana
    /// tepat (0,0,0).
    bool alphaTest = false;
    /// Ambang buang. Nilai glTF bawaan, dan alasannya sama: 0,5 adalah tengah.
    float alphaCutoff = 0.5f;

    /// Material dipadu: opasitasnya mencampur warnanya dengan apa yang sudah
    /// ada di belakangnya, bukan membuangnya.
    ///
    /// **Diputuskan per material, dijalankan per objek.** Yang dipadu menuntut
    /// urutan gambar dari belakang ke depan, jadi ruasnya keluar dari daftar
    /// buram dan masuk ke daftar tersortir — dan dengan itu keluar pula dari
    /// prepass dan dari pass bayangan, keduanya karena alasan yang sama:
    /// permukaan yang setengah tembus tidak boleh menghalangi apa pun.
    bool alphaBlend = false;

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

    /// Pin keluaran yang isinya ingin **dilihat**, bukan dipakai.
    ///
    /// Kosong berarti kompilasi biasa: yang keluar adalah permukaan yang ditulis
    /// node keluaran. Terisi berarti seluruh permukaan itu diabaikan dan yang
    /// digambar hanyalah nilai pin ini — sebagai `emissive`, dengan `baseColor`
    /// nol, sehingga yang terlihat persis angkanya dan bukan angkanya setelah
    /// dikali cahaya.
    ///
    /// **Sebuah pratinjau yang ikut dibayangi berbohong tentang isinya.** Sebuah
    /// UV yang digeser terhadap x akan tetap bergeser di bawah pencahayaan, tapi
    /// warnanya tidak lagi bisa dibaca sebagai koordinat, dan justru itu yang
    /// dicari orang saat membuka pratinjau sebuah node.
    ///
    /// Pin bertipe `Texture` disampel lebih dulu dengan `uv0`: yang dimaksud
    /// orang yang menunjuk node tekstur adalah gambarnya, bukan bahwa ia sebuah
    /// binding.
    Uuid previewNode;
    std::string previewPin;

    /// Apakah opsi ini meminta pratinjau sebuah pin.
    bool WantsPreview() const { return previewNode.IsValid() && !previewPin.empty(); }
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
