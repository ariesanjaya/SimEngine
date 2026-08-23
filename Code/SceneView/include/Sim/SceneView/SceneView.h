#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Render/IViewportRenderer.h"
#include "Sim/Render/Types.h"
#include "Sim/SceneView/SkinnedPreview.h"
#include "Sim/SceneView/TerrainStore.h"
#include "Sim/Terrain/TerrainDecal.h"
#include "Sim/SceneView/WhiteboxStore.h"
#include "Sim/SceneView/MaterialPrograms.h"
#include "Sim/SceneView/PickScene.h"
#include "Sim/Scene/World.h"

#include <filesystem>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace sim::assets {
class MeshSdfBakery;
class TextureBakery;
struct AssetRecord;
class AssetDatabase;
}

namespace sim::view {

class Selection;
class TerrainStore;

/// Jejak sebuah decal, diturunkan dari transformnya di **ruang terrain**.
///
/// Dipisah menjadi fungsi supaya bisa diuji tanpa renderer. Yang bisa salah di
/// sini bukan penggambarannya melainkan aritmatikanya: ukuran diambil dari
/// panjang kolom matriks — medan skala sudah hilang begitu transformnya menjadi
/// matriks — dan rotasinya dari arah sumbu X yang diproyeksikan ke bidang datar.
terrain::DecalProjection ProjectDecal(const scene::DecalComponent& decal, const Mat4& local);

/// Apa yang dibutuhkan untuk menggambar terrain: dokumennya, dan dari mana ia
/// dilihat.
///
/// **Digabung menjadi satu, bukan dua parameter lagi.** Keduanya hanya berarti
/// bersama — store tanpa posisi kamera tidak bisa memilih perincian, dan posisi
/// kamera tanpa store tidak menggambar apa pun — dan daftar parameter yang
/// tumbuh satu per satu setiap fitur adalah daftar yang suatu saat dipanggil
/// dengan urutan yang salah.
struct TerrainView {
    TerrainStore* store = nullptr;
    /// Posisi kamera di ruang **dunia**. Jarak ke tiap ubin diukur darinya.
    Vec3 cameraPosition{0.0f};
    /// Perincian terkasar yang boleh dipilih.
    int maxLod = 4;
    /// Pengali ambang jarak; dua kali lipat berarti perincian penuh bertahan
    /// dua kali lebih jauh.
    float quality = 1.0f;
};

/// Sinar dalam ruang dunia.
struct Ray {
    Vec3 origin{0.0f};
    Vec3 direction{0.0f, 0.0f, -1.0f};
};

/// Persegi layar, sudut mana pun boleh jadi titik awal.
struct ScreenRect {
    Vec2 min{0.0f};
    Vec2 max{0.0f};

    static ScreenRect FromCorners(const Vec2& a, const Vec2& b) {
        return ScreenRect{Vec2(glm::min(a.x, b.x), glm::min(a.y, b.y)),
                          Vec2(glm::max(a.x, b.x), glm::max(a.y, b.y))};
    }
    float Width() const { return max.x - min.x; }
    float Height() const { return max.y - min.y; }
};

/// Menerjemahkan World menjadi daftar yang bisa digambar, sekaligus menjadi
/// sumber data untuk picking.
///
/// Keduanya disatukan dengan sengaja: apa yang terlihat dan apa yang bisa
/// diklik harus berasal dari daftar yang sama, kalau tidak akan ada objek yang
/// tampak tapi tak bisa dipilih (atau sebaliknya) dan penyebabnya sulit dilacak.
///
/// Dibangun ulang tiap frame. Untuk jumlah entity di fase editor ini jauh lebih
/// murah daripada menjaga cache tetap sinkron dengan perubahan hierarki.
/// Menyalin pengaturan langit dari adegan ke `desc`.
///
/// **Satu tempat, dipakai viewport editor maupun jalur headless.** Sebelumnya
/// hanya panel viewport yang membacanya, dan setiap pengukuran headless karena
/// itu menggambar langit bawaan berapa pun angka yang tertulis di level —
/// `intensity` 1 dan 100 menghasilkan gambar yang sama persis byte demi byte.
/// Ia cacat yang sama bentuknya dengan texture bakery yang tidak pernah
/// diserahkan: sebuah permukaan pengarangan yang diam-diam tidak ada di alat
/// ukurnya.
///
/// Mengembalikan false — dan mematikan `skyEnabled` — bila adegan tidak punya
/// satu pun `SkyComponent`. Level tanpa langit tidak menggambar langit sama
/// sekali, dan itulah yang membuat adegan interior berhenti membayar empat pass
/// LUT untuk sesuatu yang tidak pernah terlihat.
bool ApplySceneSky(const scene::World& world, render::ViewportDesc& desc);

class SceneView {
public:
    /// Satu objek bergeometri yang bisa diklik, sudah dalam ruang dunia.
    /// Satu titik di permukaan geometri, beserta pemiliknya.
    struct SurfaceHit {
        bool hit = false;
        scene::Entity entity = scene::kNullEntity;
        Vec3 position{0.0f};
        /// Normal geometri ruang dunia, sudah dinormalkan dan **menghadap ke
        /// arah datangnya sinar** — sehingga benda yang dijatuhkan berdiri di
        /// atas permukaan, bukan menembusnya.
        Vec3 normal{0.0f, 1.0f, 0.0f};
        float distance = 0.0f;

