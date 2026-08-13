#include "Sim/Whitebox/WhiteboxExport.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace sim::whitebox {
namespace {

/// Satu bilangan, ditulis dengan cara yang sama di mana pun.
///
/// **`std::to_string` tidak dipakai**: ia menghormati locale, dan locale yang
/// memakai koma sebagai pemisah desimal menghasilkan "v 0,5 0,5 0,5" — berkas
/// yang tampak wajar dan ditolak setiap pembaca OBJ di dunia. Kegagalannya
/// bergantung pada setelan mesin yang mengekspor, jadi ia tidak pernah muncul
/// pada mesin yang mengujinya.
void AppendFloat(std::string& out, float value) {
    char buffer[32];
    const int written = std::snprintf(buffer, sizeof(buffer), "%.6g", static_cast<double>(value));
    if (written > 0) {
        out.append(buffer, static_cast<std::size_t>(written));
    }
}

void AppendVector(std::string& out, const char* tag, float x, float y, float z) {
    out += tag;
    out += ' ';
    AppendFloat(out, x);
    out += ' ';
    AppendFloat(out, y);
    out += ' ';
    AppendFloat(out, z);
    out += '\n';
}

std::set<int> UsedSlots(const WhiteboxMesh& box) {
    std::set<int> slots;
    for (const assets::SubMesh& part : box.BuildMeshData().parts) {
        slots.insert(part.material);
    }
    return slots;
}

}  // namespace

std::string MaterialName(int slot) {
    // Slot yang belum ditetapkan punya namanya sendiri, bukan "slot_-1": tanda
    // minus di dalam nama material membingungkan sebagian pembaca, dan yang
    // membacanya manusia tetap perlu tahu bedanya "belum dipilih" dari "nomor
    // sekian".
    return slot == kNoMaterial ? "whitebox_unassigned" : "whitebox_slot_" + std::to_string(slot);
}

std::string BuildObj(const WhiteboxMesh& box, const std::string& materialLibrary) {
    const assets::MeshData mesh = box.BuildMeshData();

    std::string out;
    out += "# dihasilkan SimEngine dari sebuah whitebox\n";
    if (!materialLibrary.empty()) {
        out += "mtllib " + materialLibrary + "\n";
    }
    out += "o Whitebox\n";

    for (const assets::MeshVertex& vertex : mesh.vertices) {
        AppendVector(out, "v", vertex.position.x, vertex.position.y, vertex.position.z);
    }
    for (const assets::MeshVertex& vertex : mesh.vertices) {
        AppendVector(out, "vn", vertex.normal.x, vertex.normal.y, vertex.normal.z);
    }
    for (const assets::MeshVertex& vertex : mesh.vertices) {
        out += "vt ";
        AppendFloat(out, vertex.uv.x);
        out += ' ';
        AppendFloat(out, vertex.uv.y);
        out += '\n';
    }

    // Ruas demi ruas, bukan segitiga demi segitiga: `usemtl` berlaku sampai yang
    // berikutnya, dan menuliskannya per segitiga menghasilkan berkas yang benar
    // tetapi tiga kali lebih besar tanpa satu pun keterangan tambahan.
    for (const assets::SubMesh& part : mesh.parts) {
        out += "usemtl " + MaterialName(part.material) + "\n";
        const std::size_t end = static_cast<std::size_t>(part.firstIndex) + part.indexCount;
        for (std::size_t i = part.firstIndex; i + 2 < end; i += 3) {
            out += 'f';
            for (std::size_t c = 0; c < 3; ++c) {
                // OBJ mengindeks dari satu, dan ketiga atributnya sejajar di
                // sini — satu simpul, satu normal, satu UV.
                const std::string index = std::to_string(mesh.indices[i + c] + 1);
                out += ' ' + index + '/' + index + '/' + index;
            }
            out += '\n';
        }
    }
    return out;
}

std::string BuildMtl(const WhiteboxMesh& box) {
    std::string out = "# dihasilkan SimEngine dari sebuah whitebox\n";
    for (const int slot : UsedSlots(box)) {
        // Abu-abu netral, sengaja. Yang menentukan tampilan sesungguhnya adalah
        // material mesin yang dipasang ke slot ini; menebak warna di sini berarti
        // blockout yang diekspor terlihat berbeda dari blockout yang dirancang.
        out += "newmtl " + MaterialName(slot) + "\n";
        out += "Kd 0.7 0.7 0.7\n";
        out += "Ka 0 0 0\n";
        out += "Ks 0 0 0\n";
        out += "d 1\n";
        out += "illum 1\n";
    }
    return out;
}

ExportResult ExportObj(const WhiteboxMesh& box, const std::filesystem::path& path) {
    ExportResult result;
    if (box.Mesh().FaceCount() == 0) {
        result.error = "whitebox itu tidak punya sisi untuk diekspor";
        return result;
    }

    std::filesystem::path materialPath = path;
    materialPath.replace_extension(".mtl");

    {
        std::ofstream stream(materialPath, std::ios::binary);
        if (!stream) {
            result.error = "tidak bisa menulis " + materialPath.string();
            return result;
        }
        stream << BuildMtl(box);
        if (!stream) {
            result.error = "gagal menulis " + materialPath.string();
            return result;
        }
    }

    std::ofstream stream(path, std::ios::binary);
    if (!stream) {
        result.error = "tidak bisa menulis " + path.string();
        return result;
    }
    stream << BuildObj(box, materialPath.filename().string());
    if (!stream) {
        result.error = "gagal menulis " + path.string();
        return result;
    }

    result.ok = true;
    return result;
}

}  // namespace sim::whitebox
