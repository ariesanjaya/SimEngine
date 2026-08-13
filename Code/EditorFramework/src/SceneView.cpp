#include "Sim/Editor/SceneView.h"

#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialNodeCatalog.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Scene/Components.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>

namespace sim::editor {
namespace {

constexpr Vec4 kMeshColor{0.62f, 0.65f, 0.70f, 1.0f};
constexpr Vec4 kLightColor{1.00f, 0.86f, 0.42f, 1.0f};
constexpr Vec4 kCameraColor{0.48f, 0.78f, 0.95f, 1.0f};
constexpr Vec4 kEmptyColor{0.45f, 0.47f, 0.52f, 0.85f};

/// Uji sinar terhadap kotak sejajar sumbu dengan metode slab.
///
/// Pembagian dengan komponen arah yang nol menghasilkan inf, dan itu justru
/// perilaku yang benar di sini: sinar yang sejajar sebuah sumbu akan punya
/// t = ±inf pada sumbu itu, sehingga irisan intervalnya tetap benar tanpa
/// pengecekan khusus. Yang perlu dijaga hanya kasus 0/0 → NaN, dan itu
/// tersaring oleh perbandingan di akhir yang selalu false untuk NaN.
bool RayIntersectsAabb(const Vec3& origin, const Vec3& direction, const Vec3& boundsMin,
                       const Vec3& boundsMax, float& outDistance) {
    const Vec3 inverse = 1.0f / direction;
    const Vec3 t0 = (boundsMin - origin) * inverse;
    const Vec3 t1 = (boundsMax - origin) * inverse;
    const Vec3 near = glm::min(t0, t1);
    const Vec3 far = glm::max(t0, t1);

    const float tNear = glm::max(glm::max(near.x, near.y), near.z);
    const float tFar = glm::min(glm::min(far.x, far.y), far.z);

    if (!(tNear <= tFar) || tFar < 0.0f) {
        return false;
    }
    // Sinar yang berawal di dalam kotak tetap mengenainya, jaraknya nol.
    outDistance = tNear >= 0.0f ? tNear : 0.0f;
    return true;
}

/// `"float3(0.62, 0.65, 0.70)"` menjadi sebuah warna.
///
/// **Nilai pin material adalah teks kode shader, bukan angka.** Itu yang
/// membuatnya bisa berupa ekspresi apa pun; yang bisa dibaca di sini hanyalah
/// bentuk harfiahnya, dan yang bukan literal jatuh ke `fallback` alih-alih
/// menjadi tebakan. Satu angka berarti ketiganya sama — `float3(0.5)`.
Vec3 ParseFloat3(std::string_view text, const Vec3& fallback) {
    const std::size_t open = text.find('(');
    const std::size_t close = text.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open) {
        return fallback;
    }
    std::array<float, 3> parsed{};
    int count = 0;
    std::size_t cursor = open + 1;
    while (cursor < close && count < 3) {
        const std::size_t comma = text.find(',', cursor);
        const std::size_t end = comma == std::string_view::npos || comma > close ? close : comma;
        const std::string_view piece = text.substr(cursor, end - cursor);
        float value = 0.0f;
        const char* begin = piece.data();
        const char* stop = piece.data() + piece.size();
        while (begin < stop && (*begin == ' ' || *begin == '\t')) {
            ++begin;
        }
        if (std::from_chars(begin, stop, value).ec != std::errc{}) {
            return fallback;
        }
        parsed[static_cast<std::size_t>(count++)] = value;
        cursor = end + 1;
    }
    if (count == 1) {
        return Vec3(parsed[0]);
    }
    return count == 3 ? Vec3(parsed[0], parsed[1], parsed[2]) : fallback;
}

}  // namespace

