#include "Sim/Editor/SceneView.h"

#include "Sim/Editor/EditorContext.h"
#include "Sim/Editor/Icons.h"
#include "Sim/Editor/Selection.h"
#include "Sim/Scene/Components.h"

#include <algorithm>
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

}  // namespace

void SceneView::Build(scene::World& world, const Selection& selection) {
    meshes_.clear();
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
            // Kubus satuan sampai mesh sungguhan bisa dimuat di E5/E8. Batas
            // ini dipakai bersama oleh yang digambar dan yang bisa diklik,
            // jadi keduanya tidak mungkin berbeda.
            instance.boundsMin = Vec3(-0.5f);
            instance.boundsMax = Vec3(0.5f);
            instance.color = kMeshColor;
            instance.selected = selected;
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

render::ViewportScene SceneView::Scene() const {
    render::ViewportScene scene;
    scene.meshes = meshes_;
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
