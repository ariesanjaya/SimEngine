#pragma once

#include <cstdint>

/// Backend penelusuran yang menjawab sebuah query.
///
/// **Satu keputusan yang menentukan bentuk seluruh modul: backend adalah plugin,
/// dan yang memanggil `Sim::Raycast` tidak boleh tahu siapa yang menelusuri
/// BVH-nya.** Aturan yang sama dengan `render::TraceBackendKind` untuk GI, dan
/// alasannya juga sama — begitu satu pemanggil bercabang menurut backend,
/// menukar backend berhenti menjadi perubahan lokal.
///
/// Konsekuensi paling penting dari aturan itu bukan enum ini melainkan yang
/// **tidak** ada di header mana pun modul ini: satu pun tipe Embree. Begitu
/// `RTCScene` muncul di header publik, setiap pemanggil ikut menyertakan
/// `embree4/rtcore.h`, dan R6 berhenti bisa dibatalkan.
namespace sim::raycast {

enum class BackendKind : uint8_t {
    /// BVH biner ber-SAH milik sendiri. Tanpa dependensi, dan satu-satunya yang
    /// ada di R0.
    Bvh,
    /// Intel Embree. Lihat R6 di `docs/PLAN-EMBREE.md` — ia masuk hanya kalau
    /// profil path tracer referensi menuntutnya, dan sampai itu terjadi nilai
    /// ini tidak pernah dikembalikan.
    Embree,
};

const char* ToString(BackendKind kind);

/// Backend yang dipakai build ini.
///
/// Sebuah fungsi, bukan konstanta: begitu R6 ada, yang menentukan bukan lagi
/// bendera kompilasi semata melainkan juga apa yang berhasil diinisialisasi saat
/// jalan — dan pemanggil yang menuliskannya sebagai `if constexpr` akan
/// menghalangi perubahan itu.
BackendKind SelectedBackend();

}  // namespace sim::raycast
