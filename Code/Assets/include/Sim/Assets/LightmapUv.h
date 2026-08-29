#pragma once

#include "Sim/Assets/MeshData.h"

#include <cstdint>
#include <filesystem>
#include <vector>
#include <string>

namespace sim::assets {

/// Apakah UV pertama sebuah mesh layak dipakai sebagai UV lightmap (S4 di
/// docs/PLAN-STATIC-GI.md).
///
/// **Yang menentukan adalah injektivitas, bukan kotak satuan.** Sebuah lightmap
/// menuntut tiap titik permukaan punya texelnya sendiri; UV yang tumpang tindih
/// melanggar itu — dua sisi sebuah tiang yang memakai petak yang sama adalah
/// cara yang benar untuk menghemat tekstur dan cara yang salah untuk menyimpan
/// cahaya. UV yang berulang melanggarnya juga, dan pengulangan itu muncul
/// sebagai tumpang tindih: petak yang sama ditempati berkali-kali.
///
/// **Berada di luar `[0,1]` bukan cacat, hanya skala yang belum disetel**, dan
/// itu diukur bukan diasumsikan: UV ubin terrain ditulis dalam **meter**, karena
/// layer terrain menyebut ukuran pengulangannya dalam meter. Ia unik menurut
/// konstruksi dan sepenuhnya di luar kotak satuan — dan menolaknya berarti
/// membayar unwrap untuk satu-satunya UV di pohon ini yang memang sudah layak.
/// Yang dibutuhkannya satu skala dan satu geseran, bukan parameterisasi baru.
struct LightmapUvSuitability {
    /// True bila UV pertamanya bisa dipakai sebagai parameterisasi lightmap —
    /// dengan penyekalaan bila `needsRescale`.
    bool suitable = false;

    /// True bila ia injektif tetapi berada di luar `[0,1]`. Yang mengurusnya
    /// `AdoptFirstUvAsLightmapUv`, dengan satu skala dan satu geseran.
    bool needsRescale = false;

    /// Kotak UV-nya, yang dipakai penyekalaan itu.
    Vec2 uvMinimum{0.0f};
    Vec2 uvMaximum{0.0f};

    /// Berapa segitiga yang salah satu sudutnya keluar dari `[0,1]`. Dilaporkan,
    /// bukan dipakai menolak.
    uint32_t outsideUnitSquare = 0;
    /// Berapa **pasang** segitiga yang tumpang tindih di ruang UV.
    ///
    /// Dihitung sampai batas, bukan seluruhnya: sebuah mesh yang UV-nya
    /// seluruhnya bertumpuk punya jutaan pasangan, dan yang dibutuhkan
    /// pemanggil cuma "lebih dari nol".
    uint32_t overlappingPairs = 0;
    /// Berapa segitiga yang luas UV-nya nol — sudut yang berimpit di ruang UV.
    /// Ia tidak membuat UV-nya tidak layak, tetapi ia texel yang tidak akan
    /// pernah terisi, dan itu lubang hitam di lightmap.
    uint32_t degenerateTriangles = 0;

    /// Berapa segitiga yang diperiksa seluruhnya.
    uint32_t triangleCount = 0;

    /// Alasan singkat, untuk log dan panel. Kosong bila layak.
    std::string reason;
};

/// Memeriksa UV pertama sebuah mesh.
///
/// `maxOverlapPairs` membatasi berapa pasangan yang dilaporkan; pemeriksaannya
/// berhenti begitu batas itu tercapai. Nol berarti tanpa batas, dan itu hanya
/// masuk akal untuk mesh uji.
LightmapUvSuitability CheckLightmapUv(const MeshData& mesh, uint32_t maxOverlapPairs = 64);

/// Menyalin UV pertama ke UV lightmap, **diskalakan ke `[0,1]`**, dan
/// menandainya terisi.
///
/// Dipakai ketika `CheckLightmapUv` menyatakan layak — dan itu satu-satunya
/// keadaan ia boleh dipanggil. Penyekalaannya seragam di kedua sumbu: skala
/// yang berbeda per sumbu meregangkan texel lightmap, sehingga sebuah ubin
/// terrain yang persegi panjang mendapat kerapatan cahaya yang berbeda menurut
/// arah.
void AdoptFirstUvAsLightmapUv(MeshData& mesh);

/// Hasil pembangkitan UV lightmap (S4).
struct LightmapUnwrapResult {
    bool ok = false;
    /// Berapa chart yang dihasilkan. Satu chart per pulau UV; mesh yang pecah
    /// menjadi ratusan chart membayar tepi yang jauh lebih banyak, dan tepi
    /// adalah texel yang tidak bisa diinterpolasi.
    uint32_t chartCount = 0;
    /// Berapa vertex sesudahnya. **Bisa lebih banyak daripada sebelumnya**:
    /// sebuah vertex yang dipakai dua chart harus dipecah, karena ia tidak bisa
    /// membawa dua UV sekaligus.
    uint32_t vertexCount = 0;
    /// Bagian kotak UV yang benar-benar tertutup chart, 0..1. Yang rendah berarti
    /// sebagian besar texel lightmap tidak akan pernah terbaca.
    float utilisation = 0.0f;
    std::string error;
};

/// Membangkitkan UV lightmap untuk sebuah mesh, **mengubahnya di tempat**.
///
/// **Vertexnya bisa bertambah, dan indeksnya ditulis ulang.** Sebuah vertex yang
/// dipakai dua chart harus dipecah — ia tidak bisa membawa dua UV lightmap
/// sekaligus — jadi yang keluar bukan mesh yang sama dengan satu atribut
/// tambahan melainkan mesh dengan topologi yang sama dan daftar vertex yang
/// lain. Ruas material ikut dipetakan ulang.
///
/// **Tidak ikut dibangun tanpa xatlas**, dan yang terjadi lalu bukan diam
/// melainkan penolakan yang menyebut sakelarnya: mesh tanpa UV lightmap yang
/// lolos tanpa pesan akan terbaca sebagai "mesh ini memang tidak butuh".
LightmapUnwrapResult GenerateLightmapUv(MeshData& mesh);

/// True bila pembangkit UV lightmap ikut dibangun.
bool HasLightmapUnwrapper();

// --- artefak masak ----------------------------------------------------------
//
// **Yang disimpan hanya yang mahal, bukan seluruh mesh.** Yang mahal ada tiga:
// mengurai berkasnya, memeriksa kelayakan UV-nya, dan meng-unwrap-nya. Yang
// pertama sudah punya jalurnya sendiri; yang kedua dan ketiga yang disimpan di
// sini.
//
// **Menyalin seluruh `MeshData` ke artefak sempat menjadi rancangannya, lalu
// ditinggalkan:** ia memuat material, rangka, dan blok OpenPBR yang
// masing-masing punya string dan `optional` — dan menuliskan serialisasinya
// dengan tangan berarti satu salinan lagi dari setiap medan, yang harus
// diperbarui setiap kali salah satunya tumbuh. Yang meleset di sana tidak
// menghasilkan galat, hanya material yang diam-diam kehilangan satu lapisan.
//
// Yang di sini POD seluruhnya, dan ia diterapkan **di atas** mesh yang baru
// diurai. Sumber yang berubah mengubah kuncinya, jadi penerapan itu selalu
// terjadi di atas mesh yang sama dengan yang memanggangnya.

/// Apa yang dihasilkan cook UV lightmap sebuah mesh.
struct LightmapUvArtifact {
    /// True bila UV pertamanya sudah layak dan cuma diskalakan — tanpa unwrap,
    /// tanpa vertex yang dipecah.
    bool fromFirstUv = false;

