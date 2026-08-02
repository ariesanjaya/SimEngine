#pragma once

namespace sim::imguix {

/// Tema gelap editor.
///
/// Palet dan bentuknya mengikuti tata letak acuan pada docs/EDITOR-PANELS.md:
/// abu-abu netral tanpa warna dominan, aksen biru hanya untuk seleksi dan
/// fokus, sudut nyaris tegak. Alasannya bukan selera — panel editor berisi
/// ribuan piksel konten berwarna (thumbnail, viewport, kurva), jadi UI-nya
/// harus mundur ke belakang, bukan bersaing.
void ApplyDarkTheme();

/// Skala ukuran (padding, rounding, ketebalan) untuk DPI tertentu.
///
/// Dipisah dari skala font karena ImGui 1.92 sudah menskalakan font sendiri
/// lewat io.ConfigDpiScaleFonts, sementara ukuran gaya belum. Memanggil
/// ScaleAllSizes berulang kali akan menumpuk skala, jadi fungsi ini selalu
/// mulai dari gaya dasar yang bersih.
void ApplyScale(float scale);

}  // namespace sim::imguix
