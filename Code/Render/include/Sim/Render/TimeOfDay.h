#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sim::render {

// --- Geometri matahari -------------------------------------------------------

/// Tempat dan saat yang menentukan letak matahari.
struct SunPlacement {
    /// Lintang derajat, positif di utara.
    float latitudeDegrees = -6.2f;  // Jakarta
    /// Hari ke berapa dalam setahun, 1..365. Menentukan deklinasi, yaitu musim.
    uint32_t dayOfYear = 172;  // titik balik matahari Juni
    /// Jam matahari lokal, 0..24.
    float hour = 12.0f;
    /// Rotasi arah utara terhadap sumbu −Z, derajat. Adegan jarang dibangun
    /// menghadap utara sungguhan, dan memutar seluruh adegan agar cocok jauh
    /// lebih mahal daripada memutar mataharinya.
    float northOffsetDegrees = 0.0f;
};

/// Letak matahari di langit.
struct SunPosition {
    /// **Arah dari permukaan ke matahari**, konvensi yang sama dengan
    /// `ViewportDesc::sunDirection` dan `LightComponent`. Membalikkannya tidak
    /// menghasilkan galat apa pun — hanya adegan yang tersinari dari arah
    /// berlawanan, dan bayangan yang jatuh ke sisi yang salah.
    Vec3 direction{0.0f, 1.0f, 0.0f};
    /// Ketinggian di atas horizon, radian. Negatif berarti matahari terbenam.
    float altitude = 0.0f;
    /// Azimut dari utara searah jarum jam, radian.
    float azimuth = 0.0f;
};

/// Rotasi yang membuat sumbu −Z sebuah entity menunjuk ke `forward`.
///
/// **Dipisah karena kasus tegak lurusnya nyata.** Matahari melewati zenit setiap
/// tengah hari di lintang rendah, dan di sanalah `forward` menjadi sejajar
/// dengan sumbu atas — `quatLookAt` menghasilkan hasil yang tidak terdefinisi
/// tepat di sana. Yang terlihat: lampu yang berputar liar sekejap setiap tengah
/// hari, satu frame saja, di lokasi tertentu saja.
Quat LookRotation(const Vec3& forward);

/// Menghitung letak matahari dari tempat dan waktu.
///
/// **Geometri matahari sungguhan, bukan busur yang digambar tangan.** Yang
/// diminta editor Time-of-Day adalah matahari yang benar-benar berputar, dan
/// tempat ia terbit ditentukan lintang dan musim — busur yang digambar tangan
/// tidak bisa menyatakan keduanya, dan bayangan tengah harinya akan jatuh ke
/// arah yang salah untuk lokasi mana pun kecuali yang kebetulan dipakai saat
/// menggambarnya.
SunPosition ComputeSunPosition(const SunPlacement& placement);

// --- Kurva terhadap jam ------------------------------------------------------

/// Satu titik kunci pada kurva harian.
struct TimeOfDayKey {
    /// Jam 0..24.
    float hour = 0.0f;
    Vec3 value{0.0f};
};

/// Kurva sebuah parameter atmosfer terhadap jam.
///
/// **Siklis atas 24 jam, bukan terputus di tengah malam.** Kurva yang berhenti
/// di jam 24 menjadikan tengah malam sebuah loncatan — dan tengah malam justru
/// saat yang pasti dilewati setiap siklus siang-malam. Ruas dari kunci terakhir
/// ke kunci pertama melintasi tengah malam, dan itulah ruas yang paling mudah
/// terlupa.
class TimeOfDayCurve {
public:
    TimeOfDayCurve() = default;
    explicit TimeOfDayCurve(std::vector<TimeOfDayKey> keys);

    /// Menyisipkan sebuah kunci, menjaga urutan jamnya. Kunci pada jam yang sama
    /// diganti — dua kunci pada jam yang sama membuat hasilnya bergantung urutan
    /// penyisipan, yang tidak terlihat di mana pun kecuali pada berkas yang
    /// disimpan.
    void Set(float hour, const Vec3& value);
    void Remove(std::size_t index);
    void Clear();

