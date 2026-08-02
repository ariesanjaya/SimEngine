#include "Sim/Editor/Gizmo.h"

#include <imgui.h>

#include <ImGuizmo.h>

#include <cmath>

namespace sim::editor {
namespace {

ImGuizmo::OPERATION ToImGuizmo(GizmoOperation operation) {
    switch (operation) {
        case GizmoOperation::Translate:
            return ImGuizmo::TRANSLATE;
        case GizmoOperation::Rotate:
            return ImGuizmo::ROTATE;
        case GizmoOperation::Scale:
            return ImGuizmo::SCALE;
        case GizmoOperation::None:
            break;
    }
    return ImGuizmo::TRANSLATE;
}

/// Membulatkan ke kelipatan terdekat dengan pembulatan simetris.
///
/// std::round dipakai, bukan std::floor(x + 0.5): yang kedua salah untuk nilai
/// negatif tepat di tengah, dan koordinat negatif biasa saja di sebuah level.
float SnapToStep(float value, float step) {
    if (step <= 0.0f) {
        return value;
    }
    return std::round(value / step) * step;
}

/// Membulatkan hanya komponen yang benar-benar berubah.
///
/// ImGuizmo mengunci gerakan ke sumbu yang ditarik, sehingga komponen lain
/// keluar persis sama seperti masuknya. Membulatkan semuanya akan membuat
/// objek yang posisi Y-nya 2.76 tiba-tiba melompat ke 2.5 hanya karena
/// penggunanya menggeser sumbu X — perubahan yang tidak pernah dia minta.
Vec3 SnapChanged(const Vec3& original, const Vec3& candidate, float step) {
    Vec3 result = candidate;
    for (int i = 0; i < 3; ++i) {
        if (candidate[i] != original[i]) {
            result[i] = SnapToStep(candidate[i], step);
        }
    }
    return result;
}

}  // namespace

void BeginGizmoFrame() {
    ImGuizmo::BeginFrame();
}

void DecomposeTransform(const Mat4& matrix, Vec3& position, Quat& rotation, Vec3& scale) {
    position = Vec3(matrix[3]);

    Vec3 columns[3]{Vec3(matrix[0]), Vec3(matrix[1]), Vec3(matrix[2])};
    scale = Vec3(glm::length(columns[0]), glm::length(columns[1]), glm::length(columns[2]));

    // Skala negatif tidak bisa dibedakan dari rotasi hanya dari panjang kolom.
    // Determinan negatif berarti ada pencerminan; membebankan seluruhnya ke
    // sumbu X adalah konvensi yang sama dengan yang dipakai glm::decompose.
    if (glm::determinant(Mat3(matrix)) < 0.0f) {
        scale.x = -scale.x;
        columns[0] = -columns[0];
    }

    for (int i = 0; i < 3; ++i) {
        const float length = glm::length(columns[i]);
        columns[i] = length > 1e-8f ? columns[i] / length : Vec3(0.0f);
    }
    // Sumbu yang skalanya nol tidak menyisakan informasi rotasi apa pun.
    // Membiarkannya nol membuat quat_cast menghasilkan NaN, jadi matriks
    // rotasinya dikembalikan ke identitas untuk sumbu itu.
    for (int i = 0; i < 3; ++i) {
        if (glm::dot(columns[i], columns[i]) < 1e-12f) {
            columns[i] = Vec3(0.0f);
            columns[i][i] = 1.0f;
        }
    }

    rotation = glm::normalize(glm::quat_cast(Mat3(columns[0], columns[1], columns[2])));
}

Mat4 ComposeTransform(const Vec3& position, const Quat& rotation, const Vec3& scale) {
    return glm::translate(Mat4(1.0f), position) * glm::mat4_cast(rotation) *
           glm::scale(Mat4(1.0f), scale);
}

GizmoResult DrawGizmo(const Vec2& origin, const Vec2& size, const Mat4& view, const Mat4& projection,
                      GizmoOperation operation, GizmoSpace space, const GizmoSnap& snap,
                      const Mat4& transform, bool interactive) {
    GizmoResult result;
    result.transform = transform;
    if (operation == GizmoOperation::None || size.x < 1.0f || size.y < 1.0f) {
        return result;
    }

    // Keadaan ImGuizmo bersifat global dan bertahan antar frame, jadi ini harus
    // ditetapkan setiap frame, bukan sekali saja.
    ImGuizmo::Enable(interactive);

    // Disimpulkan dari matriksnya, bukan diminta sebagai parameter: proyeksi
    // perspektif menaruh -1 di [2][3] untuk membagi dengan w, ortografis 0.
    // Satu sumber kebenaran, dan mustahil lupa disinkronkan dengan kamera.
    ImGuizmo::SetOrthographic(projection[2][3] == 0.0f);

    // ImGuizmo memproyeksikan sendiri titik dunia ke layar dengan asumsi NDC
    // gaya OpenGL, yaitu +Y ke atas, lalu membalik Y saat menghitung piksel.
    // Proyeksi kita sudah membalik [1][1] untuk Vulkan (+Y ke bawah), jadi
    // membiarkannya berarti dibalik dua kali: gizmo tergambar tercermin dan
    // panah sumbu Y menunjuk ke bawah. Pembalikannya dikembalikan khusus untuk
    // ImGuizmo — matriks hasilnya tidak terpengaruh, karena yang berubah hanya
    // cara ia memetakan dunia ke piksel.
    Mat4 gizmoProjection = projection;
    gizmoProjection[1][1] = -gizmoProjection[1][1];

    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(origin.x, origin.y, size.x, size.y);

    Mat4 candidate = transform;
    // Snapping tidak diserahkan ke ImGuizmo. Pustaka itu membulatkan *selisih*
    // seretan, sehingga objek yang posisi awalnya bukan kelipatan akan tetap
    // bukan kelipatan selamanya, dan sisa pecahannya menumpuk seretan demi
    // seretan. Yang dibulatkan di sini adalah nilai akhirnya, jadi hasilnya
    // selalu kelipatan persis berapa kali pun objek digeser.
    const bool manipulated =
        ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(gizmoProjection),
                             ToImGuizmo(operation),
                             space == GizmoSpace::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
                             glm::value_ptr(candidate));

