#include "Sim/Volume/SdfBake.h"
#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/Log.h"
#include "Sim/Core/Math.h"
#include "Sim/Editor/Command.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Physics/PhysicsScene.h"
#include "Sim/Render/LightCluster.h"
#include "Sim/Editor/Gizmo.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Notifications.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelRegistry.h"
#include "Sim/Editor/SceneCommands.h"
#include "Sim/SceneView/SceneView.h"
#include "Sim/SceneView/Selection.h"
#include "Sim/Editor/Widgets.h"
#include "Sim/Editor/WhiteboxCommands.h"
#include "Sim/SceneView/WhiteboxStore.h"
#include "Sim/Terrain/TerrainBrush.h"
#include "Sim/Terrain/TerrainPicking.h"
#include "Sim/Render/IViewportRenderer.h"
#include "Sim/Scene/Components.h"
#include "Sim/Whitebox/Picking.h"
#include "Sim/Whitebox/PolygonOutline.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace sim::editor {
namespace {
/// Langit pertama yang ditemukan di dunia, atau nullptr.
///
/// **Yang pertama, bukan yang digabung.** Dua entity langit di satu level adalah
/// kesalahan penyusunan, dan menggabungkan keduanya menghasilkan langit yang
/// bukan salah satu dari yang diminta — lebih baik satu yang menang dan yang
/// lain terlihat tidak berpengaruh, karena itu yang mengarahkan orang mencari
/// duplikatnya.
/// Warna kotak batas volume. Sengaja beda dari warna seleksi dan grid: yang
/// digambar di sini bukan sesuatu yang bisa dipilih, melainkan jangkauan sebuah
/// efek — dan menyamakan warnanya membuat orang mencoba mengkliknya.
constexpr Vec4 kVolumeBoundsColor{0.35f, 0.75f, 1.0f, 0.55f};

/// Memuat volume `.vdb` ketika jalur atau nama gridnya berubah.
///
/// **Tidak melakukan apa-apa bila keduanya tidak berubah.** Berkas volume
/// berukuran puluhan megabyte dan pemadatannya berjalan di thread ini; memuatnya
/// ulang tiap frame akan menghentikan editor sepenuhnya.
///
/// Revisi dinaikkan setiap muat berhasil. Renderer memakainya untuk memutuskan
/// unggahan ulang — membandingkan pointer saja tidak cukup, karena grid kedua
/// bisa mendarat di alamat yang baru saja dibebaskan grid pertama.
void LoadVolumeIfChanged(EditorContext& context) {
    static std::string loadedPath;
    static std::string loadedGrid;
    auto& volume = context.volume;

    if (volume.path == loadedPath && volume.gridName == loadedGrid) {
        return;
    }
    loadedPath = volume.path;
    loadedGrid = volume.gridName;

    volume.grid.reset();
    if (volume.path.empty()) {
        volume.status.clear();
        return;
    }

    auto grid = std::make_shared<VolumeGrid>();
    sim::volume::VdbLoadSettings settings;
    settings.gridName = volume.gridName;
    const sim::volume::SdfBakeResult result =
        sim::volume::LoadVdb(std::filesystem::path(volume.path), settings, *grid);
    if (!result.ok) {
        volume.status = result.error;
        SIM_WARN("Editor", "volume: {}", result.error);
        return;
    }

    volume.grid = std::move(grid);
    ++volume.revision;
    volume.status = "grid '" + volume.grid->name + "', " + std::to_string(volume.grid->sizeX) +
                    "x" + std::to_string(volume.grid->sizeY) + "x" +
                    std::to_string(volume.grid->sizeZ);
    SIM_INFO("Editor", "volume loaded from {}: {}", volume.path, volume.status);
}


constexpr float kLookSensitivity = 0.005f;
constexpr float kFlySpeedMin = 0.05f;
constexpr float kFlySpeedMax = 500.0f;

/// Jarak seret (piksel) yang membedakan klik dari seleksi kotak. Di bawah ini
/// tetap dianggap klik, supaya tangan yang sedikit bergeser saat mengklik tidak
/// tiba-tiba membuka kotak seleksi.
constexpr float kDragThreshold = 4.0f;

/// Besar glyph ikon entity terhadap ukuran font teks.
constexpr float kIconScale = 1.6f;

/// Warna entity terpilih, sama dengan yang dipakai renderer untuk wireframe.
constexpr Vec4 kSelectionColor{1.0f, 0.62f, 0.20f, 1.0f};

// --- saringan tampilan viewport ---------------------------------------------
//
// **Ikon dan nama menyebut mode yang sedang berlaku, bukan mode berikutnya.**
// Tombol berputar mudah ditulis terbalik — "tekan untuk jadi Wireframe" — dan
// yang dicari mata saat melihat viewport orang lain adalah "sedang apa ini",
// bukan "kalau kutekan jadi apa". Tombol perspektif/ortografi di sebelahnya
// sudah memakai kesepakatan yang sama.

/// Dari mana viewport memandang.
///
/// **Sebuah konsep editor, bukan konsep renderer.** Yang diterima
/// `ViewportDesc` tetap sebuah `render::Camera` biasa; yang di sini hanya
/// memutuskan bagaimana kamera itu disusun. Renderer tidak pernah tahu bahwa
/// ada yang namanya "tampak Depan".
enum class Viewpoint : uint8_t {
    /// Orbit bebas, proyeksi perspektif. Bawaannya.
    Perspective,
    Front,
    Back,
    Left,
    Right,
    Top,
    Bottom,
    /// Memandang lewat sebuah entity berkamera di dalam adegan.
    SceneCamera,
};

/// Tampak terkunci sumbu: ortografik, dan rotasinya tidak bisa diseret.
///
/// `SceneCamera` sengaja tidak termasuk — ia bukan tampak sumbu, dan
/// proyeksinya ditentukan `CameraComponent` miliknya sendiri.
bool IsAxisView(Viewpoint viewpoint) {
    return viewpoint != Viewpoint::Perspective && viewpoint != Viewpoint::SceneCamera;
}

const char* ViewpointLabel(Viewpoint viewpoint) {
    switch (viewpoint) {
        case Viewpoint::Front: return "Front";
        case Viewpoint::Back: return "Back";
        case Viewpoint::Left: return "Left";
        case Viewpoint::Right: return "Right";
        case Viewpoint::Top: return "Top";
        case Viewpoint::Bottom: return "Bottom";
        case Viewpoint::SceneCamera: return "Camera";
        case Viewpoint::Perspective: break;
    }
    return "Perspective";
}

const char* ViewpointIcon(Viewpoint viewpoint) {
    switch (viewpoint) {
        case Viewpoint::SceneCamera: return icons::kCamera;
        case Viewpoint::Perspective: return icons::kPerspective;
        default: break;
    }
    return icons::kOrthographic;
}

/// Yaw dan pitch yang menempatkan kamera pada sumbu sebuah tampak terkunci.
///
/// **Diturunkan dari `OrbitCamera::Offset()`, yang mengembalikan arah dari titik
/// fokus *menuju* kamera** — bukan arah pandang. Membalik keduanya menghasilkan
/// tampak Depan yang sebenarnya Belakang, dan tidak ada satu pun galat yang
/// menyertainya: adegan simetris terlihat benar sampai seseorang memindahkan
/// satu benda.
///
/// Pitch tampak Atas/Bawah tepat ±90°, bukan dijepit seperti navigasi bebas.
/// Itu yang menuntut sumbu atas yang aman di `ApplyTo`.
void AxisViewAngles(Viewpoint viewpoint, float& yaw, float& pitch) {
    pitch = 0.0f;
    switch (viewpoint) {
        case Viewpoint::Front: yaw = 0.0f; return;                  // kamera di +Z
        case Viewpoint::Back: yaw = kPi; return;                    // kamera di -Z
        case Viewpoint::Right: yaw = -kHalfPi; return;              // kamera di +X
        case Viewpoint::Left: yaw = kHalfPi; return;                // kamera di -X
        case Viewpoint::Top: yaw = 0.0f; pitch = kHalfPi; return;   // kamera di +Y
        case Viewpoint::Bottom: yaw = 0.0f; pitch = -kHalfPi; return;
        default: yaw = 0.0f; return;
    }
}

const char* ShadingIcon(render::DrawMode mode) {
    switch (mode) {
        case render::DrawMode::Unlit: return icons::kShadingUnlit;
        case render::DrawMode::Clay: return icons::kShadingClay;
        case render::DrawMode::MaterialWireframe: return icons::kShadingMaterialWireframe;
        case render::DrawMode::Wireframe: return icons::kShadingWireframe;
        case render::DrawMode::Material: break;
    }
    return icons::kShadingMaterial;
}

const char* ShadingLabel(render::DrawMode mode) {
    switch (mode) {
        case render::DrawMode::Unlit: return "Unlit";
        case render::DrawMode::Clay: return "Clay (lighting only)";
        case render::DrawMode::MaterialWireframe: return "Material + wireframe";
        case render::DrawMode::Wireframe: return "Wireframe";
        case render::DrawMode::Material: break;
    }
    return "Material";
}

/// Berkelompok, bukan berderet: tiga mode permukaan lebih dulu — material apa
/// adanya, material tanpa cahaya, lalu cahaya tanpa material — baru dua mode
/// rusuk. Yang mengelompokkan mode menurut pertanyaan yang dijawabnya adalah
/// urutan yang bisa diingat; yang berurut menurut kapan modenya ditulis, tidak.
///
/// **Ini juga satu-satunya daftar urutan mode di panel ini.** Menu klik-kanan
/// dibangun dengan memanggil fungsi ini sampai kembali ke titik awal, bukan dari
/// larik kedua yang harus diingat untuk ikut diperbarui — dua daftar urutan
/// adalah dua daftar yang suatu saat berselisih, dan yang berselisih adalah menu
/// yang diam-diam kehilangan satu mode.
///
/// `switch` tanpa `default` yang menyebut setiap nilai membuat mode baru menjadi
/// galat kompilasi di sini, bukan mode yang tidak pernah bisa terpilih.
render::DrawMode NextDrawMode(render::DrawMode mode) {
    switch (mode) {
        case render::DrawMode::Material: return render::DrawMode::Unlit;
        case render::DrawMode::Unlit: return render::DrawMode::Clay;
        case render::DrawMode::Clay: return render::DrawMode::MaterialWireframe;
        case render::DrawMode::MaterialWireframe: return render::DrawMode::Wireframe;
        case render::DrawMode::Wireframe: break;
    }
    return render::DrawMode::Material;
}

/// Nama popup daftar mode. Tetap, walaupun ikon dan tooltip tombolnya berganti
/// tiap mode: ID popup yang ikut berubah adalah popup yang menutup dirinya
/// sendiri pada frame sesudah sebuah mode dipilih.
constexpr const char* kShadingPopupId = "viewport-shading-modes";

// --- rangka kawat fisika ----------------------------------------------------
//
// **Warnanya menyebut jenis bendanya, bukan sekadar "ini collider".** Yang
// paling sering salah bukan ukurannya melainkan jenisnya: sebuah lantai yang
// tidak sengaja Dynamic akan jatuh menembus dirinya sendiri pada frame pertama
// Play, dan sampai frame itu tidak ada satu pun petunjuk di layar. Tiga warna
// membuatnya terbaca sebelum Play ditekan.
constexpr Vec4 kColliderStaticColor{0.35f, 0.90f, 0.45f, 0.85f};
constexpr Vec4 kColliderKinematicColor{0.35f, 0.70f, 1.00f, 0.85f};
constexpr Vec4 kColliderDynamicColor{1.00f, 0.85f, 0.30f, 0.85f};
/// Collider tanpa RigidBody, dan benda tegar tanpa collider. Keduanya tidak
/// pernah sampai ke solver.
constexpr Vec4 kColliderInertColor{0.85f, 0.35f, 0.35f, 0.85f};
constexpr Vec4 kJointColor{0.85f, 0.55f, 1.00f, 0.90f};
constexpr Vec4 kVehicleColor{1.00f, 0.55f, 0.25f, 0.90f};

/// Setengah-sisi kisi yang menggambarkan bidang tak hingga, meter.
constexpr float kPlaneWireExtent = 5.0f;
/// Setengah-panjang lengan salib penanda, meter.
constexpr float kMarkerSize = 0.25f;
/// Setengah-panjang garis sumbu sendi, meter.
constexpr float kJointAxisLength = 0.5f;

// --- gizmo lampu ------------------------------------------------------------
//
// **Radiansi ambang tempat "jangkauan berguna" diukur.** Satu, dalam satuan
// radiansi yang sama yang dipakai shader — kira-kira seterang permukaan putih di
// bawah matahari bawaan. Angkanya sendiri sembarang; yang tidak sembarang adalah
// bahwa ia tetap, sehingga dua lampu bisa dibandingkan dari besar bolanya.
constexpr float kLightUsefulThreshold = 1.0f;
/// Panjang berkas lampu directional, meter. Ia tak hingga; yang digambar sekadar
/// cukup untuk membaca arahnya.
constexpr float kDirectionalRayLength = 2.0f;
/// Jari-jari cincin tempat berkas directional berangkat.
constexpr float kDirectionalRingRadius = 0.25f;
/// Panjang sayap kepala panah, sebagai pecahan panjang berkasnya.
constexpr float kArrowHeadFraction = 0.18f;

/// Dorongan terkecil (meter) yang masih dianggap ekstrusi.
///
/// Di bawahnya sisi barunya berupa dinding setipis itu — geometri yang tidak
/// bisa dipilih lagi karena tidak ada piksel yang mengenainya, dan yang tetap
/// ikut tersimpan ke berkas selamanya.
constexpr float kWhiteboxExtrudeMin = 1e-3f;

// --- sub-objek whitebox (W7.2) ----------------------------------------------
//
// **Ukurannya dalam piksel, bukan dalam meter.** Sebuah simpul tidak punya
// ukuran di dunia, dan penanda yang berskala dunia menghilang begitu kamera
// mundur — persis saat perancang sedang melihat keseluruhan ruangan dan paling
// butuh melihat sudut mana yang bisa ditarik.
constexpr float kVertexMarkerRadius = 3.5f;
/// Jangkauan klik, piksel. Lebih besar dari penandanya: yang harus mudah dikenai
/// adalah simpulnya, bukan gambarnya.
constexpr float kVertexPickRadius = 9.0f;
constexpr float kEdgePickRadius = 7.0f;

constexpr Vec4 kSubObjectIdle{0.60f, 0.64f, 0.70f, 0.95f};
constexpr Vec4 kSubObjectHovered{1.00f, 0.85f, 0.35f, 1.00f};
constexpr Vec4 kSubObjectSelected{1.00f, 0.62f, 0.20f, 1.00f};

ImU32 ToColor(const Vec4& color) {
    return ImGui::GetColorU32(ImVec4(color.x, color.y, color.z, color.w));
}

/// Jarak titik ke ruas garis, ruang layar. Dipakai memilih rusuk.
float DistanceToSegment(const Vec2& point, const Vec2& a, const Vec2& b) {
    const Vec2 span = b - a;
    const float lengthSquared = glm::dot(span, span);
    if (lengthSquared < 1e-6f) {
        return glm::length(point - a);
    }
    const float t = std::clamp(glm::dot(point - a, span) / lengthSquared, 0.0f, 1.0f);
    return glm::length(point - (a + span * t));
}

/// Kamera editor bergaya orbit + fly.
///
/// Orbit disimpan sebagai yaw/pitch/jarak terhadap titik fokus, bukan sebagai
/// matriks. Alasannya: pengguna berpikir dalam "berputar mengelilingi objek"
/// dan "mendekat", dan menyimpan keadaan dalam bentuk itu membuat pembatasan
/// pitch, snapping sudut, dan perintah "focus ke seleksi" jadi sepele.
///
/// Gerakan terbang menggeser `focus`, bukan posisi kamera secara langsung.
/// Karena posisi kamera diturunkan dari focus, keduanya bergerak bersama —
/// dan begitu tombol kanan dilepas, orbit langsung berputar mengelilingi titik
/// baru di depan kamera alih-alih titik lama yang sudah tertinggal jauh.
struct OrbitCamera {
    Vec3 focus{0.0f, 0.0f, 0.0f};
    float distance = 12.0f;
    float yaw = -0.6f;
    float pitch = 0.45f;

    static constexpr float kPitchLimit = kHalfPi - 0.02f;

    /// Arah dari titik fokus menuju kamera.
    ///
    /// Tanda minus pada komponen X disengaja. Tanpa itu, yaw yang bertambah
    /// menggeser kamera ke +X sehingga arah pandang justru berayun ke kiri —
    /// kebalikan dari yang diharapkan saat menyeret mouse ke kanan. Konvensi
    /// yang dipakai di sini: pada yaw = 0 kamera memandang ke -Z, dan yaw yang
    /// bertambah memutar pandangan ke kanan.
    Vec3 Offset() const {
        return Vec3{-std::cos(pitch) * std::sin(yaw), std::sin(pitch),
                    std::cos(pitch) * std::cos(yaw)};
    }

    Vec3 Forward() const { return -Offset(); }

    /// Sumbu kanan diturunkan dari yaw saja, bukan dari cross(forward, up).
    /// Pada pitch mendekati tegak lurus, cross(forward, up) mendekati nol dan
    /// arah strafe jadi tidak stabil; rumus ini tetap presisi di sudut mana pun.
    Vec3 Right() const { return Vec3{std::cos(yaw), 0.0f, std::sin(yaw)}; }

