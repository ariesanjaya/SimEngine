#pragma once

#include "Sim/ImageIO/Image.h"

#include <string_view>

/// Aturan ruang warna dan alfa untuk tekstur.
///
/// **Bagian yang paling halus dan paling sering salah**, karena kesalahannya
/// tidak pernah muncul sebagai galat. Normal map yang melewati dekode sRGB
/// menghasilkan pencahayaan yang salah **sedikit** di seluruh permukaan — tidak
/// ada peringatan, tidak ada piksel yang jelas keliru, hanya bayangan yang
/// bentuknya agak lain. Albedo yang tidak didekode menghasilkan warna yang
/// terlalu terang, yang biasanya "diperbaiki" dengan menurunkan lampu, dan
/// setelah itu seluruh adegan salah bersama-sama.
namespace sim::imageio {

/// Untuk apa piksel sebuah tekstur dipakai — dan karena itu, **apakah ia warna
/// atau angka**.
enum class TextureUsage : uint8_t {
    /// Warna yang dilihat mata: base color, emissive. Berkas 8-bit yang berisi
    /// warna hampir selalu disandikan sRGB, karena itulah cara menyimpan warna
    /// di delapan bit tanpa membuang ketelitian di bagian gelap.
    Color,
    /// Angka yang kebetulan disimpan sebagai gambar: normal, roughness,
    /// metalness, height, occlusion, mask, opacity. **Tidak pernah disandikan**
    /// — nilainya sudah berarti apa adanya.
    Data,
};

/// Slot material → kegunaannya.
///
/// **Inilah aturan defaultnya, dan ia eksplisit dengan sengaja.** Nama slotnya
/// datang dari model material (`baseColor`, `normal`, `roughness`, …), dan
/// tabel ini tinggal di sini — bukan di `Sim::Material` — karena setiap modul
/// yang memuat tekstur membutuhkannya, dan modul ini ada di bawah semuanya.
///
/// **Slot yang tidak dikenal dianggap `Data`, bukan `Color`.** Arahnya dipilih
/// menurut kesalahan mana yang lebih mudah ditemukan: slot warna baru yang
/// terlupa didaftarkan menghasilkan tekstur yang terlalu terang — terlihat
/// seketika. Slot data baru yang terlupa akan didekode sRGB, dan itu tidak
/// terlihat sama sekali sampai seseorang membandingkan render dengan acuan.
TextureUsage UsageForSlot(std::string_view slot);

/// Ruang warna yang dituntut kegunaan ini.
ColorSpace ExpectedColorSpace(TextureUsage usage);

/// Apakah unggahan GPU untuk gambar ini harus memakai format `_SRGB`.
///
/// **Untuk tekstur warna 8-bit, inilah jalur yang benar** — bukan dekode di
/// CPU. Perangkat kerasnya mendekode saat menyampel, gratis dan dengan
/// ketelitian penuh, sementara mendekode ke delapan bit di CPU menghancurkan
/// bagian gelapnya: nilai sRGB 0..15 seluruhnya jatuh ke linear 0 atau 1.
///
/// Float dan 16-bit tidak pernah memakai format sRGB — keduanya punya cukup
/// ketelitian untuk menyimpan linear apa adanya, dan berkas float memang sudah
/// linear.
bool NeedsSrgbGpuFormat(TextureUsage usage, const ImageDesc& desc);

/// Mendekode piksel menjadi linear di CPU, bila kegunaannya menuntut itu.
///
/// Dipakai konsumen CPU — baker, path tracer acuan, pembanding gambar — yang
/// tidak punya perangkat keras untuk mendekodekannya. Jalur GPU memakai
/// `NeedsSrgbGpuFormat` dan tidak memanggil ini.
///
/// **Gambar 8-bit dinaikkan ke float.** Mendekode sRGB di tempat, tetap di
/// delapan bit, membuang justru bagian yang sandi sRGB ada untuk melindunginya.
///
/// Alfa tidak pernah ikut didekode: ia cakupan, bukan warna.
///
/// No-op untuk `Data`, dan untuk gambar yang berkasnya sudah menyatakan diri
/// linear. Bila berkas menyatakan sRGB sementara slotnya `Data`, konflik itu
/// **dicatat** dan datanya dibiarkan apa adanya — slot yang menentukan.
void ToLinear(TextureUsage usage, Image& image);

/// Mengubah alfa premultiplied menjadi *straight* (unassociated).
///
/// **Straight adalah konvensi tunggal di mesin ini.** Ia yang dihasilkan alat
/// gambar dan yang diharapkan artis; premultiply adalah keputusan sisi render
/// yang bergantung pada apakah ia dilakukan sebelum atau sesudah dekode sRGB,
/// jadi ia tidak boleh sudah terlanjur dilakukan di dalam berkas.
///
/// **Urutannya penting**: ini dijalankan **sebelum** dekode sRGB, karena berkas
/// yang premultiplied melakukannya pada nilai yang tersimpan — yaitu pada nilai
/// yang masih tersandikan. Membalik urutannya menghasilkan tepi yang salah
/// terang pada setiap tekstur beralfa. `PrepareTexture` di bawah menegakkan
/// urutan itu supaya tidak ada pemanggil yang perlu mengingatnya.
///
/// Piksel yang alfanya nol menjadi hitam: warnanya memang tidak bisa dipulihkan
/// dari nol dikali apa pun, dan piksel yang sepenuhnya tembus pandang tidak
/// punya warna yang berarti.
void ToStraightAlpha(Image& image);

/// Menyiapkan tekstur untuk konsumen CPU: alfa diluruskan, lalu didekode.
///
/// Satu pintu masuk, karena urutan kedua langkah itu persis hal yang salahnya
/// tidak kelihatan.
void PrepareTexture(TextureUsage usage, Image& image);

}  // namespace sim::imageio