        explicit operator bool() const { return hit; }
    };

    struct Pickable {
        scene::Entity entity = scene::kNullEntity;
        Mat4 worldMatrix{1.0f};
        Vec3 boundsMin{-0.5f, -0.5f, -0.5f};
        Vec3 boundsMax{0.5f, 0.5f, 0.5f};
        /// Kunci geometri CPU-nya di `assets::MeshGeometryCache` — jalur berkas
        /// untuk mesh yang diimpor, dan kunci sintetis untuk bentuk yang lahir di
        /// editor. Kosong berarti entity ini tidak punya segitiga untuk
        /// ditembak, dan picking presisi melewatkannya.
        ///
        /// **Kunci, bukan geometrinya.** Yang menyusun daftar ini berjalan tiap
        /// frame; menaruh segitiga di sini berarti menyalin adegan enam puluh
        /// kali per detik.
        std::string meshKey;
    };

    /// Penanda untuk entity tanpa geometri — lampu, kamera, node kosong.
    ///
    /// Digambar panel sebagai glyph dari font ikon, bukan oleh renderer.
    /// "Entity ini lampu, jadi gambarkan bohlam" adalah pengetahuan editor;
    /// menaruhnya di renderer berarti renderer harus mengenali tipe komponen.
    struct EntityIcon {
        scene::Entity entity = scene::kNullEntity;
        Vec3 position{0.0f};
        const char* glyph = nullptr;
        Vec4 color{1.0f};
        bool selected = false;
        bool pickable = true;
    };

    /// Glyph penanda per jenis entity, diisi pemanggilnya.
    ///
    /// **Nama glyph-nya milik yang menggambarnya, bukan modul ini.** Komentar di
    /// `EntityIcon` di atas sudah menyebutnya bertahun-tahun: "entity ini lampu,
    /// jadi gambarkan bohlam" adalah pengetahuan editor. Sampai E9 kalimat itu
    /// benar sementara kodenya tidak — modul ini menyebut `icons::kLight`
    /// sendiri, dan font ikon lalu ikut terseret ke mana pun terjemahan
    /// `World` → `ViewportScene` dipakai, player termasuk.
    ///
    /// Yang membiarkannya kosong tetap mendapat seluruh daftar penanda beserta
    /// posisi, warna, dan status pickable-nya; yang hilang hanya glyph-nya. Dan
    /// yang tidak menggambar penanda memang tidak punya font untuk membacanya.
    struct IconGlyphs {
        const char* directionalLight = nullptr;
        const char* light = nullptr;
        const char* camera = nullptr;
        const char* empty = nullptr;
    };

    void SetIconGlyphs(const IconGlyphs& glyphs) { iconGlyphs_ = glyphs; }

