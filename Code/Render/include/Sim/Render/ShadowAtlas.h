#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Render/LightCluster.h"
#include "Sim/Render/Types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace sim::render {

struct ShadowAtlasSettings {
    /// Sisi atlas, piksel. Persegi dan kelipatan `maxTile`.
    uint32_t resolution = 4096;
    /// Ubin terbesar dan terkecil yang boleh diberikan ke satu muka.
    uint32_t maxTile = 1024;
    uint32_t minTile = 128;
    /// Batas atas banyaknya muka yang dibayangi dalam satu frame.
    ///
    /// Ada supaya adegan dengan dua ratus lampu tidak menghabiskan seluruh
    /// atlas untuk lampu yang masing-masing hanya beberapa piksel di layar —
    /// dan supaya biaya pass bayangannya punya langit-langit yang bisa disebut.
    uint32_t maxFaces = 32;
};

/// Satu muka bayangan yang mendapat tempat di atlas.
///
/// Spot punya satu muka; point punya enam. Keduanya diwakili entri yang sama
/// bentuknya — yang membedakan hanya `face` dan matriksnya — sehingga pass
/// bayangannya tidak perlu tahu jenis lampu sama sekali.
struct ShadowAtlasEntry {
    uint32_t light = 0;
    /// 0 untuk spot; 0..5 untuk keenam muka point, urutan Vulkan.
    uint32_t face = 0;
    /// Posisi dan ukuran ubin di dalam atlas, piksel.
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t size = 0;
    Mat4 viewProjection{1.0f};
    /// Bidang dekat dan jauh yang dipakai matriksnya. Dibutuhkan shader untuk
    /// membalik depth non-linear menjadi jarak.
    float nearZ = 0.05f;
    float farZ = 10.0f;
};

struct ShadowAtlasResult {
    std::vector<ShadowAtlasEntry> entries;
    /// Indeks entri pertama milik sebuah lampu, atau -1 bila lampu itu tidak
    /// kebagian tempat. Diindeks sama dengan daftar lampu yang diberikan.
    std::vector<int32_t> firstEntry;
    /// Lampu yang meminta bayangan tapi tidak kebagian tempat.
    uint32_t dropped = 0;
};

/// Membagi atlas kepada lampu yang meminta bayangan.
///
/// **Ubinnya berukuran pangkat dua dan dialokasikan seperti quadtree.** Ukuran
/// tetap untuk semua akan memberi lampu meja di ujung ruangan resolusi yang sama
/// dengan lampu sorot yang memenuhi layar — dan yang pertama membuang sebagian
/// besar atlas sementara yang kedua kekurangan. Ukurannya karena itu dipilih
/// dari seberapa besar lampu itu di layar.
///
/// **Yang tidak kebagian dilaporkan, bukan didiamkan.** Lampu yang diam-diam
/// kehilangan bayangannya terlihat sebagai benda yang mengambang, dan tidak ada
/// yang akan menghubungkannya dengan atlas yang penuh.
///
/// Urutannya deterministik: lampu diurutkan menurut kepentingan, dan yang
/// kepentingannya sama diurutkan menurut indeksnya. Urutan yang bergoyang antar-
/// frame membuat lampu bergantian kehilangan bayangan — kedipan yang jauh lebih
/// mencolok daripada bayangan yang memang tidak ada.
ShadowAtlasResult AllocateShadowAtlas(std::span<const LightInstance> lights, const Vec3& eye,
                                      const ShadowAtlasSettings& settings);

/// Kepentingan sebuah lampu: jari-jari layarnya, kira-kira.
///
/// Dipisah supaya bisa diuji sendiri — di sinilah diputuskan lampu mana yang
/// mendapat resolusi, dan keputusan itu yang paling terlihat saat salah.
float ShadowImportance(const LightInstance& light, const Vec3& eye);

/// Matriks untuk sebuah muka bayangan.
///
/// Spot memakai perspektif dengan sudut bukaan dua kali sudut kerucut luarnya;
/// point memakai enam perspektif 90°. Keduanya **bukan** reversed-Z: atlas
/// dibaca dengan perbandingan biasa, sama dengan cascade, dan mencampur dua
/// konvensi depth di satu berkas shader adalah cara tercepat membalik bayangan
/// menjadi sorotan.
Mat4 ShadowFaceMatrix(const LightInstance& light, uint32_t face, float nearZ);

/// Arah pandang keenam muka point light, urutan Vulkan: +X, −X, +Y, −Y, +Z, −Z.
Vec3 ShadowFaceDirection(uint32_t face);

}  // namespace sim::render
