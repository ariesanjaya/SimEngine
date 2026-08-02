#pragma once

#include "Sim/Core/Uuid.h"
#include "Sim/Script/GraphCompiler.h"

#include "Sim/Script/Graph.h"

#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sim::script {

/// Hasil kompilasi graph, tersimpan sebagai berkas `.lua` di samping cache aset.
///
/// **Kenapa di sini dan bukan sebagai importer aset.** Kompilasi butuh
/// GraphCompiler, yang tinggal di modul Script — sedangkan Script sendiri
/// bergantung pada Assets. Menaruhnya sebagai importer akan membalik arah
/// ketergantungan itu dan menutup jalan bagi runtime memakai Assets tanpa Lua.
/// Yang tersisa hanyalah menempatkan kompilasinya di sisi Script, dan itulah
/// kelas ini.
///
/// **Kompilasi ulang hanya ketika perlu.** Memuat level yang memakai graph
/// tidak mengompilasi apa pun selama hasil kompilasinya masih lebih baru
/// daripada berkas graph-nya. Menyunting graph membuat sumbernya lebih baru,
/// dan Play berikutnya memakai hasil yang baru — tanpa langkah build manual.
///
/// **Satu compiler untuk kedua jalur.** Yang berjalan saat memuat level dan
/// yang dilihat pengguna di panel "Compiled Lua" berasal dari fungsi yang sama
/// persis. Hasil yang berbeda antara editor dan runtime adalah kelas bug yang
/// tidak boleh dibuka.
class GraphCache final : public GraphLibrary {
public:
    void Initialize(std::filesystem::path directory);

    /// Cara menemukan berkas `.simgraph` dari GUID-nya. Diisi editor dari
    /// AssetDatabase; tanpa ini, graph tidak bisa memanggil graph lain.
    void SetSourceResolver(std::function<std::filesystem::path(const Uuid&)> resolver);

    // --- GraphLibrary ---
    const Graph* Find(const Uuid& guid) const override;
    std::string NameOf(const Uuid& guid) const override;

    /// Berkas `.lua` untuk sebuah graph, dikompilasi ulang bila sudah usang.
    ///
    /// Mengembalikan path kosong bila graph-nya tidak bisa dibaca atau tidak
    /// bisa dikompilasi; alasannya ada di `LastResult(guid)`.
    std::filesystem::path EnsureCompiled(const Uuid& guid,
                                         const std::filesystem::path& source);

    /// Memaksa kompilasi ulang, mengabaikan waktu ubah berkas. Dipakai editor
    /// ketika graph disimpan dari panel — di sana sumbernya sudah pasti berubah,
    /// dan menunggu resolusi waktu berkas berarti perubahan bisa terlewat.
    std::filesystem::path Rebuild(const Uuid& guid, const std::filesystem::path& source);

    /// Hasil kompilasi terakhir sebuah graph, atau null bila belum pernah.
    /// Peta sumbernya inilah yang dipakai menyorot node saat runtime gagal.
    const CompileResult* LastResult(const Uuid& guid) const;

    /// Node yang diberi breakpoint pada sebuah graph.
    ///
    /// Disimpan di sini, bukan di panel, supaya SETIAP jalur kompilasi memakai
    /// daftar yang sama — termasuk kompilasi ulang yang dipicu pemantau berkas,
    /// yang tidak tahu-menahu tentang panel mana pun. Breakpoint yang hilang
    /// begitu berkasnya disimpan adalah breakpoint yang tidak bisa dipercaya.
    void SetBreakpoints(const Uuid& guid, std::vector<Uuid> nodes);
    const std::vector<Uuid>& Breakpoints(const Uuid& guid) const;

    /// Nama chunk yang dipakai Lua untuk graph ini, mis. "spin.simgraph.lua".
    /// Traceback menyebut nama ini, dan dari sanalah node penyebabnya dicari.
    static std::string ChunkName(const std::filesystem::path& source);

    const std::filesystem::path& Directory() const { return directory_; }

private:
    std::filesystem::path PathFor(const Uuid& guid) const;
    std::filesystem::path Compile(const Uuid& guid, const std::filesystem::path& source);
    /// Waktu ubah terbaru di antara sebuah graph dan seluruh graph yang
    /// dipanggilnya, langsung maupun tidak langsung.
    ///
    /// Menyunting sebuah template harus membuat setiap pemakainya ikut usang.
    /// Membandingkan berkasnya sendiri saja akan membuat pemakainya menjalankan
    /// hasil kompilasi lama — dan perbedaan antara yang dilihat di editor dan
    /// yang berjalan saat Play adalah kelas bug yang paling mahal di sini.
    std::filesystem::file_time_type NewestSourceTime(const Uuid& guid,
                                                     const std::filesystem::path& source,
                                                     std::vector<Uuid>& seen) const;

    std::filesystem::path directory_;
    std::function<std::filesystem::path(const Uuid&)> sourceResolver_;
    std::unordered_map<Uuid, CompileResult> results_;
    std::unordered_map<Uuid, std::vector<Uuid>> breakpoints_;
    /// Graph yang sudah diurai, supaya memanggil graph yang sama dari beberapa
    /// tempat tidak membaca berkasnya berulang kali.
    mutable std::unordered_map<Uuid, Graph> parsed_;
};

}  // namespace sim::script
