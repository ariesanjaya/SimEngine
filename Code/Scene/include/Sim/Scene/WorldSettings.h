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
    /// Seluruhnya dipanggang: lingkungan, transport cahaya, dan oklusinya.
    ///
    /// **Bernama `Baked` sampai S0, dan namanya berubah karena artinya
    /// berubah.** Yang lama berarti "lingkungannya dipanggang" — sembilan angka
    /// untuk seluruh level, tanpa oklusi dan tanpa pantulan. Yang sekarang
    /// berarti kategori: adegan yang mataharinya diam, sehingga transport
    /// cahayanya bisa ikut dipanggang. Rinciannya di docs/PLAN-STATIC-GI.md.
    ///
    /// Berkas yang menulis `"Baked"` dinaikkan migrasi skema 4 → 5.
    Precomputed,
    /// Probe GI yang ditelusuri saat berjalan. Untuk adegan yang mataharinya
    /// bergerak — dan yang membayarnya tiap frame.
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
    IndirectLighting indirect = IndirectLighting::Precomputed;
    EnvironmentSource environment = EnvironmentSource::Sky;
    ExposureModeKind exposureMode = ExposureModeKind::Automatic;
    /// Kompensasi dalam stop, berlaku pada kedua mode. Positif berarti lebih
    /// terang.
    float exposureCompensation = 0.0f;

    /// Keluarkan matahari dari berkas lingkungan sebelum ia dipanggang (B4).
    ///
    /// **Berkas HDR sudah berisi mataharinya.** Kalau level juga punya lampu
    /// directional, ada dua — dan tidak ada satu pun galat yang menyebutkannya,
    /// hanya bayangan yang dua kali lebih tegas daripada yang dimaksudkan
    /// pengarangnya. Menyalakan ini memisahkan keduanya: petanya kehilangan
    /// mataharinya, dan lampunya yang mengantarkannya.
    ///
    /// **Mati secara bawaan, dan itu disengaja.** Menyalakannya tanpa mengisi
    /// lampu directional dengan hasil ekstraksinya menghasilkan adegan yang
    /// kehilangan mataharinya sama sekali — jadi yang menyalakannya sebaiknya
    /// tombol di World Settings yang mengerjakan keduanya sekaligus, bukan
    /// sebuah centang yang berdiri sendiri.
    ///
    /// Hanya berlaku untuk `environment: File`; langit prosedural tidak pernah
    /// memanggang cakram mataharinya sejak B1.
    bool extractSun = false;

    /// Jarak antar-probe iradiansi, meter (S1 di docs/PLAN-STATIC-GI.md).
    ///
    /// **Ada di level, bukan di project, dan itu keputusan 8.** Sebuah koridor
    /// sempit dan sebuah lembah terbuka menuntut kerapatan yang berbeda, dan
    /// keduanya bisa berada di project yang sama — menaruh angkanya di project
    /// berarti yang satu boros dan yang lain bocor. Ia maksud pengarang atas
    /// adegan ini, sejenis dengan `indirect`, bukan anggaran perangkat.
    ///
    /// **Yang dibayar tumbuh kubik**, dan itu sebabnya panel menampilkan jumlah
    /// probe serta ukuran artefaknya sebelum ada yang menekan Bake: memotong
    /// jaraknya jadi separuh melipatgandakan biayanya delapan kali, dan itu
    /// angka yang harus terlihat alih-alih ditemukan setelah menunggu.
    ///
    /// Hanya berlaku untuk `Precomputed`. Nol atau negatif tidak sah; yang
    /// membacanya menjepitnya lewat `ProbeSpacingOf`.
    float probeSpacing = 2.0f;

    /// Kerapatan lightmap permukaan statis, texel per meter (S5).
    ///
    /// **Per meter, bukan per meter persegi**, dan itu bukan kerapian
    /// penamaan: yang kedua membuat menggandakan ukuran sebuah benda
    /// melipatkan kerapatannya empat kali, dan anggaran atlasnya meledak pada
    /// benda besar.
    ///
    /// **Ada di level, bukan di project** — alasan yang sama dengan
    /// `probeSpacing`: sebuah interior yang kontaknya harus tajam dan sebuah
    /// lanskap luas menuntut kerapatan yang berbeda, dan keduanya bisa berada di
    /// project yang sama. Objek yang menuntut lebih halus daripada levelnya
    /// menimpanya lewat `MeshRendererComponent::lightmapTexelDensity`.
    float lightmapTexelsPerMeter = 4.0f;
};

