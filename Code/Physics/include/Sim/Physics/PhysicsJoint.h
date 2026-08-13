#pragma once

#include "Sim/Physics/PhysicsTypes.h"

#include <cstdint>

/// Sendi: dua benda yang gerakannya saling dibatasi.
///
/// **Yang dibatasi adalah derajat kebebasan, bukan posisi.** Sebuah engsel tidak
/// "menahan pintu di tempatnya" melainkan mencabut lima dari enam derajat
/// kebebasannya dan menyisakan satu putaran. Cara memandangnya begitu membuat
/// daftar di bawah ini bukan lima fitur terpisah melainkan satu mekanisme dengan
/// lima setelan — dan itu persis yang dilakukan `D6`, yang menyatakan keempat
/// lainnya.
namespace sim::physics {

/// Rujukan ke sebuah sendi di dalam simulasi. Angka, bukan pointer, dengan
/// alasan yang sama seperti `BodyHandle`.
enum class JointHandle : uint64_t { Invalid = 0 };

/// Derajat kebebasan yang disisakan sebuah sendi.
enum class JointKind : uint8_t {
    /// Tidak menyisakan apa pun: kedua benda bergerak sebagai satu.
    ///
    /// **Bukan pengganti menggabungkan mesh.** Dua benda yang dipaku tetap dua
    /// benda yang solvernya rukunkan tiap langkah, dan seratus di antaranya jauh
    /// lebih mahal daripada satu benda berbentuk sama. Gunanya adalah yang
    /// nantinya akan patah — dan di situ ia tak tergantikan.
    Fixed,
    /// Satu putaran mengelilingi satu sumbu. Engsel pintu, poros roda, bandul.
    Revolute,
    /// Satu geseran sepanjang satu sumbu. Laci, piston, lift.
    Prismatic,
    /// Tiga putaran, tanpa geseran. Sendi bola: bahu, rantai, tali.
    Spherical,
    /// Keenam derajat kebebasan diatur satu per satu.
    ///
    /// Yang paling luwes sekaligus **yang paling mudah dipakai salah**: sendi D6
    /// yang seluruh sumbunya dibiarkan bebas bukan sendi sama sekali, dan itu
    /// tidak terbaca sebagai galat melainkan sebagai dua benda yang saling
    /// mengabaikan. Empat yang di atas ada justru supaya yang lazim tidak perlu
    /// lewat sini.
    D6,
};

/// Batas gerak sebuah sendi.
///
/// **Tidak aktif secara bawaan**, dan itu disengaja: engsel tanpa batas berayun
/// penuh, yang salah tetapi terlihat jelas. Engsel yang batasnya kebetulan nol
/// tidak bergerak sama sekali, dan yang terlihat adalah sendi yang seolah rusak.
struct JointLimit {
    bool enabled = false;
    /// Radian untuk `Revolute`, meter untuk `Prismatic`, radian setengah-sudut
    /// kerucut untuk `Spherical`.
    float lower = 0.0f;
    float upper = 0.0f;

    /// Nol berarti batas keras — benda berhenti mendadak di batasnya. Di atas
    /// nol, batasnya berpegas dengan kekakuan ini, dan benda memantul balik.
    float stiffness = 0.0f;
    float damping = 0.0f;
};

/// Sumbu yang dibatasi, untuk `D6`.
struct D6Axes {
    /// Geseran sepanjang X, Y, Z.
    bool freeLinearX = false;
    bool freeLinearY = false;
    bool freeLinearZ = false;
    /// Putaran mengelilingi X, Y, Z.
    bool freeAngularX = false;
    bool freeAngularY = false;
    bool freeAngularZ = false;
};

/// Sebuah sendi yang akan ditambahkan ke simulasi.
struct JointDesc {
    JointKind kind = JointKind::Fixed;

    /// Kedua ujungnya. **`bodyA` boleh `Invalid`**, dan artinya bukan galat: ujung
    /// itu menjadi dunia, sehingga `bodyB` tergantung pada titik tetap di ruang.
    /// Itulah cara bandul dan pintu dipasang — tanpa ini setiap adegan harus
    /// menyediakan benda statis pura-pura hanya untuk digantungi.
    BodyHandle bodyA = BodyHandle::Invalid;
    BodyHandle bodyB = BodyHandle::Invalid;

    /// Bingkai sendi pada masing-masing benda, relatif terhadap benda itu.
    ///
    /// **Sumbu sendi adalah +X bingkai ini**, mengikuti konvensi PhysX: engsel
    /// berputar mengelilingi X lokalnya, dan prismatic bergeser sepanjang X
    /// lokalnya. Yang menginginkan engsel bersumbu Y memutar bingkainya, bukan
    /// mengganti sumbunya — karena tidak ada "sumbunya" untuk diganti.
    Vec3 localAnchorA{0.0f};
    Quat localRotationA{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 localAnchorB{0.0f};
    Quat localRotationB{1.0f, 0.0f, 0.0f, 0.0f};

    JointLimit limit;
    D6Axes d6;

    /// Kedua benda saling menabrak atau saling menembus.
    ///
    /// Bawaannya tidak: benda yang disendi hampir selalu bersinggungan di
    /// sendinya, dan membiarkan mereka bertabrakan membuat solver mendorongnya
    /// menjauh sementara sendinya menariknya kembali — bergetar yang terlihat
    /// seperti sendi yang rusak.
    bool collisionEnabled = false;

    /// Gaya dan torsi yang membuatnya patah. Nol berarti tidak pernah patah.
    float breakForce = 0.0f;
    float breakTorque = 0.0f;
};

/// Keadaan sebuah sendi sesudah satu langkah.
struct JointState {
    /// False bila sendinya sudah patah karena melampaui `breakForce`/`breakTorque`.
    bool alive = true;
    /// Sudut sekarang untuk `Revolute` (radian), atau geseran untuk `Prismatic`
    /// (meter). Nol untuk jenis lain.
    float position = 0.0f;
};

}  // namespace sim::physics
