#pragma once

#include "Sim/ImageIO/Image.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// Dekode dan enkode gambar untuk pengondisian aset.
///
/// **Bukan loader runtime.** Batasnya ditulis di docs/PLAN-IMAGEIO.md dan
/// ditegakkan oleh graf ketergantungan: modul ini boleh dipakai importir,
/// editor, dan baker, dan ia sendiri tidak boleh bergantung pada `Sim::RHI`.
/// Yang dihasilkannya adalah piksel di memori; yang mengunggahnya ke GPU adalah
/// pemanggilnya.
namespace sim::imageio {

/// Bentuk yang diminta pemanggil — bukan bentuk yang ada di berkas.
///
/// Konversinya dilakukan backend saat dekode, bukan setelahnya oleh pemanggil.
/// Alasannya bukan kerapian: menaikkan 8-bit ke float setelah dekode berarti
/// dua buffer hidup bersamaan untuk gambar yang bisa berukuran ratusan megabyte.
struct ReadOptions {
    /// 1, 3, atau 4. Nol berarti "seperti di berkas".
    uint32_t channels = 0;
    /// Kosong berarti tipe berkasnya dipakai apa adanya.
    std::optional<PixelType> type;
};

/// Hasil satu operasi, mengikuti pola `TerrainIoResult` dan `VegetationIoResult`.
///
/// `error` selalu terisi ketika `ok` bernilai false, dan selalu menyebut jalur
/// berkasnya — pesan "cannot decode image" tanpa nama berkas mengirim orang
/// memeriksa berkas yang salah.
struct ImageIoResult {
    bool ok = false;
    std::string error;

    explicit operator bool() const { return ok; }
};

/// Membaca gambar dari berkas.
ImageIoResult Read(const std::filesystem::path& path, const ReadOptions& options, Image& out);

/// Membaca gambar dari memori.
///
/// `extensionHint` (dengan titik, misalnya ".png") boleh kosong untuk backend
/// yang mengenali formatnya dari byte pertamanya sendiri. Ia diminta karena
/// backend lain memilih pembaca berdasarkan nama — dan menebak dari isi berkas
/// adalah tebakan yang gagalnya diam.
ImageIoResult Read(std::span<const uint8_t> bytes, std::string_view extensionHint,
                   const ReadOptions& options, Image& out);

/// Membaca kepala berkas saja: dimensi, jumlah kanal, tipe piksel.
///
/// Ada karena indeks aset hanya butuh angka-angka itu, dan mendekode gambar 8K
/// untuk mengisi dua angka di panel detail adalah pemborosan yang langsung
/// terasa saat folder berisi ribuan tekstur dipindai.
ImageIoResult Probe(const std::filesystem::path& path, ImageDesc& out);

/// Menulis gambar ke berkas. Formatnya dari ekstensinya.
///
/// **Jauh lebih sedikit format yang bisa ditulis daripada dibaca**, dan itu
/// disengaja: menulis format berarti menjanjikan berkas yang bisa dibuka alat
/// lain, dan janji itu hanya dibuat untuk format yang memang ditulis jalur kerja
/// di sini — PNG greyscale untuk heightmap dan mask, TIFF untuk pertukaran
/// dengan alat terrain.
ImageIoResult Write(const std::filesystem::path& path, const Image& image);

/// Menulis gambar ke memori; `extension` (dengan titik) menentukan formatnya.
///
/// Tidak semua yang bisa ditulis ke berkas bisa ditulis ke memori — TIFF tidak —
/// dan yang tidak bisa menolak dengan menyebutnya.
ImageIoResult Encode(const Image& image, std::string_view extension,
                     std::vector<uint8_t>& out);

/// Ekstensi (huruf kecil, dengan titik) yang bisa dibaca backend yang aktif,
/// terurut. **Dibangkitkan, bukan dipatok** — inilah yang membuat daftar format
/// di `AssetTypes` menyusut atau bertambah mengikuti kemampuan yang sungguh ada.
const std::vector<std::string>& ReadableExtensions();

/// Ekstensi yang bisa ditulis, terurut. Selalu himpunan bagian dari yang bisa
/// dibaca.
const std::vector<std::string>& WritableExtensions();

bool CanWrite(std::string_view extension);

/// Huruf besar-kecil pada `extension` diabaikan; titiknya wajib.
bool CanRead(std::string_view extension);

/// Nama backend yang akan menangani ekstensi ini ("stb", "oiio"), atau kosong
/// bila tidak ada. Dipakai pesan galat: "butuh backend oiio" memberi tahu apa
/// yang harus dipasang, sementara "format tidak dikenal" tidak.
std::string_view BackendFor(std::string_view extension);

/// Backend yang aktif, sesuai urutan prioritasnya.
const std::vector<std::string>& BackendNames();

/// Semua backend yang bisa membaca ekstensi ini, sesuai urutan prioritasnya.
/// Kosong bila tidak ada.
std::vector<std::string> BackendsFor(std::string_view extension);

/// Membaca lewat backend bernama, bukan lewat yang didahulukan.
///
/// **Ada untuk satu alasan yang disebutkan rencananya**: kalau dua backend
/// membaca berkas yang sama secara berbeda, salah satunya salah, dan yang harus
/// dilakukan adalah menyelidikinya — bukan memilih yang kelihatan benar. Tanpa
/// jalan masuk ini, membandingkan keduanya menuntut membangun ulang seluruh
/// proyek dengan konfigurasi lain, dan perbandingan yang semahal itu tidak akan
/// pernah dijalankan.
///
/// Gagal dengan pesan bila backend itu tidak ada atau tidak menangani formatnya.
ImageIoResult ReadWith(std::string_view backend, const std::filesystem::path& path,
                       const ReadOptions& options, Image& out);

/// Satu baris untuk log startup dan untuk DEPENDENCIES.md, misalnya
/// "stb (7 format)" atau "oiio 2.5.18 + stb (9 format)".
std::string BackendSummary();

}  // namespace sim::imageio
