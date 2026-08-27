#pragma once

#include <cstdint>

namespace sim::scene {

/// Dari mana cahaya tak-langsung sebuah adegan datang.
///
/// **Maksud pengarang, bukan anggaran mesin.** "Adegan ini dirancang dengan GI
/// real-time" adalah keputusan yang harus bertahan saat levelnya dibuka di mesin
/// lain; resolusi probe dan jumlah kaskade tidak, dan karena itu keduanya tinggal
/// di project alih-alih di sini. Alasan lengkapnya keputusan 3 di
/// docs/PLAN-IBL.md.
enum class IndirectLighting : uint8_t {
    /// Tidak ada cahaya tak-langsung sama sekali. Untuk pengukuran dan untuk
    /// melihat albedo apa adanya.
    None,
    /// Dipanggang sekali dari lingkungannya: SH9 untuk difus, prefilter untuk
    /// spekular.
    Baked,
    /// Probe GI yang ditelusuri saat berjalan. Satu-satunya tingkat yang punya
    /// oklusi benar — dan satu-satunya yang membayarnya tiap frame.
    RealTime,
};

/// Apa yang menyinari tingkat panggang.
///
/// Tidak berlaku untuk `RealTime`, dan itu bukan kelalaian melainkan keputusan 1
/// di docs/PLAN-IBL.md: berkas HDR adalah satu foto yang **sudah** berisi
/// matahari, langit, dan pantulan — pengganti transport cahaya, bukan
/// masukannya. Menyuntikkannya ke sistem yang sudah punya transport menghasilkan
/// matahari yang terhitung dua kali.
enum class EnvironmentSource : uint8_t {
    /// Langit yang tergambar. Ikut bergerak saat Time-of-Day menggerakkan
    /// matahari.
    Sky,
    /// Berkas `.hdr`/`.exr` yang disebut `SkyComponent`. Skenario pra-GI.
    File,
};

/// Cerminan `render::ExposureMode`, di sisi scene.
///
/// **Disalin alih-alih dipakai bersama**, dengan alasan yang persis sama dengan
/// `SkySourceKind`: `Sim::Scene` tidak boleh melihat `Sim::Render` — itu batas
/// modul yang sama yang menjaga scene bisa dimuat tanpa satu pun perangkat
/// grafis. Pemetaannya terjadi sekali, di `Sim::SceneView`.
enum class ExposureModeKind : uint8_t {
    Automatic,
    Manual,
};

/// Bagaimana sebuah level disinari, dan bagaimana hasilnya dipetakan ke layar.
///
/// **Bukan komponen, dan tidak dipegang entity mana pun.** Ia milik levelnya
/// sendiri: sebuah adegan punya tepat satu tingkat pencahayaan, dan menaruhnya
/// di sebuah entity berarti pertanyaan "yang mana yang berlaku kalau ada dua"
/// harus dijawab di setiap tempat yang membacanya. Karena itu ia dipegang
/// `World` dan ditulis sebagai blok `"world"`, saudara `"entities"`, di dalam
/// berkas level.
///
/// **Didaftarkan ke `reflect::TypeRegistry` seperti komponen**, dan itu yang
/// membuatnya hampir gratis: PropertyGrid merender tipe terdaftar apa pun, dan
/// `SetWorldSettingsCommand` memberinya undo/redo. Yang **tidak** dilakukan
/// adalah mendaftarkannya ke `ComponentRegistry` — di sanalah daftar yang
/// menentukan apa yang boleh menempel di entity dan apa yang ikut ke dalam
/// `.simprefab`.
///
/// **Alat MCP karena itu belum melihatnya**, dan itu akibat langsung dari
/// pilihan di atas: `AiSceneTools` menelusuri `ComponentRegistry`, bukan
/// `TypeRegistry`. Sebuah agen belum bisa membaca atau mengubah tingkat
/// pencahayaan sebuah level. Yang dibutuhkan sepasang tool tersendiri —
/// `world.settings.get`/`set` — bukan pelonggaran aturan di atas: memasukkan
/// World Settings ke `ComponentRegistry` demi MCP akan membuatnya ikut ke dalam
/// setiap `.simprefab`, yaitu persis cacat yang keputusan 5 di
/// docs/PLAN-IBL.md cegah.
///
/// **`SkyComponent` tidak dipindahkan ke sini.** Ia tetap memegang langit yang
/// *tergambar* — sumber, berkas, rotasi, gain — dan keberadaannya tetap yang
/// menyalakan pass langit. Yang di sini adalah bagaimana adegan *disinari*. Satu
/// sumber untuk "langit yang mana", satu setelan untuk "menyinari bagaimana".
struct WorldSettings {
    IndirectLighting indirect = IndirectLighting::Baked;
    EnvironmentSource environment = EnvironmentSource::Sky;
    ExposureModeKind exposureMode = ExposureModeKind::Automatic;
    /// Kompensasi dalam stop, berlaku pada kedua mode. Positif berarti lebih
    /// terang.
    float exposureCompensation = 0.0f;
};

/// True bila kombinasinya tidak sah.
///
/// `RealTime` + `File` adalah satu-satunya yang tidak: probe GI menelusuri langit
/// yang tergambar, dan sebuah foto tidak bisa menjadi masukannya tanpa matahari
/// terhitung dua kali. Menyatakannya di sini, sebagai fungsi, supaya editor dan
/// jalur headless memeriksanya dengan aturan yang sama persis alih-alih
/// masing-masing menuliskan syaratnya sendiri.
///
/// Yang **memberitahu** pengguna adalah B6; yang di sini cuma jawabannya.
inline bool IsUnsupported(const WorldSettings& settings) {
    return settings.indirect == IndirectLighting::RealTime &&
           settings.environment == EnvironmentSource::File;
}

}  // namespace sim::scene
