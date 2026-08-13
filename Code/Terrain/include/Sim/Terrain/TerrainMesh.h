#pragma once

#include "Sim/Assets/MeshData.h"
#include "Sim/Terrain/Terrain.h"

/// Heightmap menjadi geometri yang bisa digambar.
///
/// **Satu mesh per ubin, bukan satu untuk seluruh terrain.** Terrain 4×4 km
/// dengan sampel 0,25 m adalah 256 juta segitiga; tidak ada yang mengunggahnya
/// dan tidak ada yang menyimpannya sebagai satu `MeshData`. Pengubinan bukan
/// optimasi yang ditunda melainkan syarat supaya ada sesuatu yang tergambar
/// sama sekali.
namespace sim::terrain {

/// Sampel per ubin pada sebuah tingkat perincian.
///
/// Dipisah supaya pemanggil bisa menghitung anggaran sebelum membangun apa pun:
/// yang baru tahu ukurannya sesudah meshnya jadi sudah terlanjut membayarnya.
int LodStep(int lod);

/// Mesh sebuah ubin, di **ruang lokal terrain** — titik asalnya sudut peta,
/// bukan sudut ubin.
///
/// Ruang terrain, bukan ruang ubin: seluruh ubin karena itu memakai transform
/// entity yang sama persis, dan tidak ada satu pun ubin yang bisa tergeser
/// karena transform per-ubin yang salah dihitung. Yang dibayar adalah koordinat
/// yang membesar pada peta yang luas, dan float32 masih menyimpan milimeter
/// pada empat kilometer.
///
/// Mesh kosong (tanpa segitiga) bila seluruh ubin berlubang, atau bila
/// `tileX`/`tileY` di luar peta.
///
/// **Tidak mewujudkan ubin mana pun.** Ia hanya membaca lewat `RawAt`, yang
/// menjawab tinggi dasar untuk ubin yang belum pernah ditulis — jadi membangun
/// mesh tidak membatalkan alokasi malas yang menjadi seluruh alasan terrain
/// sebesar ini muat di memori.
assets::MeshData BuildTileMesh(const Terrain& terrain, int tileX, int tileY, int lod = 0);

/// Normal permukaan pada sebuah **sampel**, dari beda tengah heightmap-nya.
///
/// Bukan normal segitiga, dan bukan `NormalAtWorld`. Dua alasan, keduanya
/// tentang jahitan:
///
/// - Normal segitiga membuat lereng landai terlihat berundak, karena dua
///   segitiga satu quad punya normal berbeda sementara permukaannya mulus.
/// - Beda tengah adalah fungsi murni dari koordinat sampel global, jadi sebuah
///   simpul yang dimiliki dua ubin mendapat normal yang **sama persis** dari
///   keduanya. `NormalAtWorld` menjawab gradien sel bilinear, dan tepat di titik
///   sampel pilihan selnya asimetris — benar, tetapi tidak simetris, dan yang
///   tidak simetris di tepi ubin adalah garis terang yang membelah peta.
Vec3 SampleNormal(const Terrain& terrain, int x, int y);

}  // namespace sim::terrain
