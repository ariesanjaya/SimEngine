// Rangka kawat viewport, tahap fragment: satu warna dan tidak satu pun hal lain.
//
// **Tahap vertexnya `shadow.vert`, bukan modul tersendiri.** Yang dibutuhkan
// rangka kawat sama persis dengan yang dibutuhkan pass bayangan — posisi klip
// sesudah kulit diterapkan, dan tidak ada varying sama sekali — sampai ke
// nomor descriptor set-nya: keduanya hanya mengikat palet kulit dan transform
// instance, jadi keduanya memakai `shadowLayout_`. Tahap vertex kedua yang
// isinya `mul(viewProj, mul(model, skinPosition(v)))` adalah salinan yang suatu
// saat berselisih satu ULP dengan aslinya, dan selisih sekecil itu muncul
// sebagai rusuk yang berkedip di uji kedalaman, bukan sebagai galat.
//
// Karena itu berkas ini tidak punya masukan sama sekali. `shadow.vert` hanya
// mengeluarkan `SV_Position` — sebuah built-in, bukan varying — jadi tidak ada
// keluaran tahap vertex yang tertinggal tanpa pembaca di sini.

// Warna rangka kawat, radiansi linier.
//
// **Sebuah konstanta di shader, bukan medan `ViewportDesc`.** Push constant
// satu-satunya yang boleh dibaca pipeline ini adalah `BoxPush`, dan tata
// letaknya wajib sama persis dengan milik pass forward, prepass, dan bayangan —
// ketiga layout itu harus tetap *compatible* (lihat `kBoxPushStages`). Menambah
// satu float4 di sana berarti seluruh pass adegan membayar sebuah warna yang
// hanya dibaca satu pass.
//
// Besarnya sekitar satu, sederajat dengan warna garis bantu editor
// (`kSelectionColor`): keduanya ditulis ke gambar HDR yang sama dan sama-sama
// melewati eksposur, bloom, lalu operator nada. Nilai yang jauh lebih besar
// tidak membuat rangka kawat lebih terbaca — ia hanya mekar.
static const float3 kWireColor = float3(0.85, 0.90, 1.00);

[shader("fragment")]
float4 main() : SV_Target {
    return float4(kWireColor, 1.0);
}