void SceneView::Build(scene::World& world, const Selection& selection,
                      const assets::AssetDatabase* assets,
                      render::IViewportRenderer* renderer,
                      const SkinnedPreview* animation,
                      const assets::AssetDatabase* builtinAssets) {
    meshes_.clear();
    skinMatrices_.clear();
    partColors_.clear();
    lights_.clear();
    lines_.clear();
    icons_.clear();
    pickables_.clear();

    // view<> menghasilkan entt::entity, sedangkan scene::Entity adalah enum
    // tersendiri supaya tipe entity kita tidak otomatis tertukar dengan tipe
    // pustaka. Konversinya eksplisit di satu tempat ini saja.
    for (const auto raw : world.Registry().view<scene::IdComponent>()) {
        const auto entity = static_cast<scene::Entity>(raw);
        const auto* visibility = world.TryGet<scene::VisibilityComponent>(entity);
        if (visibility != nullptr && !visibility->visible) {
            continue;
        }

        const Mat4& matrix = world.WorldMatrix(entity);
        const bool selected = selection.Contains(ToSelectionId(entity));
        // Entity terkunci tetap digambar tapi tidak masuk daftar pickable —
        // itu justru gunanya: latar yang terlihat tanpa terpilih tak sengaja.
        const bool pickable = visibility == nullptr || !visibility->locked;

        if (world.Has<scene::MeshRendererComponent>(entity)) {
            render::MeshInstance instance;
            instance.transform = matrix;
            // Kubus satuan adalah nilai mundur, bukan satu-satunya pilihan lagi:
            // aset mesh yang bisa dimuat menggantikan batas ini dengan batasnya
            // sendiri. Batas itu dipakai bersama oleh yang digambar dan yang
            // bisa diklik, jadi keduanya tidak mungkin berbeda.
            instance.boundsMin = Vec3(-0.5f);
            instance.boundsMax = Vec3(0.5f);
            instance.color = kMeshColor;
            instance.selected = selected;
            if (const auto* meshRenderer = world.TryGet<scene::MeshRendererComponent>(entity)) {
                // Warna mundur untuk ruas yang tidak punya material di mana pun:
                // material bawaan editor.
                instance.color = BuiltinColor(builtinAssets);
                instance.castShadows = meshRenderer->castShadows;
                instance.receiveShadows = meshRenderer->receiveShadows;

                // GUID → jalur → geometri. **Renderer yang menyimpan cache-nya**,
                // bukan di sini: yang bisa memutuskan sebuah mesh sudah ada di
                // GPU hanyalah yang memegang buffer-nya, dan cache kedua di
                // editor adalah cache yang suatu saat tidak sepakat dengan yang
                // pertama.
                if (assets != nullptr && renderer != nullptr && meshRenderer->mesh.IsValid()) {
                    if (const assets::AssetRecord* record = assets->Find(meshRenderer->mesh.guid)) {
                        const render::MeshAsset mesh =
                            renderer->AcquireMesh(assets->AbsolutePath(*record).string());
                        if (mesh.loaded) {
                            instance.mesh = mesh.handle;
                            instance.boundsMin = mesh.boundsMin;
                            instance.boundsMax = mesh.boundsMax;
                            AppendPartColors(*meshRenderer, mesh.partCount, assets, instance);
                            AppendSkinPalette(mesh.boneCount,
                                              animation != nullptr
                                                  ? animation->PaletteFor(entity)
                                                  : std::span<const Mat4>{},
                                              instance);
                        }
                    }
                }
            }
            meshes_.push_back(instance);

            if (pickable) {
                pickables_.push_back(
                    Pickable{entity, matrix, instance.boundsMin, instance.boundsMax});
            }
            continue;
        }

        EntityIcon icon;
        icon.entity = entity;
        icon.position = Vec3(matrix[3]);
        icon.selected = selected;
        icon.pickable = pickable;

        if (const auto* light = world.TryGet<scene::LightComponent>(entity)) {
            AppendLight(*light, matrix);
            // Lampu directional dibedakan dari yang lain: ia menerangi seluruh
            // scene tanpa punya posisi bermakna, dan membedakannya sekilas
            // menghemat satu klik ke Inspector.
            icon.glyph = light->type == scene::LightType::Directional ? icons::kSunLight
                                                                     : icons::kLight;
            icon.color = kLightColor;
        } else if (world.Has<scene::CameraComponent>(entity)) {
            icon.glyph = icons::kCamera;
            icon.color = kCameraColor;
        } else {
            icon.glyph = icons::kEntity;
            icon.color = kEmptyColor;
        }
        icons_.push_back(icon);
    }
}

