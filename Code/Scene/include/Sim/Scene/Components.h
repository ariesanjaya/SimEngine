#pragma once

#include "Sim/Core/AssetRef.h"
#include "Sim/Core/Math.h"
#include "Sim/Core/Uuid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sim::scene {

/// Pengenal entity di dalam satu sesi.
///
/// Bukan yang tertulis di berkas level — itu tugas IdComponent::guid. Indeks
/// ini dipakai ulang setelah entity dihapus, jadi menyimpannya ke disk akan
/// membuat referensi menunjuk objek yang salah setelah level dimuat ulang.
enum class Entity : uint32_t { Null = 0xFFFFFFFFu };

inline constexpr Entity kNullEntity = Entity::Null;
inline constexpr bool IsValid(Entity entity) {
    return entity != kNullEntity;
}

// --- komponen inti: selalu ada di setiap entity -----------------------------

/// Pengenal yang bertahan lintas-sesi. Inilah yang dirujuk berkas level,
/// prefab, dan (nanti) tool MCP.
struct IdComponent {
    Uuid guid;
};

struct NameComponent {
    std::string name;
};

/// Hubungan induk-anak. Tidak dipantulkan: strukturnya diserialisasi lewat
/// GUID induk pada tiap entity, bukan sebagai field biasa.
struct HierarchyComponent {
    Entity parent = kNullEntity;
    std::vector<Entity> children;
};

struct TransformComponent {
    Vec3 position{0.0f, 0.0f, 0.0f};
    /// Disimpan sebagai quaternion, bukan sudut Euler: Euler punya gimbal lock
    /// dan urutan putarannya ambigu. Inspector tetap menampilkan derajat.
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    Mat4 LocalMatrix() const;
};

/// Cache matriks dunia. Bukan komponen yang dipantulkan maupun diserialisasi —
/// seluruh isinya bisa dihitung ulang dari TransformComponent dan hierarki.
struct TransformCache {
    Mat4 world{1.0f};
    bool dirty = true;
};

// --- komponen opsional ------------------------------------------------------

struct VisibilityComponent {
    bool visible = true;
    /// Terkunci: masih terlihat, tapi tidak bisa dipilih di viewport.
    bool locked = false;
};

struct StaticFlagComponent {
    bool isStatic = false;
};

struct MeshRendererComponent {
    /// Rujukan berisi GUID, bukan nama berkas. Itulah yang membuat mengganti
    /// nama atau memindahkan mesh tidak menyentuh satu pun berkas level.
    AssetRef mesh;
    AssetRef material;
    bool castShadows = true;
    bool receiveShadows = true;
    /// Menggeser pemilihan tingkat detail: negatif memilih mesh yang lebih
    /// rinci lebih lama, positif lebih cepat turun. Dipakai renderer di E8.
    float lodBias = 0.0f;
};

enum class LightType : uint8_t {
    Directional = 0,
    Point = 1,
    Spot = 2,
};

/// Menautkan entity ke sebuah skrip Lua.
///
/// Rujukannya AssetRef, sama seperti mesh dan material: mengganti nama berkas
/// skrip tidak boleh memutus entity mana pun.
enum class ScriptPropertyKind : uint8_t {
    Number = 0,
    Bool = 1,
    Text = 2,
};

/// Satu properti yang diekspos sebuah skrip ke Inspector.
///
/// Ketiga nilainya disimpan berdampingan alih-alih dalam union: komponen ini
/// diserialisasi lewat reflection, dan reflection tidak punya cara menyatakan
/// "field mana yang sah tergantung field lain". Biayanya beberapa byte per
/// properti; imbalannya adalah properti yang tipenya berubah di skrip tidak
/// kehilangan nilai lamanya begitu saja ketika pengguna mengembalikannya.
struct ScriptProperty {
    std::string name;
    ScriptPropertyKind kind = ScriptPropertyKind::Number;
    float number = 0.0f;
    bool flag = false;
    std::string text;
};

struct ScriptComponent {
    AssetRef script;
    /// True saat skripnya sudah dimuat dan siap dipanggil. Diisi runtime, bukan
    /// pengguna — karena itu ditandai ReadOnly di Inspector.
    bool loaded = false;

    /// Nilai properti yang diekspos skrip. Yang tersimpan di sini adalah nilai
    /// untuk entity INI; bawaannya tetap tinggal di berkas skrip. Karena itu
    /// mengubah bawaan di skrip tidak menyentuh entity yang sudah punya nilai
    /// sendiri, dan entity yang belum pernah disunting ikut berubah.
    ///
    /// Digambar Inspector secara khusus, bukan lewat grid generik — daftar
    /// struct yang bisa ditambah-kurangi bukan yang dibutuhkan di sini.
    std::vector<ScriptProperty> properties;
};

struct LightComponent {
    LightType type = LightType::Point;
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    /// Meter. Tidak berarti untuk lampu directional.
    float range = 10.0f;
    float innerAngleRadians = 0.436f;  // 25°
    float outerAngleRadians = 0.611f;  // 35°
    bool castShadows = true;
};

struct CameraComponent {
    float fovYRadians = 1.047f;  // 60°
    float nearZ = 0.05f;
    float farZ = 2000.0f;
    bool orthographic = false;
    float orthoHeight = 10.0f;
};

/// Mendaftarkan seluruh tipe komponen ke TypeRegistry dan ComponentRegistry.
///
/// Dipanggil eksplisit, bukan lewat inisialisasi statik. Panel bisa memakai
/// pendaftaran statik karena urutannya tidak penting; komponen tidak — urutan
/// pendaftaran menentukan urutan komponen di berkas level, dan urutan
/// inisialisasi statik antar-TU tidak ditentukan bahasa. Berkas level harus
/// byte-per-byte sama setiap kali disimpan.
void RegisterCoreComponents();

}  // namespace sim::scene
