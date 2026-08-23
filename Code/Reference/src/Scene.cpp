#include "Sim/Reference/Scene.h"

#include <cmath>

namespace sim::reference {
namespace {

Surface Matte(const Vec3& albedo) {
    Surface s;
    s.baseColor = albedo;
    // **Spekular dimatikan, bukan disetel kecil.** Adegan acuan menilai GI
    // difus; sebuah lobe spekular sempit menambahkan derau yang tidak dinilai
    // siapa pun, dan menutupi selisih yang justru dicari.
    s.specularWeight = 0.0f;
    return s;
}

}  // namespace

uint32_t Scene::AddMaterial(const Surface& surface, const Vec3& emission) {
    materials_.push_back(surface);
    emissions_.push_back(emission);
    return static_cast<uint32_t>(materials_.size() - 1);
}

void Scene::AddQuad(const Vec3& origin, const Vec3& edgeU, const Vec3& edgeV,
                    uint32_t material) {
    const uint32_t base = static_cast<uint32_t>(positions_.size());
    positions_.push_back(origin);
    positions_.push_back(origin + edgeU);
    positions_.push_back(origin + edgeU + edgeV);
    positions_.push_back(origin + edgeV);
    indices_.insert(indices_.end(), {base, base + 1, base + 2, base, base + 2, base + 3});

    const Vec3 normal = glm::normalize(glm::cross(edgeU, edgeV));
    triangles_.push_back(Triangle{material, normal});
    triangles_.push_back(Triangle{material, normal});
}

void Scene::AddQuadLight(const Vec3& origin, const Vec3& edgeU, const Vec3& edgeV,
                         const Vec3& radiance, bool doubleSided) {
    // Albedo nol: lampu di adegan acuan tidak ikut memantulkan. Lampu yang juga
    // memantul sah secara fisis, tetapi ia menambahkan jalur yang tidak dihitung
    // rumus pembandingnya.
    const uint32_t material = AddMaterial(Matte(Vec3(0.0f)), radiance);
    AddQuad(origin, edgeU, edgeV, material);

    QuadLight light;
    light.origin = origin;
    light.edgeU = edgeU;
    light.edgeV = edgeV;
    light.radiance = radiance;
    light.doubleSided = doubleSided;
    lights_.Add(light);
}

void Scene::AddBox(const Vec3& minimum, const Vec3& maximum, uint32_t material) {
    const Vec3 size = maximum - minimum;
    const Vec3 x(size.x, 0.0f, 0.0f);
    const Vec3 y(0.0f, size.y, 0.0f);
    const Vec3 z(0.0f, 0.0f, size.z);

    // Urutan sisi dipilih supaya `cross(edgeU, edgeV)` menghadap keluar di
    // keenamnya. Normal yang menghadap ke dalam tidak menghasilkan galat — ia
    // menghasilkan kotak yang menyala dari sisi yang salah.
    AddQuad(minimum, z, y, material);                     // -x
    AddQuad(minimum + x, y, z, material);                 // +x
    AddQuad(minimum, x, z, material);                     // -y
    AddQuad(minimum + y, z, x, material);                 // +y
    AddQuad(minimum, y, x, material);                     // -z
    AddQuad(minimum + z, x, y, material);                 // +z
}

void Scene::Commit(raycast::RayScene& scene) const {
    const auto geometry = scene.AddMesh(positions_, indices_);
    scene.AddInstance(geometry, Mat4(1.0f));
    scene.Commit();
}

SurfaceResolver Scene::Resolver() const {
    // Disalin, bukan ditangkap lewat referensi — lihat catatan di header.
    const std::vector<Triangle> triangles = triangles_;
    const std::vector<Surface> materials = materials_;
    const std::vector<Vec3> emissions = emissions_;

    return [triangles, materials, emissions](const raycast::RayHit& hit, const Vec3& origin,
                                             const Vec3& direction) {
        SurfaceHit out;
        out.position = origin + direction * hit.distance;
        if (hit.primitive >= triangles.size()) {
            return out;
        }
        const Triangle& triangle = triangles[hit.primitive];
        out.normal = triangle.normal;
        out.surface = materials[triangle.material];

        // **Emisi hanya dari sisi depannya.** Lampu langit-langit yang memancar
        // dua sisi menerangi ruangan di atasnya juga, dan di adegan tertutup
        // yang terlihat bukan itu melainkan jawaban yang terlalu terang tanpa
        // sebab yang kelihatan.
        if (glm::dot(triangle.normal, -direction) > 0.0f) {
            out.emission = emissions[triangle.material];
        }
        return out;
    };
}

CornellBox MakeCornellBox() {
    CornellBox box;

    const uint32_t white = box.scene.AddMaterial(Matte(Vec3(0.73f)));
    const uint32_t red = box.scene.AddMaterial(Matte(Vec3(0.65f, 0.05f, 0.05f)));
    const uint32_t green = box.scene.AddMaterial(Matte(Vec3(0.12f, 0.45f, 0.15f)));

    // **Kotak satuan-satu yang tertutup rapat, dengan kameranya di dalam.**
    // Versi pertama membiarkan sisi depan terbuka supaya kamera bisa melihat
    // ke dalam — dan itu membuat uji kebocoran cahaya kehilangan artinya:
    // sebuah kotak yang memang berlubang tentu saja kemasukan langit.
    box.scene.AddQuad(Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f),
                      red);  // kiri
    box.scene.AddQuad(Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f),
                      green);  // kanan
    box.scene.AddQuad(Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f),
                      white);  // lantai
    box.scene.AddQuad(Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f), Vec3(1.0f, 0.0f, 0.0f),
                      white);  // langit-langit
    box.scene.AddQuad(Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f),
                      white);  // dinding belakang
    box.scene.AddQuad(Vec3(0.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f),
                      white);  // dinding depan, di belakang kamera

    // Lampu di langit-langit, memancar ke bawah saja.
    box.scene.AddQuadLight(Vec3(0.35f, 0.998f, 0.35f), Vec3(0.3f, 0.0f, 0.0f),
                           Vec3(0.0f, 0.0f, 0.3f), Vec3(18.0f));

    // Kamera **di dalam** kotaknya, tepat di depan dinding depan.
    //
    // **Perhatikan arah mana yang muncul di sisi mana.** Dengan pandangan ke
    // +z dan `up` +y, `cross(forward, up)` menunjuk ke **-x**: dinding merah
    // di `x = 0` karena itu muncul di sisi **kanan** gambar, bukan kiri. Itu
    // benar untuk sistem koordinat tangan-kanan, dan menebaknya terbalik adalah
    // kesalahan yang lulus setengah ujinya.
    box.camera.position = Vec3(0.5f, 0.5f, 0.06f);
    box.camera.target = Vec3(0.5f, 0.5f, 1.0f);
    box.camera.up = Vec3(0.0f, 1.0f, 0.0f);
    box.camera.verticalFov = 62.0f;
    box.camera.focusDistance = 0.94f;
    return box;
}