/// Menyediakan warna tiap slot material sebuah instance.
///
/// **Slot kosong ditinggalkan ber-alpha nol, bukan diisi warna bawaan di sini.**
/// Nol berarti "tidak ditetapkan", dan renderer lalu memakai material yang
/// tertulis di berkas mesh untuk ruas itu — yang tidak diketahui editor. Mengisi
/// warna bawaan di sini akan menimpanya, dan model yang berkasnya membawa
/// warnanya sendiri akan tergambar abu-abu seluruhnya.
void SceneView::AppendPartColors(const scene::MeshRendererComponent& renderer, uint32_t partCount,
                                 const assets::AssetDatabase* assets,
                                 render::MeshInstance& instance) {
    if (partCount == 0) {
        return;
    }
    instance.partFirst = static_cast<uint32_t>(partColors_.size());
    instance.partCount = partCount;
    for (uint32_t slot = 0; slot < partCount; ++slot) {
        // Daftar yang lebih pendek daripada jumlah ruas diperlakukan sebagai
        // slot kosong: menambah material di berkas mesh tidak boleh merusak
        // level yang sudah menyebut sebagian di antaranya.
        const bool assigned = slot < renderer.materials.size() &&
                              renderer.materials[slot].IsValid();
        partColors_.push_back(assigned ? MaterialColor(assets, renderer.materials[slot].guid)
                                       : Vec4(0.0f));
    }
}

/// Warna dasar sebuah material, dibaca dari `.simmat`-nya sekali lalu diingat.
///
/// **Yang dibaca hanya warna dasarnya, dan itu memang seluruh yang bisa
/// diperlihatkan sekarang.** Pass forward masih memakai `box.frag`, yang tidak
/// punya roughness, metalness, maupun tekstur; begitu pipeline material
/// menggantikannya, yang di sini berganti menjadi material seutuhnya.
Vec4 SceneView::MaterialColor(const assets::AssetDatabase* assets, const Uuid& guid) {
    if (const auto found = materialColor_.find(guid); found != materialColor_.end()) {
        return found->second;
    }
    Vec4 color = kMeshColor;
    if (assets != nullptr) {
        if (const assets::AssetRecord* record = assets->Find(guid)) {
            material::MaterialGraph graph;
            if (material::LoadMaterialFromFile(graph, assets->AbsolutePath(*record)).ok) {
                for (const material::MaterialNode& node : graph.nodes) {
                    if (node.type != material::kSurfaceOutputType) {
                        continue;
                    }
                    const auto pin = node.pinValues.find("baseColor");
                    if (pin != node.pinValues.end()) {
                        color = Vec4(ParseFloat3(pin->second, Vec3(kMeshColor)), 1.0f);
                    }
                    break;
                }
            }
        }
    }
    // Yang gagal dibaca ikut diingat: mengurai berkas rusak tiap frame adalah
    // editor yang melambat tanpa sebab yang terlihat.
    materialColor_.emplace(guid, color);
    return color;
}

/// Warna material bawaan editor — yang mengisi mesh tanpa material sendiri.
Vec4 SceneView::BuiltinColor(const assets::AssetDatabase* builtinAssets) {
    if (builtinAssets == nullptr) {
        return kMeshColor;
    }
    const assets::AssetRecord* record =
        builtinAssets->FindByRelativePath("Materials/Default.simmat");
    return record != nullptr ? MaterialColor(builtinAssets, record->guid) : kMeshColor;
}