/// Kerapatan lightmap yang benar-benar dipakai, dengan yang tidak masuk akal
/// dijepit. Alasan penjepitannya sama dengan `ProbeSpacingOf`.
inline float LightmapDensityOf(const WorldSettings& settings) {
    if (!(settings.lightmapTexelsPerMeter > 0.0f)) {  // termasuk NaN
        return 4.0f;
    }
    return settings.lightmapTexelsPerMeter < 0.25f
               ? 0.25f
               : (settings.lightmapTexelsPerMeter > 64.0f ? 64.0f
                                                          : settings.lightmapTexelsPerMeter);
}

/// Jarak antar-probe yang benar-benar dipakai, dengan yang tidak masuk akal
/// dijepit.
///
/// **Dijepit, bukan ditolak.** Nol datang dari berkas level yang ditulis skema
/// lama dan dari medan yang dikosongkan pengguna saat mengetik; keduanya
/// menghasilkan kisi tak-hingga kalau dipercaya apa adanya. Batas atasnya ada
/// karena jarak sebesar adegan menghasilkan kisi 2×2×2 yang tidak bisa
/// menjelaskan apa pun.
inline float ProbeSpacingOf(const WorldSettings& settings) {
    if (!(settings.probeSpacing > 0.0f)) {  // termasuk NaN
        return 2.0f;
    }
    return settings.probeSpacing < 0.1f ? 0.1f : (settings.probeSpacing > 32.0f ? 32.0f : settings.probeSpacing);
}

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

/// True bila tingkat `RealTime` diminta tetapi probe tidak punya langit yang
/// bisa dicuplik (B6).
///
/// **Ini cacat yang sesungguhnya, dan ia berbeda dari yang di atas.** Yang di
/// atas soal maksud yang bertentangan — "disinari berkas" sambil "disinari
/// probe". Yang ini soal apa yang benar-benar terjadi saat digambar: probe GI
/// mencuplik LUT sky-view atmosferik, dan sebuah langit HDRI tidak punya LUT
/// itu. Sampai B6, yang dicuplik probe di keadaan itu adalah gradien analitik
/// yang tidak ada hubungannya dengan langit yang tergambar — adegan disinari
/// langit yang bukan langitnya, tanpa satu pun galat.
///
/// Sekarang yang dicuplik nol, dan adegan yang tiba-tiba gelap adalah pertanyaan
/// yang diajukan alih-alih kesalahan yang disembunyikan. `proceduralSky` benar
/// hanya bila adegan punya `SkyComponent` bersumber `Atmosphere` yang menyala.
inline bool RealTimeHasNoSky(const WorldSettings& settings, bool proceduralSky) {
    return settings.indirect == IndirectLighting::RealTime && !proceduralSky;
}

/// True bila Time-of-Day menggerakkan matahari di adegan yang transportnya
/// dipanggang (S0).
///
/// **Precomputed berdiri di atas satu andaian: mataharinya diam.** Itu bukan
/// pembatasan yang dipilih melainkan syarat yang membuat transport bisa
/// dipanggang sama sekali. Menggerakkannya berarti pantulan dan oklusi yang
/// datang dari matahari di tempat lain — dan tidak ada satu pun galat yang
/// menyebutkannya, hanya bayangan langsung yang bergerak sementara bayangan
/// tak-langsungnya diam.
///
/// Dinyatakan, bukan didiamkan; pola yang sama dengan `RealTimeHasNoSky` dan
/// dengan B6.
inline bool PrecomputedFightsTimeOfDay(const WorldSettings& settings, bool timeOfDayDrivesSun) {
    return settings.indirect == IndirectLighting::Precomputed && timeOfDayDrivesSun;
}

}  // namespace sim::scene
