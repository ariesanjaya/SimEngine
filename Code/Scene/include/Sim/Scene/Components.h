#pragma once

#include "Sim/Core/AssetRef.h"
#include "Sim/Core/Math.h"
#include "Sim/Core/Uuid.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sim::scene {

/// Pengenal entity di dalam satu sesi.
///
/// Bukan yang tertulis di berkas level — itu tugas IdComponent::guid. Indeks
/// ini dipakai ulang setelah entity dihapus, jadi menyimpannya ke disk akan
/// membuat referensi menunjuk objek yang salah setelah level dimuat ulang.
enum class Entity : uint32_t { Null = 0xFFFFFFFFu };

inline constexpr Entity kNullEntity = Entity::Null;
inline constexpr bool IsValid(Entity entity) {
    return entity != kNullEntity;
}

// --- komponen inti: selalu ada di setiap entity -----------------------------

/// Pengenal yang bertahan lintas-sesi. Inilah yang dirujuk berkas level,
/// prefab, dan (nanti) tool MCP.
struct IdComponent {
    Uuid guid;
};

struct NameComponent {
    std::string name;
};

/// Hubungan induk-anak. Tidak dipantulkan: strukturnya diserialisasi lewat
/// GUID induk pada tiap entity, bukan sebagai field biasa.
struct HierarchyComponent {
    Entity parent = kNullEntity;
    std::vector<Entity> children;
};

struct TransformComponent {
    Vec3 position{0.0f, 0.0f, 0.0f};
    /// Disimpan sebagai quaternion, bukan sudut Euler: Euler punya gimbal lock
    /// dan urutan putarannya ambigu. Inspector tetap menampilkan derajat.
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f, 1.0f, 1.0f};

    Mat4 LocalMatrix() const;
};

/// Cache matriks dunia. Bukan komponen yang dipantulkan maupun diserialisasi —
/// seluruh isinya bisa dihitung ulang dari TransformComponent dan hierarki.
struct TransformCache {
    Mat4 world{1.0f};
    bool dirty = true;
};

// --- komponen opsional ------------------------------------------------------

struct VisibilityComponent {
    bool visible = true;
    /// Terkunci: masih terlihat, tapi tidak bisa dipilih di viewport.
    bool locked = false;
};

struct StaticFlagComponent {
    bool isStatic = false;
};

/// Menggambar sebuah aset mesh.
///
/// **Tidak ada lagi warna dasar di sini.** Ia dulu penambal — satu-satunya cara
/// sebuah adegan bisa punya permukaan berbeda warna sebelum material sungguhan
/// ada — dan begitu berkas mesh membawa materialnya sendiri, membiarkannya
/// mengalikan warna material berarti Inspector punya dua tempat yang mengatur
/// hal yang sama, sementara membiarkannya diam-diam tidak berpengaruh adalah
/// antarmuka yang berbohong. Warna sekarang datang dari material: yang tertulis
/// di berkas mesh untuk tiap ruasnya, `material` di bawah untuk yang menimpanya
/// seluruhnya, dan material bawaan editor untuk ruas yang tidak punya keduanya.
struct MeshRendererComponent {
    /// Rujukan berisi GUID, bukan nama berkas. Itulah yang membuat mengganti
    /// nama atau memindahkan mesh tidak menyentuh satu pun berkas level.
    AssetRef mesh;
    /// Satu slot per ruas mesh, sesuai urutan `assets::MeshData::parts`.
    ///
    /// **Sebuah daftar, bukan satu rujukan.** Sebuah berkas mesh kerap membawa
    /// lebih dari satu material — rig Mixamo dua, shader ball lima — dan satu
    /// rujukan untuk seluruh mesh berarti kelima ruasnya terpaksa memakai
    /// material yang sama. Slot kosong diisi material bawaan editor; daftar yang
    /// lebih pendek daripada jumlah ruas diperlakukan sama, jadi menambah
    /// material di berkasnya tidak merusak level yang sudah ada.
    std::vector<AssetRef> materials;
    bool castShadows = true;
    bool receiveShadows = true;
    /// Menggeser pemilihan tingkat detail: negatif memilih mesh yang lebih
    /// rinci lebih lama, positif lebih cepat turun. Dipakai renderer di E8.
    float lodBias = 0.0f;
};