/// Menyediakan palet kulit sebuah instance dan menunjuknya dari instance itu.
///
/// **Palet yang panjangnya tidak cocok ditolak, bukan dipotong atau dipanjangkan.**
/// Panjang yang berbeda berarti pose itu milik rangka yang lain — dan rangka
/// yang lain berarti indeks bone yang menunjuk tulang yang salah. Bind pose yang
/// jelas-jelas diam jauh lebih mudah dilacak daripada kulit yang terpelintir.
///
/// Nilai mundurnya matriks satuan, yaitu bind pose: matriks kulit adalah
/// `global × invers bind`, dan pada bind pose keduanya saling meniadakan.
///
/// **Diisi walaupun hasilnya sama dengan tidak diisi.** Jalur berkulit yang
/// tidak pernah dijalankan siapa pun adalah jalur yang cacatnya baru ditemukan
/// pada hari klip pertama masuk; diisi begini, seluruh rantainya — buffer skin,
/// palet, pipeline, pass bayangan — berjalan di setiap frame editor, dan
/// kesalahan apa pun di dalamnya langsung terlihat sebagai karakter yang cacat.
void SceneView::AppendSkinPalette(uint32_t boneCount, std::span<const Mat4> palette,
                                  render::MeshInstance& instance) {
    if (boneCount == 0) {
        return;
    }
    instance.skinFirst = static_cast<uint32_t>(skinMatrices_.size());
    instance.skinCount = boneCount;
    if (palette.size() == boneCount) {
        skinMatrices_.insert(skinMatrices_.end(), palette.begin(), palette.end());
        return;
    }
    skinMatrices_.insert(skinMatrices_.end(), boneCount, Mat4(1.0f));
}

/// Menerjemahkan sebuah `LightComponent` menjadi lampu ruang dunia.
///
/// **Di sini, bukan di renderer.** Renderer tidak boleh mengenal tipe komponen
/// — itu seam #1 di docs/ARCHITECTURE.md — dan yang paling mudah bocor lewat
/// batas itu justru penerjemahan seperti ini: bagaimana rotasi entity menjadi
/// arah pancar, dan bagaimana sudut kerucut menjadi kosinus.
void SceneView::AppendLight(const scene::LightComponent& light, const Mat4& matrix) {
    render::LightInstance instance;
    instance.position = Vec3(matrix[3]);
    // -Z lokal, sama dengan arah hadap kamera. Sumbu yang berbeda antara lampu
    // dan kamera berarti gizmo yang sama menunjuk ke arah yang berbeda
    // bergantung apa yang terpilih.
    const Vec3 forward = glm::normalize(Vec3(matrix * Vec4(0.0f, 0.0f, -1.0f, 0.0f)));
    instance.color = light.color;
    instance.intensity = light.intensity;
    instance.range = light.range;
    instance.sourceRadius = light.sourceRadius;
    instance.castShadows = light.castShadows;

    switch (light.type) {
        case scene::LightType::Directional:
            instance.kind = render::LightKind::Directional;
            // Dibalik: yang disimpan arah **ke** cahaya, sedangkan yang dihadapi
            // entity adalah arah pancarnya.
            instance.direction = -forward;
            break;
        case scene::LightType::Point:
            instance.kind = render::LightKind::Point;
            break;
        case scene::LightType::Spot:
            instance.kind = render::LightKind::Spot;
            instance.direction = forward;
            break;
    }
    // Kerucut dalam harus selalu di dalam kerucut luar. Sudut yang tertukar
    // menghasilkan pembagi negatif di shader, dan tepi berkasnya menyala alih-alih
    // memudar.
    //
    // Keduanya diambil dari pasangan yang sama, bukan yang satu dari yang lain.
    // Bentuk pertama saya menjepit `inner` terhadap `outer` yang sudah menjadi
    // maksimum — hasilnya kedua sudut menjadi sama persis, pembaginya nol, dan
    // kerucutnya kehilangan seluruh gradasinya. Ditemukan test, bukan dengan
    // membaca ulang.
    const float outer = std::max(light.outerAngleRadians, light.innerAngleRadians);
    const float inner = std::min(light.outerAngleRadians, light.innerAngleRadians);
    instance.cosOuter = std::cos(outer);
    instance.cosInner = std::cos(inner);
    lights_.push_back(instance);
}

void SceneView::AddWireBox(const Vec3& boxMin, const Vec3& boxMax, const Vec4& color) {
    // Dua belas rusuk, disusun dari delapan pojok. Pojoknya diindeks lewat bit
    // supaya ketiga sumbunya diperlakukan sama — daftar rusuk yang ditulis
    // tangan adalah tempat satu rusuk hilang tanpa ada yang menyadarinya.
    const auto corner = [&](int index) {
        return Vec3((index & 1) != 0 ? boxMax.x : boxMin.x,
                    (index & 2) != 0 ? boxMax.y : boxMin.y,
                    (index & 4) != 0 ? boxMax.z : boxMin.z);
    };
    for (int index = 0; index < 8; ++index) {
        for (int axis = 0; axis < 3; ++axis) {
            const int bit = 1 << axis;
            // Hanya arah naik, supaya tiap rusuk digambar sekali dan bukan dua
            // kali — rusuk ganda menggandakan biaya dan menebalkan garisnya di
            // tempat yang tidak beraturan.
            if ((index & bit) != 0) {
                continue;
            }
            lines_.push_back(render::LineSegment{corner(index), corner(index | bit), color});
        }
    }
}

