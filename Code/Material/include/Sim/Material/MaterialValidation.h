#pragma once

#include "Sim/Material/MaterialGraph.h"

#include <string>
#include <vector>

namespace sim::material {

/// Satu masalah pada graph, beserta tempatnya.
///
/// `node` yang terisi bukan kemewahan: pesan yang hanya menyebut "ada siklus"
/// memaksa pengguna menelusuri graph sendiri, sementara editor sudah tahu
/// persis node mana yang harus disorot.
struct MaterialIssue {
    Uuid node;
    /// Pin yang bersangkutan, bila masalahnya menyangkut satu pin tertentu.
    std::string pin;
    std::string message;
};

struct ValidationResult {
    bool ok = false;
    std::vector<MaterialIssue> errors;

    bool Mentions(const Uuid& node) const;
};

/// Memeriksa graph tanpa menghasilkan kode.
///
/// Tiga hal yang dicari, dan ketiganya adalah kesalahan yang tidak akan
/// terlihat sampai shader-nya gagal dikompilasi jauh dari sini:
///
/// 1. **Tipe yang tidak cocok** pada sebuah koneksi.
/// 2. **Siklus** — graph material murni dataflow, jadi lingkar apa pun berarti
///    nilai yang bergantung pada dirinya sendiri, dan penelusuran yang tidak
///    pernah berhenti kalau tidak dijaga.
/// 3. **Pin wajib yang kosong** — pin tanpa nilai bawaan, seperti `texture`
///    pada node Sample. Tidak ada angka yang masuk akal untuk "tekstur yang
///    tidak dipilih", jadi menebaknya hanya menunda kesalahannya.
///
/// Node keluaran yang tidak ada, atau lebih dari satu, ikut dilaporkan: graph
/// tanpa keluaran tidak punya arti, dan dua keluaran tidak punya arti tunggal.
ValidationResult ValidateMaterial(const MaterialGraph& graph);

}  // namespace sim::material