    /// Menyusun ulang seluruh daftar dari isi world.
    /// Membangun daftar gambar dan daftar pickable untuk frame ini.
    ///
    /// `assets` dan `renderer` boleh null — daftarnya tetap terbangun, hanya
    /// setiap mesh renderer menggambar kubus satuan. **Itu perilaku yang
    /// dipertahankan dengan sengaja:** entity yang punya MeshRenderer tapi tidak
    /// menggambar apa pun adalah entity yang tidak bisa diklik, tidak bisa
    /// dipilih, dan karena itu tidak bisa diperbaiki.
    /// `animation` boleh null — mesh ber-rig lalu digambar pada bind pose-nya.
    void Build(scene::World& world, const Selection& selection,
               const assets::AssetDatabase* assets = nullptr,
               render::IViewportRenderer* renderer = nullptr,
               const SkinnedPreview* animation = nullptr,
               const assets::AssetDatabase* builtinAssets = nullptr,
               WhiteboxStore* whiteboxes = nullptr, const TerrainView& terrain = {});

    /// Baker tekstur yang dipakai menerjemahkan berkas sumber menjadi `.ktx2`.
    ///
    /// **Setter, bukan parameter `Build` yang kesembilan.** Ia tidak berubah dari
    /// frame ke frame, dan daftar parameter yang sudah delapan panjangnya adalah
    /// tempat argumen tertukar tanpa satu pun galat kompilasi.
    ///
    /// Null berarti tidak ada tekstur material yang tergambar sama sekali —
    /// bukan bahwa berkas sumbernya dipakai apa adanya. Renderer hanya menerima
    /// `.ktx2`.
    void SetTextureBakery(assets::TextureBakery* bakery) { bakery_ = bakery; }

    /// Baker medan jarak mesh untuk clipmap GI.
    ///
    /// Null berarti tidak ada yang dibake, dan clipmap kembali menyusun tiap
    /// mesh sebagai kotak batasnya — keadaan sebelum M1, dan keadaan yang benar
    /// untuk build tanpa OpenVDB.
    void SetMeshSdfBakery(assets::MeshSdfBakery* bakery) { sdfBakery_ = bakery; }

    /// Penjaga shader material. Null berarti tidak ada material yang
    /// dikompilasi sama sekali — dan setiap ruas digambar jalur mundur
    /// `box.frag`, seperti sebelum jalur ini ada.
    void SetMaterialPrograms(MaterialPrograms* programs) { materialPrograms_ = programs; }

    /// Salinan CPU geometri, untuk picking presisi (R2).
    ///
    /// Null berarti setiap picking memakai jalur kotak batas — keadaan sebelum
    /// R2, dan keadaan yang benar untuk build tanpa cache.
    void SetMeshGeometryCache(assets::MeshGeometryCache* cache) { picks_.SetCache(cache); }

    /// Berapa benda yang geometrinya masih dimuat. Nol berarti picking sudah
    /// presisi untuk seluruh isi adegan.
    std::size_t PendingPickGeometry() const { return picks_.PendingCount(); }

    /// Menambahkan kotak wireframe sejajar sumbu, sesudah `Build`.
    ///
    /// Terbuka karena tidak semua yang perlu digambar berasal dari dunia:
    /// batas volume `.vdb` datang dari pengaturan viewport, bukan dari sebuah
    /// entity. `Build` mengosongkan daftarnya, jadi ini dipanggil setelahnya —
    /// dan `Scene()` menyusun span-nya saat itu juga, jadi menambah di antara
    /// keduanya aman.
    void AddWireBox(const Vec3& boxMin, const Vec3& boxMax, const Vec4& color);

    /// Satu ruas garis di ruang dunia.
    void AddLine(const Vec3& from, const Vec3& to, const Vec4& color);

    /// Menggambar ruas berikutnya menembus geometri, sampai dikembalikan.
    ///
    /// **Sakelar, bukan parameter di setiap fungsi.** Rangka kawat sebuah kapsul
    /// adalah seratus ruas yang semuanya menginginkan jawaban yang sama; menaruh
    /// pilihannya di tanda tangan setiap primitif berarti seratus kesempatan
    /// meneruskannya salah, dan yang salah tidak terlihat sebagai galat melainkan
    /// sebagai satu bagian rangka yang tertutup dinding.
    void SetLinesThroughGeometry(bool through) { linesThroughGeometry_ = through; }

