#pragma once

#include "Sim/Terrain/Terrain.h"

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

/// Bobot brush pada jarak tertentu dari pusatnya. Dipisah supaya panel bisa
/// menggambar profil brush dengan rumus yang sama persis dengan yang menyunting
/// terrain — profil yang digambar ulang dengan rumus kedua adalah profil yang
/// akan berbeda.
float BrushWeight(const Brush& brush, float distance);

/// Satu sentuhan brush pada posisi dunia. `dt` detik sejak sentuhan sebelumnya.
///
/// Sentuhan, bukan goresan: pemanggil membungkus rangkaian sentuhan di antara
/// `Terrain::BeginStroke` dan `EndStroke` supaya seluruhnya menjadi satu langkah
/// undo.
void ApplyDab(Terrain& terrain, const Brush& brush, float worldX, float worldZ, float dt);

/// Ramp linear antara dua titik dunia, selebar `brush.radius` di kiri-kanan
/// garisnya. Bukan sentuhan: ramp ditentukan dua titik, jadi ia satu operasi.
void ApplyRamp(Terrain& terrain, const Brush& brush, const Vec3& start, const Vec3& end);

/// Erosi termal: material di lereng yang lebih curam dari `talusDegrees`
/// meluncur ke tetangga yang lebih rendah.
///
/// Dipisah dari brush karena ia operasi area yang diulang, bukan sentuhan yang
/// diseret — dan karena hasilnya baru terlihat setelah puluhan iterasi.
void ApplyThermalErosion(Terrain& terrain, const SampleRect& rect, int iterations,
                         float talusDegrees, float strength);

}  // namespace sim::terrain
