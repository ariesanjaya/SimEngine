#pragma once

#include "Sim/Core/Uuid.h"
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
    std::string Evaluate(std::string_view code);

    LuaVM& VM() { return *vm_; }

private:
    struct Instance;

    void LoadFor(scene::Entity entity, const Uuid& guid);
    void RegisterBindings();

    std::unique_ptr<LuaVM> vm_;
    scene::World* world_ = nullptr;
    assets::AssetDatabase* assets_ = nullptr;
    std::vector<std::unique_ptr<Instance>> instances_;
    bool running_ = false;
};

}  // namespace sim::script