    // --- rangka kawat bentuk tabrakan ---------------------------------------
    //
    // **Sumbunya mengikuti PhysX, bukan selera.** Kapsul dan silinder berbaring
    // di sumbu **X** lokal dan bidang tak hingga bernormal **+X** lokal, karena
    // itulah yang dibuat `PxCapsuleGeometry` dan `PxPlaneGeometry`. Rangka kawat
    // yang memakai sumbu lain akan terlihat masuk akal dan berbohong — dan yang
    // membukanya adalah orang yang sedang memeriksa apakah bentuknya benar.
    //
    // `transform` membawa posisi dan putaran saja. Skala tidak masuk ke sana
    // melainkan ke ukurannya, mengikuti `physics::DescribeCollider`: sebuah
    // jari-jari tidak bisa diregangkan ke satu arah, jadi menskalakan matriksnya
    // akan menggambar telur di tempat bola disimulasikan.

    void AddWireBox(const Mat4& transform, const Vec3& halfExtents, const Vec4& color);
    void AddWireSphere(const Mat4& transform, float radius, const Vec4& color);
    /// Setengah-tinggi **bagian silindernya**, tidak termasuk kedua tudung —
    /// konvensi `ColliderComponent::halfHeight` dan `PxCapsuleGeometry`.
    void AddWireCapsule(const Mat4& transform, float radius, float halfHeight,
                        const Vec4& color);
    void AddWireCylinder(const Mat4& transform, float radius, float halfHeight,
                         const Vec4& color);
    /// Bidang tak hingga digambar sepotong: `extent` setengah-sisi kotaknya.
    /// Ditambah satu garis normal, karena kotak tanpa normal tidak memberi tahu
    /// sisi mana yang padat.
    void AddWirePlane(const Mat4& transform, float extent, const Vec4& color);
    /// Salib tiga sumbu. Untuk yang tidak punya bentuk untuk digambar.
    void AddWireCross(const Vec3& center, float size, const Vec4& color);

    /// Lingkaran pada bidang yang direntang `axisA` dan `axisB`, keduanya
    /// diharapkan satuan dan saling tegak lurus.
    void AddWireCircle(const Vec3& center, const Vec3& axisA, const Vec3& axisB, float radius,
                       const Vec4& color);

    /// Kerucut terbuka: puncak, empat rusuk, dan lingkaran mulutnya.
    ///
    /// `halfAngle` sudut dari sumbu ke rusuknya — separuh bukaan penuh, mengikuti
    /// `LightComponent::outerAngleRadians`. `length` diukur sepanjang sumbu, bukan
    /// sepanjang rusuknya: itu yang membuat mulutnya tepat di jangkauan lampu.
    void AddWireCone(const Vec3& apex, const Vec3& direction, float halfAngle, float length,
                     const Vec4& color);

private:
    /// Mesh decal yang sudah dibangun, beserta keadaan yang menghasilkannya.
    ///
    /// Di-cache dengan alasan yang sama seperti mesh ubin: `Build` berjalan tiap
    /// frame, dan menyalin permukaan terrain di dalam jejak decal tiap frame
    /// adalah ribuan simpul yang dihitung untuk hasil yang sama persis.
    struct CachedDecal {
        assets::MeshData mesh;
        Mat4 builtTransform{0.0f};
        Vec4 builtColor{0.0f};
        float builtLift = -1.0f;
        int builtSteps = -1;
        uint32_t builtTerrain = 0;
        uint64_t upload = 1;
        /// Frame terakhir yang memakainya. Decal yang hilang dari scene tidak
        /// boleh menahan meshnya selamanya.
        uint64_t touched = 0;
    };

    void AppendDecal(const scene::DecalComponent& component, scene::Entity entity,
                     const Mat4& matrix, bool selected, bool pickable,
                     const assets::AssetDatabase* assets, render::IViewportRenderer* renderer,
                     const TerrainView& view, scene::World& world);

    void AppendTerrain(const scene::TerrainComponent& component, scene::Entity entity,
                       const Mat4& matrix, bool selected, bool pickable,
                       const assets::AssetDatabase* assets, render::IViewportRenderer* renderer,
                       const TerrainView& view);
    void AppendLight(const scene::LightComponent& light, const Mat4& matrix);
    void AppendSkinPalette(uint32_t boneCount, std::span<const Mat4> palette,
                           render::MeshInstance& instance);
    /// Warna dasar sebuah material, dari cache atau dibaca dari berkasnya.
    Vec4 MaterialColor(const assets::AssetDatabase* assets, const Uuid& guid);
    /// Tekstur warna dasar sebuah material, sudah diunggah, atau nol.
    render::TextureHandle MaterialTexture(const assets::AssetDatabase* assets,
                                          render::IViewportRenderer* renderer, const Uuid& guid);
    /// Sebuah aset beserta indeks yang memilikinya.
    struct AssetLookup {
        const assets::AssetDatabase* database = nullptr;
        const assets::AssetRecord* record = nullptr;
        explicit operator bool() const { return record != nullptr; }
    };