/// Memutar sebuah klip animasi pada mesh ber-rig entity ini.
///
/// **Rangkanya tidak dirujuk, dan itu disengaja.** Rangka datang dari aset mesh
/// di `MeshRendererComponent` — ia satu-satunya rangka yang indeks bone-nya
/// cocok dengan bobot skin yang sudah ada di GPU. Rujukan rangka tersendiri
/// berarti dua sumber yang bisa berselisih, dan selisihnya muncul sebagai kulit
/// yang mengikuti tulang yang salah, bukan sebagai galat.
struct AnimatorComponent {
    /// Klipnya. Boleh `.simanim` maupun `.fbx` — yang kedua diimpor saat itu
    /// juga, sehingga sebuah berkas Mixamo bisa dipasang tanpa langkah impor
    /// tersendiri lebih dulu.
    AssetRef clip;
    /// Berjalan di editor, bukan hanya saat Play. Animasi yang hanya bergerak di
    /// Play adalah animasi yang tidak bisa disetel — dan menyetelnya justru yang
    /// dilakukan orang di editor.
    bool playing = true;
    bool loop = true;
    /// Pengali kecepatan. Negatif memutar mundur.
    float speed = 1.0f;

    /// Waktu pemutaran, detik. **Keadaan runtime, tidak ikut disimpan**: waktu
    /// yang ikut ke berkas level berarti setiap frame yang berjalan mengotori
    /// level yang belum disunting siapa pun, dan tanda "ada perubahan belum
    /// disimpan" yang menyala sendiri adalah tanda yang berhenti berarti.
    float time = 0.0f;
};

enum class LightType : uint8_t {
    Directional = 0,
    Point = 1,
    Spot = 2,
};

/// Menautkan entity ke sebuah skrip Lua.
///
/// Rujukannya AssetRef, sama seperti mesh dan material: mengganti nama berkas
/// skrip tidak boleh memutus entity mana pun.
enum class ScriptPropertyKind : uint8_t {
    Number = 0,
    Bool = 1,
    Text = 2,
};

/// Satu properti yang diekspos sebuah skrip ke Inspector.
///
/// Ketiga nilainya disimpan berdampingan alih-alih dalam union: komponen ini
/// diserialisasi lewat reflection, dan reflection tidak punya cara menyatakan
/// "field mana yang sah tergantung field lain". Biayanya beberapa byte per
/// properti; imbalannya adalah properti yang tipenya berubah di skrip tidak
/// kehilangan nilai lamanya begitu saja ketika pengguna mengembalikannya.
struct ScriptProperty {
    std::string name;
    ScriptPropertyKind kind = ScriptPropertyKind::Number;
    float number = 0.0f;
    bool flag = false;
    std::string text;
};

struct ScriptComponent {
    AssetRef script;
    /// True saat skripnya sudah dimuat dan siap dipanggil. Diisi runtime, bukan
    /// pengguna — karena itu ditandai ReadOnly di Inspector.
    bool loaded = false;

    /// Nilai properti yang diekspos skrip. Yang tersimpan di sini adalah nilai
    /// untuk entity INI; bawaannya tetap tinggal di berkas skrip. Karena itu
    /// mengubah bawaan di skrip tidak menyentuh entity yang sudah punya nilai
    /// sendiri, dan entity yang belum pernah disunting ikut berubah.
    ///
    /// Digambar Inspector secara khusus, bukan lewat grid generik — daftar
    /// struct yang bisa ditambah-kurangi bukan yang dibutuhkan di sini.
    std::vector<ScriptProperty> properties;
};

/// Menautkan entity ke sebuah graph visual scripting.
///
/// Sejajar dengan ScriptComponent, dan itu bukan kebetulan: graph dikompilasi
/// menjadi Lua yang bentuknya sama persis dengan skrip tulis tangan, jadi yang
/// membedakan keduanya hanya berkas yang dirujuk. Propertinya pun menempuh
/// jalur yang sama — kompiler membangkitkan deklarasi `properties` biasa dari
/// variabel graph yang diekspos.
struct GraphComponent {
    AssetRef graph;
    /// True saat graph-nya sudah dikompilasi dan dimuat. Diisi runtime.
    bool loaded = false;
    std::vector<ScriptProperty> properties;
};

struct LightComponent {
    LightType type = LightType::Point;
    Vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    /// Meter. Tidak berarti untuk lampu directional.
    float range = 10.0f;

    /// Jari-jari sumber cahaya, meter. Tidak berarti untuk lampu directional.
    ///
    /// **Peredupan kuadrat terbalik meledak di jarak nol, dan jari-jari inilah
    /// batasnya**: cahaya tidak pernah lebih dekat daripada permukaan lampunya
    /// sendiri. Sebelum ini angkanya ada tapi tersembunyi — renderer menjepit
    /// jarak ke 1 cm lewat konstanta di dalam shader, jadi lampu sebesar apa pun
    /// tetap berperilaku seperti titik seukuran kelereng dan tidak ada yang bisa
    /// mengubahnya.
    ///
    /// Bawaannya sama dengan konstanta yang dulu tersembunyi itu, jadi setiap
    /// lampu yang sudah ada tampak persis seperti sebelumnya.
    float sourceRadius = 0.01f;
    float innerAngleRadians = 0.436f;  // 25°
    float outerAngleRadians = 0.611f;  // 35°
    bool castShadows = true;
};

