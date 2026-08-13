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

/// Tingkat perincian keempat tetangga sebuah ubin.
///
/// **Yang halus menjahit ke yang kasar, bukan sebaliknya**, dan hanya satu sisi
/// yang mengerjakannya. Kalau keduanya menyesuaikan diri, keduanya bergerak dan
/// tidak ada yang menjadi acuan — retakannya berpindah alih-alih tertutup.
///
/// Nilai yang sama dengan atau lebih kecil daripada LOD ubinnya berarti "tidak
/// ada yang perlu dijahit": tetangga yang lebih halus punya simpul di setiap
/// tempat ubin ini punya, jadi tepinya sudah berimpit.
struct TileNeighborLods {
    int negativeX = 0;
    int positiveX = 0;
    int negativeY = 0;
    int positiveY = 0;
};

/// LOD sebuah ubin pada jarak tertentu dari kamera.
///
/// **Fungsi murni**, tanpa kamera dan tanpa keadaan: yang menentukan perincian
/// harus bisa diuji tanpa menggambar apa pun, dan yang membaca keadaan global
/// hanya bisa diuji dengan menyiapkan keadaan global.
///
/// Aturannya satu tingkat per penggandaan jarak, diukur terhadap ukuran ubin
/// itu sendiri: ubin sepuluh meter dan ubin satu kilometer tidak boleh berbagi
/// ambang yang sama, karena yang menentukan seberapa kasar sebuah ubin boleh
/// digambar adalah berapa piksel yang ditempatinya.
///
/// `quality` menggeser seluruh ambangnya: dua kali lipat berarti perincian penuh
/// bertahan dua kali lebih jauh.
int SelectLod(float distanceMeters, float tileSizeMeters, int maxLod, float quality = 1.0f);

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
assets::MeshData BuildTileMesh(const Terrain& terrain, int tileX, int tileY, int lod = 0,
                               const TileNeighborLods& neighbors = {});

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
