#pragma once

#include "Sim/Material/MaterialCompiler.h"
#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialInstance.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sim::material {

/// Letak sebuah parameter di dalam blok uniform.
struct ParameterSlot {
    std::string name;
    ValueKind kind = ValueKind::Float;
    /// Offset byte dari awal blok.
    uint32_t offset = 0;
    /// Byte yang benar-benar dipakai nilainya — belum termasuk sisipan sesudahnya.
    uint32_t size = 0;
};

/// Tata letak blok uniform sebuah material, beserta tabel indeks teksturnya.
///
/// **Ini ABI antara kode Slang yang dihasilkan dan sisi C++.** Kedua sisi harus
/// menghitung offset yang sama persis dari daftar parameter yang sama persis;
/// selisih satu sisipan tidak menghasilkan galat apa pun, hanya material yang
/// warnanya salah dengan cara yang sulit dilacak. Karena itu tata letaknya
/// dihitung di satu tempat ini, dan diuji terhadap contoh yang sisipannya sudah
/// dihitung tangan.
///
/// **Aturannya std140, dan `float3` di sana punya dua angka yang berbeda:
/// penjajaran 16 tapi ukuran 12.** Keduanya harus dipegang sekaligus, dan salah
/// satunya saja menghasilkan tata letak yang meleset ke arah yang berlawanan:
///
///  - Lupa bahwa *penjajarannya* 16 → sebuah `float3` yang menyusul sebuah
///    `float` akan ditaruh di offset 4, padahal ia harus didorong ke 16.
///  - Mengira ia juga *berukuran* 16 → sebuah `float` yang menyusul `float3`
///    akan didorong ke 16 byte berikutnya, padahal ia justru mengisi celah di
///    offset +12.
///
/// Yang kedua sempat saya tulis terbalik di sini, dan test yang menemaninya
/// ikut salah karena ditulis dari keyakinan yang sama.
///
/// **Seluruh parameter yang dideklarasikan masuk, bukan hanya yang dipakai
/// graph.** Kompiler menulis cbuffer dari daftar deklarasi apa adanya (lihat
/// catatan di `MaterialCompiler`), jadi menyaring yang tidak terpakai di sini
/// akan membuat kedua sisi berbeda tepat pada material yang sedang disunting.
class MaterialParameterBlock {
public:
    /// Menyusun tata letak dari daftar parameter, mengikuti urutan deklarasinya.
    ///
    /// Urutan deklarasi, bukan urutan yang dioptimasi. Menyusun ulang menurut
    /// ukuran memang menghemat sisipan, tapi ia membuat tata letak berubah saat
    /// sebuah parameter diganti tipenya — dan setiap material instance yang
    /// sudah tersimpan menunjuk offset yang lama.
    void Build(const std::vector<MaterialParameter>& parameters);
    void Clear();

    int SlotCount() const { return static_cast<int>(slots_.size()); }
    const ParameterSlot& Slot(int index) const;
    const std::vector<ParameterSlot>& Slots() const { return slots_; }
    int Find(std::string_view name) const;

    /// Ukuran blok, sudah dibulatkan ke kelipatan 16.
    uint32_t Bytes() const { return bytes_; }

    /// Mengisi blok dengan nilai bawaan tiap parameter, lalu menimpanya dengan
    /// override milik sebuah instance.
    ///
    /// Override yang menunjuk parameter yang sudah tidak ada **diabaikan diam-
    /// diam**. Itu memang yang diinginkan: menghapus sebuah parameter dari
    /// material induk tidak boleh membuat setiap instance-nya gagal dimuat, dan
    /// override yang menganggur tetap tersimpan di berkasnya kalau-kalau
    /// parameternya dikembalikan.
    void Fill(const std::vector<MaterialParameter>& parameters,
              const std::vector<ParameterOverride>& overrides, std::vector<uint8_t>& out) const;

    /// Menulis satu nilai ke dalam blok yang sudah terisi. Dipakai panel
    /// preview, yang mengubah satu slider tanpa menyusun ulang seluruhnya.
    bool Write(std::vector<uint8_t>& block, std::string_view name,
               const MaterialValue& value) const;

private:
    std::vector<ParameterSlot> slots_;
    uint32_t bytes_ = 0;
};

/// Byte yang dipakai sebuah tipe, dan penjajarannya menurut std140.
uint32_t ValueSize(ValueKind kind);
uint32_t ValueAlignment(ValueKind kind);

/// Slot tekstur sebuah material.
///
/// **Indeks, bukan handle.** Renderer mengikat seluruh tekstur sekali sebagai
/// satu larik bindless dan material hanya menyimpan nomornya — itulah yang
/// membuat mengganti material tidak menuntut mengikat ulang descriptor set.
/// Indeksnya diberikan renderer; yang ada di sini hanyalah urutan slotnya, yang
/// harus sama dengan urutan binding yang ditulis kompiler.
struct TextureSlot {
    std::string binding;
    Uuid texture;
};

/// Menyusun tabel slot tekstur dari hasil kompilasi.
///
/// Instance **tidak** menimpa tekstur, dan itu bukan kelalaian: `.simmatinst`
/// yang sudah dipakai hanya menyimpan timpaan parameter, dan menambahkan
/// timpaan tekstur berarti mengubah skema berkas yang sudah tersimpan di proyek
/// orang. Kalau nanti benar-benar dibutuhkan, ia masuk sebagai versi skema baru
/// — bukan diselundupkan lewat sini.
std::vector<TextureSlot> BuildTextureSlots(const std::vector<TextureBinding>& bindings);

}  // namespace sim::material