    void ApplyTo(render::Camera& camera) const {
        camera.position = focus + Offset() * distance;
        const Vec3 forward = Forward();
        // **Sumbu atas dipilih, bukan selalu `kUp`.** `quatLookAt` tidak
        // terdefinisi ketika arah pandang sejajar sumbu atasnya, dan tampak
        // Atas/Bawah adalah persis arah itu — pitch tepat ±90°, yang tidak
        // pernah dihasilkan navigasi bebas karena `Look` menjepitnya sedikit di
        // bawah. Yang muncul tanpa cabang ini bukan gambar yang miring
        // melainkan quaternion NaN, dan viewport yang kosong.
        //
        // −Z dipilih sebagai atas layar saat memandang lurus ke bawah: itu yang
        // membuat tampak Atas terbaca seperti denah — +X ke kanan, +Z ke bawah.
        const Vec3 up = std::abs(forward.y) > 0.999f
                            ? Vec3(0.0f, 0.0f, forward.y > 0.0f ? 1.0f : -1.0f)
                            : kUp;
        camera.rotation = glm::quatLookAt(forward, up);
    }

    /// deltaPitch positif berarti kamera naik dan pandangan menunduk.
    void Look(float deltaYaw, float deltaPitch) {
        yaw += deltaYaw;
        pitch = std::clamp(pitch + deltaPitch, -kPitchLimit, kPitchLimit);
    }

    void Pan(float deltaX, float deltaY) {
        const Vec3 right = Right();
        const Vec3 up = glm::cross(right, Forward());
        // Kecepatan pan diskalakan jarak: menyeret satu piksel harus memindah
        // dunia sejauh yang terlihat sama, sedekat atau sejauh apa pun kamera.
        const float scale = distance * 0.0018f;
        focus += right * (-deltaX * scale) + up * (deltaY * scale);
    }

    void Zoom(float steps) {
        // Perkalian, bukan penambahan: mendekat dari 100 m dan dari 1 m harus
        // terasa sama cepatnya.
        distance = std::clamp(distance * std::pow(0.88f, steps), 0.05f, 5000.0f);
    }

    void Fly(const Vec3& direction, float metersPerSecond, float deltaSeconds) {
        focus += direction * (metersPerSecond * deltaSeconds);
    }
};

class ViewportPanel final : public Panel {
public:
    ViewportPanel()
        : Panel(panel_id::kViewport, std::string(icons::kPanelViewport) + "  Viewport",
                PanelCategory::Scene) {
        // **Glyph penanda diisi di sini, bukan di `SceneView`.** Modul itu
        // dipakai player juga, dan sebuah player tidak punya font ikon untuk
        // membaca `ICON_LC_SUN`. "Entity ini lampu, jadi gambarkan bohlam"
        // memang selalu pengetahuan editor — komentar di `EntityIcon` sudah
        // menyebutnya sejak lama; yang baru adalah kodenya ikut mengatakannya.
        SceneView::IconGlyphs glyphs;
        glyphs.directionalLight = icons::kSunLight;
        glyphs.light = icons::kLight;
        glyphs.camera = icons::kCamera;
        glyphs.empty = icons::kEntity;
        sceneView_.SetIconGlyphs(glyphs);
    }

    bool WantsZeroPadding() const override { return true; }

    // Viewport dikunci di dockspace. Kalau ditarik keluar, ia menjadi viewport
    // platform tersendiri: satu swapchain, satu acquire, dan satu present
    // tambahan per frame untuk gambar seukuran panel penuh — biaya yang jauh
    // lebih terasa dibanding panel berisi teks.
    bool IsDockLocked() const override { return true; }

    void OnDraw(EditorContext& context) override {
        render::IViewportRenderer* renderer = context.viewportRenderer;
        if (renderer == nullptr || context.world == nullptr || context.selection == nullptr) {
            ImGui::TextDisabled("No renderer.");
            return;
        }

        const ImVec2 size = ImGui::GetContentRegionAvail();
        if (size.x < 8.0f || size.y < 8.0f) {
            // Overlay tidak digambar frame ini, jadi ia tidak sedang menunjuk
            // apa pun. Tanpa baris ini, panel yang dilipat sementara sebuah
            // tombolnya ditunjuk membuka kembali dengan klaim yang tidak pernah
            // dilepas.
            overlayOwnsPointer_ = false;
            return;  // panel terlalu kecil atau sedang dilipat
        }

        const auto width = static_cast<uint32_t>(size.x);
        const auto height = static_cast<uint32_t>(size.y);
        renderer->Resize(width, height);

        TerrainView terrainView;
        terrainView.store = context.terrains;
        terrainView.cameraPosition = camera_.focus + camera_.Offset() * camera_.distance;
        // Baker-nya dipasang tiap frame, bukan sekali saat panel dibuat: panel
        // bisa hidup lebih dulu daripada `EditorContext` yang lengkap, dan yang
        // dipasang sekali dari konteks setengah jadi adalah null selamanya.
        sceneView_.SetTextureBakery(context.textureBakery);
        sceneView_.SetMeshSdfBakery(context.meshSdfBakery);
        sceneView_.SetMeshGeometryCache(context.meshGeometry);
        sceneView_.SetMaterialPrograms(context.materialPrograms);
        sceneView_.Build(*context.world, *context.selection, context.assets, renderer,
                         context.animation, context.builtinAssets, context.whiteboxes,
                         terrainView);

        // Batas geometrinya dicatat untuk panel lain — World Settings memakainya
        // menghitung kisi probe. Setelah Build, karena sebelum itu isinya milik
        // frame sebelumnya.
        context.sceneBounds.valid =
            sceneView_.GeometryBounds(context.sceneBounds.minimum, context.sceneBounds.maximum);
        sceneView_.BakeItems(context.bakeItems);

#if SIM_WITH_PROBE_BAKE
        // Panggangan yang selesai dipungut di sini dan dipasang ke renderer.
        // Di viewport, bukan di panel World Settings: yang memegang renderer ini
        // adalah viewport, dan sebuah panel yang kebetulan tidak terbuka tidak
        // boleh membuat panggangan menggantung selamanya.
        if (context.probeBakery != nullptr) {
            if (std::shared_ptr<const render::ProbeVolume> baked = context.probeBakery->Take()) {
                context.probeVolume = std::move(baked);
            }
        }
        if (context.lightmapBakery != nullptr) {
            if (std::shared_ptr<const render::Lightmap> baked = context.lightmapBakery->Take()) {
                context.lightmap = std::move(baked);
            }
        }
#endif
        // **Dibandingkan, bukan dipasang saat selesai memanggang.** Kisinya juga
        // bisa *hilang* — level yang ditutup melepasnya — dan sebuah pemasangan
        // yang hanya terjadi di ujung panggangan tidak pernah memberitahu
        // renderer tentang itu. Yang tersisa lalu adalah level baru yang disinari
        // cahaya tak-langsung level sebelumnya.
        if (pushedProbes_ != context.probeVolume) {
            pushedProbes_ = context.probeVolume;
            renderer->SetProbeVolume(pushedProbes_);
        }
        // Dibandingkan, bukan dipasang saat selesai memanggang — alasan yang
        // sama: atlasnya juga bisa hilang saat level berganti.
        if (pushedLightmap_ != context.lightmap) {
            pushedLightmap_ = context.lightmap;
            sceneView_.SetLightmap(pushedLightmap_);
            renderer->SetLightmap(pushedLightmap_);
        }

        HandleCameraInput();

        render::ViewportDesc desc;
        desc.width = width;
        desc.height = height;
        desc.mode = drawMode_;
        desc.showGrid = showGrid_;
        desc.gi = context.gi;
        desc.post = context.post;
        desc.computeGradient = context.computeGradient;
        desc.gpuClusters = context.gpuClusters;
        desc.gpuSdf = context.gpuSdf;
        // **Langit datang dari scene, bukan dari sakelar viewport.** Level tanpa
        // entity ber-SkyComponent tidak menggambar langit sama sekali — itu yang
        // membuat adegan interior berhenti membayar empat pass LUT untuk sesuatu
        // yang tidak pernah terlihat. Prefab "Sky Dome" yang menyalakannya.
        editor::ApplySceneSky(*context.world, desc);
        // **Tingkat pencahayaan datang dari level, bukan dari sakelar viewport**
        // — dan ia sengaja dipanggil sesudah `desc.gi = context.gi` di atas,
        // supaya yang tertulis di berkas yang menang. Panel Statistics menyunting
        // World Settings, bukan `context.gi.enabled`; yang tersisa di sana
        // hanyalah alat ukur, yang memang tidak disimpan ke mana pun.
        editor::ApplyWorldSettings(*context.world, desc);
        // **Satu turunan, di satu tempat.** Panel lain — Statistics — membaca
        // `context.gi.enabled` untuk memutuskan apa yang ditampilkannya, dan
        // menurunkannya sendiri di sana berarti dua tempat yang harus sepakat
        // tentang arti `indirect`. Yang di sini cerminan; yang di berkas level
        // kebenarannya.
        context.gi.enabled = desc.gi.enabled;
        // Jalur HDR yang relatif dilengkapi di sisi editor, bukan di renderer:
        // yang tahu di mana `Resources` bawaan berada adalah aplikasi yang
        // menyalinnya ke sebelah executable-nya. `hdriPath` hidup sampai
        // `Render` di bawah — `desc` hanya menunjuknya.
        const std::string hdriPath = editor::ResolveHdriPath(desc.hdriPath, context.builtinDir);
        desc.hdriPath = hdriPath;

        // Volume dimuat di sini, bukan di renderer: OpenVDB adalah pengondisi
        // aset. `LoadVolumeIfChanged` tidak melakukan apa-apa bila jalur dan
        // nama gridnya tidak berubah — memuat ulang berkas puluhan megabyte
        // tiap frame akan menghentikan editor.
        LoadVolumeIfChanged(context);
        if (context.volume.grid != nullptr) {
            // Batas volume digambar sebagai wireframe. Kotaknya dihitung
            // `VolumeGrid::WorldBounds` — definisi yang sama persis yang dipakai
            // raymarch-nya, jadi kotak ini tidak bisa berbohong tentang di mana
            // asapnya berhenti.
            Vec3 boxMin;
            Vec3 boxMax;
            context.volume.grid->WorldBounds(context.volume.position, context.volume.scale,
                                             boxMin, boxMax);
            sceneView_.AddWireBox(boxMin, boxMax, kVolumeBoundsColor);

            desc.volume.grid = context.volume.grid.get();
            desc.volume.revision = context.volume.revision;
            desc.volume.position = context.volume.position;
            desc.volume.scale = context.volume.scale;
            desc.volume.extinction = context.volume.extinction;
            desc.volume.stepSize = context.volume.stepSize;
            desc.volume.scatterAlbedo = context.volume.albedo;
            desc.volume.incomingLight = Vec3(context.volume.lightIntensity);
        }
        // Rangka kawat komponen, hanya untuk yang sedang terpilih.
        DrawSelectionWires(context);

        desc.clouds = context.clouds;
        camera_.ApplyTo(desc.camera);
        // Permintaan kamera dari luar panel — `viewport.capture` milik track AI.
        // Dikonsumsi di sini, sebelum `desc.camera` disusun, supaya frame ini
        // sudah menggambar dari sudut yang diminta alih-alih frame berikutnya.
        if (context.cameraRequest.pending) {
            const Vec3 offset = context.cameraRequest.from - context.cameraRequest.lookAt;
            const float distance = glm::length(offset);
            if (distance > 1e-4f) {
                const Vec3 direction = offset / distance;
                camera_.focus = context.cameraRequest.lookAt;
                camera_.distance = std::clamp(distance, 0.05f, 5000.0f);
                // Kebalikan `OrbitCamera::Offset()`, termasuk tanda minus pada X
                // yang dijelaskan di sana. Menurunkannya dengan cara lain
                // menghasilkan kamera yang menghadap ke arah cermin.
                camera_.pitch = std::clamp(std::asin(std::clamp(direction.y, -1.0f, 1.0f)),
                                           -OrbitCamera::kPitchLimit, OrbitCamera::kPitchLimit);
                camera_.yaw = std::atan2(-direction.x, direction.z);
            }
            context.cameraRequest.pending = false;
        }

        // **Tampak terkunci menyetel sudutnya tiap frame, bukan sekali saat
        // dipilih.** Perintah kamera dari luar panel — `viewport.capture` milik
        // track AI — menulis yaw dan pitch tepat di atas sini, dan tampak yang
        // hanya disetel sekali akan diam-diam berputar begitu perintah itu
        // datang. Yang terlihat bukan galat melainkan tampak Depan yang bukan
        // depan lagi.
        if (IsAxisView(viewpoint_)) {
            AxisViewAngles(viewpoint_, camera_.yaw, camera_.pitch);
            camera_.ApplyTo(desc.camera);
        }

        desc.camera.orthographic = orthographic_ || IsAxisView(viewpoint_);
        desc.camera.orthoHeight = camera_.distance;

        // Memandang lewat kamera adegan menggantikan seluruh kameranya, bukan
        // sebagian: posisi, rotasi, dan proyeksinya datang dari entity itu.
        // Mencampurnya dengan `camera_` berarti bidang dekat editor dipakai
        // menggambar apa yang seharusnya dilihat kamera adegan.
        if (viewpoint_ == Viewpoint::SceneCamera) {
            ApplySceneCamera(context, desc.camera);
        }

        renderer->Render(desc, sceneView_.Scene());

        const ImVec2 imagePos = ImGui::GetCursorScreenPos();
        // Diumumkan supaya `viewport.capture` bisa memotong tangkapan jendela ke
        // gambar ini saja. Relatif terhadap titik asal viewport utama: `imagePos`
        // berada di ruang layar virtual ImGui, yang titik nolnya bukan pojok
        // jendela ketika multi-viewport menyala.
        if (const ImGuiViewport* main = ImGui::GetMainViewport()) {
            context.viewportRect.position = Vec2(imagePos.x - main->Pos.x, imagePos.y - main->Pos.y);
            context.viewportRect.size = Vec2(size.x, size.y);
            context.viewportRect.mainSize = Vec2(main->Size.x, main->Size.y);
        }
        const render::TextureHandle texture = renderer->ColorTarget();
        if (texture != render::kInvalidTexture) {
            // UV diambil dari renderer, bukan (1,1): target render boleh lebih
            // besar dari panel supaya menyeret pemisah dock tidak memicu
            // alokasi ulang tiap frame.
            const Vec2 uvMax = renderer->ColorTargetUvMax();
            ImGui::Image(static_cast<ImTextureID>(texture), size, ImVec2(0.0f, 0.0f),
                         ImVec2(uvMax.x, uvMax.y));
        } else {
            ImGui::Dummy(size);
        }

        // Urutan pengajuan item di bawah ini menentukan siapa yang berhak atas
        // sebuah klik, dan itu bukan detail yang bisa ditukar-tukar.
        //
        // Tombol overlay diajukan lebih dulu di antara item interaktif. ImGui
        // memberikan klik kepada item terhovering yang diajukan paling awal,
        // jadi tombol Rotate menang atas permukaan viewport yang menaunginya.
        // Urutan sebaliknya membuat dua hal salah sekaligus: tombolnya tidak
        // bereaksi, dan kliknya ikut terhitung sebagai klik pada scene sehingga
        // seleksi terhapus.
        // Hover overlay dilaporkan oleh widget-nya sendiri, bukan ditanyakan ke
        // ImGui::IsAnyItemHovered().
        //
        // Fungsi itu ikut memeriksa hover frame SEBELUMNYA
        // (`HoveredIdPreviousFrame`), dan permukaan viewport di bawah ini adalah
        // sebuah item juga. Begitu kursor masuk ke viewport — persis keadaan
        // ketika gizmo dipakai — jawabannya selalu "ya", gizmo diberi
        // `interactive = false`, dan menyeretnya tidak menggerakkan apa pun.
        const WhiteboxTarget whiteboxTarget = FindWhiteboxTarget(context);
        const TerrainTarget terrainTarget = FindTerrainTarget(context);
        const bool overlayHovered =
            DrawOverlays(context, imagePos, size, *renderer, whiteboxTarget, terrainTarget);
        // Dibaca `HandleCameraInput` pada frame BERIKUTNYA — alasannya di tempat
        // medannya dideklarasikan.
        overlayOwnsPointer_ = overlayHovered;

        // Permukaan viewport sebagai item sungguhan, bukan status mouse mentah:
        // dengan begitu ImGui sendiri yang memutuskan klik ini milik siapa.
        //
        // Kecuali ketika gizmo yang memegang kursor. ImGuizmo menolak memulai
        // manipulasi selama `IsAnyItemHovered()` atau `IsAnyItemActive()` benar
        // — dan permukaan ini menutupi seluruh viewport, jadi selama ia diajukan
        // gizmo TIDAK PERNAH bisa dipakai. Yang menentukan keadaan frame lalu,
        // karena gizmo baru digambar setelah baris ini; jeda satu frame itu
        // tidak terasa, sedangkan urutan sebaliknya mustahil: keduanya
        // saling menunggu.
        //
        // Tidak menyerah saat seretan lain sedang berlangsung — seleksi kotak
        // yang kebetulan melewati gizmo tidak boleh putus di tengah jalan, dan
        // aset yang sedang diseret tetap butuh sasaran jatuh.
        const bool yieldToGizmo = gizmoOwnsPointer_ && !boxSelecting_ &&
                                  ImGui::GetDragDropPayload() == nullptr;
        bool surfacePressed = false;
        bool surfaceHeld = false;
        if (!yieldToGizmo) {
            ImGui::SetCursorScreenPos(imagePos);
            ImGui::InvisibleButton("##viewport_surface", size, ImGuiButtonFlags_MouseButtonLeft);
            surfacePressed = ImGui::IsItemActivated();
            surfaceHeld = ImGui::IsItemActive();
            HandleAssetDrop(context, imagePos, size, desc.camera);
        }

        const float aspect = size.x / size.y;
        const Mat4 view = desc.camera.View();
        const Mat4 projection = desc.camera.Projection(aspect);

        // Ikon digambar sebelum gizmo supaya gizmo selalu di atasnya.
        DrawEntityIcons(projection * view, imagePos, size);

        // Sorotan sisi: yang di bawah kursor dicari ulang tiap frame. Sinar
        // terhadap mesh blockout berbiaya beberapa puluh segitiga — jauh lebih
        // murah daripada menebak lewat cache yang harus dibatalkan tiap kali
        // bentuknya berubah, dan bentuknya berubah persis saat sedang diseret.
        if (EditingSides(whiteboxTarget)) {
            whitebox::PolygonHandle hovered = whitebox::PolygonHandle::Invalid;
            whitebox::VertexHandle hoveredVertex = whitebox::VertexHandle::Invalid;
            whitebox::EdgeHandle hoveredEdge = whitebox::EdgeHandle::Invalid;
            const bool pointing = !flying_ && !overlayHovered && !gizmoOwnsPointer_ &&
                                  ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
            // **Hanya yang sejenis mode aktif yang dicari.** Menyorot ketiganya
            // sekaligus berarti kursor di dekat sebuah sudut menyalakan simpul,
            // rusuk, dan sisi bersamaan — dan klik yang menyusul menjadi tebakan
            // di antara tiga jawaban yang sama-sama masuk akal.
            if (pointing) {
                const SubObject mode = context.whiteboxes->SubObjectMode();
                const Mat4 clip = projection * view;
                const ImVec2 cursor = ImGui::GetMousePos();
                switch (mode) {
                    case SubObject::Vertex:
                        hoveredVertex = PickWhiteboxVertex(whiteboxTarget, clip, imagePos, size,
                                                           cursor);
                        break;
                    case SubObject::Edge:
                        hoveredEdge =
                            PickWhiteboxEdge(whiteboxTarget, clip, imagePos, size, cursor);
                        break;
                    case SubObject::Face:
                        hovered = PickWhiteboxSide(whiteboxTarget, view, projection, imagePos,
                                                   size, cursor)
                                      .polygon;
                        break;
                }
            }
            DrawWhiteboxSubObjects(context, whiteboxTarget, projection * view, imagePos, size,
                                   hoveredVertex, hoveredEdge);
            DrawWhiteboxOverlay(context, whiteboxTarget, projection * view, imagePos, size,
                                hovered);
        }

        // Gizmo tetap tergambar walau kursor sedang di atas tombol overlay, tapi
        // tidak boleh ikut merespons kliknya: ia membaca mouse langsung dan tidak
        // tahu apa pun tentang item ImGui.
        //
        // Sisi yang terpilih mengambil alih gizmo dari entity-nya. Keduanya
        // tidak bisa tampil sekaligus: dua gizmo bertumpuk di tempat yang
        // berdekatan berarti separuh seretan mengenai yang bukan dimaksud.
        // Memahat mengambil alih tombol kiri sepenuhnya: gizmo tidak digambar,
        // dan klik tidak memilih apa pun. Alat yang menyeret tanah sambil
        // sesekali memindahkan objek yang kebetulan di bawah kursor adalah alat
        // yang tidak bisa dipercaya.
        if (SculptingTerrain(terrainTarget)) {
            HandleTerrainSculpt(context, terrainTarget, imagePos, size, view, projection,
                                !overlayHovered);
            gizmoOwnsPointer_ = false;
            HandleShortcuts(context, whiteboxTarget, terrainTarget);
            return;
        }

        // Gizmo mana yang berdiri ditentukan **jenis sub-objek yang terpilih**,
        // bukan hanya oleh mode blockout. Satu gizmo untuk ketiganya akan
        // menuntut sumbu yang berarti tiga hal berbeda menurut keadaan yang
        // tidak terlihat di gizmonya sendiri.
        bool gizmoBusy = false;
        if (EditingSides(whiteboxTarget)) {
            const SubObject mode = context.whiteboxes->SubObjectMode();
            if (mode == SubObject::Face) {
                gizmoBusy = whitebox::IsValid(SelectedSide(context, whiteboxTarget)) &&
                            DrawWhiteboxGizmo(context, whiteboxTarget, imagePos, size, view,
                                              projection, !overlayHovered);
            } else {
                gizmoBusy = DrawWhiteboxVertexGizmo(context, whiteboxTarget, imagePos, size,
                                                    view, projection, !overlayHovered);
            }
        }
        if (!gizmoBusy && !EditingSides(whiteboxTarget)) {
            gizmoBusy = DrawGizmoAndApply(context, imagePos, size, view, projection,
                                          !overlayHovered);
        }
        gizmoOwnsPointer_ = gizmoBusy;
        HandleSelectionInput(context, whiteboxTarget, imagePos, size, view, projection, gizmoBusy,
                             surfacePressed, surfaceHeld);
        HandleShortcuts(context, whiteboxTarget, terrainTarget);
    }

private:
    // --- kamera -------------------------------------------------------------

