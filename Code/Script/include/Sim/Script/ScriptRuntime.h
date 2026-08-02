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

    std::unique_ptr<LuaVM> vm_;
    scene::World* world_ = nullptr;
    assets::AssetDatabase* assets_ = nullptr;
    GraphCache graphs_;
    std::vector<std::unique_ptr<Instance>> instances_;
    /// Detik sejak Play ditekan, dibaca skrip lewat `sim.time()`.
    float elapsed_ = 0.0f;
    bool running_ = false;
};

}  // namespace sim::script
