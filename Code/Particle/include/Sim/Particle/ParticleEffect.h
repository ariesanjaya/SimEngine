#pragma once

#include "Sim/Core/AssetRef.h"
#include "Sim/Core/Curve.h"
#include "Sim/Core/Math.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace sim::particle {

/// Versi skema berkas `.simfx` yang ditulis sekarang.
///
/// v2 membungkus modul di dalam daftar `emitters`. Berkas v1 — satu tumpukan
/// modul di akar — tetap terbaca, sebagai efek dengan satu emitter.
inline constexpr int kEffectSchemaVersion = 2;

/// Bentuk ruang tempat partikel lahir.
enum class EmitterShape {
    Point,
    Sphere,
    Box,
    Cone,
};

const char* ToString(EmitterShape shape);
EmitterShape ShapeFromString(std::string_view text);

/// Bagaimana partikel digambar.
enum class RenderMode {
    Billboard,
    /// Direntangkan searah kecepatan — untuk percikan dan hujan.
    Stretched,
    Mesh,
};

const char* ToString(RenderMode mode);
RenderMode RenderModeFromString(std::string_view text);

/// Jenis modul. Urutannya menentukan urutan penerapan saat simulasi, dan
/// karena itu tidak boleh diubah tanpa memikirkan efek yang sudah ada.
enum class ModuleKind {
    Spawn,
    Shape,
    Initial,
    OverLifetime,
    Force,
    Collision,
    Render,
};

const char* ToString(ModuleKind kind);

/// Yang dimiliki setiap modul, apa pun jenisnya.
///
/// **`enabled` tidak menghapus data.** Mematikan modul harus bisa dibatalkan
/// dengan menyalakannya kembali — penulis efek mematikan modul untuk melihat
/// apa sumbangannya, bukan untuk membuangnya. Ini kriteria terima E7.2 nomor 3,
/// dan bentuk penyimpanannya yang menjaminnya: setiap modul selalu ada di
/// berkas, lengkap, dengan satu bendera di sampingnya.
struct ModuleBase {
    bool enabled = true;
};

struct SpawnModule : ModuleBase {
    /// Partikel per detik.
    float rate = 20.0f;
    /// Ledakan sekali di awal, di luar `rate`.
    int burstCount = 0;
    float burstTime = 0.0f;
    /// Nol berarti emitter berjalan terus.
    float duration = 0.0f;
    bool looping = true;
};

struct ShapeModule : ModuleBase {
    EmitterShape shape = EmitterShape::Sphere;
    /// Jari-jari untuk Sphere dan Cone; setengah-ukuran untuk Box.
    Vec3 size{0.5f, 0.5f, 0.5f};
    /// Sudut bukaan kerucut, radian.
    float coneAngle = 0.436f;  // 25°
    /// Arah kecepatan awal mengikuti bentuknya, bukan lurus ke atas.
    bool emitFromShell = false;
};

struct InitialModule : ModuleBase {
    Vec3 velocity{0.0f, 2.0f, 0.0f};
    /// Sebaran acak di sekitar `velocity`, per sumbu.
    Vec3 velocityJitter{0.5f, 0.5f, 0.5f};
    float size = 0.2f;
    float sizeJitter = 0.05f;
    Vec3 color{1.0f, 1.0f, 1.0f};
    float rotation = 0.0f;
    float rotationJitter = 0.0f;
    float lifetime = 2.0f;
    float lifetimeJitter = 0.5f;
};

/// Perubahan sepanjang umur partikel. Inilah pemakai `Curve` dan `Gradient`.
struct OverLifetimeModule : ModuleBase {
    /// Pengali terhadap ukuran awal, bukan ukuran mutlak: efek yang ukurannya
    /// diacak tetap mempertahankan sebarannya sepanjang hidup.
    Curve sizeOverLife{1.0f};
    Curve velocityScale{1.0f};
    Curve rotationRate{0.0f};
    Gradient colorOverLife;
};

struct ForceModule : ModuleBase {
    Vec3 gravity{0.0f, -9.81f, 0.0f};
    /// Hambatan linear, per detik.
    float drag = 0.0f;
    Vec3 vortexAxis{0.0f, 1.0f, 0.0f};
    float vortexStrength = 0.0f;
    Vec3 attractorPosition{0.0f, 0.0f, 0.0f};
    float attractorStrength = 0.0f;
    float noiseStrength = 0.0f;
    float noiseFrequency = 1.0f;
};