Scene MakeEnclosedFurnace(float albedo, float emission) {
    Scene scene;
    const uint32_t wall = scene.AddMaterial(Matte(Vec3(albedo)), Vec3(emission));

    // Kotak dengan normal menghadap **ke dalam**: kamera berada di dalamnya,
    // jadi yang harus menghadapinya sisi dalam tiap dinding. Dibangun dengan
    // menukar kedua sisinya terhadap `AddBox`.
    const Vec3 lo(-1.0f);
    const Vec3 hi(1.0f);
    const Vec3 size = hi - lo;
    const Vec3 x(size.x, 0.0f, 0.0f);
    const Vec3 y(0.0f, size.y, 0.0f);
    const Vec3 z(0.0f, 0.0f, size.z);

    scene.AddQuad(lo, y, z, wall);            // -x, menghadap +x
    scene.AddQuad(lo + x, z, y, wall);        // +x, menghadap -x
    scene.AddQuad(lo, z, x, wall);            // -y, menghadap +y
    scene.AddQuad(lo + y, x, z, wall);        // +y, menghadap -y
    scene.AddQuad(lo, x, y, wall);            // -z, menghadap +z
    scene.AddQuad(lo + z, y, x, wall);        // +z, menghadap -z
    return scene;
}

}  // namespace sim::reference
