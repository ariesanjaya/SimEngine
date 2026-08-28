#pragma once

#include "Sim/Assets/MeshData.h"

#include <cstdint>
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

}  // namespace sim::assets
