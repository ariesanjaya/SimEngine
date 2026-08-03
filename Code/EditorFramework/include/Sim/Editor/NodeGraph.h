#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sim::editor {

/// Kanvas node: pan, zoom, node, pin, dan koneksi.
///
/// Membungkus imgui-node-editor sepenuhnya — tidak ada tipe pustaka itu yang
/// bocor ke header ini. Alasannya sama dengan Gizmo membungkus ImGuizmo: kalau
/// suatu saat harus diganti, yang berubah satu berkas, bukan setiap panel yang
/// memakainya. Dan ada dua yang akan memakainya (visual scripting di E6.5,
/// graph material di E7.1), jadi antarmuka bersama ini bukan spekulasi.
///
/// **Identitas.** Semua id adalah `uint64_t` milik pemanggil dan harus BUKAN
/// nol — pustaka di baliknya memakai nol sebagai "tidak ada". Panel memetakan
/// GUID node-nya sendiri ke id-id ini.
///
/// **Urutan pemanggilan.** Begin() … [BeginNode()/EndNode(), Link()] …
/// HandleCreate()/HandleDelete() … End(). Query koneksi baru dan penghapusan
/// hanya sah setelah seluruh node dan link digambar; sebelum itu pustaka belum
/// tahu apa yang ada di kanvas.
class NodeCanvas {
public:
    NodeCanvas();
    ~NodeCanvas();

    NodeCanvas(const NodeCanvas&) = delete;
    NodeCanvas& operator=(const NodeCanvas&) = delete;

    /// Menyiapkan konteks kanvas. `settingsFile` kosong berarti posisi node
    /// tidak disimpan pustaka — dan memang tidak boleh: posisinya milik berkas
    /// `.simgraph`, bukan berkas setelan di samping editor.
    void Initialize();
    void Shutdown();

    void Begin(const char* id, const Vec2& size);
    void End();

    void BeginNode(uint64_t id);
    void EndNode();

    /// Node grup: kotak berlatar tembus pandang yang **memindahkan seluruh node
    /// di dalamnya** ketika digeser, dan bisa diubah ukurannya dari tepinya.
    ///
    /// Keanggotaannya ditentukan letak, bukan daftar: apa pun yang berada di
    /// dalam kotak ikut terbawa. Itu yang membuat grup tetap benar setelah node
    /// ditambah atau dipindahkan tanpa ada yang perlu memperbarui daftar
    /// anggota — dan tidak ada keadaan tersembunyi yang bisa bertentangan
    /// dengan apa yang terlihat di kanvas.
    ///
    /// Urutan: BeginGroupNode() → isi judul → GroupArea() → EndGroupNode().
    void BeginGroupNode(uint64_t id, const Vec4& color);
    /// Luas kotak grup. Dipanggil setelah judulnya digambar.
    void GroupArea(const Vec2& size);
    void EndGroupNode();

    Vec2 GetNodeSize(uint64_t id) const;
    void SetGroupSize(uint64_t id, const Vec2& size);

    /// Pin masukan (kiri) dan keluaran (kanan). Isi visualnya digambar pemanggil
    /// dengan ImGui biasa di antara Begin dan End.
    void BeginInputPin(uint64_t id);
    void BeginOutputPin(uint64_t id);
    void EndPin();

    void Link(uint64_t id, uint64_t fromPin, uint64_t toPin, const Vec4& color, float thickness);

    /// Percobaan koneksi baru oleh pengguna.
    ///
    /// **QueryNewLink() dipanggil SEKALI per frame, bukan diulang.** Yang
    /// dilaporkan adalah satu calon koneksi yang sedang ditahan pengguna, dan
    /// jawabannya tidak berubah sampai frame berikutnya — menanyakannya di
    /// dalam `while` berputar selamanya begitu kedua ujungnya sah. Perhatikan
    /// bahwa QueryDeletedLink()/QueryDeletedNode() di bawah justru kebalikannya:
    /// keduanya antrean yang memang harus dikuras.
    ///
    /// Pemanggil memutuskan sendiri sah atau tidak lewat AcceptLink()/
    /// RejectLink() — di situlah pemeriksaan tipe pin dilakukan, dan karena itu
    /// koneksi yang tidak masuk akal tidak pernah sempat terbentuk.
    bool BeginCreate();
    bool QueryNewLink(uint64_t& fromPin, uint64_t& toPin);
    /// True pada frame ketika pengguna melepas mouse untuk menerima koneksinya.
    bool AcceptLink();
    void RejectLink();
    void EndCreate();

    /// Penghapusan yang diminta pengguna. Antrean: dipanggil berulang sampai
    /// mengembalikan false — lihat catatan di QueryNewLink() untuk kontrasnya.
    bool BeginDelete();
    bool QueryDeletedLink(uint64_t& id);
    bool QueryDeletedNode(uint64_t& id);
    bool AcceptDeletion();
    void RejectDeletion();
    void EndDelete();

    void SetNodePosition(uint64_t id, const Vec2& position);
    Vec2 GetNodePosition(uint64_t id) const;

    std::vector<uint64_t> SelectedNodes() const;
    std::vector<uint64_t> SelectedLinks() const;
    void Select(uint64_t nodeId, bool append);
    void ClearSelection();

    /// Menempatkan seluruh isi graph di dalam layar.
    void FitToContent();
    void CenterOnNode(uint64_t id);

    /// Link yang baru saja diklik ganda, atau 0. Dipakai menyisipkan titik
    /// belok di tengah kabel — gerakan yang sama dengan editor node lain.
    ///
    /// Catatan untuk pemanggil: di antara Begin() dan End(), posisi mouse ImGui
    /// SUDAH berada di ruang kanvas — pustaka menulis ulangnya supaya widget di
    /// dalam node tetap bisa diklik saat di-zoom. Jadi `ImGui::GetMousePos()`
    /// bisa dipakai apa adanya sebagai posisi node baru, tanpa konversi.
    uint64_t DoubleClickedLink() const;


    /// True pada frame ketika pengguna meminta menu konteks di latar kanvas.
    ///
    /// Ditanyakan ke pustaka, bukan disimpulkan dari `IsMouseReleased` +
    /// `IsWindowHovered`: kanvas menangani tombol kanan sendiri (pan), jadi
    /// menebaknya dari keadaan mouse mentah menghasilkan menu yang tidak pernah
    /// muncul — atau muncul di tengah gerakan pan.
    ///
    /// Hanya sah di antara Suspend() dan Resume().
    bool RequestedBackgroundMenu();

    /// Menangguhkan kanvas supaya popup ImGui bisa digambar di koordinat layar
    /// biasa. Tanpa ini, popup ikut ter-zoom bersama kanvas dan menjadi tidak
    /// terbaca saat pengguna memperkecil tampilan.
    void Suspend();
    void Resume();

    /// Latar node, untuk menggambar bingkai penanda (error, breakpoint).
    /// Sah setelah node yang bersangkutan digambar pada frame ini.
    void DrawNodeBackground(uint64_t id, const Vec4& color, float thickness);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sim::editor
