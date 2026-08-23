#pragma once

#include "Sim/Whitebox/HalfEdgeMesh.h"
#include "Sim/Whitebox/Polygon.h"

#include <span>

#include <string>

/// Operasi yang membuat whitebox berguna: dorong sebuah sisi dan ruangan
/// bertambah panjang.
///
/// **Meshnya dibangun ulang, bukan disulam.** Menyulam pointer half-edge di
/// tempat adalah tempat bug topologi hidup, dan mesh blockout berukuran puluhan
/// sampai ratusan sisi — membangunnya ulang memakan mikrodetik. Yang ditukar
/// adalah kerumitan dengan waktu, dan pada ukuran ini waktunya tidak terasa.
///
/// Konsekuensinya yang harus diketahui: **nomor face dipertahankan**, jadi
/// poligon dan seleksi bertahan melewati operasi. Face baru selalu ditambahkan
/// di belakang, tidak pernah disisipkan.
namespace sim::whitebox {

/// Hasil sebuah operasi.
struct EditResult {
    bool ok = false;
    /// Poligon yang menjadi hasilnya — sisi yang baru saja didorong, supaya
    /// penyunting bisa membiarkannya tetap terpilih.
    PolygonHandle polygon = PolygonHandle::Invalid;
    std::string error;

    explicit operator bool() const { return ok; }
};

/// Mendorong sebuah poligon sepanjang normalnya, menambahkan dinding di
/// sekelilingnya.
///
/// Jarak nol **tidak mengubah apa pun** — bukan menghasilkan dinding berluas nol
/// yang normalnya tidak tertentu dan merusak pencahayaan jauh kemudian.
EditResult ExtrudePolygon(HalfEdgeMesh& mesh, PolygonSet& polygons, PolygonHandle polygon,
                          float distance);

/// Menggeser seluruh simpul sebuah poligon tanpa menambah geometri.
///
/// Berbeda dari ekstrusi: yang ini memindahkan sisi beserta dinding yang sudah
/// menempel padanya, bukan menumbuhkan dinding baru. Keduanya dibutuhkan, dan
/// menyatukannya menjadi satu operasi bersaklar berarti pemanggil harus ingat
/// saklar mana yang berarti apa.
EditResult TranslatePolygon(HalfEdgeMesh& mesh, PolygonSet& polygons, PolygonHandle polygon,
                            const Vec3& displacement);

// --- sub-objek: simpul dan rusuk (W7.1) --------------------------------------
//
// **Keempatnya bekerja atas simpul, bahkan yang namanya menyebut rusuk.** Sebuah
// rusuk di half-edge tidak punya posisi sendiri — ia sepasang simpul — jadi
// menggesernya berarti menggeser keduanya. Menuliskannya sebagai operasi simpul
// membuat itu terlihat, alih-alih menyembunyikannya di balik nama yang
// menjanjikan sesuatu yang lain.

/// Menggeser sekumpulan simpul sekaligus.
///
/// Setiap face yang menyentuh salah satunya ikut berubah bentuk — itu memang
/// yang diminta, dan itu pula yang membedakannya dari `TranslatePolygon`, yang
/// menggeser sebuah sisi beserta dinding yang menempel padanya.
///
/// Handle kembar di dalam `vertices` tidak menggeser dua kali: yang digeser
/// simpulnya, bukan penyebutannya.
EditResult TranslateVertices(HalfEdgeMesh& mesh, PolygonSet& polygons,
                             std::span<const VertexHandle> vertices, const Vec3& displacement);

/// Ke mana simpul diratakan pada sumbu yang dipilih.
///
/// **Tiga, dan bukan satu.** "Ratakan" yang berarti *rata-rata* dipakai saat
/// merapikan sudut yang hampir sejajar; yang berarti *ke minimum* atau *ke
/// maksimum* dipakai saat menempelkan sesuatu ke dinding yang sudah berdiri.
/// Menawarkan satu saja membuat separuh pemakaiannya menuntut mengetik angka.
enum class AlignMode : uint8_t {
    Mean,
    Minimum,
    Maximum,
};

/// Menyamakan satu koordinat seluruh simpul terpilih.
///
/// `axis` 0, 1, atau 2 untuk X, Y, Z. Kurang dari dua simpul tidak mengubah apa
/// pun — meratakan satu simpul terhadap dirinya sendiri adalah operasi yang
/// tidak punya arti, dan menghasilkan entri undo untuknya hanya membuat Ctrl+Z
/// terasa rusak.
EditResult AlignVertices(HalfEdgeMesh& mesh, PolygonSet& polygons,
                         std::span<const VertexHandle> vertices, int axis, AlignMode mode);

/// Menggeser satu simpul sepanjang salah satu rusuk yang menempel padanya.
///
/// `t` adalah kedudukan di sepanjang rusuk itu: 0 menaruhnya di ujung yang satu
/// lagi, 1 membiarkannya di tempatnya. **Dijepit ke [0, 1]** — melewati ujungnya
/// membalik urutan simpul di face yang bertetangga, dan hasilnya bukan bentuk
/// yang salah melainkan mesh yang tidak sah.
///
/// Gagal bila rusuknya tidak menyentuh simpul itu.
EditResult SlideVertexAlongEdge(HalfEdgeMesh& mesh, PolygonSet& polygons, VertexHandle vertex,
                                EdgeHandle edge, float t);

/// Menyisipkan sebuah simpul di tengah setiap rusuk terpilih.
///
/// **Ini menambah simpul, bukan menambah rusuk.** Face yang bertetangga dengan
/// rusuk itu naik derajatnya satu — sebuah quad menjadi segilima — dan
/// poligonnya tidak pecah. Yang menghubungkan dua simpul baru menjadi rusuk
/// sungguhan adalah penyisipan loop, dan itu operasi tersendiri yang dibangun di
/// atas yang ini.
///
/// Beberapa rusuk boleh diberikan sekaligus, termasuk dua rusuk pada face yang
/// sama; nomor simpul yang baru mengikuti urutan `edges`.
EditResult SplitEdges(HalfEdgeMesh& mesh, PolygonSet& polygons,
                      std::span<const EdgeHandle> edges);

/// Menghubungkan rusuk-rusuk terpilih dengan rusuk baru, memecah face di
/// antaranya.
///
/// **Inilah yang membuat sebuah dinding bisa dijadikan kusen pintu.** Pilih dua
/// rusuk berseberangan pada sebuah quad, dan quad itu menjadi dua quad yang
/// masing-masing bisa dipilih dan diekstrusi sendiri. `SplitEdges` di atas hanya
/// menyisipkan simpul; yang ini menyisipkan rusuk sungguhan.
///
/// **Menghubungkan yang dipilih, bukan merambat sendiri.** Pemotong loop ala DCC
/// menelusuri strip quad sampai mentok dari satu rusuk saja; yang di sini
/// berhenti pada apa yang benar-benar ditunjuk. Untuk blockout itu yang
/// diinginkan — dinding yang diam-diam ikut terbelah sampai ujung ruangan adalah
/// operasi yang harus dibatalkan, bukan disyukuri.
///
/// Face yang menyentuh **tepat dua** rusuk terpilih terbelah menjadi dua. Yang
/// menyentuh satu hanya mendapat simpul di tengah rusuknya, tanpa terbelah —
/// itu wajar, karena rusuk selalu dimiliki dua face dan hanya salah satunya
/// yang mungkin punya pasangannya.
///
/// **Face yang menyentuh lebih dari dua ditolak**, dan seluruh operasi
/// dibatalkan. Menghubungkan empat rusuk pada satu quad tidak punya satu jawaban
/// — silang menuntut simpul di tengah, kipas menuntut memilih titik pusat — dan
/// menebak salah satunya berarti setengah pemakaian menghasilkan bentuk yang
/// tidak diminta. Kisi dibuat dengan memanggilnya berulang: belah dua, lalu
/// belah salah satu belahannya.
///
/// Belahan pertama mewarisi nomor face aslinya; belahan kedua ditambahkan di
/// belakang dan karena itu **memulai poligonnya sendiri** — yang justru
/// dibutuhkan, karena rusuk barunya harus terlihat dan kedua belahan harus bisa
/// dipilih terpisah.
EditResult ConnectEdges(HalfEdgeMesh& mesh, PolygonSet& polygons,
                        std::span<const EdgeHandle> edges);

}  // namespace sim::whitebox
