#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace sim {

/// Pengenal 128-bit untuk entity dan aset.
///
/// Dipakai sebagai referensi yang bertahan lintas-sesi: nama bisa berubah dan
/// path bisa dipindah, tapi GUID tidak. Aturan itu yang membuat rename aset di
/// E5 tidak memutus referensi level yang memakainya.
///
/// Formatnya UUID versi 4 (acak) dan ditulis dalam bentuk baku
/// 8-4-4-4-12 huruf kecil, supaya berkas level tetap terbaca manusia dan bisa
/// dibandingkan dengan alat biasa.
struct Uuid {
    uint64_t high = 0;
    uint64_t low = 0;

    constexpr bool IsValid() const { return high != 0 || low != 0; }

    friend constexpr bool operator==(const Uuid& a, const Uuid& b) {
        return a.high == b.high && a.low == b.low;
    }
    friend constexpr bool operator!=(const Uuid& a, const Uuid& b) { return !(a == b); }
    /// Urutan total, supaya Uuid bisa jadi kunci std::map dan hasil serialisasi
    /// bisa dibuat deterministik.
    friend constexpr bool operator<(const Uuid& a, const Uuid& b) {
        return a.high != b.high ? a.high < b.high : a.low < b.low;
    }

    /// Membuat GUID acak baru (UUID v4).
    static Uuid Generate();

    /// Mengurai bentuk teks. Mengembalikan Uuid tidak valid bila formatnya salah,
    /// bukan melempar — berkas level rusak harus menghasilkan pesan yang berguna,
    /// bukan menjatuhkan editor.
    static Uuid Parse(std::string_view text);

    std::string ToString() const;
};

}  // namespace sim

template <>
struct std::hash<sim::Uuid> {
    std::size_t operator()(const sim::Uuid& id) const noexcept {
        // Kedua bagian sudah acak seragam, jadi XOR dengan putaran cukup dan
        // jauh lebih murah daripada hash campuran.
        return static_cast<std::size_t>(id.high ^ (id.low + 0x9e3779b97f4a7c15ULL +
                                                   (id.high << 6) + (id.high >> 2)));
    }
};
