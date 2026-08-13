#pragma once

#include "Sim/Whitebox/HalfEdgeMesh.h"
#include "Sim/Whitebox/Polygon.h"

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

}  // namespace sim::whitebox
