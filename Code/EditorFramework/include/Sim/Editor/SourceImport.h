#pragma once

#include <filesystem>
#include <string>
#include <vector>

/// Membongkar satu berkas sumber — FBX, glTF, atau USD — menjadi aset-aset
/// mesin ini sekaligus.
///
/// **Satu berkas FBX bukan satu aset.** Yang keluar dari DCC membawa geometri,
/// rangka, beberapa take animasi, dan sejumlah material di dalam satu berkas.
/// Mesin ini menyimpan keempatnya sebagai aset yang berbeda — mesh dirujuk
/// `MeshRendererComponent`, `.simskel` oleh rig, `.simanim` oleh Animator,
/// `.simmatinst` oleh material — jadi tanpa langkah ini setiap orang yang
/// mengimpor sebuah karakter harus membuat keempatnya sendiri, satu per satu,
/// dan menamainya konsisten supaya bisa ditemukan lagi.
///
/// **Penerjemahannya ada di editor, bukan di salah satu modulnya.** `Sim::Assets`
/// tidak boleh mengenal tipe animasi dan sebaliknya — keduanya modul sejajar —
/// jadi yang menjembatani adalah yang memakai keduanya. Alasan yang sama
/// menempatkan `SkinnedPreview` di sini.
namespace sim::editor {

/// Bagian berkas sumber yang ikut dijadikan aset.
///
/// Seluruhnya menyala secara bawaan: yang mengimpor sebuah karakter hampir
/// selalu menginginkan semuanya. Mematikan sebagiannya adalah untuk dua hal yang
/// memang lazim — berkas Mixamo yang hanya dipakai animasinya (rig-nya sudah
/// ada), dan mesh statis yang animasinya tidak berarti apa-apa.
struct SourceImportOptions {
    /// Menyalin berkas sumbernya ke folder tujuan. Yang mematikannya tetap bisa
    /// mengambil animasi maupun materialnya — aset yang dihasilkan berdiri
    /// sendiri dan tidak menunjuk kembali ke berkas sumbernya.
    bool mesh = true;
    bool skeleton = true;
    bool animation = true;
    bool materials = true;
};

/// Apa yang benar-benar ADA di dalam berkasnya.
///
/// Dibaca lebih dulu supaya dialog impor bisa meredupkan pilihan yang tidak ada
/// isinya. Sebuah kotak centang "Animasi" yang bisa dicentang pada berkas tanpa
/// animasi menjanjikan sesuatu yang tidak akan terjadi.
struct SourceContents {
    bool hasMesh = false;
    int boneCount = 0;
    int clipCount = 0;
    int materialCount = 0;
    /// Terisi bila berkasnya tidak bisa dibaca sama sekali.
    std::string error;
};

/// Membaca berkasnya tanpa menulis apa pun.
SourceContents InspectSource(const std::filesystem::path& path);

struct SourceImportResult {
    bool ok = false;
    std::string error;
    /// Nama berkas yang ditulis, relatif terhadap folder tujuan.
    std::vector<std::string> written;
};

/// Menulis aset untuk tiap bagian yang diminta `options`.
///
/// Seluruh berkas diberi nama menurut nama berkas sumbernya — `Y Bot.fbx`
/// menghasilkan `Y Bot.simskel` dan `Y Bot - Running.simanim` — supaya keempat
/// jenis aset itu berdampingan saat diurutkan menurut nama, dan supaya jelas
/// dari mana asalnya. Nama yang sudah dipakai diberi akhiran nomor, tidak
/// ditimpa: menimpa aset yang sudah dirujuk level adalah kehilangan yang tidak
/// bisa dibatalkan dari sini.
SourceImportResult ImportSource(const std::filesystem::path& source,
                                const std::filesystem::path& destinationFolder,
                                const SourceImportOptions& options);

}  // namespace sim::editor