render::ViewportScene SceneView::Scene() const {
    render::ViewportScene scene;
    scene.meshes = meshes_;
    scene.skinMatrices = skinMatrices_;
    scene.partColors = partColors_;
    scene.lines = lines_;
    scene.lights = lights_;
    return scene;
}

scene::Entity SceneView::PickIcon(const Mat4& viewProjection, const Vec2& origin, const Vec2& size,
                                  const Vec2& point, float radiusPixels) const {
    scene::Entity best = scene::kNullEntity;
    float bestDistance = radiusPixels * radiusPixels;

    for (const EntityIcon& icon : icons_) {
        if (!icon.pickable) {
            continue;
        }
        Vec2 screen;
        if (!WorldToScreen(viewProjection, origin, size, icon.position, screen)) {
            continue;
        }
        const Vec2 delta = screen - point;
        const float distance = glm::dot(delta, delta);
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = icon.entity;
        }
    }
    return best;
}

scene::Entity SceneView::Raycast(const Ray& ray) const {
    scene::Entity best = scene::kNullEntity;
    float bestDistance = std::numeric_limits<float>::max();

    for (const Pickable& pickable : pickables_) {
        // Sinar dibawa ke ruang lokal objek, bukan sebaliknya. Dengan begitu
        // ujinya tetap AABB sederhana walau objeknya diputar dan diskala, dan
        // dua objek bertumpuk terpisah tepat pada bentuk masing-masing.
        const Mat4 inverse = glm::inverse(pickable.worldMatrix);
        const Vec3 localOrigin = Vec3(inverse * Vec4(ray.origin, 1.0f));
        const Vec3 localDirection = Vec3(inverse * Vec4(ray.direction, 0.0f));

        float distance = 0.0f;
        if (!RayIntersectsAabb(localOrigin, localDirection, pickable.boundsMin, pickable.boundsMax,
                               distance)) {
            continue;
        }
        // Jarak dibandingkan dalam satuan parameter sinar yang sama untuk
        // semua objek karena arah sinar dunia tidak dinormalkan ulang per
        // objek — skala objek ikut terbawa ke localDirection, dan itu yang
        // membuat perbandingannya tetap adil.
        if (distance < bestDistance) {
            bestDistance = distance;
            best = pickable.entity;
        }
    }
    return best;
}

std::vector<scene::Entity> SceneView::RectSelect(const Mat4& viewProjection, const Vec2& origin,
                                                 const Vec2& size, const ScreenRect& rect) const {
    std::vector<scene::Entity> result;

    for (const Pickable& pickable : pickables_) {
        Vec2 boxMin(std::numeric_limits<float>::max());
        Vec2 boxMax(std::numeric_limits<float>::lowest());
        bool anyVisible = false;

        for (int i = 0; i < 8; ++i) {
            const Vec3 local((i & 1) ? pickable.boundsMax.x : pickable.boundsMin.x,
                             (i & 2) ? pickable.boundsMax.y : pickable.boundsMin.y,
                             (i & 4) ? pickable.boundsMax.z : pickable.boundsMin.z);
            const Vec3 world = Vec3(pickable.worldMatrix * Vec4(local, 1.0f));

            Vec2 screen;
            if (!WorldToScreen(viewProjection, origin, size, world, screen)) {
                continue;  // sudut di belakang kamera, abaikan
            }
            boxMin = glm::min(boxMin, screen);
            boxMax = glm::max(boxMax, screen);
            anyVisible = true;
        }

        if (!anyVisible) {
            continue;
        }
        const bool overlaps = boxMin.x <= rect.max.x && boxMax.x >= rect.min.x &&
                              boxMin.y <= rect.max.y && boxMax.y >= rect.min.y;
        if (overlaps) {
            result.push_back(pickable.entity);
        }
    }

    // Ikon diuji sebagai titik: yang digambar memang penanda seukuran piksel
    // tetap, bukan volume, jadi tidak ada kotak yang bisa dibandingkan.
    for (const EntityIcon& icon : icons_) {
        if (!icon.pickable) {
            continue;
        }
        Vec2 screen;
        if (!WorldToScreen(viewProjection, origin, size, icon.position, screen)) {
            continue;
        }
        if (screen.x >= rect.min.x && screen.x <= rect.max.x && screen.y >= rect.min.y &&
            screen.y <= rect.max.y) {
            result.push_back(icon.entity);
        }
    }
    return result;
}

