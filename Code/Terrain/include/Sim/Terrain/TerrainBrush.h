#pragma once

#include "Sim/Terrain/Terrain.h"

#include <functional>

namespace sim::terrain {

enum class BrushKind {
    Raise,
    Lower,
    Flatten,
    Smooth,
    Noise,
};

const char* ToString(BrushKind kind);

struct Brush {
    BrushKind kind = BrushKind::Raise;
    /// Jari-jari, meter.
    float radius = 10.0f;
    /// Raise/Lower: meter per detik di pusat. Flatten/Smooth: laju konvergensi
    /// per detik. Noise: amplitudo meter per detik.
    float strength = 5.0f;
    /// 0 = tepi tajam, 1 = melembut dari pusat.
    float falloff = 0.5f;
    /// Tinggi tujuan Flatten, meter.
    float targetHeight = 0.0f;
    /// Noise: siklus per meter, dan benihnya.
    float noiseFrequency = 0.05f;
    uint32_t seed = 1;
};

/// Kuas cat: bobot layer material dan peta hole.
///
/// **Dipisah dari `Brush`, bukan disatukan dengannya**, walaupun jari-jari dan
/// falloff-nya sama artinya. Yang berbeda adalah kekuatannya: pada `Brush` ia
/// meter per detik, di sini laju konvergensi ke bobot tujuan. Satu struct
/// bersama berarti satu slider yang artinya berganti diam-diam saat alat
/// berpindah — dan angka 20 yang tadinya wajar menjadi "penuh seketika".
struct PaintBrush {
    /// Jari-jari, meter.
    float radius = 10.0f;
    /// Laju konvergensi ke bobot tujuan, per detik.
    float strength = 4.0f;
    /// 0 = tepi tajam, 1 = melembut dari pusat.
    float falloff = 0.5f;
    /// Bobot tujuan di pusat brush, 0..1. 0 berarti menghapus.
    float target = 1.0f;
};

/// Bobot brush pada jarak tertentu dari pusatnya. Dipisah supaya panel bisa
/// menggambar profil brush dengan rumus yang sama persis dengan yang menyunting
/// terrain — profil yang digambar ulang dengan rumus kedua adalah profil yang
/// akan berbeda.
float BrushWeight(const Brush& brush, float distance);
float BrushWeight(const PaintBrush& brush, float distance);

/// Satu sentuhan brush pada posisi dunia. `dt` detik sejak sentuhan sebelumnya.
///
/// Sentuhan, bukan goresan: pemanggil membungkus rangkaian sentuhan di antara
/// `Terrain::BeginStroke` dan `EndStroke` supaya seluruhnya menjadi satu langkah
/// undo.
void ApplyDab(Terrain& terrain, const Brush& brush, float worldX, float worldZ, float dt);

/// Ramp linear antara dua titik dunia, selebar `brush.radius` di kiri-kanan
/// garisnya. Bukan sentuhan: ramp ditentukan dua titik, jadi ia satu operasi.
void ApplyRamp(Terrain& terrain, const Brush& brush, const Vec3& start, const Vec3& end);

/// Satu sentuhan cat pada peta bobot sebuah layer.
void ApplyLayerDab(Terrain& terrain, const PaintBrush& brush, int layer, float worldX,
                   float worldZ, float dt);

/// Satu sentuhan pada peta hole.
///
/// **Tanpa `dt`.** Hole itu biner: tidak ada yang bisa dikonvergensikan, jadi
/// tidak ada yang bergantung pada berapa lama kuas berada di sana. `falloff`
/// karenanya bekerja sebagai ambang, bukan sebagai gradasi — separuh quad yang
/// berlubang bukan sesuatu yang bisa digambar perender mana pun.
void ApplyHoleDab(Terrain& terrain, const PaintBrush& brush, bool hole, float worldX,
                  float worldZ);

/// Satu goresan brush: sentuhan yang disebar di sepanjang lintasan kursor.
///
/// **Sentuhan dipancarkan pada langkah waktu tetap, bukan sekali per frame.**
/// Mengalikan kekuatan brush dengan `dt` frame terdengar benar tapi tidak: pada
/// mesin 144 Hz sebuah goresan menerima lebih dari empat kali lipat jumlah
/// sentuhan dibanding mesin 30 Hz, dan karena setiap sentuhan dijepit ke rentang
/// tinggi serta dibulatkan ke sampel 16-bit, hasilnya bukan sekadar berbeda
/// sedikit — bukit yang sama digores dengan cara yang sama menjadi bukit yang
/// berbeda. Penyunting yang tidak bisa menduga hasil goresannya berhenti
/// mempercayai alatnya.
///
/// Langkah tetapnya alasan yang sama dengan langkah tetap simulasi partikel, dan
/// sengaja angka yang sama.
class BrushStroke {
public:
    static constexpr float kDabStep = 1.0f / 60.0f;

