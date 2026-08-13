#pragma once

#include "Sim/Physics/PhysicsTypes.h"

#include <cstddef>
#include <cstdint>
#include <vector>

/// Kendaraan beroda: bodi tegar tunggal ditambah roda yang gayanya dihitung
/// tersendiri.
///
/// **Rodanya bukan benda tegar, dan itu keputusan PhysX yang perlu diketahui.**
/// Sebuah mobil yang dibangun dari lima rigid body yang disendi akan bergetar,
/// melar, dan menembus tanah pada kecepatan tinggi — yang disimulasikan
/// `PxVehicle` adalah **satu** benda tegar, sementara tiap roda diwakili sebuah
/// derajat kebebasan putar, sebuah pegas suspensi, dan model ban yang
/// menghasilkan gaya. Konsekuensi praktisnya: roda tidak punya `BodyHandle`,
/// tidak muncul di scene query, dan posenya dibaca lewat `VehicleState`.
///
/// Yang dikerjakan di sini adalah **direct drive**: torsi masuk langsung ke
/// roda. Mesin, kopling, dan girboks (`PxVehicleEngineDrive`) adalah lapisan di
/// atasnya yang belum dipasang — lihat docs/PLAN-PHYSICS.md.
namespace sim::physics {

/// Rujukan ke sebuah kendaraan. Angka, bukan pointer.
enum class VehicleHandle : uint64_t { Invalid = 0 };

/// Bagaimana torsi sampai ke roda.
enum class VehicleDriveModel : uint8_t {
    /// Torsi masuk langsung ke roda, tetap berapa pun laju rodanya.
    ///
    /// **Cukup untuk banyak permainan, dan tidak cukup untuk mobil.** Torsi yang
    /// tidak pernah turun di putaran tinggi membuat roda penggerak selip terus —
    /// terukur 39% pada 53 m/s — dan mobil terasa seperti didorong tangan
    /// raksasa alih-alih dikendarai.
    DirectDrive,
    /// Mesin dengan kurva torsi, kopling, dan girboks bergigi.
    ///
    /// Torsinya turun di putaran tinggi dan naik lagi setiap kali gigi berpindah,
    /// yang justru itulah yang membuat mobil terasa punya mesin.
    EngineDrive,
};

/// Mesin, kopling, dan girboks. Dipakai hanya pada `EngineDrive`.
struct VehicleEngineDesc {
    /// Torsi puncak mesin, N·m — bukan torsi di roda: girboks mengalikannya.
    float peakTorque = 500.0f;
    /// Putaran mesin, rad/s. Idle ~1000 rpm, redline ~6000 rpm.
    float idleSpeed = 105.0f;
    float maxSpeed = 630.0f;
    /// Momen inersia poros engkol, kg·m². Kecil berarti mesin melonjak; besar
    /// berarti ia berat dan lamban.
    float momentOfInertia = 1.0f;

    /// Perbandingan gigi, dari mundur ke gigi tertinggi. **Netral disebutkan
    /// eksplisit** sebagai indeks di dalam daftar, karena PhysX menuntutnya dan
    /// menebaknya berarti girboks yang tidak pernah bisa dinetralkan.
    std::vector<float> gearRatios{-4.0f, 0.0f, 4.0f, 2.5f, 1.6f, 1.15f, 1.0f};
    std::size_t neutralGear = 1;
    /// Dikalikan ke setiap gigi. Ini yang menerjemahkan putaran mesin menjadi
    /// putaran roda.
    float finalRatio = 4.0f;
    /// Lama perpindahan gigi, detik. Selama itu tidak ada torsi yang lewat.
    float gearSwitchTime = 0.5f;

    /// Girboks otomatis berpindah naik di atas pecahan putaran maksimum ini, dan
    /// turun di bawahnya.
    float upshiftFraction = 0.65f;
    float downshiftFraction = 0.5f;
    /// Jeda minimum antar perpindahan, detik. Tanpa ini girboks berpindah
    /// bolak-balik di ambang batasnya.
    float autoboxLatency = 2.0f;

    /// Kekuatan kopling. Terlalu kecil membuat mesin meraung tanpa mobil
    /// bergerak; terlalu besar membuatnya mati saat berhenti.
    float clutchStrength = 10.0f;
};

/// Satu roda beserta suspensinya.
struct VehicleWheelDesc {
    /// Titik pusat roda pada posisi istirahat, relatif terhadap titik asal
    /// chassis.
    Vec3 centerOffset{0.0f};

