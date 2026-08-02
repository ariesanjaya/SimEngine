#pragma once

#include "Sim/Core/Uuid.h"
#include "Sim/Scene/Components.h"
#include "Sim/Scene/World.h"
#include "Sim/Script/GraphCache.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace sim::assets {
class AssetDatabase;
}

namespace sim::script {

class LuaVM;

/// Satu nilai hasil evaluasi, sudah berbentuk teks.
///
/// Tabel dipecah menjadi `children` supaya panel bisa melipatnya. Bentuknya
/// sengaja tidak memuat apa pun dari sol2: panel Editor hanya me-link inti
/// ImGui, dan menyeret header Lua ke sana akan menembus aturan modul yang sama
/// yang menjaga Vulkan tetap di luar EditorFramework.
struct EvalNode {
    std::string label;
    std::string value;
    std::vector<EvalNode> children;
};

/// Kesalahan runtime terakhir sebuah aset skrip atau graph.
///
/// Disimpan, bukan sekadar dicatat ke log, karena satu pemakainya menuntut lebih
/// dari teks: panel Graph Editor menerjemahkan `line` kembali menjadi node lewat
/// peta sumber, lalu menyorotnya di kanvas. Tanpa nomor baris yang tersimpan,
/// yang bisa diberikan editor kepada pengguna graph hanyalah nomor baris di
/// berkas yang tidak pernah ia lihat.
struct RuntimeFailure {
    /// Baris di dalam chunk, 0 bila tidak bisa dibaca dari pesannya.
    int line = 0;
    std::string message;
};

struct EvalResult {
    bool ok = true;
    /// Terisi saat gagal — pesan beserta traceback.
    std::string error;
    std::vector<EvalNode> values;
};

/// Menjalankan skrip Lua yang menempel pada entity.
///
/// **Kontrak skrip.** Sebuah berkas `.lua` mengembalikan tabel yang boleh
/// memuat `OnStart(self)` dan `OnUpdate(self, dt)`. Setiap entity mendapat
/// salinan tabelnya sendiri, dengan `self.entity` berisi entity-nya dan
/// `self.state` sebagai tabel bebas untuk data yang bertahan antar frame —
/// dan yang dipertahankan saat hot reload.
///
/// **Kesalahan skrip tidak pernah menjatuhkan editor.** Setiap panggilan
/// dibungkus penangan yang mencatat pesan beserta traceback ke Console, lalu
/// menonaktifkan instance yang bersangkutan supaya ia tidak membanjiri log
/// enam puluh kali per detik dengan kesalahan yang sama.
class ScriptRuntime {
public:
    ScriptRuntime();
    ~ScriptRuntime();

    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;

    /// `graphCacheDir` adalah tempat hasil kompilasi `.simgraph` disimpan.
    /// Kosong berarti GraphComponent diabaikan — dipakai test yang hanya
    /// menguji skrip.
    bool Initialize(scene::World& world, assets::AssetDatabase* assets,
                    std::filesystem::path graphCacheDir = {});
    void Shutdown();

    /// Memuat seluruh skrip dan graph di world lalu memanggil OnStart.
    /// Dipanggil saat Play.
    void Start();

    /// Memanggil OnUpdate untuk setiap instance yang masih hidup.
    void Update(float deltaSeconds);

    /// Melepas seluruh instance. Scene-nya sendiri dikembalikan pemanggil.
    void Stop();

    bool IsRunning() const { return running_; }

    /// Memuat ulang satu skrip atau graph tanpa menghentikan permainan,
    /// mempertahankan tabel `state` tiap instance.
    void Reload(const Uuid& assetGuid);

    /// Cache hasil kompilasi graph. Panel Graph Editor memakainya untuk
    /// mengompilasi ulang saat graph disunting dan untuk membaca peta sumbernya.
    GraphCache& Graphs() { return graphs_; }

    /// Kesalahan runtime terakhir sebuah aset, atau null bila belum ada.
    const RuntimeFailure* LastFailure(const Uuid& assetGuid) const;