    /// Mencari sebuah aset di indeks project, **lalu di indeks bawaan editor**.
    ///
    /// **Keduanya, dan bukan hanya yang pertama.** Prefab bawaan — Shader Ball,
    /// Ground, Sky Dome — merujuk aset milik editor, dan setiap level yang
    /// memasukkannya membawa GUID itu apa adanya. Mencari di indeks project saja
    /// membuat semuanya jatuh ke kubus satuan: bukan galat, hanya kotak di
    /// tempat yang seharusnya ada mesh.
    ///
    /// Indeksnya ikut dikembalikan karena `AbsolutePath` milik indeks yang
    /// memegang recordnya — memakai yang salah menghasilkan jalur yang menunjuk
    /// ke folder project untuk berkas yang ada di folder editor.
    AssetLookup FindAsset(const assets::AssetDatabase* assets, const Uuid& guid) const;

    /// Sebuah aset gambar menjadi handle tekstur, lewat baker.
    ///
    /// Mengembalikan placeholder selama hasil bake-nya belum ada, dan nol bila
    /// tidak ada baker atau bake-nya gagal.
    render::TextureHandle UploadedTexture(const assets::AssetDatabase* assets,
                                          render::IViewportRenderer* renderer, const Uuid& image);
    /// Material sebuah ruas sebagai pipeline yang sudah dibangun renderer, atau
    /// nol bila ia harus digambar jalur mundur.
    ///
    /// **Yang mengompilasinya `MaterialPrograms`, di `TaskPool`.** Yang di sini
    /// hanya menanyakannya dan menerima jawaban "belum" tanpa menunggu.
    render::MaterialHandle ForwardMaterial(const assets::AssetDatabase* assets,
                                           render::IViewportRenderer* renderer, const Uuid& guid);
    /// Warna material bawaan editor — yang mengisi mesh tanpa material sendiri.
    Vec4 BuiltinColor(const assets::AssetDatabase* builtinAssets);
    void AppendPartColors(const scene::MeshRendererComponent& renderer, uint32_t partCount,
                          const assets::AssetDatabase* assets,
                          render::IViewportRenderer* textureRenderer,
                          render::MeshInstance& instance);

public:

    /// Tampilan yang siap diserahkan ke IViewportRenderer::Render().
    /// Span-nya menunjuk ke penyimpanan milik SceneView, jadi objek ini harus
    /// hidup lebih lama daripada panggilan Render().
    render::ViewportScene Scene() const;

    const std::vector<Pickable>& Pickables() const { return pickables_; }
    const std::vector<EntityIcon>& Icons() const { return icons_; }

    /// Entity bergeometri terdekat yang ditembus sinar, atau kNullEntity.
    ///
    /// Ujinya terhadap AABB dalam ruang lokal objek — bukan AABB dunia — jadi
    /// objek yang diputar tetap bisa diklik tepat pada bentuknya.
    scene::Entity Raycast(const Ray& ray) const;

    /// Permukaan yang tertembus sinar: titiknya, normalnya, dan pemiliknya.
    ///
    /// **Hanya jalur presisi — kotak batas sengaja tidak dipakai di sini**, dan
    /// itu perbedaan yang menentukan terhadap `Raycast` di atas. Memilih benda
    /// dari kotaknya sedikit terlalu murah hati dan hasilnya tetap benda yang
    /// dimaksud; menaruh sesuatu di *permukaan* sebuah kotak menaruhnya di udara,
    /// dengan orientasi mengikuti sisi kotak yang tidak ada di bentuk aslinya.
    /// Yang geometrinya belum dimuat karena itu menjawab "tidak kena", dan
    /// pemanggil melewatkannya alih-alih memindahkannya ke tempat yang salah.
    ///
    /// `ignore` melewatkan entity tertentu — benda yang sedang dijatuhkan tidak
    /// boleh mendarat di dirinya sendiri.
    SurfaceHit RaycastSurface(const Ray& ray, std::span<const scene::Entity> ignore = {}) const;