    /// Berapa vertex yang dipunyai mesh sumbernya. Diperiksa saat menerapkan:
    /// artefak yang dipanggang untuk mesh lain adalah remap yang menunjuk ke
    /// luar daftar vertex.
    uint32_t sourceVertexCount = 0;

    /// Untuk tiap vertex hasil, indeks vertex asalnya. Kosong bila
    /// `fromFirstUv` — di sana daftarnya tidak berubah.
    std::vector<uint32_t> vertexRemap;
    /// Indeks hasil. Kosong bila `fromFirstUv`.
    std::vector<uint32_t> indices;
    /// UV lightmap tiap vertex hasil.
    std::vector<Vec2> lightmapUv;

    bool IsValid() const {
        return !lightmapUv.empty() &&
               (fromFirstUv ? (vertexRemap.empty() && indices.empty())
                            : (vertexRemap.size() == lightmapUv.size() && !indices.empty()));
    }
};

/// Memanggang UV lightmap sebuah mesh: memeriksa kelayakannya, lalu menyalin
/// atau meng-unwrap. Mesh-nya sendiri tidak diubah.
LightmapUvArtifact CookLightmapUv(const MeshData& mesh, LightmapUvSuitability& outCheck,
                                  std::string& error);

/// Menerapkan artefak ke mesh yang baru diurai. Mengembalikan false bila
/// artefaknya bukan milik mesh ini.
bool ApplyLightmapUv(MeshData& mesh, const LightmapUvArtifact& artifact, std::string& error);

/// Kunci artefak: berkas sumbernya dan versi pemanggangnya.
uint64_t LightmapUvCacheKey(const std::filesystem::path& source);

std::filesystem::path LightmapUvCachePath(const std::filesystem::path& cacheDir, uint64_t key);

/// **Atomik** — berkas sementara lalu dipindahkan, dengan alasan yang sama
/// seperti cache SDF dan IBL: proses yang mati di tengah tulis meninggalkan
/// berkas terpotong yang jalan berikutnya temukan sebagai cache yang sah.
bool WriteLightmapUvArtifact(const std::filesystem::path& file,
                             const LightmapUvArtifact& artifact, std::string& error);

/// Gagal membaca bukan galat — yang benar lalu memanggang ulang.
bool ReadLightmapUvArtifact(const std::filesystem::path& file, LightmapUvArtifact& out,
                            std::string& error);

/// Memuat mesh beserta UV lightmap-nya, memanggangnya bila artefaknya belum ada.
///
/// **`cacheDir` kosong berarti tanpa UV lightmap sama sekali** — `LoadMesh`
/// biasa, dan mesh yang keluar `hasLightmapUv` false. Itu jalur yang benar untuk
/// pemakai satu-kali: panel mesh, `--dump-mesh`, dan impor sumber tidak
/// membutuhkannya, dan memaksa mereka memanggang berarti membayar unwrap untuk
/// melihat sebuah thumbnail.
///
/// **Yang memakainya wajib memakai `cacheDir` yang sama.** Unwrap menyusun ulang
/// daftar vertex, jadi dua pemuat yang satu memanggang dan satu tidak
/// menghasilkan dua mesh dengan daftar vertex yang berbeda untuk berkas yang
/// sama — dan yang menemukannya adalah orang yang bertanya kenapa picking
/// mengenai segitiga yang lain daripada yang tergambar.
MeshData LoadMeshWithLightmapUv(const std::filesystem::path& source,
                                const std::filesystem::path& cacheDir, std::string& error);

}  // namespace sim::assets
