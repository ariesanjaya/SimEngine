#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sim::platform {

/// Satu monitor sebagaimana dilaporkan SDL.
struct DisplayInfo {
    uint32_t id = 0;
    std::string name;
    IVec2 position{0, 0};   ///< posisi dalam koordinat desktop virtual
    IVec2 size{0, 0};       ///< ukuran dalam koordinat logis (bukan piksel)
    float contentScale = 1.0f;  ///< 1.0 = 96 dpi, 2.0 = layar HiDPI
    float refreshRate = 0.0f;
    bool isPrimary = false;
};

/// Enumerasi monitor. Dipanggil ulang setiap kali SDL mengirim event
/// SDL_EVENT_DISPLAY_ADDED/REMOVED — konfigurasi monitor bisa berubah saat
/// editor berjalan (kabel dicabut, dock dilepas), dan layout tersimpan yang
/// menunjuk monitor yang hilang harus ditarik kembali ke monitor utama.
std::vector<DisplayInfo> EnumerateDisplays();

/// Monitor yang memuat titik tersebut, atau nullopt kalau titiknya di luar
/// semua monitor (bisa terjadi setelah konfigurasi monitor berubah).
const DisplayInfo* DisplayContaining(const std::vector<DisplayInfo>& displays, IVec2 point);

/// Skala konten monitor utama. Dipakai untuk menentukan skala UI awal sebelum
/// jendela dibuat.
float PrimaryDisplayScale();

/// Refresh rate terendah di antara semua monitor yang terpasang.
///
/// Inilah laju yang dipakai untuk mengunci frame editor. Alasannya: dengan
/// multi-viewport, sebuah panel bisa berada di monitor mana pun, dan panel
/// tersebut ikut digambar pada frame yang sama dengan jendela utama. Kalau
/// editor berjalan pada laju monitor tercepat, panel di monitor yang lebih
/// lambat akan menerima frame yang tidak pernah sempat ditampilkan — terlihat
/// sebagai patah-patah dan sobek (tearing), bukan lebih mulus. Mengunci ke laju
/// terendah membuat semua monitor menerima laju yang sama dan konsisten.
///
/// Contoh: 60 Hz + 100 Hz -> 60. 100 Hz + 144 Hz -> 100.
///
/// Mengembalikan `fallback` bila tidak ada monitor yang melaporkan refresh rate
/// (bisa terjadi di sesi remote atau driver tertentu).
float LowestRefreshRate(float fallback = 60.0f);

}  // namespace sim::platform