/// Dari mana langit datang. Cerminan `render::SkySource`, di sisi scene.
///
/// **Disalin alih-alih dipakai bersama** karena `Sim::Scene` tidak boleh melihat
/// `Sim::Render` — itu batas modul yang sama yang menjaga scene bisa dimuat
/// tanpa satu pun perangkat grafis. Pemetaannya terjadi sekali, di editor.
enum class SkySourceKind : uint8_t {
    Atmosphere,
    HdrMap,
};

/// Langit sebuah level.
///
/// **Keberadaannya yang menyalakan langit.** Level tanpa entity ber-komponen ini
/// tidak menggambar langit sama sekali — dan itu bukan kekurangan melainkan
/// yang diminta: tidak semua permainan punya langit. Yang di bawah tanah, yang
/// di dalam kapal, yang seluruhnya interior, semuanya membayar empat pass LUT
/// untuk sesuatu yang tak pernah terlihat kalau langit selalu menyala.
///
/// **Pengaturannya ikut di dalam level**, bukan di viewport. Langit adalah
/// bagian dari adegan seperti lampu matahari: memindahkannya ke pengaturan
/// editor berarti level yang sama terlihat berbeda di dua mesin, dan tidak ada
/// yang tercatat di berkasnya tentang mana yang benar.
struct SkyComponent {
    SkySourceKind source = SkySourceKind::Atmosphere;
    /// Pengali radiansi langit atmosferik.
    float intensity = 20.0f;
    /// Ketinggian kamera di atas permukaan planet, kilometer. Menentukan warna
    /// cakrawala dan seberapa tebal udara yang dilihat.
    float cameraHeightKm = 0.5f;
    /// Kabut jarak jauh. Terpisah dari langit karena biayanya pass tersendiri.
    bool aerialPerspective = true;
    float aerialHaze = 1.0f;

    /// Berkas HDR, dipakai saat `source` adalah `HdrMap`.
    std::string hdriPath;
    float hdriRotation = 0.0f;
    /// Pengalinya sendiri, bukan `intensity`: berkas HDR sudah berisi radiansi,
    /// jadi rentang yang bergunanya di sekitar satu.
    float hdriIntensity = 1.0f;
};

/// Bagaimana sebuah entity ikut disimulasikan. Cerminan `physics::BodyKind`.
///
/// **Disalin alih-alih dipakai bersama**, dengan alasan yang persis sama dengan
/// `SkySourceKind` di atas: yang di sini adalah maksud yang ditulis pengguna dan
/// disimpan ke berkas level, yang di sana adalah masukan solver. Keduanya
/// kebetulan mirip sekarang dan tidak akan selamanya — `physics::ShapeDesc`
/// nanti menerima convex hull yang sudah dimasak, sementara yang ditulis di
/// Inspector tetap sebuah rujukan aset. Pemetaannya terjadi sekali, di
/// `PhysicsScene`.
enum class RigidBodyKind : uint8_t {
    Static,
    Kinematic,
    Dynamic,
};

/// Menjadikan entity benda tegar dalam simulasi.
///
/// **Tidak berguna sendirian.** Tanpa `ColliderComponent` di entity yang sama,
/// benda ini tidak punya bentuk — ia akan jatuh menembus segalanya. `PhysicsScene`
/// melewatinya dan mengatakan alasannya di log, alih-alih membuat benda tak
/// kasatmata yang perilakunya tidak bisa dijelaskan.
struct RigidBodyComponent {
    RigidBodyKind kind = RigidBodyKind::Dynamic;

    /// Massa dalam kilogram. **Nol berarti dihitung dari bentuk dan `density`**,
    /// dan itu bawaannya karena hampir selalu yang benar: massa yang diketik
    /// tangan mudah tidak cocok dengan ukuran bendanya, dan peti sebesar lemari
    /// yang beratnya dua kilogram bergerak dengan cara yang salah tanpa satu pun
    /// pesan yang menyebutkannya.
    float mass = 0.0f;
    /// Kerapatan kg/m³ saat massa dihitung. Bawaannya air; kayu ~700, baja ~7800.
    float density = 1000.0f;

    float linearDamping = 0.0f;
    float angularDamping = 0.05f;

    /// Benda yang boleh tertidur berhenti dihitung saat diam. Dimatikan untuk
    /// yang harus bereaksi pada sentuhan sekecil apa pun.
    bool allowSleeping = true;
};

/// Bentuk tabrakan. Cerminan `physics::ShapeKind`.
enum class ColliderShape : uint8_t {
    Box,
    Sphere,
    Capsule,
    /// Bidang tak hingga; hanya masuk akal untuk benda statis.
    Plane,
};