    /// Mencatat tombol mana yang ditekan sementara kursor berada di atas
    /// overlay, dan melepasnya kembali begitu tombolnya lepas.
    ///
    /// **Klaimnya dipegang sampai tombol dilepas, bukan diperiksa ulang tiap
    /// frame**, dan itu justru inti persoalannya. `IsMouseDragging` menjawab
    /// menurut keadaan kursor sekarang, bukan menurut tempat seretan itu
    /// dimulai — jadi syarat per-frame menyerahkan seretan kepada kamera persis
    /// pada saat kursor bergeser keluar dari tombolnya. Tombol overlay
    /// berukuran beberapa puluh piksel, jadi "bergeser keluar" itu yang biasa
    /// terjadi, bukan pengecualian.
    void UpdateOverlayPointerClaims() {
        for (int button = 0; button < ImGuiMouseButton_COUNT; ++button) {
            auto& claimed = overlayOwnsButton_[static_cast<std::size_t>(button)];
            if (!ImGui::IsMouseDown(button)) {
                claimed = false;
            } else if (ImGui::IsMouseClicked(button)) {
                claimed = overlayOwnsPointer_;
            }
        }
    }

    bool OverlayOwns(ImGuiMouseButton button) const {
        return overlayOwnsButton_[static_cast<std::size_t>(button)];
    }

    void HandleCameraInput() {
        ImGuiIO& io = ImGui::GetIO();
        const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);
        UpdateOverlayPointerClaims();

        // Mode terbang bertahan selama tombol kanan ditahan, walaupun kursor
        // sudah keluar dari panel. Kalau syaratnya "hovered" terus-menerus,
        // memutar pandangan cepat akan terputus di tengah jalan.
        //
        // **Tidak dimulai kalau tombolnya ditekan di atas overlay.** Tombol
        // overlay tidak dibuat dengan `ImGuiButtonFlags_MouseButtonRight`, jadi
        // ImGui tidak pernah memberinya `ActiveId` untuk klik kanan — dan tanpa
        // klaim di sini, klik kanan di atas tombol mana pun memutar pandangan.
        if (hovered && !OverlayOwns(ImGuiMouseButton_Right) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            flying_ = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            flying_ = false;
        }
        // Terbang mengubah yaw dan pitch, yaitu persis dua angka yang dikunci
        // tampak sumbu — dan pada kamera adegan ia menerbangkan kamera editor
        // yang sedang tidak dipakai menggambar apa pun.
        if (viewpoint_ != Viewpoint::Perspective) {
            flying_ = false;
        }

        if (flying_) {
            const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Right);
            // +delta.y: sumbu Y layar tumbuh ke bawah, dan pitch positif berarti
            // pandangan menunduk. Menyeret ke bawah = melihat ke bawah.
            camera_.Look(delta.x * kLookSensitivity, delta.y * kLookSensitivity);

            // Roda mengatur kecepatan terbang, bukan jarak orbit — konvensi yang
            // sama dengan editor lain, dan lebih berguna: saat sedang terbang
            // yang ingin diubah adalah laju, bukan zoom.
            if (io.MouseWheel != 0.0f) {
                flySpeed_ = std::clamp(flySpeed_ * std::pow(1.15f, io.MouseWheel), kFlySpeedMin,
                                       kFlySpeedMax);
            }
            ApplyFlyMovement(io.DeltaTime);
            return;
        }

        if (!hovered) {
            return;
        }