struct CollisionModule : ModuleBase {
    /// Bidang tanah, satu-satunya penghalang sampai fisika datang di E9.
    float planeHeight = 0.0f;
    float bounce = 0.4f;
    float friction = 0.2f;
    bool killOnContact = false;
};

struct RenderModule : ModuleBase {
    RenderMode mode = RenderMode::Billboard;
    AssetRef material;
    AssetRef mesh;
    /// Panjang regangan untuk mode Stretched, dalam satuan kecepatan.
    float stretchScale = 1.0f;
    /// Partikel jauh digambar lebih dulu. Mematikannya lebih murah tapi
    /// membuat partikel tembus pandang bertumpuk terlihat salah.
    bool sortByDistance = true;
};

/// Satu emitter: satu tumpukan modul yang melahirkan dan menggerakkan
/// partikelnya sendiri.
///
/// Modulnya field tetap, bukan daftar polimorfik. Sebuah emitter selalu punya
/// ketujuhnya; yang berbeda hanya mana yang menyala. Daftar polimorfik akan
/// menambah pertanyaan "bagaimana kalau ada dua modul Force" yang tidak ada
/// jawabannya, dan membuat berkasnya berubah urutan setiap kali disunting.
///
/// **Keragaman datang dari menggabungkan emitter, bukan dari menumpuk modul
/// sejenis.** Api yang sungguhan adalah nyala inti, percikan yang melompat,
/// asap yang naik pelan, dan bara yang jatuh — empat perilaku dengan bentuk,
/// umur, dan gaya yang berbeda-beda. Memaksa keempatnya ke dalam satu tumpukan
/// modul berarti setiap parameter harus bisa bercabang, dan tidak ada satu pun
/// yang bisa disetel tanpa mengganggu tiga yang lain.
struct ParticleEmitter {
    std::string name = "Emitter";
    /// Emitter yang dimatikan tidak melahirkan apa pun, tapi datanya utuh —
    /// aturan yang sama dengan modul.
    bool enabled = true;
    /// Benih RNG. Emitter dengan benih sama menghasilkan sebaran yang sama
    /// persis di mesin mana pun — lihat ParticleSystem.
    ///
    /// Milik emitter, bukan diturunkan dari urutannya di daftar. Kalau
    /// diturunkan dari urutan, menyusun ulang emitter akan mengubah efek yang
    /// sudah jadi — perubahan yang tidak diminta siapa pun.
    uint32_t seed = 12345;
    /// Batas atas jumlah partikel hidup emitter ini. Simulasi berhenti
    /// melahirkan setelah ini, bukan tumbuh tanpa batas sampai editor membeku.
    int maxParticles = 10000;

    SpawnModule spawn;
    ShapeModule shape;
    InitialModule initial;
    OverLifetimeModule overLifetime;
    ForceModule force;
    CollisionModule collision;
    RenderModule render;

    /// Modul menurut jenisnya, untuk panel yang menggambar daftarnya seragam.
    bool IsEnabled(ModuleKind kind) const;
    void SetEnabled(ModuleKind kind, bool enabled);
};

/// Satu efek partikel — isi berkas `.simfx`.
///
/// Sebuah efek adalah **beberapa emitter yang berjalan bersama** pada satu
/// timeline. Itu bentuk yang dipakai editor partikel mapan mana pun, dan
/// alasannya ada di catatan `ParticleEmitter` di atas.
struct ParticleEffect {
    std::string name;
    std::vector<ParticleEmitter> emitters;

    /// Benih bawaan untuk emitter berikutnya, dibuat berbeda dari yang sudah
    /// ada. Dua emitter berbenih sama menghasilkan partikel yang bertumpuk
    /// tepat, dan yang terlihat adalah satu emitter yang kelihatan lebih terang
    /// — bukan dua.
    uint32_t NextSeed() const;
};

struct EffectIoResult {
    bool ok = false;
    std::string error;
    int sourceVersion = kEffectSchemaVersion;
};

/// Keluarannya deterministik — urutan field tetap, angka ditulis tanpa
/// bergantung locale — supaya dua kali menyimpan efek yang sama menghasilkan
/// byte yang sama dan berkasnya ramah diff.
std::string SaveEffectToString(const ParticleEffect& effect);
EffectIoResult SaveEffectToFile(const ParticleEffect& effect,
                                const std::filesystem::path& path);

EffectIoResult LoadEffectFromString(ParticleEffect& effect, const std::string& text);
EffectIoResult LoadEffectFromFile(ParticleEffect& effect, const std::filesystem::path& path);

}  // namespace sim::particle
