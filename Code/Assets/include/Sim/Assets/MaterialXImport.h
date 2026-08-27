#pragma once

#include "Sim/Assets/MeshData.h"
#include "Sim/Assets/OpenPbrMaterial.h"

#include <filesystem>
#include <string>
#include <vector>

/// Membaca material OpenPBR dari dokumen MaterialX (`.mtlx`).
///
/// **Kenapa lewat berkas kedua, dan bukan dari FBX saja.** FBX tidak punya slot
/// MaterialX. Yang bisa dibawanya cuma material Lambert/Phong-nya sendiri
/// ditambah blok properti kustom milik DCC-nya — dan blok itu daftar angka,
/// bukan graph: ia tidak bisa menyatakan satu tekstur yang mengemudikan satu
/// input, apalagi node di antaranya. Dokumen `.mtlx` di sebelah berkas mesh
/// bisa, dan ia pula bentuk yang sama yang dipakai Arnold, USD, dan 3ds Max
/// sendiri. Jadi ia yang menang bila ada.
///
/// **Yang dibaca nilainya, bukan shading-nya.** Mesin ini sudah punya OpenPBR
/// Surface v1.1 di `Shaders/openpbr.slang`, dan pin-nya sudah sejalan dengan
/// `open_pbr_surface.mtlx` — itu keputusan E7.1, bukan kebetulan. Jadi yang
/// diambil dari dokumen hanya angka dan jalur teksturnya; tidak sebaris pun
/// kode MaterialX ikut ke dalam game.
namespace sim::assets {

/// Apakah dukungan `.mtlx` ikut dibangun (`SIM_WITH_MATERIALX`).
///
/// **Ditanyakan lewat fungsi, bukan lewat makro di header.** Makro akan
/// membocorkan sakelar build ke setiap pemakainya, dan yang menyalakannya di
/// satu modul tetapi tidak di modul lain mendapat dua ukuran `MeshMaterial`
/// yang menaut mulus.
bool MaterialXAvailable();

/// Satu dokumen yang sudah dibaca.
struct MaterialXDocument {
    /// Material di dalamnya, sesuai urutan dokumennya.
    std::vector<OpenPbrMaterial> materials;
    /// Apa yang **tidak** terwakili: input yang dikemudikan node di luar
    /// gambar, shader yang bukan OpenPBR, dan sejenisnya.
    ///
    /// **Dikumpulkan, bukan didiamkan.** Yang membuat impor "mirip tapi tidak
    /// sama" hampir selalu adalah sesuatu yang dilewati tanpa berkata apa-apa;
    /// daftar ini membuat setiap kelewatan punya satu baris di log.
    std::vector<std::string> notes;
};

/// Membaca seluruh material dari sebuah dokumen `.mtlx`.
///
/// Nama material diambil dari node `surfacematerial`-nya bila ada, dan dari
/// node shader-nya bila dokumen itu hanya berisi shader. Dokumen yang tidak
/// memuat satu pun surface shader yang dikenali bukan galat — `materials`-nya
/// kosong dan alasannya masuk ke `notes`.
bool LoadMaterialXDocument(const std::filesystem::path& path, MaterialXDocument& out,
                           std::string& error);

/// Mencari dokumen `.mtlx` pendamping sebuah berkas mesh, lalu memasangkan
/// materialnya ke `mesh.materials` berdasarkan nama.
///
/// Urutan pencariannya, dan berhenti pada yang pertama terbaca:
///
///  1. `hints` — jalur yang **disebut berkas mesh itu sendiri**, relatif
///     terhadapnya. Berkas dari 3ds Max yang materialnya memakai MaterialX
///     menyimpan jalur dokumennya di properti kustom materialnya.
///  2. `<nama berkas mesh>.mtlx` di folder yang sama.
///  3. Satu-satunya `.mtlx` di folder itu — **dan hanya bila memang cuma ada
///     satu.** Dua berkas berarti tidak ada jawaban yang bisa ditebak, dan
///     menebaknya menghasilkan material yang salah tanpa satu pun tanda.
///
/// Mengembalikan berapa material mesh yang benar-benar terisi. Nol bukan
/// kegagalan: berkas mesh tanpa dokumen pendamping adalah keadaan yang lazim,
/// dan jalur impor lamanya tetap berlaku seluruhnya.
std::size_t ApplyMaterialX(MeshData& mesh, const std::filesystem::path& meshPath,
                           const std::vector<std::string>& hints = {});

}  // namespace sim::assets
