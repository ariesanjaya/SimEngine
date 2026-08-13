#pragma once

#include "Sim/Whitebox/WhiteboxMesh.h"

#include <cstdint>
#include <vector>

/// Bentuk tabrakan sebuah whitebox.
///
/// **Terpisah dari mesh yang digambar**, dan bukan karena kerapian: yang
/// digambar dipecah per material supaya renderer bisa mengganti materialnya
/// sekali per ruas, dan pemecahan itu menggandakan simpul di setiap batas ruas.
/// Solver tidak peduli material sama sekali — memberinya mesh gambar berarti
/// membayar simpul ganda untuk jawaban yang sama persis.
namespace sim::whitebox {

struct CollisionShape {
    std::vector<Vec3> points;
    /// Tiga indeks per segitiga, menunjuk ke `points`.
    std::vector<uint32_t> indices;
    /// True bila bentuknya cembung, sehingga selubung cembungnya persis sama
    /// dengan bentuk ini.
    ///
    /// Yang menanyakannya adalah fisika: benda dinamis hanya boleh memakai
    /// selubung, dan pada bentuk cekung selubung itu **bukan** yang digambar —
    /// ruangannya tertutup dan lubangnya rata. Itu perlu dikatakan, bukan
    /// didiamkan.
    bool convex = false;
};

CollisionShape BuildCollisionShape(const WhiteboxMesh& box);

/// Apakah setiap simpul berada di belakang setiap bidang face.
///
/// Toleransinya dalam meter, bukan relatif: yang diperiksa adalah jarak simpul
/// ke sebuah bidang, dan blockout dirancang dalam meter. Nilai bawaannya
/// seperempat milimeter — cukup longgar untuk galat pembulatan sesudah beberapa
/// kali ekstrusi, cukup ketat untuk menangkap cekungan yang bisa dilihat mata.
bool IsConvex(const WhiteboxMesh& box, float toleranceMeters = 2.5e-4f);

}  // namespace sim::whitebox
