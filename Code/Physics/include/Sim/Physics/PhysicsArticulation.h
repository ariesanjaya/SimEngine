#pragma once

#include "Sim/Physics/PhysicsTypes.h"

#include <cstdint>
#include <string>
#include <vector>

/// Rantai benda yang gerakannya diselesaikan sebagai satu sistem.
///
/// **Bedanya dengan rantai sendi biasa bukan kenyamanan, melainkan kekakuan.**
/// Sendi biasa diselesaikan satu per satu, dan galat tiap sendi menumpuk di
/// sepanjang rantai: dua puluh tautan yang digantungi beban akan melar
/// kelihatan, dan menaikkan iterasi solver hanya mengurangi tanpa
/// menghilangkannya. Articulation memakai koordinat tereduksi — yang
/// disimpannya adalah sudut sendi, bukan posisi tiap benda — sehingga rantai
/// tidak punya derajat kebebasan untuk melar. Itulah sebabnya ragdoll dan
/// lengan robot memakainya, dan itu pula yang diuji.
///
/// Harganya: strukturnya tetap. Link tidak bisa ditambah atau dilepas setelah
/// articulation masuk ke scene, jadi bagian yang harus bisa patah tetap perlu
/// sendi biasa.
namespace sim::physics {

/// Rujukan ke sebuah articulation. Angka, bukan pointer.
enum class ArticulationHandle : uint64_t { Invalid = 0 };

/// Derajat kebebasan yang disisakan sendi masuk sebuah link.
enum class ArticulationJointKind : uint8_t {
    /// Terkunci pada induknya. Berguna untuk menyatukan beberapa bentuk menjadi
    /// satu tubuh kaku di tengah rantai.
    Fixed,
    /// Satu putaran. Siku, lutut, engsel.
    Revolute,
    /// Satu geseran. Piston, aktuator linear.
    Prismatic,
    /// Dua putaran ayun. Bahu, pinggul.
    ///
    /// **Bukan tiga.** PhysX menyediakan puntiran sebagai sumbu terpisah, dan
    /// bahu yang bisa berputar bebas pada ketiganya menghasilkan pose yang tidak
    /// mungkin dicapai manusia — ragdoll yang lengannya terpuntir ke belakang
    /// terbaca sebagai simulasi yang rusak, bukan sebagai batas yang lupa
    /// dipasang.
    Spherical,
};

inline constexpr int kArticulationRootParent = -1;

/// Satu tautan beserta sendi yang menghubungkannya ke induknya.
struct ArticulationLinkDesc {
    /// Indeks link induk di dalam `ArticulationDesc::links`, atau
    /// `kArticulationRootParent` untuk akar.
    ///
    /// **Harus lebih kecil daripada indeks link ini sendiri.** Aturan yang sama
    /// dengan `animation::Skeleton`, dan alasannya sama: induk yang selalu
    /// mendahului anaknya membuat pembangunannya satu lintasan maju, dan
    /// membuat siklus mustahil dinyatakan alih-alih harus dideteksi.
    int parent = kArticulationRootParent;

    /// Nama, untuk pesan galat. Kosong boleh.
    std::string name;

    ShapeDesc shape;
    MaterialDesc material;

    /// Pose link pada saat dibangun, ruang dunia.
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};

    float mass = 0.0f;
    float density = 1000.0f;

    /// Diabaikan untuk akar.
    ArticulationJointKind joint = ArticulationJointKind::Spherical;
    /// Bingkai sendi pada induk dan pada link ini, relatif terhadap
    /// masing-masing. Sumbunya +X, seperti sendi biasa.
    Vec3 parentAnchor{0.0f};
    Quat parentFrame{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 childAnchor{0.0f};
    Quat childFrame{1.0f, 0.0f, 0.0f, 0.0f};

    /// Batas gerak. Radian untuk putaran, meter untuk geseran.
    bool limitEnabled = false;
    float lowerLimit = 0.0f;
    float upperLimit = 0.0f;
    /// Batas ayun kedua untuk `Spherical`, radian. Yang pertama memakai
    /// `lowerLimit`/`upperLimit`.
    float swing2Limit = 0.0f;
};

/// Sebuah articulation yang akan dibangun.
struct ArticulationDesc {
    /// Terurut topologis: induk selalu mendahului anaknya. Yang pertama adalah
    /// akar.
    std::vector<ArticulationLinkDesc> links;

    /// Akar dipaku ke dunia.
    ///
    /// Benar untuk lengan robot yang menempel di lantai, salah untuk ragdoll —
    /// dan ragdoll yang akarnya terpaku menggantung di udara alih-alih jatuh.
    bool fixBase = false;

    /// Iterasi solver. **Lebih tinggi daripada bawaan scene dengan sengaja:**
    /// rantai panjang adalah justru kasus yang menuntutnya, dan articulation
    /// yang bergetar hampir selalu kekurangan iterasi posisi.
    uint32_t solverPositionIterations = 8;
    uint32_t solverVelocityIterations = 1;

    /// Boleh tertidur saat diam.
    bool allowSleeping = true;