        // **Roda tidak ikut diklaim, dan itu keputusan, bukan kelalaian.**
        // Sebuah putaran roda selesai pada frame yang sama; ia tidak punya awal
        // yang bisa jatuh di satu tempat dan akhir di tempat lain. Tombol
        // overlay juga tidak melakukan apa-apa dengan roda, jadi menahannya
        // hanya membuat menggulung di atas ikon sebesar dua puluh piksel
        // berhenti bekerja — dan yang menggulung di sana memang sedang meminta
        // viewport di belakangnya mendekat.
        if (io.MouseWheel != 0.0f) {
            camera_.Zoom(io.MouseWheel);
        }
        // **Rotasi mati pada tampak terkunci; geser dan zoom tidak.** Sebuah
        // tampak Atas yang bisa diputar bukan tampak Atas lagi, dan yang
        // memutarnya tidak punya cara menemukan kembali sudut tepatnya. Yang
        // tetap hidup justru dua yang dipakai bekerja di tampak ortografik:
        // menggeser bidangnya dan mengubah cakupannya.
        if (io.KeyAlt && !IsAxisView(viewpoint_) && !OverlayOwns(ImGuiMouseButton_Left) &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            camera_.Look(delta.x * kLookSensitivity, delta.y * kLookSensitivity);
        } else if (!OverlayOwns(ImGuiMouseButton_Middle) &&
                   ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
            camera_.Pan(delta.x, delta.y);
        }
    }

    /// Pemilih titik pandang. Mengembalikan true selama ia memegang pointer.
    bool DrawViewpointCombo(EditorContext& context) {
        scene::World* world = context.world;
        const bool namedCamera = viewpoint_ == Viewpoint::SceneCamera && world != nullptr &&
                                 world->IsAlive(viewpointCamera_);
        char label[160];
        std::snprintf(label, sizeof(label), "%s  %s",
                      namedCamera ? icons::kCamera : ViewpointIcon(viewpoint_),
                      namedCamera ? world->NameOf(viewpointCamera_).c_str()
                                  : ViewpointLabel(viewpoint_));

        const auto entry = [this](Viewpoint kind) {
            char item[64];
            std::snprintf(item, sizeof(item), "%s  %s", ViewpointIcon(kind),
                          ViewpointLabel(kind));
            if (ImGui::Selectable(item, viewpoint_ == kind)) {
                viewpoint_ = kind;
                viewpointCamera_ = scene::kNullEntity;
            }
        };

        bool open = false;
        if (ImGui::BeginCombo("##viewpoint", label)) {
            open = true;
            entry(Viewpoint::Perspective);
            ImGui::Separator();
            entry(Viewpoint::Front);
            entry(Viewpoint::Back);
            entry(Viewpoint::Left);
            entry(Viewpoint::Right);
            entry(Viewpoint::Top);
            entry(Viewpoint::Bottom);
            ImGui::Separator();
            // **Kamera adegan didaftar walau kosong.** Sebuah bagian yang
            // menghilang ketika tidak ada isinya membuat orang mengira fiturnya
            // tidak ada; sebuah baris yang menyebutkan ketiadaannya membuat
            // mereka tahu apa yang harus dibuat.
            const std::vector<scene::Entity> cameras =
                world != nullptr ? SceneCameras(*world) : std::vector<scene::Entity>{};
            if (cameras.empty()) {
                ImGui::BeginDisabled();
                ImGui::TextUnformatted("No cameras in scene");
                ImGui::EndDisabled();
            }
            for (const scene::Entity camera : cameras) {
                char item[160];
                std::snprintf(item, sizeof(item), "%s  %s", icons::kCamera,
                              world->NameOf(camera).c_str());
                ImGui::PushID(static_cast<int>(static_cast<uint32_t>(camera)));
                if (ImGui::Selectable(item, viewpoint_ == Viewpoint::SceneCamera &&
                                                viewpointCamera_ == camera)) {
                    viewpoint_ = Viewpoint::SceneCamera;
                    viewpointCamera_ = camera;
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        return open || ImGui::IsItemHovered() || ImGui::IsItemActive();
    }

    /// Mengumpulkan entity berkamera, urut sesuai hierarki.
    ///
    /// **Disusun ulang tiap kali menunya dibuka, bukan disimpan.** Kamera bisa
    /// ditambah, dihapus, dan diganti nama kapan saja; sebuah daftar yang
    /// di-cache adalah daftar yang suatu saat menawarkan kamera yang tidak ada
    /// lagi. Adegan seukuran apa pun yang muat di editor punya kamera dalam
    /// hitungan belasan, jadi jalan-jalannya tidak berarti apa-apa dibanding
    /// frame yang sedang digambar di belakang menunya.
    static void CollectCameras(scene::World& world, scene::Entity entity,
                               std::vector<scene::Entity>& out) {
        if (world.TryGet<scene::CameraComponent>(entity) != nullptr) {
            out.push_back(entity);
        }
        for (const scene::Entity child : world.ChildrenOf(entity)) {
            CollectCameras(world, child, out);
        }
    }

    static std::vector<scene::Entity> SceneCameras(scene::World& world) {
        std::vector<scene::Entity> cameras;
        for (const scene::Entity root : world.Roots()) {
            CollectCameras(world, root, cameras);
        }
        return cameras;
    }

    /// Menyusun kamera viewport dari sebuah entity berkamera.
    ///
    /// Entity yang sudah mati — atau yang komponennya dicabut — mengembalikan
    /// viewport ke perspektif. **Dikembalikan, bukan dibiarkan membeku pada
    /// matriks terakhirnya:** viewport yang berhenti menanggapi navigasi tanpa
    /// satu pun petunjuk kenapa adalah viewport yang terbaca rusak.
    void ApplySceneCamera(EditorContext& context, render::Camera& camera) {
        scene::World* world = context.world;
        const scene::CameraComponent* component =
            world != nullptr && world->IsAlive(viewpointCamera_)
                ? world->TryGet<scene::CameraComponent>(viewpointCamera_)
                : nullptr;
        if (component == nullptr) {
            viewpoint_ = Viewpoint::Perspective;
            viewpointCamera_ = scene::kNullEntity;
            camera_.ApplyTo(camera);
            camera.orthographic = orthographic_;
            return;
        }

        const Mat4 matrix = world->WorldMatrix(viewpointCamera_);
        camera.position = Vec3(matrix[3]);
        // **Basisnya dinormalkan lebih dulu.** `quat_cast` menuntut matriks
        // rotasi ortonormal, dan sebuah kamera yang kebetulan berada di bawah
        // parent berskala membawa skala itu di basisnya — yang keluar bukan
        // rotasi yang sedikit meleset melainkan quaternion tak sah.
        Mat3 basis(matrix);
        for (int axis = 0; axis < 3; ++axis) {
            const float length = glm::length(basis[axis]);
            basis[axis] = length > 1e-6f ? basis[axis] / length : Vec3(axis == 0, axis == 1,
                                                                      axis == 2);
        }
        camera.rotation = glm::normalize(glm::quat_cast(basis));
        camera.fovYRadians = component->fovYRadians;
        camera.nearZ = component->nearZ;
        camera.farZ = component->farZ;
        camera.orthographic = component->orthographic;
        camera.orthoHeight = component->orthoHeight;
    }

    void ApplyFlyMovement(float deltaSeconds) {
        ImGuiIO& io = ImGui::GetIO();
        // Jangan rebut tombol saat pengguna sedang mengetik di panel lain,
        // kalau tidak mengetik "was" di kotak pencarian akan menerbangkan kamera.
        if (io.WantTextInput) {
            return;
        }

        Vec3 move{0.0f};
        const Vec3 forward = camera_.Forward();
        const Vec3 right = camera_.Right();

        if (ImGui::IsKeyDown(ImGuiKey_W)) {
            move += forward;
        }
        if (ImGui::IsKeyDown(ImGuiKey_S)) {
            move -= forward;
        }
        if (ImGui::IsKeyDown(ImGuiKey_D)) {
            move += right;
        }
        if (ImGui::IsKeyDown(ImGuiKey_A)) {
            move -= right;
        }
        // E/Q naik-turun mengikuti sumbu dunia, bukan sumbu kamera: saat kamera
        // menunduk, "naik" yang diharapkan tetap ke langit.
        if (ImGui::IsKeyDown(ImGuiKey_E)) {
            move += kUp;
        }
        if (ImGui::IsKeyDown(ImGuiKey_Q)) {
            move -= kUp;
        }

        const float lengthSquared = glm::dot(move, move);
        if (lengthSquared < 1e-6f) {
            return;
        }
        // Dinormalkan supaya bergerak diagonal (W+D) tidak lebih cepat daripada
        // lurus.
        move /= std::sqrt(lengthSquared);

        float speed = flySpeed_;
        if (io.KeyShift) {
            speed *= 4.0f;
        }
        if (io.KeyCtrl) {
            speed *= 0.25f;
        }
        camera_.Fly(move, speed, deltaSeconds);
    }

    // --- menjatuhkan aset ---------------------------------------------------

    /// Menerima aset yang diseret dari Asset Browser dan membuat entity untuknya.
    void HandleAssetDrop(EditorContext& context, const ImVec2& imagePos, const ImVec2& size,
                         const render::Camera& camera) {
        if (!ImGui::BeginDragDropTarget()) {
            return;
        }
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("SIM_ASSET");
        if (payload == nullptr || payload->DataSize != sizeof(Uuid)) {
            ImGui::EndDragDropTarget();
            return;
        }
        const Uuid guid = *static_cast<const Uuid*>(payload->Data);
        ImGui::EndDragDropTarget();

        const assets::AssetRecord* record =
            context.assets != nullptr ? context.assets->Find(guid) : nullptr;
        if (record == nullptr) {
            return;
        }
        // Mesh dan whitebox yang bisa dijatuhkan ke scene. Menjatuhkan tekstur
        // di sini tidak punya arti yang jelas — tekstur dipasang ke field
        // material, bukan berdiri sendiri sebagai objek.
        const bool isWhitebox = record->type == assets::AssetType::Whitebox;
        const bool isTerrain = record->type == assets::AssetType::Terrain;
        if (record->type != assets::AssetType::Mesh && !isWhitebox && !isTerrain) {
            context.notifications->Warning(std::string("Cannot place a ") +
                                           assets::ToString(record->type) + " in the scene");
            return;
        }

        const ImVec2 mouse = ImGui::GetMousePos();
        // Terrain berdiri di titik asal, bukan di bawah kursor. Titik asalnya
        // adalah **sudut** petanya, bukan pusatnya, jadi menjatuhkannya di
        // tempat kursor kebetulan berada menaruh peta empat kilometer di
        // sembarang tempat — dan hampir setiap kali yang berikutnya dilakukan
        // orang adalah menolkan transformnya kembali.
        const Vec3 position =
            isTerrain ? Vec3(0.0f)
                      : GroundPointUnderCursor(
                            camera, size, Vec2(mouse.x - imagePos.x, mouse.y - imagePos.y));

        context.history->CloseMergeGroup();
        const Uuid entityGuid = Uuid::Generate();
        const std::string name = record->name;
        context.history->Execute(std::make_unique<LambdaCommand>(
            "Place " + name,
            [world = context.world, selection = context.selection, entityGuid, guid, position, name,
             isWhitebox, isTerrain]() {
                const scene::Entity entity = world->CreateWithGuid(entityGuid, name);
                world->TryGet<scene::TransformComponent>(entity)->position = position;
                world->MarkTransformDirty(entity);
                if (isTerrain) {
                    scene::TerrainComponent terrain;
                    terrain.terrain.guid = guid;
                    world->Add<scene::TerrainComponent>(entity, terrain);
                } else if (isWhitebox) {
                    scene::WhiteboxComponent whitebox;
                    whitebox.whitebox.guid = guid;
                    world->Add<scene::WhiteboxComponent>(entity, whitebox);
                } else {
                    scene::MeshRendererComponent renderer;
                    renderer.mesh.guid = guid;
                    world->Add<scene::MeshRendererComponent>(entity, renderer);
                }
                selection->SelectOnly(ToSelectionId(entity));
            },
            [world = context.world, selection = context.selection, entityGuid]() {
                world->Destroy(world->FindByGuid(entityGuid));
                selection->Clear();
            }));
    }

    /// Titik pada bidang tanah (y = 0) di bawah kursor.
    ///
    /// Kalau sinarnya menjauh dari tanah — kamera menengadah — objek diletakkan
    /// beberapa meter di depan kamera. Menolak menempatkan apa pun dalam
    /// keadaan itu terasa seperti seretan yang gagal tanpa sebab.
    static Vec3 GroundPointUnderCursor(const render::Camera& camera, const ImVec2& size,
                                       const Vec2& point) {
        const float aspect = size.x / size.y;
        const Ray ray = ScreenPointToRay(camera.View(), camera.Projection(aspect),
                                         Vec2(size.x, size.y), point);
        constexpr float kMinSlope = 1e-4f;
        if (ray.direction.y < -kMinSlope) {
            const float distance = -ray.origin.y / ray.direction.y;
            if (distance > 0.0f) {
                return ray.origin + ray.direction * distance;
            }
        }
        return ray.origin + ray.direction * 10.0f;
    }

    // --- ikon entity --------------------------------------------------------

    /// Menggambar penanda lampu, kamera, dan node kosong sebagai glyph.
    ///
    /// Memakai font ikon yang sudah dimuat editor, bukan tekstur billboard.
    /// Hasilnya tajam di skala DPI mana pun, tidak menambah satu pun aset, dan
    /// ikonnya sama persis dengan yang dipakai Outliner untuk entity yang sama.
    void DrawEntityIcons(const Mat4& viewProjection, const ImVec2& imagePos, const ImVec2& size) {
        const auto& icons = sceneView_.Icons();
        if (icons.empty()) {
            return;
        }

        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImFont* font = ImGui::GetFont();
        const float glyphSize = ImGui::GetFontSize() * kIconScale;

        // Dipotong ke area gambar: tanpa ini ikon di luar tepi viewport akan
        // tergambar menumpuk panel tetangga.
        draw->PushClipRect(imagePos, ImVec2(imagePos.x + size.x, imagePos.y + size.y), true);
        for (const SceneView::EntityIcon& icon : icons) {
            Vec2 screen;
            if (!WorldToScreen(viewProjection, Vec2(imagePos.x, imagePos.y), Vec2(size.x, size.y),
                               icon.position, screen)) {
                continue;
            }
            // Penanda tanpa glyph dilewati. `SceneView::IconGlyphs` boleh
            // kosong — itulah yang membuatnya bisa dipakai player — dan
            // `CalcTextSizeA(nullptr)` bukan gambar yang hilang melainkan
            // segfault di dalam ImGui.
            if (icon.glyph == nullptr) {
                continue;
            }
            const Vec4 color = icon.selected ? kSelectionColor : icon.color;
            const ImU32 packed = ImGui::ColorConvertFloat4ToU32(
                ImVec4(color.r, color.g, color.b, color.a));
            const ImVec2 extent = font->CalcTextSizeA(glyphSize, FLT_MAX, 0.0f, icon.glyph);
            const ImVec2 at(screen.x - extent.x * 0.5f, screen.y - extent.y * 0.5f);

            // Bayangan tipis di belakang glyph. Ikon melayang di atas grid dan
            // wireframe yang warnanya berdekatan; tanpa ini penanda gelap
            // hilang di latar gelap.
            draw->AddText(font, glyphSize, ImVec2(at.x + 1.0f, at.y + 1.0f),
                          IM_COL32(0, 0, 0, 140), icon.glyph);
            draw->AddText(font, glyphSize, at, packed, icon.glyph);
        }
        draw->PopClipRect();
    }

    // --- gizmo --------------------------------------------------------------

    /// Entity terpilih yang tidak punya leluhur yang juga terpilih.
    ///
    /// Hanya entity ini yang digerakkan gizmo. Keturunannya ikut bergerak
    /// dengan sendirinya lewat hierarki; menggerakkannya sekali lagi secara
    /// langsung akan menggandakan perpindahannya.
    std::vector<scene::Entity> TopLevelSelection(const EditorContext& context) const {
        std::vector<scene::Entity> entities;
        for (const uint64_t id : context.selection->Items()) {
            const scene::Entity entity = ToEntity(id);
            if (context.world->IsAlive(entity)) {
                entities.push_back(entity);
            }
        }
        std::vector<scene::Entity> result;
        for (const scene::Entity entity : entities) {
            const bool hasSelectedAncestor =
                std::any_of(entities.begin(), entities.end(), [&](scene::Entity other) {
                    return other != entity && context.world->IsDescendantOf(entity, other);
                });
            if (!hasSelectedAncestor) {
                result.push_back(entity);
            }
        }
        return result;
    }

    /// Menggambar gizmo dan menerapkan hasilnya. Mengembalikan true bila gizmo
    /// sedang dipakai atau ditunjuk, sehingga klik tidak boleh dianggap seleksi.
    bool DrawGizmoAndApply(EditorContext& context, const ImVec2& imagePos, const ImVec2& size,
                           const Mat4& view, const Mat4& projection, bool interactive) {
        if (operation_ == GizmoOperation::None) {
            return false;
        }
        const scene::Entity primary = ToEntity(context.selection->Primary());
        if (!context.world->IsAlive(primary)) {
            return false;
        }

        const Mat4 pivot = context.world->WorldMatrix(primary);
        const GizmoResult result =
            DrawGizmo(Vec2(imagePos.x, imagePos.y), Vec2(size.x, size.y), view, projection,
                      operation_, space_, snap_, pivot, interactive);

        if (result.active && !dragging_) {
            BeginDrag(context, pivot);
        }
        if (result.changed && dragging_) {
            ApplyDrag(context, result.transform);
        }
        if (!result.active && dragging_) {
            EndDrag(context);
        }
        return result.active || result.hovered;
    }

    void BeginDrag(const EditorContext& context, const Mat4& pivot) {
        dragging_ = true;
        pivotAtDragStart_ = pivot;
        dragItems_.clear();

        for (const scene::Entity entity : TopLevelSelection(context)) {
            const auto* transform = context.world->TryGet<scene::TransformComponent>(entity);
            if (transform == nullptr) {
                continue;
            }
            DragItem item;
            item.guid = context.world->GuidOf(entity);
            item.entity = entity;
            item.before = *transform;
            item.worldAtDragStart = context.world->WorldMatrix(entity);
            dragItems_.push_back(item);
        }
    }

    void ApplyDrag(EditorContext& context, const Mat4& newPivot) {
        if (dragItems_.empty()) {
            return;
        }
        // Selisih dihitung terhadap keadaan awal seretan, bukan terhadap frame
        // sebelumnya. Menumpuk selisih per frame akan menumpuk galat pembulatan
        // juga, dan hasil snapping tidak akan pernah persis kelipatan.
        const Mat4 delta = newPivot * glm::inverse(pivotAtDragStart_);

        std::vector<SetTransformsCommand::Item> items;
        items.reserve(dragItems_.size());

        for (const DragItem& drag : dragItems_) {
            if (!context.world->IsAlive(drag.entity)) {
                continue;
            }
            const Mat4 newWorld = delta * drag.worldAtDragStart;

            // Hasil gizmo selalu dalam ruang dunia, sedangkan yang disimpan
            // komponen adalah transform lokal. Untuk entity yang punya induk,
            // keduanya berbeda — dan melewatkan langkah ini membuat anak
            // melompat begitu induknya tidak di titik nol.
            const scene::Entity parent = context.world->ParentOf(drag.entity);
            const Mat4 local = context.world->IsAlive(parent)
                                   ? glm::inverse(context.world->WorldMatrix(parent)) * newWorld
                                   : newWorld;

            SetTransformsCommand::Item item;
            item.guid = drag.guid;
            item.before = drag.before;
            item.after = drag.before;
            DecomposeTransform(local, item.after.position, item.after.rotation, item.after.scale);
            items.push_back(item);
        }
        if (items.empty()) {
            return;
        }

        context.history->Execute(std::make_unique<SetTransformsCommand>(
            context.world, std::move(items), OperationLabel()));
    }

    void EndDrag(EditorContext& context) {
        dragging_ = false;
        dragItems_.clear();
        // Menutup grup merge di sini adalah yang membuat satu seretan menjadi
        // tepat satu entri undo: selama tombol ditahan setiap frame digabung ke
        // entri yang sama, dan begitu dilepas entri berikutnya berdiri sendiri.
        context.history->CloseMergeGroup();
    }

    std::string OperationLabel() const {
        switch (operation_) {
            case GizmoOperation::Translate:
                return "Move";
            case GizmoOperation::Rotate:
                return "Rotate";
            case GizmoOperation::Scale:
                return "Scale";
            case GizmoOperation::None:
                break;
        }
        return "Transform";
    }

    // --- whitebox -----------------------------------------------------------

    /// Sasaran penyuntingan sisi: entity terpilih yang membawa whitebox.
    ///
    /// Dicari sekali per frame dan diedarkan, bukan dicari ulang oleh tiap
    /// bagian: pemilihan, sorotan, dan gizmo harus berbicara tentang whitebox
    /// yang sama persis, dan tiga pencarian terpisah bisa menjawab beda ketika
    /// seleksi berubah di tengah frame.
    struct WhiteboxTarget {
        whitebox::WhiteboxMesh* box = nullptr;
        Uuid guid;
        Mat4 world{1.0f};
        Mat4 worldInverse{1.0f};

        explicit operator bool() const { return box != nullptr; }
    };

    WhiteboxTarget FindWhiteboxTarget(EditorContext& context) const {
        WhiteboxTarget target;
        if (context.whiteboxes == nullptr || context.assets == nullptr) {
            return target;
        }
        const scene::Entity primary = ToEntity(context.selection->Primary());
        if (!context.world->IsAlive(primary)) {
            return target;
        }
        const auto* component = context.world->TryGet<scene::WhiteboxComponent>(primary);
        if (component == nullptr || !component->whitebox.IsValid()) {
            return target;
        }
        const assets::AssetRecord* record = context.assets->Find(component->whitebox.guid);
        if (record == nullptr) {
            return target;
        }
        whitebox::WhiteboxMesh* box =
            context.whiteboxes->Get(component->whitebox.guid, context.assets->AbsolutePath(*record));
        if (box == nullptr) {
            return target;
        }
        target.box = box;
        target.guid = component->whitebox.guid;
        target.world = context.world->WorldMatrix(primary);
        target.worldInverse = glm::inverse(target.world);
        return target;
    }

    static whitebox::PolygonHandle SelectedSide(const EditorContext& context,
                                                const WhiteboxTarget& target) {
        const SideSelection& side = context.whiteboxes->Selected();
        return side.asset == target.guid ? side.PrimaryPolygon() : whitebox::PolygonHandle::Invalid;
    }

    /// True bila klik di viewport berarti "pilih sisi", bukan "pilih entity".
    bool EditingSides(const WhiteboxTarget& target) const {
        return whiteboxMode_ && static_cast<bool>(target);
    }

    /// Sisi yang tertembus sinar layar.
    ///
    /// **Sinarnya yang dipindahkan ke ruang lokal, bukan meshnya ke ruang
    /// dunia:** satu matriks dikalikan dua vektor, bukan sekali per simpul.
    whitebox::PolygonHit PickWhiteboxSide(const WhiteboxTarget& target, const Mat4& view,
                                          const Mat4& projection, const ImVec2& imagePos,
                                          const ImVec2& size, const ImVec2& point) const {
        const Ray ray = ScreenPointToRay(view, projection, Vec2(size.x, size.y),
                                         Vec2(point.x - imagePos.x, point.y - imagePos.y));
        const Vec3 origin = Vec3(target.worldInverse * Vec4(ray.origin, 1.0f));
        const Vec3 direction = Vec3(target.worldInverse * Vec4(ray.direction, 0.0f));
        return whitebox::PickPolygon(*target.box, origin, direction);
    }

    /// Simpul dan rusuk whitebox di layar, beserta yang tersorot dan terpilih.
    ///
    /// **Rusuk yang digambar rusuk poligon, bukan rusuk face.** Diagonal yang
    /// disembunyikan penggabungan sebidang adalah detail mesin; menunjukkannya
    /// berarti membatalkan seluruh guna lapisan poligon — dan menawarkan
    /// perancang memilih sesuatu yang tidak ia anggap ada.
    ///
    /// Keduanya tergambar di mode mana pun. Yang ditentukan mode hanyalah apa
    /// yang bisa diklik; melihat bentuk yang sedang disunting selalu berguna.
    void DrawWhiteboxSubObjects(const EditorContext& context, const WhiteboxTarget& target,
                                const Mat4& viewProjection, const ImVec2& imagePos,
                                const ImVec2& size, whitebox::VertexHandle hoveredVertex,
                                whitebox::EdgeHandle hoveredEdge) {
        const whitebox::HalfEdgeMesh& mesh = target.box->Mesh();
        const SideSelection& side = context.whiteboxes->Selected();
        const bool mine = side.asset == target.guid;
        const Mat4 clip = viewProjection * target.world;
        const Vec2 origin(imagePos.x, imagePos.y);
        const Vec2 extent(size.x, size.y);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(imagePos, ImVec2(imagePos.x + size.x, imagePos.y + size.y), true);

        for (uint32_t e = 0; e < mesh.EdgeCount(); ++e) {
            const whitebox::EdgeHandle edge = static_cast<whitebox::EdgeHandle>(e);
            if (target.box->Polygons().IsEdgeHidden(edge)) {
                continue;
            }
            const auto [halfA, halfB] = mesh.EdgeHalfEdges(edge);
            if (!whitebox::IsValid(halfA) || !whitebox::IsValid(halfB)) {
                continue;
            }
            Vec2 a;
            Vec2 b;
            if (!WorldToScreen(clip, origin, extent,
                               mesh.GetVertex(mesh.GetHalfEdge(halfA).origin).position, a) ||
                !WorldToScreen(clip, origin, extent,
                               mesh.GetVertex(mesh.GetHalfEdge(halfB).origin).position, b)) {
                continue;
            }

            Vec4 color = kSubObjectIdle;
            float thickness = 1.4f;
            if (mine && side.Contains(edge)) {
                color = kSubObjectSelected;
                thickness = 2.6f;
            } else if (edge == hoveredEdge) {
                color = kSubObjectHovered;
                thickness = 2.2f;
            }
            draw->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), ToColor(color), thickness);
        }

        for (uint32_t v = 0; v < mesh.VertexCount(); ++v) {
            const whitebox::VertexHandle vertex = static_cast<whitebox::VertexHandle>(v);
            Vec2 screen;
            if (!WorldToScreen(clip, origin, extent, mesh.GetVertex(vertex).position, screen)) {
                continue;
            }

            Vec4 color = kSubObjectIdle;
            float radius = kVertexMarkerRadius;
            if (mine && side.Contains(vertex)) {
                color = kSubObjectSelected;
                radius += 1.5f;
            } else if (vertex == hoveredVertex) {
                color = kSubObjectHovered;
                radius += 1.0f;
            }
            // Persegi, bukan lingkaran: pada tiga piksel sebuah lingkaran
            // menjadi gumpalan yang sudutnya tidak bisa dibaca, dan yang sedang
            // dicari mata justru sudut.
            draw->AddRectFilled(ImVec2(screen.x - radius, screen.y - radius),
                                ImVec2(screen.x + radius, screen.y + radius), ToColor(color));
            draw->AddRect(ImVec2(screen.x - radius, screen.y - radius),
                          ImVec2(screen.x + radius, screen.y + radius),
                          ToColor(Vec4(0.05f, 0.06f, 0.07f, 0.9f)));
        }

        draw->PopClipRect();
    }

    /// Simpul terdekat ke kursor, atau invalid bila tidak ada yang cukup dekat.
    ///
    /// **Di ruang layar, bukan dengan sinar.** Simpul tidak punya luas untuk
    /// ditembak; yang menentukan mana yang dimaksud adalah jaraknya dari kursor
    /// dalam piksel — aturan yang sama yang membuat ikon entity menang atas mesh
    /// besar yang menaunginya.
    whitebox::VertexHandle PickWhiteboxVertex(const WhiteboxTarget& target,
                                              const Mat4& viewProjection,
                                              const ImVec2& imagePos, const ImVec2& size,
                                              const ImVec2& point) const {
        const whitebox::HalfEdgeMesh& mesh = target.box->Mesh();
        const Mat4 clip = viewProjection * target.world;
        const Vec2 origin(imagePos.x, imagePos.y);
        const Vec2 extent(size.x, size.y);
        const Vec2 cursor(point.x, point.y);

        whitebox::VertexHandle best = whitebox::VertexHandle::Invalid;
        float nearest = kVertexPickRadius;
        for (uint32_t v = 0; v < mesh.VertexCount(); ++v) {
            const whitebox::VertexHandle vertex = static_cast<whitebox::VertexHandle>(v);
            Vec2 screen;
            if (!WorldToScreen(clip, origin, extent, mesh.GetVertex(vertex).position, screen)) {
                continue;
            }
            const float distance = glm::length(cursor - screen);
            if (distance < nearest) {
                nearest = distance;
                best = vertex;
            }
        }
        return best;
    }

    /// Rusuk poligon terdekat ke kursor. Rusuk tersembunyi tidak ikut — ia tidak
    /// digambar, dan yang tidak terlihat tidak boleh bisa terpilih.
    whitebox::EdgeHandle PickWhiteboxEdge(const WhiteboxTarget& target,
                                          const Mat4& viewProjection, const ImVec2& imagePos,
                                          const ImVec2& size, const ImVec2& point) const {
        const whitebox::HalfEdgeMesh& mesh = target.box->Mesh();
        const Mat4 clip = viewProjection * target.world;
        const Vec2 origin(imagePos.x, imagePos.y);
        const Vec2 extent(size.x, size.y);
        const Vec2 cursor(point.x, point.y);

        whitebox::EdgeHandle best = whitebox::EdgeHandle::Invalid;
        float nearest = kEdgePickRadius;
        for (uint32_t e = 0; e < mesh.EdgeCount(); ++e) {
            const whitebox::EdgeHandle edge = static_cast<whitebox::EdgeHandle>(e);
            if (target.box->Polygons().IsEdgeHidden(edge)) {
                continue;
            }
            const auto [halfA, halfB] = mesh.EdgeHalfEdges(edge);
            if (!whitebox::IsValid(halfA) || !whitebox::IsValid(halfB)) {
                continue;
            }
            Vec2 a;
            Vec2 b;
            if (!WorldToScreen(clip, origin, extent,
                               mesh.GetVertex(mesh.GetHalfEdge(halfA).origin).position, a) ||
                !WorldToScreen(clip, origin, extent,
                               mesh.GetVertex(mesh.GetHalfEdge(halfB).origin).position, b)) {
                continue;
            }
            const float distance = DistanceToSegment(cursor, a, b);
            if (distance < nearest) {
                nearest = distance;
                best = edge;
            }
        }
        return best;
    }

    void DrawWhiteboxOverlay(const EditorContext& context, const WhiteboxTarget& target,
                             const Mat4& viewProjection, const ImVec2& imagePos,
                             const ImVec2& size, whitebox::PolygonHandle hovered) {
        const whitebox::PolygonHandle selected = SelectedSide(context, target);
        // Yang tersorot digambar belakangan supaya ia menang bila keduanya sisi
        // yang sama — dan tidak digambar dua kali dengan warna bertumpuk.
        if (whitebox::IsValid(hovered) && hovered != selected) {
            DrawSideOverlay(target, viewProjection, imagePos, size, hovered,
                            IM_COL32(255, 255, 255, 26), IM_COL32(255, 255, 255, 140), 1.5f);
        }
        if (whitebox::IsValid(selected)) {
            DrawSideOverlay(target, viewProjection, imagePos, size, selected,
                            IM_COL32(255, 158, 51, 55), IM_COL32(255, 158, 51, 235), 2.5f);
        }
    }

    void DrawSideOverlay(const WhiteboxTarget& target, const Mat4& viewProjection,
                         const ImVec2& imagePos, const ImVec2& size,
                         whitebox::PolygonHandle polygon, ImU32 fill, ImU32 line,
                         float thickness) {
        const whitebox::PolygonOutline outline =
            whitebox::BuildPolygonOutline(*target.box, polygon);
        if (outline.empty()) {
            return;
        }
        const Mat4 clip = viewProjection * target.world;
        const Vec2 origin(imagePos.x, imagePos.y);
        const Vec2 extent(size.x, size.y);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(imagePos, ImVec2(imagePos.x + size.x, imagePos.y + size.y), true);
        for (const std::array<Vec3, 3>& triangle : outline.triangles) {
            ImVec2 corners[3];
            bool visible = true;
            for (int i = 0; i < 3 && visible; ++i) {
                Vec2 screen;
                visible = WorldToScreen(clip, origin, extent, triangle[static_cast<std::size_t>(i)],
                                        screen);
                corners[i] = ImVec2(screen.x, screen.y);
            }
            if (visible) {
                draw->AddTriangleFilled(corners[0], corners[1], corners[2], fill);
            }
        }
        for (const auto& [from, to] : outline.edges) {
            Vec2 a;
            Vec2 b;
            if (WorldToScreen(clip, origin, extent, from, a) &&
                WorldToScreen(clip, origin, extent, to, b)) {
                draw->AddLine(ImVec2(a.x, a.y), ImVec2(b.x, b.y), line, thickness);
            }
        }
        draw->PopClipRect();
    }

    /// Kerangka gizmo untuk sebuah sisi: titik beratnya, dengan sumbu ketiga
    /// menghadap keluar.
    ///
    /// **Sumbu mengikuti sisinya, bukan dunia.** Yang ingin dilakukan perancang
    /// adalah mendorong sisi keluar-masuk, dan dengan sumbu dunia arah itu
    /// berganti pegangan setiap kali bloknya diputar — pada sisi yang menyerong
    /// tidak ada satu pun pegangan yang benar.
    Mat4 SideFrame(const WhiteboxTarget& target, const whitebox::PolygonOutline& outline) const {
        // Inverse-transpose, karena normal bukan arah biasa: skala tak seragam
        // memiringkannya ke arah yang salah bila diperlakukan seperti vektor.
        Vec3 normal = Vec3(glm::transpose(target.worldInverse) * Vec4(outline.normal, 0.0f));
        if (glm::length(normal) < 1e-6f) {
            normal = Vec3(0.0f, 1.0f, 0.0f);
        }
        normal = glm::normalize(normal);

        const Vec3 reference =
            std::abs(normal.y) > 0.9f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);
        const Vec3 tangent = glm::normalize(glm::cross(reference, normal));
        const Vec3 bitangent = glm::cross(normal, tangent);

        Mat4 frame(1.0f);
        frame[0] = Vec4(tangent, 0.0f);
        frame[1] = Vec4(bitangent, 0.0f);
        frame[2] = Vec4(normal, 0.0f);
        frame[3] = Vec4(Vec3(target.world * Vec4(outline.centroid, 1.0f)), 1.0f);
        return frame;
    }

    /// Simpul yang ikut bergerak untuk seleksi yang sedang aktif.
    ///
    /// Mode rusuk mengembalikan **kedua ujung** tiap rusuk terpilih: sebuah rusuk
    /// tidak punya posisi sendiri di half-edge — ia sepasang simpul — jadi
    /// menggesernya berarti menggeser keduanya. Rusuk yang berbagi ujung tidak
    /// menggeser simpul itu dua kali; yang menjaganya adalah penyaringan kembar
    /// di sini, bukan di `TranslateVertices`.
    std::vector<whitebox::VertexHandle> SelectedVertices(const EditorContext& context,
                                                         const WhiteboxTarget& target) const {
        std::vector<whitebox::VertexHandle> result;
        const SideSelection& side = context.whiteboxes->Selected();
        if (side.asset != target.guid) {
            return result;
        }
        const whitebox::HalfEdgeMesh& mesh = target.box->Mesh();

        const auto append = [&](whitebox::VertexHandle vertex) {
            if (!whitebox::IsValid(vertex) ||
                static_cast<uint32_t>(vertex) >= mesh.VertexCount()) {
                return;
            }
            if (std::find(result.begin(), result.end(), vertex) == result.end()) {
                result.push_back(vertex);
            }
        };

        switch (side.mode) {
            case SubObject::Vertex:
                for (const whitebox::VertexHandle vertex : side.vertices) {
                    append(vertex);
                }
                break;
            case SubObject::Edge:
                for (const whitebox::EdgeHandle edge : side.edges) {
                    if (static_cast<uint32_t>(edge) >= mesh.EdgeCount()) {
                        continue;
                    }
                    const auto [halfA, halfB] = mesh.EdgeHalfEdges(edge);
                    if (whitebox::IsValid(halfA)) {
                        append(mesh.GetHalfEdge(halfA).origin);
                    }
                    if (whitebox::IsValid(halfB)) {
                        append(mesh.GetHalfEdge(halfB).origin);
                    }
                }
                break;
            case SubObject::Face:
                break;
        }
        return result;
    }

    /// Gizmo untuk seleksi simpul atau rusuk.
    ///
    /// **Berdiri di rata-rata simpulnya, bukan di titik berat berbobot luas.**
    /// Pembobotan luas ada karena sebuah sisi punya luas; sekumpulan simpul
    /// tidak, dan membobotinya dengan sesuatu berarti mengarang bobot.
    ///
    /// **Ruang dunia, bukan ruang lokal.** Sebuah sisi punya normal yang
    /// memberi sumbu ketiga arti — "dorong keluar" — sementara sekumpulan simpul
    /// tidak punya orientasi sama sekali. Sumbu yang diputar sesuai sesuatu yang
    /// dikarang lebih sulit dipakai daripada sumbu dunia yang jujur.
    bool DrawWhiteboxVertexGizmo(EditorContext& context, const WhiteboxTarget& target,
                                 const ImVec2& imagePos, const ImVec2& size, const Mat4& view,
                                 const Mat4& projection, bool interactive) {
        const std::vector<whitebox::VertexHandle> vertices = SelectedVertices(context, target);
        if (vertices.empty()) {
            return false;
        }

        const whitebox::HalfEdgeMesh& mesh = target.box->Mesh();
        Vec3 centre(0.0f);
        for (const whitebox::VertexHandle vertex : vertices) {
            centre += mesh.GetVertex(vertex).position;
        }
        centre /= static_cast<float>(vertices.size());
        const Vec3 pivot = Vec3(target.world * Vec4(centre, 1.0f));

        Mat4 frame(1.0f);
        frame[3] = Vec4(pivot, 1.0f);

        const GizmoResult result =
            DrawGizmo(Vec2(imagePos.x, imagePos.y), Vec2(size.x, size.y), view, projection,
                      GizmoOperation::Translate, GizmoSpace::World, snap_, frame, interactive);

        if (result.active && !whiteboxDrag_.active) {
            whiteboxDrag_ = WhiteboxDrag{};
            whiteboxDrag_.active = true;
            whiteboxDrag_.guid = target.guid;
            whiteboxDrag_.vertices = vertices;
            whiteboxDrag_.before = target.box->ToData();
            whiteboxDrag_.pivot = pivot;
        }
        if (result.changed && whiteboxDrag_.active) {
            ApplyWhiteboxVertexDrag(context, target, result.transform);
        }
        if (!result.active && whiteboxDrag_.active) {
            whiteboxDrag_.active = false;
            context.history->CloseMergeGroup();
        }
        return result.active || result.hovered;
    }

    void ApplyWhiteboxVertexDrag(EditorContext& context, const WhiteboxTarget& target,
                                 const Mat4& transform) {
        // Tiap frame dimulai dari keadaan awal seretan — alasan yang sama
        // dengan seretan sisi: menumpuk pergeseran per frame membuat jarak yang
        // ditempuh bergantung pada berapa frame yang sempat tergambar.
        std::string error;
        if (!whitebox::WhiteboxMesh::Build(*target.box, whiteboxDrag_.before, error)) {
            SIM_ERROR("Whitebox", "seretan tidak bisa membangun ulang mesh: {}", error);
            whiteboxDrag_.active = false;
            return;
        }

        const Vec3 deltaWorld = Vec3(transform[3]) - whiteboxDrag_.pivot;
        const Vec3 delta = Vec3(target.worldInverse * Vec4(deltaWorld, 0.0f));

        const whitebox::EditResult moved =
            target.box->TranslateVertices(whiteboxDrag_.vertices, delta);
        if (!moved.ok) {
            return;
        }

        context.history->Execute(std::make_unique<WhiteboxEditCommand>(
            context.whiteboxes, whiteboxDrag_.guid, whiteboxDrag_.before, target.box->ToData(),
            "Move Vertices"));
    }

    bool DrawWhiteboxGizmo(EditorContext& context, const WhiteboxTarget& target,
                           const ImVec2& imagePos, const ImVec2& size, const Mat4& view,
                           const Mat4& projection, bool interactive) {
        const whitebox::PolygonHandle side = SelectedSide(context, target);
        const whitebox::PolygonOutline outline = whitebox::BuildPolygonOutline(*target.box, side);
        if (outline.empty()) {
            return false;
        }
        const Mat4 frame = SideFrame(target, outline);

        // Selalu Translate: sisi tidak punya rotasi maupun skala yang berarti
        // bagi pengguna — memutar satu sisi dari sebuah blok tertutup akan
        // merobek blok itu.
        const GizmoResult result =
            DrawGizmo(Vec2(imagePos.x, imagePos.y), Vec2(size.x, size.y), view, projection,
                      GizmoOperation::Translate, GizmoSpace::Local, snap_, frame, interactive);

        if (result.active && !whiteboxDrag_.active) {
            BeginWhiteboxDrag(target, side, outline, Vec3(frame[3]));
        }
        if (result.changed && whiteboxDrag_.active) {
            ApplyWhiteboxDrag(context, target, result.transform);
        }
        if (!result.active && whiteboxDrag_.active) {
            whiteboxDrag_.active = false;
            context.history->CloseMergeGroup();
        }
        return result.active || result.hovered;
    }

    void BeginWhiteboxDrag(const WhiteboxTarget& target, whitebox::PolygonHandle side,
                           const whitebox::PolygonOutline& outline, const Vec3& pivot) {
        whiteboxDrag_ = WhiteboxDrag{};
        whiteboxDrag_.active = true;
        // Shift diputuskan sekali, di awal. Membacanya tiap frame berarti
        // menahannya di tengah seretan mengubah arti gerakan yang sedang
        // berlangsung — dan gerakan yang berubah arti di tengah jalan adalah
        // gerakan yang tidak bisa diarahkan.
        whiteboxDrag_.extruding = ImGui::GetIO().KeyShift;
        whiteboxDrag_.guid = target.guid;
        whiteboxDrag_.polygon = side;
        whiteboxDrag_.before = target.box->ToData();
        whiteboxDrag_.normal = outline.normal;
        whiteboxDrag_.pivot = pivot;
    }

    void ApplyWhiteboxDrag(EditorContext& context, const WhiteboxTarget& target,
                           const Mat4& transform) {
        // **Tiap frame dimulai dari keadaan awal seretan**, bukan dari frame
        // sebelumnya. Ekstrusi yang ditumpuk menghasilkan satu lapis dinding
        // per frame: seretan sepanjang satu meter akan meninggalkan puluhan
        // dinding tersembunyi di dalam bloknya, dan tak satu pun terlihat
        // sampai bloknya dipotong.
        std::string error;
        if (!whitebox::WhiteboxMesh::Build(*target.box, whiteboxDrag_.before, error)) {
            SIM_ERROR("Whitebox", "seretan tidak bisa membangun ulang mesh: {}", error);
            whiteboxDrag_.active = false;
            return;
        }

        const Vec3 deltaWorld = Vec3(transform[3]) - whiteboxDrag_.pivot;
        const Vec3 delta = Vec3(target.worldInverse * Vec4(deltaWorld, 0.0f));
        const float along = glm::dot(delta, whiteboxDrag_.normal);

        whitebox::PolygonHandle polygon = whiteboxDrag_.polygon;
        Vec3 displacement = delta;
        if (whiteboxDrag_.extruding) {
            // Di bawah ambang, sisi baru belum ditumbuhkan sama sekali — dinding
            // setebal seperseribu meter adalah geometri yang tidak bisa dipilih
            // lagi, dan menggesernya menyamping alih-alih menumbuhkannya berarti
            // Shift diam-diam berubah arti untuk gerakan kecil.
            if (std::abs(along) <= kWhiteboxExtrudeMin) {
                return;
            }
            const whitebox::EditResult grown = target.box->Extrude(polygon, along);
            if (!grown.ok) {
                return;
            }
            polygon = grown.polygon;
            displacement = delta - whiteboxDrag_.normal * along;
        }

        const whitebox::EditResult moved = target.box->Translate(polygon, displacement);
        if (moved.ok) {
            polygon = moved.polygon;
        }

        context.history->Execute(std::make_unique<WhiteboxEditCommand>(
            context.whiteboxes, whiteboxDrag_.guid, whiteboxDrag_.before, target.box->ToData(),
            whiteboxDrag_.extruding ? "Extrude Side" : "Move Side"));
        context.whiteboxes->Select(whiteboxDrag_.guid, polygon);
    }

    /// Menjatuhkan setiap entity terpilih ke permukaan di bawahnya.
    ///
    /// **Sinarnya ditembakkan dari titik asal masing-masing, ke bawah dunia**,
    /// dan bukan dari kamera: yang diminta "letakkan ini di lantai", bukan
    /// "letakkan ini di tempat yang sedang saya lihat".
    ///
    /// Seluruh seleksi dikecualikan dari sinarnya, bukan hanya benda yang sedang
    /// dihitung. Menjatuhkan tumpukan kotak sekaligus akan membuat yang di atas
    /// mendarat di yang di bawahnya — yang belum sempat turun — dan hasilnya
    /// tumpukan yang tetap melayang dengan jarak yang sama.
    void ConformSelectionToSurface(EditorContext& context, ConformOrientation orientation) {
        if (context.world == nullptr || context.selection == nullptr ||
            context.history == nullptr) {
            return;
        }

        std::vector<scene::Entity> ignore;
        for (const uint64_t id : context.selection->Items()) {
            const scene::Entity entity = ToEntity(id);
            if (context.world->IsAlive(entity)) {
                ignore.push_back(entity);
            }
        }
        if (ignore.empty()) {
            return;
        }

        std::vector<SetTransformsCommand::Item> items;
        std::size_t missed = 0;
        for (const scene::Entity entity : ignore) {
            const auto* transform = context.world->TryGet<scene::TransformComponent>(entity);
            if (transform == nullptr) {
                continue;
            }
            const Vec3 origin = Vec3(context.world->WorldMatrix(entity)[3]);
            const Ray ray{origin, Vec3(0.0f, -1.0f, 0.0f)};
            const SceneView::SurfaceHit hit = sceneView_.RaycastSurface(ray, ignore);
            if (!hit) {
                ++missed;
                continue;
            }

            SetTransformsCommand::Item item;
            item.guid = context.world->GuidOf(entity);
            item.before = *transform;
            // Transform lokal, permukaan dunia. Untuk entity berinduk keduanya
            // bukan ruang yang sama, dan menyamakannya di sini menuntut membawa
            // titik kena kembali ke ruang induknya.
            const scene::Entity parent = context.world->ParentOf(entity);
            Vec3 localPoint = hit.position;
            Vec3 localNormal = hit.normal;
            if (scene::IsValid(parent) && context.world->IsAlive(parent)) {
                const Mat4 inverse = glm::inverse(context.world->WorldMatrix(parent));
                localPoint = Vec3(inverse * Vec4(hit.position, 1.0f));
                localNormal = glm::normalize(Vec3(inverse * Vec4(hit.normal, 0.0f)));
            }
            item.after = ConformToSurface(*transform, localPoint, localNormal, orientation);
            items.push_back(item);
        }

        if (missed > 0 && context.notifications != nullptr) {
            // **Disebutkan, bukan didiamkan.** Yang paling sering menyebabkannya
            // adalah geometri yang masih dimuat — dan benda yang diam saja tanpa
            // sepatah kata terbaca sebagai perintah yang rusak.
            context.notifications->Info(std::to_string(missed) +
                                        " tidak menemukan permukaan di bawahnya");
        }
        if (items.empty()) {
            return;
        }
        context.history->Execute(std::make_unique<SetTransformsCommand>(
            context.world, std::move(items), "Conform to Surface"));
        context.history->CloseMergeGroup();
    }

    // --- terrain ------------------------------------------------------------

    /// Sasaran pahatan: entity terpilih yang membawa terrain.
    struct TerrainTarget {
        terrain::Terrain* map = nullptr;
        Uuid guid;
        Mat4 world{1.0f};
        Mat4 worldInverse{1.0f};

        explicit operator bool() const { return map != nullptr; }
    };

    TerrainTarget FindTerrainTarget(EditorContext& context) const {
        TerrainTarget target;
        if (context.terrains == nullptr || context.assets == nullptr) {
            return target;
        }
        const scene::Entity primary = ToEntity(context.selection->Primary());
        if (!context.world->IsAlive(primary)) {
            return target;
        }
        const auto* component = context.world->TryGet<scene::TerrainComponent>(primary);
        if (component == nullptr || !component->terrain.IsValid()) {
            return target;
        }
        const assets::AssetRecord* record = context.assets->Find(component->terrain.guid);
        if (record == nullptr) {
            return target;
        }
        terrain::Terrain* map =
            context.terrains->Get(component->terrain.guid, context.assets->AbsolutePath(*record));
        if (map == nullptr) {
            return target;
        }
        target.map = map;
        target.guid = component->terrain.guid;
        target.world = context.world->WorldMatrix(primary);
        target.worldInverse = glm::inverse(target.world);
        return target;
    }

    bool SculptingTerrain(const TerrainTarget& target) const {
        return terrainMode_ && static_cast<bool>(target);
    }

    /// Titik di permukaan terrain yang ditunjuk kursor, di ruang lokalnya.
    terrain::TerrainHit PickTerrain(const TerrainTarget& target, const Mat4& view,
                                    const Mat4& projection, const ImVec2& imagePos,
                                    const ImVec2& size, const ImVec2& point) const {
        const Ray ray = ScreenPointToRay(view, projection, Vec2(size.x, size.y),
                                         Vec2(point.x - imagePos.x, point.y - imagePos.y));
        const Vec3 origin = Vec3(target.worldInverse * Vec4(ray.origin, 1.0f));
        const Vec3 direction = Vec3(target.worldInverse * Vec4(ray.direction, 0.0f));
        return terrain::RaycastTerrain(*target.map, origin, direction);
    }

    /// Lingkaran kuas, digambar **mengikuti permukaan** alih-alih sebagai
    /// lingkaran datar.
    ///
    /// Lingkaran datar berbohong justru di tempat ia paling dibutuhkan: di
    /// lereng, jangkauan kuas yang sebenarnya membentang jauh lebih panjang
    /// menuruni bukit daripada yang digambar — dan yang memahat tepi jurang
    /// akan terus-menerus mengenai lebih banyak daripada yang dimaksudnya.
    void DrawTerrainCursor(const TerrainTarget& target, const Mat4& viewProjection,
                           const ImVec2& imagePos, const ImVec2& size, const Vec3& center,
                           float radius) {
        constexpr int kSegments = 48;
        const Mat4 clip = viewProjection * target.world;
        const Vec2 origin(imagePos.x, imagePos.y);
        const Vec2 extent(size.x, size.y);

        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PushClipRect(imagePos, ImVec2(imagePos.x + size.x, imagePos.y + size.y), true);

        // Dua lingkaran: jangkauan penuh, dan tempat falloff mulai melembut.
        // Yang kedua bukan hiasan — tanpanya "seberapa lembut" hanya bisa
        // diketahui dengan mencoba lalu membatalkan.
        const float inner = radius * (1.0f - std::clamp(target.map == nullptr
                                                            ? 0.0f
                                                            : falloffPreview_,
                                                        0.0f, 1.0f));
        for (int pass = 0; pass < 2; ++pass) {
            const float ringRadius = pass == 0 ? radius : inner;
            if (ringRadius <= 0.01f) {
                continue;
            }
            ImVec2 previous{};
            bool havePrevious = false;
            for (int i = 0; i <= kSegments; ++i) {
                const float angle = 6.2831853f * static_cast<float>(i) / kSegments;
                const float x = center.x + std::cos(angle) * ringRadius;
                const float z = center.z + std::sin(angle) * ringRadius;
                const Vec3 point(x, target.map->HeightAtWorld(x, z), z);
                Vec2 screen;
                if (!WorldToScreen(clip, origin, extent, point, screen)) {
                    havePrevious = false;
                    continue;
                }
                const ImVec2 at(screen.x, screen.y);
                if (havePrevious) {
                    draw->AddLine(previous, at,
                                  pass == 0 ? IM_COL32(255, 200, 90, 230)
                                            : IM_COL32(255, 200, 90, 110),
                                  pass == 0 ? 2.0f : 1.0f);
                }
                previous = at;
                havePrevious = true;
            }
        }
        draw->PopClipRect();
    }

    /// Satu frame pahatan. Mengembalikan true bila goresan sedang berlangsung,
    /// sehingga klik tidak boleh dianggap seleksi.
    bool HandleTerrainSculpt(EditorContext& context, const TerrainTarget& target,
                             const ImVec2& imagePos, const ImVec2& size, const Mat4& view,
                             const Mat4& projection, bool allowed) {
        const bool hovered =
            allowed && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !flying_;

        terrain::TerrainHit hit;
        if (hovered || terrainStroke_.Active()) {
            hit = PickTerrain(target, view, projection, imagePos, size, ImGui::GetMousePos());
        }

        const terrain::Brush brush = EffectiveSculptBrush(
            context.terrains->SculptBrush(), ImGui::GetIO().KeyCtrl, ImGui::GetIO().KeyShift);
        falloffPreview_ = brush.falloff;

        if (hit) {
            DrawTerrainCursor(target, projection * view, imagePos, size, hit.position,
                              brush.radius);
        }

        if (hovered && hit && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            terrainStroke_.Begin(*target.map, hit.position.x, hit.position.z);
            lastSculpt_ = Vec2(hit.position.x, hit.position.z);
        }
        if (!terrainStroke_.Active()) {
            return false;
        }

        // Yang lepas dari permukaan — kursor keluar peta di tengah seretan —
        // menyentuh di tempat terakhir yang sah, bukan berhenti diam-diam.
        // Goresan yang putus di tengah menghasilkan dua entri undo untuk satu
        // sapuan tangan.
        const Vec2 at = hit ? Vec2(hit.position.x, hit.position.z) : lastSculpt_;
        lastSculpt_ = at;

        const float dt = context.deltaSeconds > 0.0f ? context.deltaSeconds : 1.0f / 60.0f;
        // **Jalur yang sama persis dengan panel**: `BrushStroke` milik
        // `Sim::Terrain`, bukan penerapan kedua yang kebetulan mirip. Jalur
        // kedua adalah dua perilaku yang berselisih di kasus tepi, dan yang
        // berselisih di kasus tepi tidak terlihat sampai seseorang mengeluh
        // bahwa undo-nya aneh.
        terrainStroke_.Advance(*target.map, brush, at.x, at.y, dt);
        context.terrains->MarkDirty(target.guid);

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            terrainStroke_.End(*target.map);
        }
        return true;
    }

    void ToggleTerrainMode(const TerrainTarget& target) {
        terrainMode_ = !terrainMode_;
        if (terrainStroke_.Active() && target) {
            terrainStroke_.End(*target.map);
        }
    }

    // --- seleksi ------------------------------------------------------------

    void HandleSelectionInput(EditorContext& context, const WhiteboxTarget& whiteboxTarget,
                              const ImVec2& imagePos, const ImVec2& size, const Mat4& view,
                              const Mat4& projection, bool gizmoBusy, bool surfacePressed,
                              bool surfaceHeld) {
        if (flying_ || ImGui::GetIO().KeyAlt) {
            return;  // tombol kiri sedang milik kamera
        }

        if (surfacePressed && !gizmoBusy) {
            boxSelecting_ = true;
            boxStart_ = ImGui::GetMousePos();
        }
        if (!boxSelecting_) {
            return;
        }
        if (!surfaceHeld) {
            FinishSelection(context, whiteboxTarget, imagePos, size, view, projection);
            boxSelecting_ = false;
            return;
        }

        const ImVec2 current = ImGui::GetMousePos();
        const float dragged = std::max(std::abs(current.x - boxStart_.x),
                                       std::abs(current.y - boxStart_.y));
        if (dragged > kDragThreshold) {
            // Kotak digambar di foreground drawlist supaya tetap terlihat di
            // atas gambar viewport tanpa mengganggu urutan widget lain.
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(boxStart_, current, IM_COL32(255, 158, 51, 40));
            draw->AddRect(boxStart_, current, IM_COL32(255, 158, 51, 220));
        }
    }

    void FinishSelection(EditorContext& context, const WhiteboxTarget& whiteboxTarget,
                         const ImVec2& imagePos, const ImVec2& size, const Mat4& view,
                         const Mat4& projection) {
        ImGuiIO& io = ImGui::GetIO();
        const ImVec2 release = ImGui::GetMousePos();
        const float dragged =
            std::max(std::abs(release.x - boxStart_.x), std::abs(release.y - boxStart_.y));

        // Sisi diuji lebih dulu, dan **sebelum seleksi entity disentuh**: klik
        // yang mengenai sebuah sisi tidak boleh ikut melepaskan entity yang
        // sisi itu miliknya. Yang meleset jatuh ke jalur biasa, sehingga
        // mengklik benda lain tetap berpindah ke benda itu alih-alih terkunci
        // di dalam mode.
        if (dragged <= kDragThreshold && EditingSides(whiteboxTarget)) {
            // Ctrl/Shift menambah ke seleksi alih-alih menggantinya — aturan yang
            // sama dengan seleksi entity, jadi tangan tidak perlu belajar dua
            // kebiasaan untuk satu gerakan.
            const bool additive = io.KeyCtrl || io.KeyShift;
            const Mat4 clip = projection * view;

            switch (context.whiteboxes->SubObjectMode()) {
                case SubObject::Vertex: {
                    const whitebox::VertexHandle vertex =
                        PickWhiteboxVertex(whiteboxTarget, clip, imagePos, size, release);
                    if (whitebox::IsValid(vertex)) {
                        if (additive) {
                            context.whiteboxes->Toggle(whiteboxTarget.guid, vertex);
                        } else {
                            context.whiteboxes->ClearSelection();
                            context.whiteboxes->Add(whiteboxTarget.guid, vertex);
                        }
                        return;
                    }
                    break;
                }
                case SubObject::Edge: {
                    const whitebox::EdgeHandle edge =
                        PickWhiteboxEdge(whiteboxTarget, clip, imagePos, size, release);
                    if (whitebox::IsValid(edge)) {
                        if (additive) {
                            context.whiteboxes->Toggle(whiteboxTarget.guid, edge);
                        } else {
                            context.whiteboxes->ClearSelection();
                            context.whiteboxes->Add(whiteboxTarget.guid, edge);
                        }
                        return;
                    }
                    break;
                }
                case SubObject::Face: {
                    const whitebox::PolygonHit hit = PickWhiteboxSide(whiteboxTarget, view,
                                                                      projection, imagePos,
                                                                      size, release);
                    if (hit) {
                        if (additive) {
                            context.whiteboxes->Toggle(whiteboxTarget.guid, hit.polygon);
                        } else {
                            context.whiteboxes->Select(whiteboxTarget.guid, hit.polygon);
                        }
                        return;
                    }
                    break;
                }
            }

            // **Meleset mengosongkan seleksi, bukan membiarkannya.** Yang
            // mengklik ruang kosong sedang membatalkan pilihannya; membiarkannya
            // membuat operasi berikutnya mengenai sesuatu yang sudah tidak
            // dianggap terpilih oleh siapa pun kecuali mesin.
            if (!additive) {
                context.whiteboxes->ClearSelection();
            }
        }

        const bool additiveEntity = io.KeyCtrl || io.KeyShift;
        if (!additiveEntity) {
            context.selection->Clear();
        }

        if (dragged <= kDragThreshold) {
            // Ikon diuji lebih dulu dan menang atas geometri di belakangnya.
            // Ikon berukuran tetap dan kecil; kalau kalah dengan mesh besar
            // yang kebetulan menaunginya, lampu di dalam ruangan jadi mustahil
            // dipilih lewat viewport.
            const float radius = ImGui::GetFontSize() * kIconScale * 0.5f;
            scene::Entity hit =
                sceneView_.PickIcon(projection * view, Vec2(imagePos.x, imagePos.y),
                                    Vec2(size.x, size.y), Vec2(release.x, release.y), radius);
            if (!scene::IsValid(hit)) {
                const Ray ray =
                    ScreenPointToRay(view, projection, Vec2(size.x, size.y),
                                     Vec2(release.x - imagePos.x, release.y - imagePos.y));
                hit = sceneView_.Raycast(ray);
            }
            if (scene::IsValid(hit)) {
                const uint64_t id = ToSelectionId(hit);
                if (io.KeyCtrl && context.selection->Contains(id)) {
                    context.selection->Remove(id);
                } else {
                    context.selection->Add(id);
                }
            }
            return;
        }

        const ScreenRect rect =
            ScreenRect::FromCorners(Vec2(boxStart_.x, boxStart_.y), Vec2(release.x, release.y));
        const std::vector<scene::Entity> hits = sceneView_.RectSelect(
            projection * view, Vec2(imagePos.x, imagePos.y), Vec2(size.x, size.y), rect);
        for (const scene::Entity entity : hits) {
            context.selection->Add(ToSelectionId(entity));
        }
    }

    // --- pintasan -----------------------------------------------------------

    void HandleShortcuts(EditorContext& context, const WhiteboxTarget& whiteboxTarget,
                         const TerrainTarget& terrainTarget) {
        if (flying_ || ImGui::GetIO().WantTextInput) {
            return;
        }
        if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !IsFocused()) {
            return;
        }

        // Q/W/E/R mengikuti kebiasaan editor lain. Tidak bentrok dengan WASD
        // terbang karena terbang hanya aktif selama tombol kanan ditahan, dan
        // jalur ini sudah keluar lebih awal saat itu terjadi.
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
            operation_ = GizmoOperation::None;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
            operation_ = GizmoOperation::Translate;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
            operation_ = GizmoOperation::Rotate;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
            operation_ = GizmoOperation::Scale;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            space_ = space_ == GizmoSpace::World ? GizmoSpace::Local : GizmoSpace::World;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false)) {
            FocusSelection(context);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_B, false) && whiteboxTarget) {
            ToggleWhiteboxMode(context);
        }
        // 1/2/3 hanya berarti sesuatu selama blockout menyala — di luar itu
        // keduanya milik siapa pun yang memakainya nanti, dan merebutnya di sini
        // akan membuat pintasan itu diam-diam tidak pernah sampai.
        if (whiteboxMode_ && whiteboxTarget && context.whiteboxes != nullptr) {
            if (ImGui::IsKeyPressed(ImGuiKey_1, false)) {
                context.whiteboxes->SetSubObject(SubObject::Vertex);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_2, false)) {
                context.whiteboxes->SetSubObject(SubObject::Edge);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_3, false)) {
                context.whiteboxes->SetSubObject(SubObject::Face);
            }
        }
        // End, konvensi yang sama dengan editor lain. Shift meratakan sumbu
        // atasnya ke normal permukaan; tanpa Shift ia hanya turun.
        if (ImGui::IsKeyPressed(ImGuiKey_End, false)) {
            ConformSelectionToSurface(context, ImGui::GetIO().KeyShift
                                                   ? ConformOrientation::AlignToNormal
                                                   : ConformOrientation::Keep);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_T, false) && terrainTarget) {
            ToggleTerrainMode(terrainTarget);
        }
    }

    void ToggleWhiteboxMode(EditorContext& context) {
        whiteboxMode_ = !whiteboxMode_;
        if (whiteboxDrag_.active) {
            // Seretan yang ditinggalkan tanpa penutup membuat suntingan
            // berikutnya menyatu dengannya, dan satu Ctrl+Z membatalkan dua
            // gerakan yang dipisahkan oleh keluar-masuk mode.
            whiteboxDrag_.active = false;
            context.history->CloseMergeGroup();
        }
        if (!whiteboxMode_ && context.whiteboxes != nullptr) {
            // Keluar dari mode juga melepas sisinya. Sorotan yang tertinggal di
            // panel setelah alatnya dimatikan menjanjikan gizmo yang tidak ada.
            context.whiteboxes->ClearSelection();
        }
    }

    void FocusSelection(const EditorContext& context) {
        std::vector<scene::Entity> entities;
        for (const uint64_t id : context.selection->Items()) {
            const scene::Entity entity = ToEntity(id);
            if (context.world->IsAlive(entity)) {
                entities.push_back(entity);
            }
        }

        Vec3 boundsMin;
        Vec3 boundsMax;
        if (entities.empty() || !sceneView_.BoundsOf(entities, boundsMin, boundsMax)) {
            camera_.focus = Vec3(0.0f);
            camera_.distance = 12.0f;
            return;
        }

        camera_.focus = (boundsMin + boundsMax) * 0.5f;
        // Jarak dipilih agar seluruh kotak muat di layar dengan sedikit ruang
        // sisa. Radius bola pembungkus dipakai, bukan sisi terpanjang, supaya
        // objek pipih tetap muat dari arah pandang mana pun.
        const float radius = glm::length(boundsMax - boundsMin) * 0.5f;
        camera_.distance = std::max(radius * 2.5f, 0.5f);
    }

    // --- rangka kawat fisika -------------------------------------------------

    /// Menggambar rangka kawat komponen entity terpilih: bentuk tabrakan, sendi,
    /// kendaraan, dan jangkauan lampu.
    ///
    /// **Hanya yang terpilih, dan itu keputusan biaya.** Sebuah level dengan
    /// ribuan collider menghasilkan ratusan ribu ruas garis per frame — dan
    /// hampir semuanya menggambarkan benda yang sedang tidak dikerjakan siapa
    /// pun. Yang dicari orang yang membuka gizmo collider adalah ukuran benda
    /// yang baru saja ia setel, dan benda itu selalu yang terpilih.
    ///
    /// Ukurannya tidak dihitung di sini melainkan diminta dari
    /// `physics::DescribeCollider` — jalur yang sama persis yang memberi makan
    /// solver. Menghitungnya sendiri berarti dua aritmetika untuk satu bentuk,
    /// dan yang kedua akan menyimpang tepat di kasus yang paling sulit dilihat.
    void DrawSelectionWires(EditorContext& context) {
        if (context.world == nullptr || context.selection == nullptr) {
            return;
        }
        scene::World& world = *context.world;
        // **Tembus geometri.** Bentuk tabrakan hampir selalu berada di dalam mesh
        // yang menggambarkannya — itu justru gunanya — jadi rangka kawat yang
        // diuji kedalaman tertutup persis oleh benda yang ukurannya sedang
        // diperiksa orang.
        sceneView_.SetLinesThroughGeometry(true);
        for (const SelectionId id : context.selection->Items()) {
            const scene::Entity entity = ToEntity(id);
            if (entity == scene::kNullEntity || !world.IsAlive(entity)) {
                continue;
            }
            DrawColliderWire(world, entity);
            DrawBodyMarker(world, entity);
            DrawJointWire(world, entity);
            DrawVehicleWire(world, entity);
            DrawLightWire(world, entity);
        }
        sceneView_.SetLinesThroughGeometry(false);
    }

    /// Matriks bentuk: posisi dan putaran saja.
    ///
    /// **Skalanya sengaja ditinggalkan.** Ia sudah masuk ke ukuran bentuknya di
    /// `DescribeCollider`, dengan aturan yang berbeda per sumbu untuk kotak dan
    /// seragam untuk bola — memasukkannya lagi ke matriks berarti mengalikannya
    /// dua kali, dan yang tergambar menjadi bentuk yang tidak pernah ada.
    static Mat4 ShapeMatrix(const physics::ColliderPlacement& placement) {
        Mat4 matrix = glm::mat4_cast(placement.rotation);
        const Vec3 center =
            placement.position + Vec3(matrix * Vec4(placement.shape.localPosition, 0.0f));
        matrix[3] = Vec4(center, 1.0f);
        return matrix;
    }

    void DrawColliderWire(scene::World& world, scene::Entity entity) {
        physics::ColliderPlacement placement;
        if (!physics::DescribeCollider(world, entity, placement)) {
            return;
        }
        const auto* body = world.TryGet<scene::RigidBodyComponent>(entity);
        Vec4 color = kColliderInertColor;
        if (body != nullptr) {
            switch (body->kind) {
                case scene::RigidBodyKind::Static: color = kColliderStaticColor; break;
                case scene::RigidBodyKind::Kinematic: color = kColliderKinematicColor; break;
                case scene::RigidBodyKind::Dynamic: color = kColliderDynamicColor; break;
            }
        }

        const Mat4 matrix = ShapeMatrix(placement);
        const physics::ShapeDesc& shape = placement.shape;
        switch (shape.kind) {
            case physics::ShapeKind::Box:
                sceneView_.AddWireBox(matrix, shape.halfExtents, color);
                break;
            case physics::ShapeKind::Sphere:
                sceneView_.AddWireSphere(matrix, shape.radius, color);
                break;
            case physics::ShapeKind::Capsule:
                sceneView_.AddWireCapsule(matrix, shape.radius, shape.halfExtents.x, color);
                break;
            case physics::ShapeKind::Cylinder:
                sceneView_.AddWireCylinder(matrix, shape.radius, shape.halfExtents.x, color);
                break;
            case physics::ShapeKind::Plane:
                sceneView_.AddWirePlane(matrix, kPlaneWireExtent, color);
                break;
            case physics::ShapeKind::ConvexHull:
            case physics::ShapeKind::TriangleMesh:
            case physics::ShapeKind::HeightField:
                // Bentuknya datang dari aset — whitebox atau terrain — dan yang
                // bisa membacanya adalah `ColliderGeometrySource` yang hanya
                // dipegang `Build`. Yang digambar karena itu kotak batas
                // geometri yang tergambar, dan itu jujur: collider ini memang
                // mengikuti geometri itu, jadi kotak yang salah di sini berarti
                // geometri yang salah di sana.
                DrawEntityBounds(entity, color);
                break;
        }
    }

    /// Kotak batas geometri entity, diambil dari daftar pickable frame ini.
    void DrawEntityBounds(scene::Entity entity, const Vec4& color) {
        for (const SceneView::Pickable& pickable : sceneView_.Pickables()) {
            if (pickable.entity != entity) {
                continue;
            }
            const Vec3 half = (pickable.boundsMax - pickable.boundsMin) * 0.5f;
            Mat4 matrix = pickable.worldMatrix;
            matrix[3] = Vec4(Vec3(matrix * Vec4((pickable.boundsMin + pickable.boundsMax) * 0.5f,
                                                1.0f)),
                             1.0f);
            sceneView_.AddWireBox(matrix, half, color);
            return;
        }
    }

    /// Penanda benda tegar yang tidak punya bentuk.
    ///
    /// **Yang digambar di sini adalah ketiadaan.** `PhysicsScene::Build`
    /// melewatkan benda tanpa collider dan menyebutkannya di log — tetapi log
    /// baru dibaca setelah ada yang aneh, sedangkan salib merah di tempat benda
    /// itu terlihat sebelum Play ditekan.
    void DrawBodyMarker(scene::World& world, scene::Entity entity) {
        if (world.TryGet<scene::RigidBodyComponent>(entity) == nullptr ||
            world.Has<scene::ColliderComponent>(entity)) {
            return;
        }
        sceneView_.AddWireCross(Vec3(world.WorldMatrix(entity)[3]), kMarkerSize,
                                kColliderInertColor);
    }

    /// Garis dari benda yang bergerak ke benda yang menahannya.
    ///
    /// Sendi tidak punya bentuk untuk digambar; yang perlu terlihat adalah
    /// **pasangannya**, karena rujukannya GUID dan satu-satunya cara memeriksanya
    /// hari ini adalah membaca Inspector dan mencocokkan nama.
    void DrawJointWire(scene::World& world, scene::Entity entity) {
        const auto* joint = world.TryGet<scene::JointComponent>(entity);
        if (joint == nullptr) {
            return;
        }
        const Mat4 worldMatrix = world.WorldMatrix(entity);
        // Titik sendinya, bukan titik asal entity: `anchor` ruang lokal, dan
        // engsel pintu duduk di tepinya — bukan di tengahnya.
        const Vec3 anchor(worldMatrix * Vec4(joint->anchor, 1.0f));
        sceneView_.AddWireCross(anchor, kMarkerSize, kJointColor);

        // Sumbunya +X bingkai sendi, seperti yang tertulis di `JointComponent`.
        // Engsel yang berputar pada sumbu yang salah terlihat benar sampai Play
        // ditekan; garis ini membuatnya terlihat sebelum itu.
        const Mat4 frame = worldMatrix * glm::mat4_cast(joint->frame);
        const Vec3 axis = glm::normalize(Vec3(frame[0]));
        sceneView_.AddLine(anchor - axis * kJointAxisLength, anchor + axis * kJointAxisLength,
                           kJointColor);

        if (!joint->connectedBody.IsValid()) {
            // Kosong berarti dunia, dan itu bukan galat — pintu dan bandul
            // memang tergantung pada titik tetap di ruang.
            return;
        }
        const scene::Entity other = world.FindByGuid(joint->connectedBody);
        if (other == scene::kNullEntity) {
            // Menunjuk sesuatu yang tidak ada di level ini. Dibiarkan sebagai
            // salib tanpa garis: sendi yang pasangannya hilang tidak menyambung
            // ke mana pun, dan garis ke titik asal dunia akan berbohong bahwa ia
            // tersambung ke sana.
            return;
        }
        sceneView_.AddLine(anchor, Vec3(world.WorldMatrix(other)[3]), kJointColor);
    }

    /// Chassis dan keempat rodanya, pada ukuran yang benar-benar dipakai.
    void DrawVehicleWire(scene::World& world, scene::Entity entity) {
        const auto* vehicle = world.TryGet<scene::VehicleComponent>(entity);
        if (vehicle == nullptr) {
            return;
        }
        const Mat4 worldMatrix = world.WorldMatrix(entity);
        Mat4 matrix(1.0f);
        for (int axis = 0; axis < 3; ++axis) {
            const Vec3 column(worldMatrix[axis]);
            const float length = glm::length(column);
            matrix[axis] = Vec4(length > 1e-8f ? column / length : Vec3(axis == 0, axis == 1,
                                                                       axis == 2),
                                0.0f);
        }
        matrix[3] = worldMatrix[3];

        sceneView_.AddWireBox(matrix, vehicle->chassisHalfExtents, kVehicleColor);
        // Titik berat: satu angka yang salah di sini membuat mobil terguling di
        // tikungan pertama, dan itu terbaca sebagai fisika yang salah.
        sceneView_.AddWireCross(Vec3(matrix * Vec4(vehicle->centerOfMassOffset, 1.0f)),
                                kMarkerSize, kVehicleColor);

        // **Posisi rodanya diminta, bukan diturunkan ulang di sini.** Empat
        // koordinat yang dihitung dua kali adalah empat kesempatan menggambar
        // roda di tempat yang bukan tempatnya dibangun — dan yang paling mudah
        // luput justru tanda kiri-kanannya, karena mobil simetris terlihat sama
        // saja ketika keduanya tertukar.
        for (const physics::VehicleWheelDesc& wheel :
             physics::DescribeVehicleWheels(*vehicle)) {
            // Silinder bersumbu X, yaitu sumbu putar roda.
            Mat4 wheelMatrix = matrix;
            wheelMatrix[3] = Vec4(Vec3(matrix * Vec4(wheel.centerOffset, 1.0f)), 1.0f);
            sceneView_.AddWireCylinder(wheelMatrix, wheel.radius, wheel.width * 0.5f,
                                       kVehicleColor);
        }
    }

    /// Jangkauan dan arah sebuah lampu.
    ///
    /// **Intensitas digambar sebagai jarak, bukan sebagai warna.** `range` hanya
    /// menyebut di mana cahaya berakhir tepat nol, dan ia sama besar untuk lampu
    /// redup dan lampu menyilaukan — dua bola sebesar itu tidak memberi tahu apa
    /// pun tentang selisih keduanya. Bola kedua di dalamnya adalah sejauh mana
    /// cahayanya masih seterang ambang tetap, dan ia bergerak saat intensitasnya
    /// disetel. Jaraknya diminta dari `render::LightUsefulRadius`, yang memakai
    /// peredupan yang sama dengan shader.
    void DrawLightWire(scene::World& world, scene::Entity entity) {
        const auto* light = world.TryGet<scene::LightComponent>(entity);
        if (light == nullptr) {
            return;
        }
        const Mat4 matrix = world.WorldMatrix(entity);
        const Vec3 origin(matrix[3]);
        // Arah pancar: -Z lokal, sama dengan `SceneView::AppendLight` dan sama
        // dengan arah hadap kamera. Menurunkannya sendiri di sini berarti gizmo
        // yang menunjuk ke arah lain daripada cahayanya begitu salah satu diubah.
        const Vec3 forward = glm::normalize(Vec3(matrix * Vec4(0.0f, 0.0f, -1.0f, 0.0f)));

        // Warna lampunya sendiri, dinaikkan sampai kanal terkuatnya penuh: dua
        // lampu di adegan yang sama harus bisa dibedakan, dan lampu biru redup
        // yang digambar biru redup tidak terlihat di atas latar gelap.
        const float peak = std::max({light->color.r, light->color.g, light->color.b});
        const Vec3 hue = peak > 1e-4f ? light->color / peak : Vec3(1.0f);
        const Vec4 rangeColor(hue.r, hue.g, hue.b, 0.55f);
        const Vec4 reachColor(hue.r, hue.g, hue.b, 0.95f);

        render::LightInstance instance;
        instance.color = light->color;
        instance.intensity = light->intensity;
        instance.range = light->range;
        instance.sourceRadius = light->sourceRadius;
        const float reach = render::LightUsefulRadius(instance, kLightUsefulThreshold);

        switch (light->type) {
            case scene::LightType::Directional: {
                // Jangkauan tidak berarti untuk directional — ia menerangi
                // seluruh adegan — jadi yang digambar hanya arahnya. Cincin
                // beserta berkas sejajar: satu panah tunggal terbaca sebagai
                // "cahaya datang dari titik ini", yang justru bukan sifatnya.
                const Vec3 helper =
                    std::abs(forward.y) < 0.99f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
                const Vec3 side = glm::normalize(glm::cross(helper, forward));
                const Vec3 up = glm::cross(forward, side);
                sceneView_.AddWireCircle(origin, side, up, kDirectionalRingRadius, reachColor);
                for (const Vec3& offset : {Vec3(0.0f), side * kDirectionalRingRadius,
                                           -side * kDirectionalRingRadius,
                                           up * kDirectionalRingRadius,
                                           -up * kDirectionalRingRadius}) {
                    const Vec3 from = origin + offset;
                    const Vec3 to = from + forward * kDirectionalRayLength;
                    sceneView_.AddLine(from, to, reachColor);
                    // Kepala panah: tanpa ini kedua ujung berkas terlihat sama,
                    // dan lampu yang terbalik 180° tidak bisa dibedakan.
                    const float head = kDirectionalRayLength * kArrowHeadFraction;
                    for (const Vec3& wing : {side, -side, up, -up}) {
                        sceneView_.AddLine(to, to - forward * head + wing * (head * 0.5f),
                                           reachColor);
                    }
                }
                break;
            }
            case scene::LightType::Point:
                sceneView_.AddWireSphere(matrix, light->range, rangeColor);
                if (reach > 0.0f) {
                    sceneView_.AddWireSphere(matrix, reach, reachColor);
                }
                break;
            case scene::LightType::Spot: {
                // Kerucut luar pada jangkauannya, kerucut dalam di sebelahnya:
                // yang di antara keduanya adalah tepi yang memudar, dan itu
                // satu-satunya bagian berkas yang tidak bisa ditebak dari angka.
                const float outer = std::max(light->outerAngleRadians, light->innerAngleRadians);
                const float inner = std::min(light->outerAngleRadians, light->innerAngleRadians);
                sceneView_.AddWireCone(origin, forward, outer, light->range, rangeColor);
                sceneView_.AddWireCone(origin, forward, inner, light->range, rangeColor);
                if (reach > 0.0f) {
                    // Mulut kerucut pada jangkauan bergunanya — sejauh itu
                    // berkasnya masih seterang ambang.
                    sceneView_.AddWireCone(origin, forward, outer, reach, reachColor);
                }
                break;
            }
        }
    }

    // --- overlay ------------------------------------------------------------

    /// Menggambar overlay viewport, dan melaporkan apakah kursor sedang berada
    /// di atas salah satunya.
    ///
    /// Jawabannya dikumpulkan dari tiap widget tepat setelah diajukan. Itu
    /// bukan gaya penulisan melainkan syarat kebenaran: satu-satunya pertanyaan
    /// yang boleh dijawab di sini adalah "kursor di atas overlay?", dan cara
    /// mana pun yang menanyakannya ke keadaan global ImGui akan ikut menghitung
    /// item lain — termasuk permukaan viewport itu sendiri.
    bool DrawOverlays(EditorContext& context, const ImVec2& imagePos, const ImVec2& size,
                      const render::IViewportRenderer& renderer,
                      const WhiteboxTarget& whiteboxTarget, const TerrainTarget& terrainTarget) {
        const float pad = 10.0f;
        const float button = ImGui::GetFrameHeight() * widgets::kViewportButtonScale;

        bool hovered = false;
        const auto track = [&hovered]() { hovered = hovered || ImGui::IsItemHovered(); };

        // (D1) alat transform di kiri-atas.
        ImGui::SetCursorScreenPos(ImVec2(imagePos.x + pad, imagePos.y + pad));
        ImGui::BeginGroup();
        OperationButton(icons::kSelect, "Select (Q)", GizmoOperation::None);
        track();
        OperationButton(icons::kMove, "Move (W)", GizmoOperation::Translate);
        track();
        OperationButton(icons::kRotate, "Rotate (E)", GizmoOperation::Rotate);
        track();
        OperationButton(icons::kScale, "Scale (R)", GizmoOperation::Scale);
        track();

        ImGui::Spacing();
        const bool local = space_ == GizmoSpace::Local;
        if (widgets::ViewportButton(local ? icons::kSpaceLocal : icons::kSpaceWorld,
                                    local ? "Local space (X)" : "World space (X)")) {
            space_ = local ? GizmoSpace::World : GizmoSpace::Local;
        }
        track();
        hovered = DrawSnapControl() || hovered;

        // Hanya muncul ketika ada yang bisa disunting. Tombol yang selalu ada
        // tetapi hampir selalu tidak melakukan apa-apa mengajari orang bahwa
        // menekannya percuma, dan mereka berhenti menekannya justru ketika ia
        // berguna.
        if (whiteboxTarget) {
            ImGui::Spacing();
            if (widgets::ViewportButton(icons::kWhitebox,
                                        whiteboxMode_ ? "Editing sides (B)" : "Edit sides (B)",
                                        whiteboxMode_)) {
                ToggleWhiteboxMode(context);
            }
            track();

            // Pemilih jenis sub-objek, dan **hanya saat menyunting**. Tiga tombol
            // yang tidak berarti apa-apa selama mode blockout mati adalah tiga
            // tombol yang mengajari orang bahwa menekannya percuma.
            if (whiteboxMode_ && context.whiteboxes != nullptr) {
                const SubObject mode = context.whiteboxes->SubObjectMode();
                const auto modeButton = [&](const char* icon, const char* label,
                                            SubObject value) {
                    if (widgets::ViewportButton(icon, label, mode == value)) {
                        context.whiteboxes->SetSubObject(value);
                    }
                    track();
                };
                modeButton(icons::kSubObjectVertex, "Vertices (1)", SubObject::Vertex);
                modeButton(icons::kSubObjectEdge, "Edges (2)", SubObject::Edge);
                modeButton(icons::kSubObjectFace, "Faces (3)", SubObject::Face);
            }
        }
        if (terrainTarget) {
            ImGui::Spacing();
            if (widgets::ViewportButton(icons::kTerrainEditor,
                                        terrainMode_ ? "Sculpting terrain (T)" : "Sculpt terrain (T)",
                                        terrainMode_)) {
                ToggleTerrainMode(terrainTarget);
            }
            track();
        }
        ImGui::EndGroup();

        if (SculptingTerrain(terrainTarget)) {
            hovered = DrawSculptControls(context, imagePos, size) || hovered;
        }

        // (D2a) pemilih titik pandang, di atas kolom ikon.
        //
        // **Sebuah combo, bukan tombol berputar seperti tetangganya di bawah.**
        // Tetangga itu berputar di antara lima mode yang semuanya setara; yang
        // di sini punya tiga belas entri lebih atau kurang — enam sumbu, satu
        // perspektif, dan sebanyak apa pun kamera yang ada di adegan — dan
        // memutarinya berarti tidak satu pun bisa dituju langsung.
        const float viewpointWidth = button * 5.5f;
        ImGui::SetCursorScreenPos(
            ImVec2(imagePos.x + size.x - viewpointWidth - pad, imagePos.y + pad));
        ImGui::SetNextItemWidth(viewpointWidth);
        hovered = DrawViewpointCombo(context) || hovered;

        // (D2) mode tampilan di kanan-atas, tepat di bawahnya.
        ImGui::SetCursorScreenPos(
            ImVec2(imagePos.x + size.x - button - pad, imagePos.y + pad + button + pad));
        ImGui::BeginGroup();
        if (widgets::ViewportButton(ShadingIcon(drawMode_), ShadingLabel(drawMode_),
                                    drawMode_ != render::DrawMode::Material)) {
            drawMode_ = NextDrawMode(drawMode_);
        }
        track();
        // **Klik kiri berputar, klik kanan membuka daftarnya.** Lima mode tidak
        // muat di sebuah tombol berputar sendirian: tidak satu pun bisa dituju
        // langsung, dan yang terjauh menuntut empat tekan. Yang berputar tetap
        // ada karena itulah yang dipakai saat bolak-balik antara dua mode
        // berdekatan; daftar ini yang dipakai saat tujuannya jauh.
        //
        // Klik kanannya tidak lagi ikut menyalakan mode terbang — lihat
        // `UpdateOverlayPointerClaims`.
        if (ImGui::BeginPopupContextItem(kShadingPopupId)) {
            render::DrawMode mode = render::DrawMode::Material;
            do {
                if (ImGui::MenuItem(ShadingLabel(mode), nullptr, drawMode_ == mode)) {
                    drawMode_ = mode;
                }
                mode = NextDrawMode(mode);
            } while (mode != render::DrawMode::Material);
            ImGui::EndPopup();
        }
        // Selama menunya terbuka, overlay tetap dianggap memegang pointer:
        // kursornya sedang berada di atas menu, bukan di atas tombolnya, dan
        // tanpa ini klik untuk memilih sebuah mode ikut terbaca sebagai klik di
        // adegan di belakangnya.
        hovered = hovered || ImGui::IsPopupOpen(kShadingPopupId);
        if (widgets::ViewportButton(icons::kGrid, showGrid_ ? "Hide grid" : "Show grid",
                                    showGrid_)) {
            showGrid_ = !showGrid_;
        }
        track();
        ImGui::EndGroup();

        // Label renderer di kiri-bawah. Selama masih stub, ini penting: tanpa
        // penanda, mudah mengira grid polos berarti rendering rusak.
        char info[224];
        if (flying_) {
            std::snprintf(info, sizeof(info), "%s  |  %ux%u  |  fly %.1f m/s (WASD, Q/E, Shift)",
                          renderer.Name(), renderer.Width(), renderer.Height(),
                          static_cast<double>(flySpeed_));
        } else {
            std::snprintf(info, sizeof(info), "%s  |  %ux%u  |  %zu selected  |  dist %.1f m",
                          renderer.Name(), renderer.Width(), renderer.Height(),
                          context.selection->Count(), static_cast<double>(camera_.distance));
        }
        const ImVec2 infoSize = ImGui::CalcTextSize(info);
        ImGui::SetCursorScreenPos(
            ImVec2(imagePos.x + pad, imagePos.y + size.y - infoSize.y - pad));
        ImGui::TextDisabled("%s", info);
        return hovered;
    }

    /// Setelan kuas, hanya saat memahat. Ditaruh di viewport dan bukan hanya di
    /// panel karena yang sedang memahat sedang menatap viewport — dan alat yang
    /// menuntut pindah jendela untuk mengubah jari-jari adalah alat yang
    /// jari-jarinya jarang diubah.
    bool DrawSculptControls(EditorContext& context, const ImVec2& imagePos, const ImVec2& size) {
        terrain::Brush& brush = context.terrains->SculptBrush();
        const float width = ImGui::GetFontSize() * 9.0f;
        ImGui::SetCursorScreenPos(ImVec2(imagePos.x + 10.0f, imagePos.y + size.y * 0.5f));
        ImGui::BeginGroup();

        static const char* const kKinds[] = {"Raise", "Lower", "Flatten", "Smooth", "Noise"};
        int kind = static_cast<int>(brush.kind);
        ImGui::SetNextItemWidth(width);
        if (ImGui::Combo("##terraintool", &kind, kKinds, IM_ARRAYSIZE(kKinds))) {
            brush.kind = static_cast<terrain::BrushKind>(kind);
        }
        bool hovered = ImGui::IsItemHovered();
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("##radius", &brush.radius, 0.25f, 0.5f, 500.0f, "radius %.1f m");
        hovered = hovered || ImGui::IsItemHovered();
        ImGui::SetNextItemWidth(width);
        ImGui::DragFloat("##strength", &brush.strength, 0.1f, 0.01f, 200.0f, "strength %.2f");
        hovered = hovered || ImGui::IsItemHovered();
        ImGui::SetNextItemWidth(width);
        ImGui::SliderFloat("##falloff", &brush.falloff, 0.0f, 1.0f, "falloff %.2f");
        hovered = hovered || ImGui::IsItemHovered();
        ImGui::TextDisabled("Ctrl smooth   Shift invert");
        ImGui::EndGroup();
        return hovered;
    }

    void OperationButton(const char* icon, const char* tooltip, GizmoOperation operation) {
        if (widgets::ViewportButton(icon, tooltip, operation_ == operation)) {
            operation_ = operation;
        }
    }

    /// True bila kursor berada di atas tombolnya, atau popup setelannya terbuka.
    ///
    /// Popup ikut dihitung karena gizmo membaca mouse langsung dan tidak tahu
    /// apa-apa tentang jendela ImGui: tanpa ini, menyeret slider "Move (m)" yang
    /// kebetulan berada di atas gizmo akan ikut menggeser entity.
    bool DrawSnapControl() {
        // Penandaan aktif diserahkan ke widget, bukan diurus di sini dengan
        // push/pop sendiri. Versi sebelumnya membaca ulang snap_.enabled setelah
        // tombolnya membalik nilai itu, sehingga push dan pop tidak berpasangan
        // dan ImGui menggugurkan proses. Menyerahkannya ke widget membuat
        // kesalahan seperti itu tidak mungkin terjadi lagi.
        if (widgets::ViewportButton(icons::kSnap, snap_.enabled ? "Snapping on" : "Snapping off",
                                    snap_.enabled)) {
            snap_.enabled = !snap_.enabled;
        }
        bool hovered = ImGui::IsItemHovered();
        // Klik kanan pada tombol yang sama membuka nilainya. Menaruhnya di popup
        // terpisah menjaga bilah alat tetap sempit tanpa menyembunyikan setelan.
        if (ImGui::BeginPopupContextItem("##snap")) {
            hovered = true;
            ImGui::TextDisabled("Snap increments");
            ImGui::Separator();
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Move (m)", &snap_.translate, 0.05f, 0.001f, 100.0f, "%.3f");
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Rotate (deg)", &snap_.rotateDegrees, 1.0f, 0.1f, 180.0f, "%.1f");
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("Scale", &snap_.scale, 0.01f, 0.001f, 10.0f, "%.3f");
            ImGui::EndPopup();
        }
        return hovered;
    }

    /// Keadaan satu entity saat seretan gizmo dimulai.
    struct DragItem {
        Uuid guid;
        scene::Entity entity = scene::kNullEntity;
        scene::TransformComponent before;
        Mat4 worldAtDragStart{1.0f};
    };

    /// Keadaan satu seretan sisi whitebox.
    struct WhiteboxDrag {
        bool active = false;
        bool extruding = false;
        Uuid guid;
        whitebox::PolygonHandle polygon = whitebox::PolygonHandle::Invalid;
        /// Simpul yang ikut bergerak, untuk seretan sub-objek.
        ///
        /// **Dibekukan di awal seretan, bukan dibaca ulang tiap frame.** Setiap
        /// frame membangun ulang mesh dari `before`, dan membaca seleksi lagi
        /// sesudah itu berarti daftar yang menunjuk mesh yang berbeda dari yang
        /// sedang disunting.
        std::vector<whitebox::VertexHandle> vertices;
        /// Bentuk sebelum seretan dimulai; setiap frame membangun kembali darinya.
        whitebox::WhiteboxData before;
        Vec3 normal{0.0f};
        Vec3 pivot{0.0f};
    };

    OrbitCamera camera_;
    SceneView sceneView_;
    std::shared_ptr<const render::ProbeVolume> pushedProbes_;
    std::shared_ptr<const render::Lightmap> pushedLightmap_;
    render::DrawMode drawMode_ = render::DrawMode::Material;
    GizmoOperation operation_ = GizmoOperation::Translate;
    GizmoSpace space_ = GizmoSpace::World;
    GizmoSnap snap_;
    std::vector<DragItem> dragItems_;
    Mat4 pivotAtDragStart_{1.0f};
    ImVec2 boxStart_{0.0f, 0.0f};
    float flySpeed_ = 6.0f;
    bool flying_ = false;
    bool dragging_ = false;
    bool boxSelecting_ = false;
    /// Gizmo sedang ditunjuk atau dipakai pada frame LALU. Dipakai memutuskan
    /// apakah permukaan viewport diajukan sebagai item ImGui frame ini.
    bool gizmoOwnsPointer_ = false;
    /// Kursor berada di atas sebuah tombol overlay pada frame LALU.
    ///
    /// **Frame lalu, dengan alasan yang sama seperti `gizmoOwnsPointer_`:**
    /// `HandleCameraInput` berjalan sebelum overlay digambar, dan urutan
    /// sebaliknya mustahil — overlay menggambar dirinya dari kamera frame ini.
    /// Jeda satu frame tidak terasa: hover mendahului tekanan tombol yang
    /// menyusulnya, selalu, sejauh apa pun tangan bergerak.
    bool overlayOwnsPointer_ = false;
    /// Tombol mouse yang saat ditekan berada di atas overlay, sampai dilepas.
    /// Lihat `UpdateOverlayPointerClaims`.
    std::array<bool, ImGuiMouseButton_COUNT> overlayOwnsButton_{};
    bool orthographic_ = false;
    /// Dari mana viewport memandang. Menentukan `orthographic_` pada tampak
    /// sumbu, dan menggantikan seluruh kamera pada tampak `SceneCamera`.
    Viewpoint viewpoint_ = Viewpoint::Perspective;
    /// Entity berkamera yang sedang dipandangi. Hanya berarti bila
    /// `viewpoint_` adalah `SceneCamera`.
    ///
    /// **Entity, bukan indeks.** Daftar kamera disusun ulang setiap kali menunya
    /// dibuka, dan sebuah indeks menunjuk kamera yang berbeda begitu seseorang
    /// menghapus entity di atasnya — yaitu viewport yang diam-diam berpindah
    /// tempat. Entity yang mati terdeteksi `IsAlive`, dan viewport kembali ke
    /// perspektif alih-alih memandang dari matriks milik entity lain.
    scene::Entity viewpointCamera_ = scene::kNullEntity;
    bool showGrid_ = true;
    /// Klik memilih sisi, bukan entity. Menempel pada viewport, bukan pada
    /// whitebox-nya: ini alat yang sedang dipegang pengguna.
    bool whiteboxMode_ = false;
    WhiteboxDrag whiteboxDrag_;
    /// Klik memahat tanah, bukan memilih. Menempel pada viewport, bukan pada
    /// terrainnya: ini alat yang sedang dipegang pengguna.
    bool terrainMode_ = false;
    terrain::BrushStroke terrainStroke_;
    /// Tempat sentuhan terakhir yang sah, untuk seretan yang keluar peta di
    /// tengah jalan.
    Vec2 lastSculpt_{0.0f};
    /// Falloff yang sedang berlaku, dipakai menggambar lingkaran dalam kursor.
    float falloffPreview_ = 0.5f;
};

}  // namespace

// 31, bukan 30: urutan ini menentukan urutan panel di menu Window, dan
// Vegetation Editor menempati 30 dan Animation Editor 31, supaya keduanya
// berdiri di sebelah Terrain Editor (29) bersama penyunting E7 yang lain. Nomor kembar tidak boleh: pengurutnya
// stabil, jadi seri akan diputuskan urutan link — persis ketidakpastian yang
// nomor ini ada untuk menghilangkannya.
SIM_REGISTER_PANEL(ViewportPanel, 32)

}  // namespace sim::editor