    /// Ikon terdekat dari sebuah titik layar, dalam radius tertentu.
    ///
    /// Diuji di ruang layar, bukan ruang dunia: ikon digambar dengan ukuran
    /// piksel tetap, jadi daerah kliknya harus ikut tetap. Kotak dunia
    /// berukuran tetap akan menyusut di kejauhan sampai mustahil diklik,
    /// padahal ikonnya masih terlihat sebesar semula.
    scene::Entity PickIcon(const Mat4& viewProjection, const Vec2& origin, const Vec2& size,
                           const Vec2& point, float radiusPixels) const;

    /// Semua entity yang jejak layarnya bersinggungan dengan `rect`.
    std::vector<scene::Entity> RectSelect(const Mat4& viewProjection, const Vec2& origin,
                                          const Vec2& size, const ScreenRect& rect) const;

    /// AABB dunia yang melingkupi entity-entity yang diberikan. Dipakai
    /// perintah "focus ke seleksi".
    bool BoundsOf(const std::vector<scene::Entity>& entities, Vec3& outMin, Vec3& outMax) const;

private:
    /// Diindeks nomor entity. Bertahan lintas frame — itu gunanya.
    std::unordered_map<uint64_t, CachedDecal> decals_;
    uint64_t frame_ = 0;

    std::vector<render::MeshInstance> meshes_;
    /// Palet kulit seluruh instance ber-skin, bersambung. Tiap `MeshInstance`
    /// menunjuk ruasnya sendiri di sini.
    std::vector<Mat4> skinMatrices_;
    /// Warna per slot material seluruh instance, bersambung.
    std::vector<Vec4> partColors_;
    /// Sejajar dengan `partColors_`. Nol berarti ruas itu tanpa tekstur.
    std::vector<render::TextureHandle> partTextures_;
    /// Sejajar dengan `partColors_`, dengan alasan yang sama persis.
    std::vector<render::MaterialHandle> partMaterials_;
    assets::TextureBakery* bakery_ = nullptr;
    assets::MeshSdfBakery* sdfBakery_ = nullptr;
    /// Indeks bawaan editor, dipasang tiap `Build`. Lihat `FindAsset`.
    const assets::AssetDatabase* builtinAssets_ = nullptr;
    MaterialPrograms* materialPrograms_ = nullptr;
    std::vector<render::LineSegment> lines_;
    std::vector<render::LightInstance> lights_;
    std::vector<Pickable> pickables_;
    /// Geometri yang bisa ditembak. **`mutable` karena ia cache**: `Raycast`
    /// tidak mengubah apa yang dilihat pemanggil, ia hanya menyiapkan jawaban
    /// yang sama dengan lebih teliti.
    mutable PickScene picks_;
    /// Dipakai ulang tiap picking supaya tidak mengalokasi di dalam klik.
    mutable std::vector<PickItem> pickItems_;
    IconGlyphs iconGlyphs_;
    bool linesThroughGeometry_ = false;
    std::vector<EntityIcon> icons_;
    /// Warna dasar per material, supaya `.simmat` tidak diurai tiap frame untuk
    /// jawaban yang tidak berubah. Kunci GUID; jalur yang gagal dibaca dicatat
    /// dengan warna bawaan supaya ia juga tidak dicoba lagi.
    std::unordered_map<Uuid, Vec4> materialColor_;
    /// guid material → aset tekstur warna dasarnya. GUID tak sah berarti
    /// materialnya tidak menyebut tekstur; diingat juga supaya berkas yang sama
    /// tidak diurai ulang tiap frame.
    std::unordered_map<Uuid, Uuid> materialTexture_;
};

/// Sinar dunia yang melewati sebuah titik di layar.
///
/// `point` relatif terhadap sudut kiri-atas gambar viewport, dalam piksel.
Ray ScreenPointToRay(const Mat4& view, const Mat4& projection, const Vec2& size, const Vec2& point);

/// Memproyeksikan titik dunia ke piksel viewport. Mengembalikan false bila
/// titiknya di belakang kamera, yang membuat hasil proyeksinya tak bermakna.
bool WorldToScreen(const Mat4& viewProjection, const Vec2& origin, const Vec2& size,
                   const Vec3& world, Vec2& outScreen);

}  // namespace sim::view