    /// Waktu susulan terbanyak yang dipakai satu panggilan, detik.
    ///
    /// Membatasi **jumlah materialnya**, bukan jumlah sentuhannya. Editor yang
    /// tersendat sedetik penuh sambil tombol ditahan seharusnya tidak menggerus
    /// terrain sedalam satu detik penuh di satu titik — tapi membatasi jumlah
    /// sentuhan tidak menyelesaikan itu, ia hanya membuat goresan biasa pada
    /// mesin lambat kehilangan sebagian materialnya diam-diam. Yang dibatasi
    /// harus yang memang ingin dibatasi.
    static constexpr float kMaxCatchUpSeconds = 0.25f;

    /// Jarak antar sentuhan, sebagai pecahan jari-jari brush.
    ///
    /// Langkah waktu saja tidak cukup. Seretan cepat memindahkan kursor lebih
    /// jauh daripada satu jari-jari di antara dua sentuhan, dan yang tertinggal
    /// adalah rangkaian manik-manik alih-alih garis. Karena itu jumlah sentuhan
    /// diambil dari yang lebih besar antara langkah waktu dan langkah jarak.
    ///
    /// Jatah waktunya lalu **dibagi rata** ke seluruh sentuhan, bukan diberikan
    /// penuh ke masing-masing. Kalau tidak, menyeret cepat justru menumpuk lebih
    /// banyak material daripada menyeret pelan di lintasan yang sama — kebalikan
    /// dari yang diharapkan siapa pun.
    static constexpr float kDabSpacing = 0.25f;

    /// Batas jumlah sentuhan per panggilan. Ini batas **ongkos**, bukan batas
    /// hasil: jatah waktunya tetap dibagi habis di antara sentuhan yang ada.
    static constexpr int kMaxDabs = 64;

    void Begin(Terrain& terrain, float worldX, float worldZ);
    /// Memajukan goresan ke posisi kursor sekarang selama `dt` detik. Sentuhan
    /// disebar merata di sepanjang ruas dari posisi sebelumnya.
    void Advance(Terrain& terrain, const Brush& brush, float worldX, float worldZ, float dt);

    /// Bentuk umum `Begin`/`End`: penjadwalnya saja, tanpa dokumen.
    ///
    /// Ada karena penjadwal ini bukan milik terrain — ia menjawab "kapan sebuah
    /// sentuhan dipancarkan", dan jawaban itu sama untuk setiap alat yang
    /// diseret. Dokumen yang disunting vegetasi bukan `Terrain`, jadi kalau
    /// membuka goresan menuntut sebuah `Terrain`, penjadwalnya harus disalin —
    /// dan salinan kedua aturan langkah-tetap akan berbeda pada perbaikan
    /// berikutnya. Yang membuka dan menutup goresan dokumennya adalah
    /// pemanggil.
    void Begin(float worldX, float worldZ);
    void End();

    /// Bentuk umumnya: `dab(worldX, worldZ, dt)` dipanggil sekali per sentuhan.
    ///
    /// Penjadwalnya sengaja tidak tahu apa yang dilakukan sentuhannya, dan tidak
    /// menyentuh terrain sama sekali. Sculpt dan paint memakai jadwal yang sama
    /// persis karena masalahnya memang sama persis — laju frame yang tidak boleh
    /// mengubah hasil, dan seretan cepat yang tidak boleh meninggalkan
    /// manik-manik. Menyalin jadwalnya untuk paint berarti dua salinan aturan
    /// yang akan berbeda pada perbaikan berikutnya.
    void Advance(float radius, float worldX, float worldZ, float dt,
                 const std::function<void(float, float, float)>& dab);

    void End(Terrain& terrain);

    bool Active() const { return active_; }
    /// Sentuhan yang sudah dipancarkan goresan ini. Dipakai panel untuk
    /// memperlihatkan bahwa goresan memang berjalan.
    int Dabs() const { return dabs_; }

private:
    bool active_ = false;
    float lastX_ = 0.0f;
    float lastZ_ = 0.0f;
    float accumulator_ = 0.0f;
    int dabs_ = 0;
};

/// Erosi termal: material di lereng yang lebih curam dari `talusDegrees`
/// meluncur ke tetangga yang lebih rendah.
///
/// Dipisah dari brush karena ia operasi area yang diulang, bukan sentuhan yang
/// diseret — dan karena hasilnya baru terlihat setelah puluhan iterasi.
void ApplyThermalErosion(Terrain& terrain, const SampleRect& rect, int iterations,
                         float talusDegrees, float strength);

}  // namespace sim::terrain
