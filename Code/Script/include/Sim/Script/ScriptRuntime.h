#pragma once

#include "Sim/Core/Uuid.h"
#include "Sim/Scene/Components.h"
#include "Sim/Scene/World.h"

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

    bool Initialize(scene::World& world, assets::AssetDatabase* assets);
    void Shutdown();

    /// Memuat seluruh skrip di world dan memanggil OnStart. Dipanggil saat Play.
    void Start();

    /// Memanggil OnUpdate untuk setiap instance yang masih hidup.
    void Update(float deltaSeconds);

    /// Melepas seluruh instance. Scene-nya sendiri dikembalikan pemanggil.
    void Stop();

    bool IsRunning() const { return running_; }

    /// Memuat ulang satu skrip tanpa menghentikan permainan, mempertahankan
    /// tabel `state` tiap instance.
    void Reload(const Uuid& scriptGuid);

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
    std::vector<scene::ScriptProperty> DeclaredProperties(const Uuid& scriptGuid);

    LuaVM& VM() { return *vm_; }

private:
    struct Instance;

    void LoadFor(scene::Entity entity, const Uuid& guid);
    void RegisterBindings();

    std::unique_ptr<LuaVM> vm_;
    scene::World* world_ = nullptr;
    assets::AssetDatabase* assets_ = nullptr;
    std::vector<std::unique_ptr<Instance>> instances_;
    /// Detik sejak Play ditekan, dibaca skrip lewat `sim.time()`.
    float elapsed_ = 0.0f;
    bool running_ = false;
};

}  // namespace sim::script