    result.hovered = ImGuizmo::IsOver();
    result.active = ImGuizmo::IsUsing();

    if (!manipulated) {
        return result;
    }

    if (snap.enabled) {
        Vec3 position;
        Quat rotation;
        Vec3 scale;
        DecomposeTransform(candidate, position, rotation, scale);

        // Keadaan sebelum manipulasi frame ini, dipakai untuk mengetahui sumbu
        // mana yang sedang ditarik.
        Vec3 originalPosition;
        Quat originalRotation;
        Vec3 originalScale;
        DecomposeTransform(transform, originalPosition, originalRotation, originalScale);

        switch (operation) {
            case GizmoOperation::Translate:
                position = SnapChanged(originalPosition, position, snap.translate);
                break;
            case GizmoOperation::Rotate: {
                // Dibulatkan dalam derajat lalu dikembalikan ke radian: yang
                // ingin dilihat pengguna di Inspector adalah 45°, bukan 45.0001°.
                const Vec3 originalEuler = glm::eulerAngles(originalRotation) * kRadToDeg;
                const Vec3 euler = glm::eulerAngles(rotation) * kRadToDeg;
                rotation = Quat(SnapChanged(originalEuler, euler, snap.rotateDegrees) * kDegToRad);
                break;
            }
            case GizmoOperation::Scale:
                scale = SnapChanged(originalScale, scale, snap.scale);
                break;
            case GizmoOperation::None:
                break;
        }
        candidate = ComposeTransform(position, rotation, scale);
    }

    result.transform = candidate;
    result.changed = true;
    return result;
}

}  // namespace sim::editor