    /// Dipanggil ketika sebuah node ber-breakpoint dijalankan; argumennya GUID
    /// node itu dalam bentuk teks.
    ///
    /// Tanpa penangan, `sim.breakpoint` tidak melakukan apa pun. Itu yang
    /// membuat `.lua` hasil kompilasi tetap sah dijalankan runtime tanpa editor
    /// alih-alih gagal karena memanggil fungsi yang tidak ada.
    void SetBreakpointHandler(std::function<void(const std::string&)> handler) {
        onBreakpoint_ = std::move(handler);
    }

    /// Menjalankan potongan kode di state yang sama dengan skrip. Dipakai REPL.
    ///
    /// Kode dicoba dulu sebagai **ekspresi** (`return <kode>`), baru sebagai
    /// pernyataan. Tanpa itu, mengetik `sim` di konsol tidak menghasilkan apa
    /// pun — dan konsol yang tidak menjawab pertanyaan sederhana tidak dipakai
    /// orang.
    EvalResult Evaluate(std::string_view code);

    /// Nama yang berawalan `prefix`, untuk Tab di konsol. `prefix` boleh memuat
    /// titik: `sim.g` mencari di dalam tabel `sim`.
    std::vector<std::string> Complete(std::string_view prefix);

    /// Memeriksa sintaks tanpa menjalankan apa pun. String kosong berarti
    /// tidak ada kesalahan.
    ///
    /// Sengaja hanya memuat, tidak memanggil: berkas yang sedang diketik berisi
    /// kode setengah jadi, dan menjalankannya di setiap ketikan berarti efek
    /// samping yang tidak diminta siapa pun.
    std::string CheckSyntax(std::string_view code, std::string_view chunkName);

    /// Properti yang dideklarasikan sebuah berkas skrip, beserta nilai bawaannya.
    ///
    /// Berkasnya dijalankan untuk membacanya — sebuah deklarasi `properties`
    /// adalah tabel Lua biasa, dan tidak ada cara jujur membacanya tanpa
    /// menjalankan berkasnya. Karena itu berkas skrip tidak boleh punya efek
    /// samping di tingkat atas; yang berjalan saat Play adalah OnStart, bukan
    /// badan berkasnya.
    ///
    /// Kosong bila berkasnya tidak ada, gagal dimuat, atau tidak
    /// mendeklarasikan apa pun.
    ///
    /// Menerima GUID skrip maupun graph. Untuk graph, yang dibaca adalah hasil
    /// kompilasinya — di mana variabel yang diekspos sudah berbentuk deklarasi
    /// `properties` biasa, jadi tidak ada jalur kedua di sini.
    std::vector<scene::ScriptProperty> DeclaredProperties(const Uuid& scriptGuid);

    LuaVM& VM() { return *vm_; }

private:
    struct Instance;

    /// Memuat aset — skrip atau graph — untuk sebuah entity.
    void LoadFor(scene::Entity entity, const Uuid& guid);
    /// Menjalankan sebuah berkas Lua dan memasangnya sebagai instance.
    void LoadChunk(scene::Entity entity, const Uuid& guid, const std::filesystem::path& file,
                   const std::string& chunkName, bool fromGraph);
    /// Berkas Lua yang harus dijalankan untuk sebuah aset: berkasnya sendiri
    /// bila skrip, hasil kompilasi bila graph. Kosong bila tidak tersedia.
    std::filesystem::path ResolveChunk(const Uuid& guid, std::string& chunkName,
                                       bool& fromGraph);
    void RegisterBindings();
    /// Mencatat kegagalan sebuah instance, beserta nomor barisnya bila pesannya
    /// menyebutkannya.
    void RecordFailure(const Instance& instance, std::string_view message);

    std::unique_ptr<LuaVM> vm_;
    scene::World* world_ = nullptr;
    assets::AssetDatabase* assets_ = nullptr;
    GraphCache graphs_;
    std::function<void(const std::string&)> onBreakpoint_;
    /// Kesalahan runtime terakhir per aset. Dibersihkan setiap Play dimulai.
    std::unordered_map<Uuid, RuntimeFailure> failures_;
    std::vector<std::unique_ptr<Instance>> instances_;
    /// Detik sejak Play ditekan, dibaca skrip lewat `sim.time()`.
    float elapsed_ = 0.0f;
    bool running_ = false;
};

}  // namespace sim::script
