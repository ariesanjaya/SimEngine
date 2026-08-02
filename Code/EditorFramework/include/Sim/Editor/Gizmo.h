#pragma once

#include "Sim/Core/Math.h"

namespace sim::editor {

enum class GizmoOperation {
    None,
    Translate,
    Rotate,
    Scale,
};

enum class GizmoSpace {
    World,
    Local,
};

/// Pengaturan snapping. Nilainya disimpan Preferences dan dipakai apa adanya.
struct GizmoSnap {
    bool enabled = false;
    float translate = 0.5f;       ///< meter
    float rotateDegrees = 15.0f;  ///< derajat
    float scale = 0.1f;
};

/// Hasil satu frame gizmo.
///
/// `transform` hanya berarti bila `changed` true. `active` bertahan selama
/// seretan berlangsung dan dipakai pemanggil untuk menentukan kapan satu entri
/// undo dibuka dan ditutup.
struct GizmoResult {
    bool hovered = false;
    bool active = false;
    bool changed = false;
    Mat4 transform{1.0f};
};

/// Menyiapkan keadaan gizmo untuk satu frame. Wajib dipanggil sekali per frame
/// sebelum panel mana pun menggambar, dan hanya sekali.
///
/// Bukan formalitas: penanda "kursor sedang di atas gizmo" hanya direset di
/// sini. Tanpa panggilan ini penanda tersebut menumpuk dan gizmo berhenti
/// merespons setelah kursor sempat melewatinya satu kali.
void BeginGizmoFrame();

/// Menggambar gizmo di dalam persegi layar tertentu dan mengembalikan matriks
/// dunia yang baru.
///
/// Membungkus ImGuizmo sepenuhnya: tidak ada tipe pustaka itu yang bocor ke
/// header ini, sehingga panel tidak perlu tahu apa yang dipakai di baliknya.
/// Snapping tidak diserahkan ke pustaka melainkan dikerjakan ulang di sini —
/// lihat catatan di Gizmo.cpp.
/// `interactive` false membuat gizmo tetap tergambar tapi tidak merespons
/// mouse. Dipakai saat kursor sedang berada di atas widget lain yang menaungi
/// viewport: gizmo membaca mouse langsung, jadi tanpa ini ia akan ikut tertarik
/// oleh klik yang sebenarnya ditujukan ke sebuah tombol.
GizmoResult DrawGizmo(const Vec2& origin, const Vec2& size, const Mat4& view, const Mat4& projection,
                      GizmoOperation operation, GizmoSpace space, const GizmoSnap& snap,
                      const Mat4& transform, bool interactive = true);

/// Memecah matriks menjadi translasi, rotasi, dan skala.
///
/// Dipisah supaya pemanggil yang menerapkan hasil gizmo ke TransformComponent
/// memakai pemecahan yang sama persis dengan yang dipakai saat snapping.
void DecomposeTransform(const Mat4& matrix, Vec3& position, Quat& rotation, Vec3& scale);

Mat4 ComposeTransform(const Vec3& position, const Quat& rotation, const Vec3& scale);

}  // namespace sim::editor
