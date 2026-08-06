#pragma once

#include "Sim/Terrain/TerrainBrush.h"
#include "Sim/Vegetation/Vegetation.h"

namespace sim::vegetation {

using sim::terrain::PaintBrush;

/// Satu sentuhan kuas kepadatan.
///
/// **Kuasnya, penjadwalnya, dan profil falloff-nya dipinjam bulat-bulat dari
/// modul terrain**, bukan disalin. Mereka menjawab pertanyaan yang sama persis —
/// bagaimana bobot melembut dari pusat, dan kapan sebuah sentuhan dipancarkan
/// saat kursor diseret — dan jawaban yang disalin akan berbeda pada perbaikan
/// berikutnya. Yang khas vegetasi hanyalah apa yang disentuhnya.
void ApplyDensityDab(Vegetation& vegetation, const PaintBrush& brush, int layer, float worldX,
                     float worldZ, float dt);

/// Meratakan kepadatan ke rata-rata tetangganya.
///
/// Rumusnya sama dengan brush Smooth terrain, dan begitu juga jebakannya: nilai
/// tetangga dibaca dari salinan yang diambil sebelum sentuhan dimulai. Membacanya
/// dari peta yang sedang ditulis membuat hasilnya bergantung pada urutan
/// pemindaian, yaitu gradasi yang miring ke arah pojok kiri atas tanpa ada yang
/// memiringkannya.
void ApplySmoothDab(Vegetation& vegetation, const PaintBrush& brush, int layer, float worldX,
                    float worldZ, float dt);

/// Menghapus setiap instance di dalam lingkaran. Mengembalikan jumlahnya.
///
/// **Tanpa falloff dan tanpa `dt`.** Sebuah instance ada atau tidak ada; tidak
/// ada yang bisa dikonvergensikan dan tidak ada setengah pohon. Kuas penghapus
/// karena itu hanya punya jari-jari, dan lingkaran yang digambar kursornya
/// adalah persis batas yang dihapusnya.
std::size_t ApplyEraseDab(Vegetation& vegetation, int layer, float worldX, float worldZ,
                          float radius);

/// Menanam satu instance yang menempel di permukaan.
///
/// Skala dan rotasinya diundi dari posisinya sendiri, bukan dari pencacah yang
/// bertambah. Dua kali menanam di titik yang sama karena itu menghasilkan pohon
/// yang sama — dan yang lebih penting, menanam tidak menyimpan keadaan apa pun
/// yang harus ikut disimpan ke berkas supaya sesi berikutnya cocok.
void ApplyPlantDab(Vegetation& vegetation, const Terrain& terrain, int layer, float worldX,
                   float worldZ);

}  // namespace sim::vegetation