bool SceneView::BoundsOf(const std::vector<scene::Entity>& entities, Vec3& outMin,
                         Vec3& outMax) const {
    outMin = Vec3(std::numeric_limits<float>::max());
    outMax = Vec3(std::numeric_limits<float>::lowest());
    bool found = false;

    for (const Pickable& pickable : pickables_) {
        if (std::find(entities.begin(), entities.end(), pickable.entity) == entities.end()) {
            continue;
        }
        for (int i = 0; i < 8; ++i) {
            const Vec3 local((i & 1) ? pickable.boundsMax.x : pickable.boundsMin.x,
                             (i & 2) ? pickable.boundsMax.y : pickable.boundsMin.y,
                             (i & 4) ? pickable.boundsMax.z : pickable.boundsMin.z);
            const Vec3 world = Vec3(pickable.worldMatrix * Vec4(local, 1.0f));
            outMin = glm::min(outMin, world);
            outMax = glm::max(outMax, world);
        }
        found = true;
    }

    // Entity tanpa geometri tetap ikut dihitung sebagai titik, supaya "focus ke
    // seleksi" pada sebuah lampu tidak jatuh ke cabang "tidak ketemu" dan
    // melempar kamera kembali ke titik nol.
    for (const EntityIcon& icon : icons_) {
        if (std::find(entities.begin(), entities.end(), icon.entity) == entities.end()) {
            continue;
        }
        outMin = glm::min(outMin, icon.position);
        outMax = glm::max(outMax, icon.position);
        found = true;
    }
    return found;
}

Ray ScreenPointToRay(const Mat4& view, const Mat4& projection, const Vec2& size,
                     const Vec2& point) {
    Ray ray;
    if (size.x < 1.0f || size.y < 1.0f) {
        return ray;
    }

    // Piksel → koordinat perangkat ternormalisasi. Y tidak dibalik: proyeksi
    // kita sudah membalik [1][1] untuk Vulkan (lihat Perspective() di Math.h),
    // jadi +Y NDC sudah menunjuk ke bawah seperti koordinat layar.
    const float ndcX = (point.x / size.x) * 2.0f - 1.0f;
    const float ndcY = (point.y / size.y) * 2.0f - 1.0f;

    const Mat4 inverse = glm::inverse(projection * view);
    // Depth Vulkan [0,1]: bidang dekat di 0, bidang jauh di 1.
    Vec4 nearPoint = inverse * Vec4(ndcX, ndcY, 0.0f, 1.0f);
    Vec4 farPoint = inverse * Vec4(ndcX, ndcY, 1.0f, 1.0f);
    if (std::abs(nearPoint.w) < 1e-8f || std::abs(farPoint.w) < 1e-8f) {
        return ray;
    }
    nearPoint /= nearPoint.w;
    farPoint /= farPoint.w;

    ray.origin = Vec3(nearPoint);
    ray.direction = glm::normalize(Vec3(farPoint) - Vec3(nearPoint));
    return ray;
}

bool WorldToScreen(const Mat4& viewProjection, const Vec2& origin, const Vec2& size,
                   const Vec3& world, Vec2& outScreen) {
    const Vec4 clip = viewProjection * Vec4(world, 1.0f);
    if (clip.w <= 1e-6f) {
        return false;
    }
    const Vec3 ndc = Vec3(clip) / clip.w;
    outScreen = Vec2(origin.x + (ndc.x * 0.5f + 0.5f) * size.x,
                     origin.y + (ndc.y * 0.5f + 0.5f) * size.y);
    return true;
}

}  // namespace sim::editor