    float radius = 0.35f;
    float width = 0.25f;
    float mass = 20.0f;

    /// Momen inersia roda. Nol berarti dihitung dari massa dan jari-jarinya.
    ///
    /// Terlalu kecil membuat roda berputar liar begitu gas disentuh; terlalu
    /// besar membuat mobil terasa seperti menyeret beton. Bawaannya dihitung.
    float momentOfInertia = 0.0f;

    bool steered = false;
    bool driven = true;
    /// Ikut dikunci rem tangan.
    bool handbraked = false;

    /// Jarak tempuh suspensi, meter.
    float suspensionTravel = 0.3f;
    /// Kekakuan pegas, N/m. **Bawaannya sengaja dihitung dari massa**, bukan
    /// angka tetap: pegas yang benar untuk sedan membuat truk duduk di
    /// bempernya. Nol berarti dihitung.
    float springStrength = 0.0f;
    /// Redaman, N·s/m. Nol berarti dihitung dari kekakuan supaya suspensinya
    /// **teredam kritis** — yang tidak berosilasi, dan itu kriteria terima P6.
    float springDamperRate = 0.0f;
};

/// Sebuah kendaraan yang akan dibangun.
struct VehicleDesc {
    Vec3 chassisHalfExtents{0.9f, 0.5f, 2.2f};
    float chassisMass = 1500.0f;

    /// Geseran titik berat terhadap pusat chassis.
    ///
    /// **Hampir selalu di bawah pusat geometrinya.** Titik berat setinggi pusat
    /// kotak membuat mobil terguling di tikungan pertama, dan itu terbaca
    /// sebagai fisika yang salah alih-alih sebagai satu angka yang lupa disetel.
    Vec3 centerOfMassOffset{0.0f, -0.35f, 0.15f};

    /// Empat roda untuk mobil biasa; urutannya bebas, tetapi `steered` dan
    /// `driven` menentukan perannya.
    std::vector<VehicleWheelDesc> wheels;

    /// Sudut belok maksimum roda depan, radian.
    float maxSteerAngle = 0.6f;
    /// Torsi puncak yang dibagikan ke roda penggerak, N·m.
    float peakDriveTorque = 1500.0f;
    /// Torsi rem per roda, N·m.
    float maxBrakeTorque = 5000.0f;
    float maxHandbrakeTorque = 8000.0f;

    /// Gesekan ban terhadap permukaan. Satu berarti aspal kering.
    float tireFriction = 1.0f;

    VehicleDriveModel driveModel = VehicleDriveModel::DirectDrive;
    /// Diabaikan pada `DirectDrive`.
    VehicleEngineDesc engine;

    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

/// Perintah pengemudi, semuanya 0..1 kecuali kemudi.
struct VehicleInput {
    float throttle = 0.0f;
    float brake = 0.0f;
    /// -1 penuh ke kiri, +1 penuh ke kanan.
    float steer = 0.0f;
    float handbrake = 0.0f;
    /// Maju atau mundur. Direct drive tidak punya girboks; ini yang membalik
    /// arah torsinya.
    bool reverse = false;
};

struct VehicleWheelState {
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    /// Seberapa jauh suspensi tertekan, 0 (menjulur penuh) sampai 1 (mentok).
    float suspensionCompression = 0.0f;
    /// Kecepatan putar roda, rad/s.
    float rotationSpeed = 0.0f;
    bool onGround = false;
};

struct VehicleState {
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 linearVelocity{0.0f};
    /// Laju sepanjang arah hadap kendaraan, m/s. Negatif berarti mundur.
    float forwardSpeed = 0.0f;

    /// Putaran mesin, rad/s. Nol pada `DirectDrive` — di sana tidak ada mesin
    /// untuk dilaporkan, dan angka yang dikarang lebih buruk daripada nol yang
    /// jujur.
    float engineSpeed = 0.0f;
    /// Gigi yang sedang dipakai, indeks di `VehicleEngineDesc::gearRatios`.
    std::size_t gear = 0;

    std::vector<VehicleWheelState> wheels;
};

}  // namespace sim::physics
