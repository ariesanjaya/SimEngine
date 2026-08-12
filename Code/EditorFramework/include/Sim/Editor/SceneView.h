#pragma once

#include "Sim/Core/Math.h"
#include "Sim/Render/IViewportRenderer.h"
#include "Sim/Render/Types.h"
#include "Sim/Scene/World.h"

#include <vector>

namespace sim::assets {
class AssetDatabase;
}

namespace sim::editor {

class Selection;

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
class SceneView {
public:
    /// Satu objek bergeometri yang bisa diklik, sudah dalam ruang dunia.
    struct Pickable {
        scene::Entity entity = scene::kNullEntity;
        Mat4 worldMatrix{1.0f};
        Vec3 boundsMin{-0.5f, -0.5f, -0.5f};
        Vec3 boundsMax{0.5f, 0.5f, 0.5f};
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

    /// Menyusun ulang seluruh daftar dari isi world.
    /// Membangun daftar gambar dan daftar pickable untuk frame ini.
    ///
    /// `assets` dan `renderer` boleh null — daftarnya tetap terbangun, hanya
    /// setiap mesh renderer menggambar kubus satuan. **Itu perilaku yang
    /// dipertahankan dengan sengaja:** entity yang punya MeshRenderer tapi tidak
    /// menggambar apa pun adalah entity yang tidak bisa diklik, tidak bisa
    /// dipilih, dan karena itu tidak bisa diperbaiki.
    void Build(scene::World& world, const Selection& selection,
               const assets::AssetDatabase* assets = nullptr,
               render::IViewportRenderer* renderer = nullptr);

private:
    void AppendLight(const scene::LightComponent& light, const Mat4& matrix);

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
    std::vector<render::MeshInstance> meshes_;
    std::vector<render::LineSegment> lines_;
    std::vector<render::LightInstance> lights_;
    std::vector<Pickable> pickables_;
    std::vector<EntityIcon> icons_;
};

/// Sinar dunia yang melewati sebuah titik di layar.
///
/// `point` relatif terhadap sudut kiri-atas gambar viewport, dalam piksel.
Ray ScreenPointToRay(const Mat4& view, const Mat4& projection, const Vec2& size, const Vec2& point);

/// Memproyeksikan titik dunia ke piksel viewport. Mengembalikan false bila
/// titiknya di belakang kamera, yang membuat hasil proyeksinya tak bermakna.
bool WorldToScreen(const Mat4& viewProjection, const Vec2& origin, const Vec2& size,
                   const Vec3& world, Vec2& outScreen);

}  // namespace sim::editor
