#pragma once

#include "Sim/Core/Uuid.h"
#include "Sim/Script/Graph.h"

#include <string>
#include <string_view>
#include <vector>

namespace sim::script {

/// Satu kesalahan kompilasi, beserta node penyebabnya.
///
/// `node` yang terisi bukan kemewahan: pesan yang hanya menyebut "ada siklus"
/// memaksa pengguna menelusuri graph sendiri, sementara editor sudah tahu
/// persis node mana yang harus disorot.
struct CompileError {
    Uuid node;
    std::string message;
};

/// Baris Lua yang dihasilkan sebuah node.
struct SourceMapEntry {
    Uuid node;
    /// Nomor baris di dalam `lua`, dihitung dari 1.
    int line = 0;
};

struct CompileResult {
    bool ok = false;
    /// Sumber Lua yang dihasilkan. Terisi juga saat gagal, supaya panel bisa
    /// menampilkan sejauh mana kompilasi sempat berjalan.
    std::string lua;
    std::vector<CompileError> errors;
    std::vector<SourceMapEntry> sourceMap;

    /// Node yang bertanggung jawab atas sebuah baris Lua.
    ///
    /// Inilah yang mengubah "error di baris 42" menjadi node yang menyala merah
    /// di kanvas. Tanpa peta ini, pengguna graph diberi nomor baris di berkas
    /// yang tidak pernah ia lihat.
    Uuid NodeAtLine(int line) const;

    /// Seluruh node yang ikut menghasilkan sebuah baris.
    ///
    /// Satu baris kerap memuat beberapa node: node murni disisipkan sebagai
    /// ekspresi ke dalam pernyataan yang memakainya, karena itulah yang membuat
    /// hasilnya terbaca seperti kode tulis tangan. Ketika baris itu gagal saat
    /// runtime, semuanya adalah tersangka yang sah — dan menyorot semuanya jauh
    /// lebih jujur daripada menebak satu.
    std::vector<Uuid> NodesAtLine(int line) const;

    /// Baris pertama yang dihasilkan sebuah node, atau 0.
    int LineOfNode(const Uuid& node) const;
};

/// Mengompilasi graph menjadi sumber Lua yang bisa dibaca manusia.
///
/// **Kenapa dikompilasi, bukan ditafsirkan.** Alternatifnya adalah mesin graph
/// yang menelusuri node satu per satu saat runtime — dua jalur eksekusi yang
/// harus dijaga sama perilakunya, dan yang kedua selalu lebih lambat sekaligus
/// lebih sulit di-debug. Dengan mengompilasi, yang berjalan hanya satu runtime:
/// graph adalah *penulis kode*, bukan penafsir. Profiler, traceback, hot
/// reload, dan sandbox yang sudah ada langsung berlaku untuknya.
///
/// **Keluarannya sengaja layak dibaca.** Itu bukan kemewahan: ia yang membuat
/// graph bisa di-debug dengan alat yang sama seperti skrip biasa, dan membuat
/// pengguna bisa lulus dari visual scripting ke Lua tanpa jurang.
///
/// `chunkName` hanya masuk komentar kepala berkas; nama chunk yang dipakai Lua
/// ditentukan pemanggil saat menjalankannya.
CompileResult CompileGraph(const Graph& graph, std::string_view chunkName = {});

}  // namespace sim::script
