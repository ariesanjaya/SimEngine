#include "Sim/Render/ShadowCascades.h"

#include <algorithm>
#include <cmath>

namespace sim::render {
namespace {

/// Sumbu "atas" untuk matriks pandang cahaya.
///
/// Y dunia, kecuali saat cahaya hampir tegak lurus — di situ Y dan arah pandang
/// sejajar dan `LookAt` menghasilkan matriks yang tidak terdefinisi. Matahari
/// tepat di puncak bukan kasus aneh yang bisa diabaikan: ia keadaan tengah hari.
Vec3 StableUp(const Vec3& direction) {
    return std::abs(direction.y) > 0.99f ? Vec3(0.0f, 0.0f, 1.0f) : Vec3(0.0f, 1.0f, 0.0f);
}

}  // namespace

void ComputeCascadeSplits(const CascadeSettings& settings, float nearZ,
                          std::array<float, kMaxCascades>& out) {
    const int count = std::clamp(settings.count, 1, kMaxCascades);
    const float near = std::max(nearZ, 0.001f);
    const float far = std::max(settings.maxDistance, near * 1.001f);
    const float lambda = std::clamp(settings.splitLambda, 0.0f, 1.0f);
    const float ratio = far / near;

    out.fill(0.0f);
    for (int i = 0; i < count; ++i) {
        const float t = static_cast<float>(i + 1) / static_cast<float>(count);
        const float logarithmic = near * std::pow(ratio, t);
        const float uniform = near + (far - near) * t;
        out[static_cast<size_t>(i)] = lambda * logarithmic + (1.0f - lambda) * uniform;
    }
    // Yang terakhir dipaksa tepat, bukan dibiarkan hasil interpolasi. Selisih
    // sepersekian meter di sini berarti pita sempit di ujung jangkauan yang
    // tidak ditutupi cascade mana pun — dan yang terlihat di sana adalah
    // bayangan yang tiba-tiba hilang, bukan bayangan yang memudar.
    out[static_cast<size_t>(count - 1)] = far;
}

BoundingSphere FrustumSliceSphere(const Camera& camera, float aspect, float nearDistance,
                                  float farDistance) {
    // Bola dihitung dari geometri irisannya, bukan dari kedelapan sudutnya satu
    // per satu. Untuk frustum simetris pusatnya selalu berada di sumbu pandang,
    // dan letaknya punya bentuk tertutup — mencarinya secara iteratif hanya
    // menambah galat yang lalu muncul sebagai kilatan yang justru ingin
    // dihilangkan.
    BoundingSphere sphere;
    const Vec3 forward = camera.Forward();

    if (camera.orthographic) {
        const float halfHeight = camera.orthoHeight * 0.5f;
        const float halfWidth = halfHeight * aspect;
        const float halfDepth = (farDistance - nearDistance) * 0.5f;
        sphere.centre = camera.position + forward * (nearDistance + halfDepth);
        sphere.radius = std::sqrt(halfWidth * halfWidth + halfHeight * halfHeight +
                                  halfDepth * halfDepth);
        return sphere;
    }

    const float tanHalfY = std::tan(camera.fovYRadians * 0.5f);
    const float tanHalfX = tanHalfY * aspect;
    // k² = tan²x + tan²y — kuadrat jari-jari melintang per satuan jarak.
    const float k2 = tanHalfX * tanHalfX + tanHalfY * tanHalfY;

    // Pusat bola pada jarak d di sumbu pandang. Menyamakan jarak ke lingkaran
    // dekat dan ke lingkaran jauh memberi d dalam bentuk tertutup:
    //
    //   (n - d)² + n²k² = (f - d)² + f²k²   →   d = (f + n)(1 + k²) / 2
    float distance = (farDistance + nearDistance) * 0.5f * (1.0f + k2);
    // Kalau pusatnya jatuh di belakang bidang jauh, irisannya begitu pendek
    // sehingga bola terkecilnya adalah bola sekeliling lingkaran jauh saja.
    if (distance >= farDistance) {
        distance = farDistance;
        sphere.centre = camera.position + forward * distance;
        sphere.radius = farDistance * std::sqrt(k2);
        return sphere;
    }

    sphere.centre = camera.position + forward * distance;
    const float dx = farDistance - distance;
    sphere.radius = std::sqrt(dx * dx + farDistance * farDistance * k2);
    return sphere;
}

CascadeSet ComputeCascades(const Camera& camera, float aspect, const Vec3& lightDirection,
                           const CascadeSettings& settings) {
    CascadeSet set;
    set.count = std::clamp(settings.count, 1, kMaxCascades);

    const float lightLengthSq = glm::dot(lightDirection, lightDirection);
    if (lightLengthSq < 1e-8f) {
        // Arah cahaya nol tidak punya matriks pandang. Dikembalikan sebagai
        // himpunan identitas alih-alih NaN: NaN merambat ke matriks setiap
        // cascade dan muncul sebagai layar kosong tanpa satu pun pesan.
        return set;
    }
    const Vec3 toLight = lightDirection / std::sqrt(lightLengthSq);

    std::array<float, kMaxCascades> splits{};
    ComputeCascadeSplits(settings, camera.nearZ, splits);

    const auto resolution = static_cast<float>(std::max(settings.resolution, 1u));
    const float blend = std::clamp(settings.blendFraction, 0.0f, 0.5f);
    float sliceNear = camera.nearZ;

    for (int i = 0; i < set.count; ++i) {
        const float sliceFar = splits[static_cast<size_t>(i)];
        const BoundingSphere sphere = FrustumSliceSphere(camera, aspect, sliceNear, sliceFar);

        Cascade& cascade = set.cascades[static_cast<size_t>(i)];
        cascade.splitFar = sliceFar;
        cascade.radius = sphere.radius;
        cascade.blendBegin = sliceFar - (sliceFar - sliceNear) * blend;

        const float diameter = std::max(sphere.radius * 2.0f, 1e-4f);
        cascade.texelWorldSize = diameter / resolution;

        // Pusat bola dikancing ke kelipatan texel **di ruang cahaya**, bukan di
        // ruang dunia. Kisi texel-nya milik peta bayangan, dan mengancing di
        // ruang dunia berarti mengancing ke kisi yang miring terhadapnya —
        // yang tidak menghilangkan kilatan sama sekali.
        //
        // Kisinya diambil dari rotasi cahaya saja, dengan titik asal di origin
        // dunia. Memakai matriks pandang yang titik asalnya ikut bergerak
        // bersama bola akan membuat pusatnya selalu jatuh tepat di (0,0) —
        // pengancingannya lalu tidak melakukan apa pun, dan kilatannya tetap ada
        // sementara kodenya tampak sudah menanganinya.
        const Vec3 up = StableUp(toLight);
        const Mat4 lightGrid = LookAt(Vec3(0.0f), -toLight, up);
        Vec3 centreInLight = Vec3(lightGrid * Vec4(sphere.centre, 1.0f));
        centreInLight.x = std::floor(centreInLight.x / cascade.texelWorldSize) *
                          cascade.texelWorldSize;
        centreInLight.y = std::floor(centreInLight.y / cascade.texelWorldSize) *
                          cascade.texelWorldSize;
        const Vec3 snappedCentre = Vec3(glm::inverse(lightGrid) * Vec4(centreInLight, 1.0f));

        // **Plus, bukan minus.** `toLight` menunjuk dari permukaan ke cahaya,
        // jadi kamera bayangan duduk di arah itu — di langit, memandang turun.
        // Menguranginya menaruh kamera di sisi yang berlawanan, dan peta yang
        // dihasilkan mengukur kedalaman dari arah yang salah. Yang terlihat
        // bukan bayangan yang bergeser melainkan adegan yang seluruhnya gelap,
        // dan tidak satu pun test yang sudah ada menangkapnya: bola pembatas,
        // pengancingan texel, dan pembagian cascade semuanya tidak menyentuh
        // letak matanya.
        const Vec3 eye = snappedCentre + toLight * (sphere.radius + settings.casterPullback);
        const Mat4 view = LookAt(eye, snappedCentre, up);
        // Ortografik biasa, bukan reversed-Z. Reversed-Z menolong karena float
        // rapat di dekat nol sedangkan perspektif menumpuk objek jauh di sana;
        // depth ortografik linier, jadi menukarnya tidak memindahkan presisi ke
        // mana pun — hanya menambah satu aturan yang harus diingat.
        Mat4 projection = glm::ortho(-sphere.radius, sphere.radius, -sphere.radius, sphere.radius,
                                     0.0f, sphere.radius * 2.0f + settings.casterPullback);
        projection[1][1] *= -1.0f;  // Vulkan: Y layar menunjuk ke bawah
        cascade.viewProjection = projection * view;

        sliceNear = sliceFar;
    }

    // Cascade terakhir tidak punya penerus, jadi tidak ada yang bisa dicampur.
    set.cascades[static_cast<size_t>(set.count - 1)].blendBegin =
        set.cascades[static_cast<size_t>(set.count - 1)].splitFar;
    return set;
}

CascadeChoice ChooseCascade(const CascadeSet& set, float viewDepth) {
    CascadeChoice choice;
    if (set.count <= 0) {
        return choice;
    }
    for (int i = 0; i < set.count; ++i) {
        const Cascade& cascade = set.cascades[static_cast<size_t>(i)];
        if (viewDepth > cascade.splitFar) {
            continue;
        }
        choice.index = i;
        if (i + 1 < set.count && viewDepth > cascade.blendBegin) {
            const float span = cascade.splitFar - cascade.blendBegin;
            choice.blendWeight = span > 1e-6f ? (viewDepth - cascade.blendBegin) / span : 0.0f;
        }
        return choice;
    }
    // Di luar cascade terakhir. Dikembalikan sebagai cascade terakhir tanpa
    // campuran; yang memutuskan bahwa di sana tidak ada bayangan adalah
    // pemanggil, bukan fungsi ini.
    choice.index = set.count - 1;
    return choice;
}

}  // namespace sim::render