    /// Link yang tidak bertetangga saling menabrak.
    ///
    /// **Mati secara bawaan, dan itu bukan jalan pintas.** Bentuk yang disusun
    /// otomatis dari sebuah rangka pasti bertumpuk di percabangan — dua paha
    /// yang berjarak 20 cm dengan jari-jari 10,5 cm sudah saling menembus
    /// sebelum langkah pertama, dan solver menjawabnya dengan impuls yang
    /// melempar tubuhnya. Menyalakannya menuntut bentuk yang disetel tangan.
    ///
    /// Pasangan induk–anak selalu dikecualikan PhysX, menyala atau tidak.
    bool selfCollision = false;

    /// Kekakuan tambahan pada sendi. Nol berarti sendi bebas berputar dalam
    /// batasnya; di atas nol ia melawan gerakan, yang membuat ragdoll jatuh
    /// seperti tubuh yang masih punya tonus otot alih-alih seperti karung.
    float jointFriction = 0.05f;
};

// --- ragdoll -----------------------------------------------------------------

/// Satu bone rangka, sebagaimana yang dibutuhkan pembangun ragdoll.
///
/// **Sengaja bukan `animation::Skeleton`.** Membuat `Sim::Physics` melihat
/// `Sim::Animation` berarti setiap yang menautkan fisika ikut menautkan importir
/// FBX dan USD di baliknya — dan server dedicated yang hanya perlu
/// mensimulasikan tidak punya alasan membawa keduanya. Yang menyeberang batas
/// ini cuma angka, dan mengubah `Skeleton` menjadi daftar ini adalah satu
/// lintasan pendek yang dimiliki pemanggil.
struct RagdollBone {
    std::string name;
    int parent = -1;
    /// Pose bind **global**, ruang dunia. Bukan relatif terhadap induk:
    /// pembangun butuh posisi sesungguhnya untuk mengukur panjang tulang, dan
    /// meminta pemanggil menyediakannya menghindari dua tempat yang menghitung
    /// hal yang sama dengan cara berbeda.
    Vec3 position{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
};

/// Bagaimana rangka diterjemahkan menjadi tubuh.
struct RagdollSettings {
    /// Tulang yang lebih pendek dari ini tidak mendapat bentuk sendiri dan
    /// disatukan ke induknya.
    ///
    /// **Rig sungguhan penuh dengan tulang sepanjang nol** — bone helper,
    /// twist bone, penanda. Memberi masing-masing sebuah kapsul menghasilkan
    /// ragdoll berisi puluhan bentuk yang saling menembus, dan itulah bentuk
    /// paling umum dari "ragdoll yang meledak pada langkah pertama".
    float minBoneLength = 0.05f;

    /// Jari-jari kapsul sebagai pecahan panjang tulangnya, dibatasi keduanya.
    float radiusRatio = 0.25f;
    float minRadius = 0.02f;
    float maxRadius = 0.12f;

    /// Batas ayun dan puntir bawaan, radian.
    float swingLimit = 0.7f;   // ~40°
    float twistLimit = 0.4f;   // ~23°

    /// Massa seluruh tubuh, kilogram. Dibagikan ke tiap link menurut volumenya.
    ///
    /// **Massa total, bukan kerapatan.** Kerapatan terdengar lebih fisis, tetapi
    /// ia menyerahkan massa tiap bagian kepada bentuk yang disusun otomatis —
    /// dan bentuk itu tidak tahu apa-apa tentang anatomi. Pada rig Mixamo, tulang
    /// pinggul yang pendek menghasilkan kapsul berjari-jari 2,8 cm sementara paha
    /// menghasilkan 10,2 cm: **akar tubuh menjadi 50 kali lebih ringan daripada
    /// anaknya**, dan articulation dengan perbandingan sebesar itu di sendinya
    /// tidak sekadar bergoyang melainkan menghasilkan koordinat NaN — terukur di
    /// langkah ke-55 sebelum pembagian ini dipakai.
    float totalMass = 70.0f;

    /// Batas seberapa jauh massa sebuah link boleh menyimpang dari rata-rata,
    /// sebagai kelipatannya.
    ///
    /// Yang dijaga bukan realisme melainkan **perbandingan antar-link**: solver
    /// menyelesaikan rantai, dan rantai yang salah satu mata rantainya nyaris tak
    /// bermassa tidak punya jawaban yang stabil. Proporsinya tetap mengikuti
    /// volume di dalam rentang ini.
    float minMassRatio = 0.5f;
    float maxMassRatio = 2.5f;

    float jointFriction = 0.05f;
};

/// Menyusun `ArticulationDesc` dari sebuah rangka.
///
/// Mengembalikan desc kosong bila rangkanya kosong atau tidak terurut topologis.
/// `outSkipped` menerima nama tulang yang dilewati karena terlalu pendek —
/// dilaporkan, bukan didiamkan, karena ragdoll yang kehilangan tungkai terbaca
/// sebagai bug simulasi.
ArticulationDesc BuildRagdoll(const std::vector<RagdollBone>& bones,
                              const RagdollSettings& settings = {},
                              std::vector<std::string>* outSkipped = nullptr);

}  // namespace sim::physics
