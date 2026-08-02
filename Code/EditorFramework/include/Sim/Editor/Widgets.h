#pragma once

#include <imgui.h>

#include <array>
#include <string>

namespace sim::editor::widgets {

/// Jarak yang disisakan di tepi kanan panel properti.
///
/// Dipakai seragam oleh semua field selebar panel — kalau hanya sebagian yang
/// memakainya, tepi kanan panel jadi bergerigi.
inline constexpr float kPanelRightMargin = 8.0f;

/// Tiga kolom X/Y/Z dengan huruf sumbu berwarna.
///
/// Warna huruf bukan hiasan: di panel yang padat, warna adalah cara tercepat
/// mata menemukan sumbu yang dicari, jauh lebih cepat daripada membaca huruf.
///
/// Setiap kolom bisa diseret untuk mengubah nilai dan diklik untuk mengetik
/// angka pasti. Huruf sumbunya sendiri juga jadi target seret — melebarkan area
/// tanpa memakan ruang, yang terasa saat panel disempitkan.
///
/// `edited` diisi true selama nilai berubah; `finished` true tepat pada frame
/// pengguna melepas kendali. Pemanggil memakai `finished` untuk menutup
/// kelompok penggabungan undo, sehingga satu seretan = satu entri history.
struct Vec3Result {
    bool edited = false;
    bool finished = false;
};

Vec3Result DragVec3(const char* id, float values[3], float speed = 0.01f,
                    const char* format = "%.3f");

/// Kotak pencarian dengan ikon di kirinya, selebar sisa baris.
bool SearchField(const char* id, char* buffer, std::size_t bufferSize,
                 const char* hint = "Search...");

/// Tombol berisi ikon saja. Tooltip wajib: ikon yang tidak bisa ditebak dan
/// tidak punya keterangan lebih buruk daripada teks biasa.
bool IconButton(const char* icon, const char* tooltip, bool active = false);

/// Perbesaran tombol overlay viewport terhadap tombol ikon biasa.
inline constexpr float kViewportButtonScale = 1.45f;
inline constexpr float kViewportIconScale = 1.35f;

/// Tombol ikon untuk overlay di dalam viewport 3D.
///
/// Sama seperti IconButton tapi lebih besar, karena latarnya bukan panel polos
/// melainkan gambar scene.
bool ViewportButton(const char* icon, const char* tooltip, bool active = false);

/// Tombol toolbar (ikon + tooltip) yang otomatis menyisakan kursor di baris
/// yang sama untuk tombol berikutnya.
bool ToolbarButton(const char* icon, const char* tooltip, bool active = false);
void ToolbarSeparator();

/// Baris label kiri + isi kanan dengan lebar label seragam.
void PropertyLabel(const char* label, float labelWidth = 0.0f);

/// Header komponen yang bisa dilipat, dengan ikon.
bool ComponentHeader(const char* icon, const char* label, bool defaultOpen = true);

}  // namespace sim::editor::widgets