    std::size_t KeyCount() const { return keys_.size(); }
    const TimeOfDayKey& Key(std::size_t index) const { return keys_[index]; }
    const std::vector<TimeOfDayKey>& Keys() const { return keys_; }

    /// Nilai pada sebuah jam. Kurva kosong mengembalikan `fallback`.
    Vec3 Evaluate(float hour, const Vec3& fallback = Vec3(0.0f)) const;

private:
    std::vector<TimeOfDayKey> keys_;
};

/// Membungkus sebuah jam ke rentang [0, 24).
float WrapHour(float hour);

// --- Jam siklus --------------------------------------------------------------

/// Jam siklus siang-malam.
///
/// **Terpisah dari kurvanya.** Kurva adalah data yang disunting dan disimpan;
/// jam adalah keadaan yang berjalan. Menyatukannya berarti menggulung waktu ikut
/// mengubah berkas.
class TimeOfDayClock {
public:
    float Hour() const { return hour_; }
    void SetHour(float hour) { hour_ = WrapHour(hour); }

    /// Jam permainan per detik nyata. Bawaannya satu hari penuh dalam sepuluh
    /// menit — cukup cepat untuk melihat bayangan berputar tanpa menunggu, cukup
    /// lambat untuk menyetel sebuah saat tertentu.
    float Speed() const { return speed_; }
    void SetSpeed(float hoursPerSecond) { speed_ = hoursPerSecond; }

    bool Playing() const { return playing_; }
    void SetPlaying(bool playing) { playing_ = playing; }

    /// Memajukan jam. Tidak melakukan apa pun saat berhenti — dan itu yang
    /// membuat menyeret slider jam tidak dilawan siklusnya sendiri.
    void Advance(float deltaSeconds);

private:
    float hour_ = 12.0f;
    float speed_ = 24.0f / 600.0f;
    bool playing_ = false;
};

// --- Keadaan atmosfer yang dihasilkan ---------------------------------------

/// Kurva-kurva yang menyusun sebuah preset Time-of-Day.
///
/// Yang ada di sini hanya yang benar-benar tersambung ke sesuatu hari ini.
/// Parameter atmosfer Bruneton — Rayleigh, Mie, ketinggian lapisan — menyusul
/// bersama pass langitnya; menambahkannya sekarang berarti kurva yang tidak ada
/// pembacanya, dan kurva tanpa pembaca adalah kurva yang tidak pernah diuji.
struct TimeOfDayPreset {
    /// Warna matahari, dikalikan intensitasnya.
    TimeOfDayCurve sunColor;
    /// Intensitas matahari. x saja yang dipakai.
    TimeOfDayCurve sunIntensity;
    /// Warna langit di zenit, dipakai sebagai cahaya ambient sampai atmosfer
    /// sungguhan menggantikannya.
    TimeOfDayCurve skyZenith;
    TimeOfDayCurve skyHorizon;

    /// Preset bawaan: satu hari cerah, dari fajar sampai malam.
    static TimeOfDayPreset Default();
};

/// Hasil evaluasi sebuah preset pada satu saat.
struct TimeOfDayState {
    SunPosition sun;
    Vec3 sunRadiance{0.0f};
    Vec3 skyZenith{0.0f};
    Vec3 skyHorizon{0.0f};
    /// 0 saat matahari di bawah horizon, 1 saat sudah naik penuh. Dipakai
    /// meredam matahari saat terbit dan terbenam tanpa memutusnya mendadak.
    float daylight = 0.0f;
};

/// Mengevaluasi preset pada sebuah tempat dan saat.
TimeOfDayState EvaluateTimeOfDay(const TimeOfDayPreset& preset, const SunPlacement& placement);

}  // namespace sim::render
