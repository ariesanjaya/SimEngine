// Jalur mundur pass forward, varian bindless.
//
// Sama persis dengan `box.frag` kecuali dari mana albedo-nya datang: di sini
// dari larik tekstur bindless, diindeks nomor slot yang dibawa push constant,
// alih-alih dari descriptor set yang diikat ulang untuk setiap ruas.
//
// **Berkas tersendiri, bukan konstanta spesialisasi.** Yang berbeda antara
// keduanya bukan sebuah nilai melainkan bentuk descriptor set layout-nya, dan
// itu ditetapkan sebelum spesialisasi dinilai — satu modul tidak bisa memenuhi
// keduanya.

#include "box_varyings.slang"
#include "cluster_common.slang"
#include "gi_resolve.slang"
#include "bindless_common.slang"

/// Dipakai saat GI mati. Lihat catatannya di `box.frag`.
static const float kFallbackAmbient = 0.25;

float4 boxBaseColor(BoxVarying input) {
    // Slot nol adalah putih 1x1, jadi ruas tanpa tekstur tidak perlu satu pun
    // cabang di sini — ia mengalikan dengan nilai satuan.
    return sampleBindless(input.slots.y, input.uv);
}

#include "box_shading.slang"