/// Bentuk yang ditabrak, terpisah dari mesh yang digambar.
///
/// **Terpisah, dan itu gunanya.** Yang dipijak karakter dan yang ditembus peluru
/// tidak perlu setiap segitiga hiasan; memakai mesh render sebagai bentuk
/// tabrakan adalah cara termahal untuk mendapatkan jawaban yang sama.
struct ColliderComponent {
    ColliderShape shape = ColliderShape::Box;

    /// Setengah-ukuran untuk `Box`.
    Vec3 halfExtents{0.5f, 0.5f, 0.5f};
    /// Jari-jari untuk `Sphere` dan `Capsule`.
    float radius = 0.5f;
    /// Setengah-tinggi **bagian silinder** kapsul, tidak termasuk kedua
    /// tudungnya. Kapsul setinggi 2 m berjari-jari 0,3 m karena itu bernilai
    /// 0,7 — konvensi PhysX, disebutkan di sini supaya tidak perlu ditebak.
    float halfHeight = 0.5f;

    /// Geseran bentuk terhadap titik asal entity. Berguna untuk kapsul karakter
    /// yang asalnya di telapak kaki, bukan di pinggang.
    Vec3 offset{0.0f, 0.0f, 0.0f};

    float staticFriction = 0.5f;
    float dynamicFriction = 0.5f;
    /// Nol tidak memantul; satu memantul setinggi asalnya. Di atas satu energinya
    /// bertambah tiap pantulan — sah menurut PhysX, dan hampir selalu salah ketik.
    float restitution = 0.0f;
};

/// Derajat kebebasan yang disisakan sebuah sendi. Cerminan `physics::JointKind`.
enum class JointType : uint8_t {
    Fixed,
    Revolute,
    Prismatic,
    Spherical,
    D6,
};

/// Menyendikan entity ini dengan entity lain — atau dengan dunia.
///
/// **Rujukannya GUID, bukan `Entity`.** Indeks entity dipakai ulang setelah
/// entity dihapus, jadi sendi yang menyimpannya akan menunjuk benda yang salah
/// begitu level dimuat ulang. GUID justru yang tertulis di berkas level.
///
/// **Sendinya milik entity yang bergerak, bukan yang menahan.** Bandul membawa
/// komponen ini dan menunjuk porosnya, bukan sebaliknya. Aturan itu perlu
/// disebutkan karena keduanya sama masuk akal dilihat dari luar, sedangkan yang
/// terbalik membuat menghapus bandul meninggalkan poros dengan sendi ke sesuatu
/// yang tidak ada.
struct JointComponent {
    JointType type = JointType::Fixed;

    /// Entity di ujung satunya. **Kosong berarti dunia**, dan itu bukan galat:
    /// pintu dan bandul memang tergantung pada titik tetap di ruang, dan tanpa
    /// ini setiap adegan harus menyediakan benda statis pura-pura untuk
    /// digantungi.
    Uuid connectedBody;

    /// Titik sendi pada entity ini, ruang lokalnya.
    Vec3 anchor{0.0f, 0.0f, 0.0f};
    /// Putaran bingkai sendi. **Sumbunya adalah +X bingkai ini** — engsel
    /// berputar mengelilingi X lokalnya, prismatic bergeser sepanjang X
    /// lokalnya. Yang menginginkan sumbu lain memutar bingkainya.
    Quat frame{1.0f, 0.0f, 0.0f, 0.0f};

    bool limitEnabled = false;
    /// Radian untuk Revolute, meter untuk Prismatic, setengah-sudut kerucut
    /// untuk Spherical.
    float lowerLimit = 0.0f;
    float upperLimit = 0.0f;

    /// Kedua benda saling menabrak. Bawaannya tidak: benda yang disendi hampir
    /// selalu bersinggungan di sendinya, dan membiarkannya bertabrakan membuat
    /// solver mendorongnya menjauh sementara sendinya menarik kembali.
    bool collisionEnabled = false;

    /// Nol berarti tidak pernah patah.
    float breakForce = 0.0f;
    float breakTorque = 0.0f;
};

struct CameraComponent {
    float fovYRadians = 1.047f;  // 60°
    float nearZ = 0.05f;
    float farZ = 2000.0f;
    bool orthographic = false;
    float orthoHeight = 10.0f;
};

/// Mendaftarkan seluruh tipe komponen ke TypeRegistry dan ComponentRegistry.
///
/// Dipanggil eksplisit, bukan lewat inisialisasi statik. Panel bisa memakai
/// pendaftaran statik karena urutannya tidak penting; komponen tidak — urutan
/// pendaftaran menentukan urutan komponen di berkas level, dan urutan
/// inisialisasi statik antar-TU tidak ditentukan bahasa. Berkas level harus
/// byte-per-byte sama setiap kali disimpan.
void RegisterCoreComponents();

}  // namespace sim::scene
