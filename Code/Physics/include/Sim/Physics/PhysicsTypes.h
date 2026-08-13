#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>

/// Simulasi fisika di atas PhysX 5.
///
/// **Tidak satu pun tipe PhysX muncul di header modul ini.** Yang menyeberang
/// batasnya adalah `Vec3`, `Quat`, dan handle bilangan bulat — bukan
/// `PxRigidActor*`. Aturan yang sama yang menjaga OpenImageIO dan OpenVDB di
/// luar jalur runtime, dan yang sudah terbukti tiga kali di pohon ini: backend
/// gambar berganti pustaka dua kali tanpa satu pun titik panggil berubah.
///
/// Rencananya ada di docs/PLAN-PHYSICS.md.
namespace sim::physics {

/// Rujukan ke sebuah benda di dalam simulasi.
///
/// **Angka, bukan pointer.** Pointer ke aktor PhysX yang bocor ke luar modul ini
/// akan menggantung begitu scene-nya dibangun ulang — dan yang memegangnya tidak
/// punya cara mengetahui itu. Handle yang basi dijawab dengan penolakan yang
/// jelas alih-alih dengan alamat yang kebetulan masih terbaca.
enum class BodyHandle : uint64_t { Invalid = 0 };

/// Bagaimana sebuah benda ikut disimulasikan.
enum class BodyKind : uint8_t {
    /// Tidak pernah bergerak. Lantai, dinding, terrain.
    ///
    /// **Bukan sekadar benda dinamis bermassa tak hingga**: PhysX menyimpan yang
    /// statis di struktur terpisah yang tidak pernah dibangunkan, dan itulah
    /// yang membuat adegan dengan ribuan dinding tetap murah.
    Static,
    /// Digerakkan oleh transform, bukan oleh gaya. Lift, pintu, platform.
    ///
    /// Ia mendorong benda dinamis dan tidak pernah didorong balik — itu yang
    /// diinginkan untuk sesuatu yang jalurnya ditentukan animasi.
    Kinematic,
    /// Digerakkan oleh gaya dan tumbukan.
    Dynamic,
};

/// Bentuk tabrakan sebuah benda.
///
/// **Sengaja lebih sederhana daripada mesh yang digambar.** Itu bukan
/// keterbatasan melainkan gunanya: yang ditabrak peluru dan yang dipijak
/// karakter tidak perlu setiap segitiga hiasan, dan memakai mesh render sebagai
/// bentuk tabrakan adalah cara termahal untuk mendapatkan jawaban yang sama.
enum class ShapeKind : uint8_t {
    Box,
    Sphere,
    Capsule,
    /// Bidang tak hingga. Hanya sah untuk benda statis — bidang tak hingga yang
    /// bergerak tidak punya arti, dan PhysX menolaknya.
    Plane,
};

/// Bentuk tabrakan beserta ukurannya.
///
/// Satu struct untuk semua bentuk, bukan hierarki kelas: bentuknya hanya empat,
/// ukurannya paling banyak tiga angka, dan sebuah `union` di sini akan menukar
/// kejelasan dengan dua belas byte.
struct ShapeDesc {
    ShapeKind kind = ShapeKind::Box;
    /// Setengah-ukuran untuk `Box`. Setengah-tinggi bagian silinder di `x` untuk
    /// `Capsule` — **bukan** tinggi penuhnya, karena itu konvensi PhysX dan
    /// menerjemahkannya di sini berarti dua konvensi untuk satu angka.
    Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    /// Jari-jari untuk `Sphere` dan `Capsule`.
    float radius = 0.5f;

    /// Posisi dan putaran bentuk relatif terhadap benda yang memilikinya.
    Vec3 localPosition{0.0f};
    Quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};
};

/// Sifat permukaan.
struct MaterialDesc {
    float staticFriction = 0.5f;
    float dynamicFriction = 0.5f;
    /// Nol berarti tidak memantul sama sekali; satu berarti memantul setinggi
    /// asalnya. Di atas satu, energinya bertambah setiap pantulan — PhysX
    /// mengizinkannya, dan hampir selalu itu salah ketik.
    float restitution = 0.0f;
};

/// Sebuah benda yang akan ditambahkan ke simulasi.
struct BodyDesc {
    BodyKind kind = BodyKind::Dynamic;
    ShapeDesc shape;
    MaterialDesc material;

    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};

    /// Massa dalam kilogram, untuk benda dinamis. Nol berarti dihitung dari
    /// volume bentuknya dan kerapatan di bawah.
    float mass = 0.0f;
    float density = 1000.0f;

    /// Peredaman kecepatan linear dan sudut per detik.
    float linearDamping = 0.0f;
    float angularDamping = 0.05f;

    /// Benda yang boleh tertidur berhenti disimulasikan saat diam. Dimatikan
    /// untuk benda yang harus bereaksi pada sentuhan sekecil apa pun.
    bool allowSleeping = true;

    /// Lapisan yang ditempati benda ini, dipakai menyaring scene query.
    /// Tipenya `LayerMask` di `PhysicsQuery.h`; disebut sebagai `uint32_t` di
    /// sini supaya header ini tidak perlu menariknya.
    uint32_t layer = 1u;
};

/// Keadaan sebuah benda sesudah satu langkah.
struct BodyState {
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 linearVelocity{0.0f};
    Vec3 angularVelocity{0.0f};
    /// True bila benda sedang tertidur dan tidak ikut dihitung.
    bool sleeping = false;
};

/// Pengaturan sebuah dunia fisika.
struct WorldDesc {
    Vec3 gravity{0.0f, -9.81f, 0.0f};

    /// Panjang satu langkah simulasi, detik.
    ///
    /// **Tetap, dan terpisah dari waktu frame.** Fisika yang berlangkah
    /// mengikuti frame menghasilkan simulasi yang berbeda di mesin yang lebih
    /// cepat — dan "menara balok saya runtuh di laptop tapi tidak di desktop"
    /// adalah laporan bug yang tidak bisa ditindaklanjuti.
    float fixedTimeStep = 1.0f / 60.0f;

    /// Batas langkah yang dikejar dalam satu `Advance`.
    ///
    /// Tanpa batas ini, satu frame yang tersendat panjang memicu puluhan langkah
    /// yang membuat frame berikutnya lebih lambat lagi — spiral yang berakhir
    /// dengan editor yang tampak menggantung. Melewatkan waktu lebih baik
    /// daripada berhenti merespons.
    uint32_t maxStepsPerAdvance = 8;

    /// Ukuran khas benda di adegan, meter. Menentukan toleransi solver.
    ///
    /// Salah menyetelnya tidak muncul sebagai galat melainkan sebagai benda yang
    /// bergetar saat diam, atau jatuh seperti di bulan.
    float typicalLength = 1.0f;
    /// Kecepatan khas benda jatuh, m/s. Bawaannya kecepatan setelah jatuh satu
    /// `typicalLength` di bawah gravitasi bumi.
    float typicalSpeed = 9.81f;

    /// Banyaknya worker thread solver. Nol berarti berjalan di thread pemanggil.
    ///
    /// **Ikut menentukan hasil simulasinya.** PhysX deterministik untuk jumlah
    /// thread yang sama, bukan lintas jumlah thread — jadi angka ini bagian dari
    /// masukan simulasi, bukan sekadar setelan performa.
    uint32_t workerThreads = 0;
};

}  // namespace sim::physics
