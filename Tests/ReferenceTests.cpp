#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "Sim/Reference/Lights.h"
#include "Sim/Reference/PathTracer.h"
#include "Sim/Raycast/Query.h"
#include "Sim/Reference/Shading.h"

#include <cmath>
#include <vector>

// Path tracer acuan — R4 di docs/PLAN-EMBREE.md.
//
// **Keluaran R4 adalah angka, bukan gambar.** Regresi visual yang dinilai mata
// bukan regresi yang bisa dijalankan CI, dan sebuah acuan yang kebenarannya
// dinilai dengan melihat tidak lebih dipercaya daripada yang dinilainya.
//
// Karena itu setiap uji di bawah dibandingkan dengan jawaban yang **diketahui
// lebih dulu**, bukan dengan keluaran renderer ini sendiri di masa lalu.

using namespace sim;
using namespace sim::reference;

namespace {

/// Satu kuad, dua segitiga.
void AddQuad(std::vector<Vec3>& positions, std::vector<uint32_t>& indices, const Vec3& origin,
             const Vec3& edgeU, const Vec3& edgeV) {
    const uint32_t base = static_cast<uint32_t>(positions.size());
    positions.push_back(origin);
    positions.push_back(origin + edgeU);
    positions.push_back(origin + edgeU + edgeV);
    positions.push_back(origin + edgeV);
    indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

Surface Diffuse(const Vec3& albedo) {
    Surface s;
    s.baseColor = albedo;
    // Spekular dimatikan supaya yang diukur benar-benar lobe difusnya, dan
    // jawabannya bisa dibandingkan dengan hitungan tangan.
    s.specularWeight = 0.0f;
    return s;
}

float Luminance(const Vec3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

}  // namespace

TEST_CASE("Kamera pinhole menembakkan sinar ke arah yang disebutkan") {
    Camera camera;
    camera.position = Vec3(0.0f, 0.0f, 5.0f);
    camera.target = Vec3(0.0f);
    camera.focusDistance = 5.0f;

    Vec3 origin;
    Vec3 direction;
    camera.GenerateRay(0.5f, 0.5f, 1.0f, 0.0f, 0.0f, origin, direction);

    // Tengah layar menembak lurus ke target.
    CHECK(origin.z == doctest::Approx(5.0f));
    CHECK(direction.x == doctest::Approx(0.0f));
    CHECK(direction.y == doctest::Approx(0.0f));
    CHECK(direction.z == doctest::Approx(-1.0f));

    // Sisi kiri layar menembak ke kiri, sisi atas ke atas. Tanda yang tertukar
    // di sini menghasilkan gambar tercermin — dan gambar tercermin yang
    // dijadikan acuan membuat setiap perbandingan sesudahnya salah.
    camera.GenerateRay(0.0f, 0.5f, 1.0f, 0.0f, 0.0f, origin, direction);
    CHECK(direction.x < 0.0f);
    camera.GenerateRay(0.5f, 0.0f, 1.0f, 0.0f, 0.0f, origin, direction);
    CHECK(direction.y > 0.0f);
}

TEST_CASE("Kamera yang melihat lurus ke bawah tetap punya basis") {
    // **Regresi, dan yang ditemukannya bukan uji ini melainkan uji lain yang
    // lulus untuk alasan yang salah.** Pandangan sejajar `up` membuat
    // `cross(forward, up)` nol dan seluruh basisnya NaN, sehingga setiap sinar
    // meleset dan gambarnya menjadi langit seluruhnya. Di uji tungku putih
    // langit seragam **memang** jawaban yang benar — jadi acuan yang tidak
    // merender apa pun lulus di sana.
    Camera camera;
    camera.position = Vec3(0.0f, 5.0f, 0.0f);
    camera.target = Vec3(0.0f, 0.0f, 0.0f);
    camera.up = Vec3(0.0f, 1.0f, 0.0f);
    camera.focusDistance = 5.0f;

    for (const float sx : {0.0f, 0.5f, 1.0f}) {
        for (const float sy : {0.0f, 0.5f, 1.0f}) {
            Vec3 origin;
            Vec3 direction;
            camera.GenerateRay(sx, sy, 1.0f, 0.0f, 0.0f, origin, direction);
            CAPTURE(sx);
            CAPTURE(sy);
            CHECK(std::isfinite(direction.x));
            CHECK(std::isfinite(direction.y));
            CHECK(std::isfinite(direction.z));
            // Dan ia benar-benar menunjuk ke bawah.
            CHECK(direction.y < 0.0f);
        }
    }
}

TEST_CASE("PDF lampu kuad benar dalam sudut ruang") {
    // **Diperiksa terhadap hitungan tangan, bukan terhadap dirinya sendiri.**
    // Konversi luas ke sudut ruang adalah `d^2 / (cos * A)`, dan salah di sini
    // menghasilkan gambar yang terlalu terang atau terlalu gelap tepat di
    // sekitar lampunya — tempat yang paling terlihat dan paling mudah dikira
    // benar.
    QuadLight light;
    light.origin = Vec3(-0.5f, 2.0f, -0.5f);
    light.edgeU = Vec3(1.0f, 0.0f, 0.0f);
    light.edgeV = Vec3(0.0f, 0.0f, 1.0f);
    light.radiance = Vec3(10.0f);

    CHECK(light.Area() == doctest::Approx(1.0f));
    // Kuad mendatar dengan edgeU x edgeV menunjuk ke bawah.
    CHECK(std::abs(light.Normal().y) == doctest::Approx(1.0f));

    LightList lights;
    lights.Add(light);

    const Vec3 from(0.0f, 0.0f, 0.0f);       // tepat 2 satuan di bawah pusatnya
    const Vec3 toward(0.0f, 1.0f, 0.0f);

    // d = 2, cos = 1, A = 1  ->  pdf = 4
    CHECK(lights.Pdf(from, toward) == doctest::Approx(4.0f).epsilon(0.02));

    // Arah yang menjauhi lampu tidak punya peluang sama sekali.
    CHECK(lights.Pdf(from, Vec3(0.0f, -1.0f, 0.0f)) == doctest::Approx(0.0f));
}

TEST_CASE("Sampling lampu dan PDF-nya sepakat") {
    // Dua jalur menuju besaran yang sama: `Sample` menghitungnya saat memilih
    // titik, `Pdf` menghitungnya dari arah jadi. Keduanya harus menjawab sama —
    // dan kalau tidak, estimator campurannya membagi dengan angka yang bukan
    // peluang sampelnya sendiri, yang bias.
    QuadLight light;
    light.origin = Vec3(-1.0f, 3.0f, -1.0f);
    light.edgeU = Vec3(2.0f, 0.0f, 0.0f);
    light.edgeV = Vec3(0.0f, 0.0f, 2.0f);
    LightList lights;
    lights.Add(light);

    const Vec3 from(0.3f, 0.0f, -0.2f);
    for (const float u : {0.1f, 0.5f, 0.9f}) {
        for (const float v : {0.2f, 0.7f}) {
            const LightSample s = lights.Sample(from, 0.0f, u, v);
            REQUIRE(s.pdf > 0.0f);
            CAPTURE(u);
            CAPTURE(v);
            CHECK(lights.Pdf(from, s.direction) == doctest::Approx(s.pdf).epsilon(0.01));
        }
    }
}

TEST_CASE("Tungku putih: permukaan difus putih di dalam langit seragam") {
    // **Uji yang paling menentukan, dan jawabannya diketahui persis.** Sebuah
    // permukaan difus ber-albedo 1 di dalam lingkungan yang memancarkan
    // radiansi L dari segala arah harus memantulkan tepat L — berapa pun
    // pantulannya, berapa pun kekasarannya. Yang membuatnya gagal adalah energi
    // yang hilang, dan energi yang hilang tidak pernah muncul sebagai galat: ia
    // muncul sebagai adegan yang perlahan menggelap setiap kali cahaya
    // memantul.
    std::vector<Vec3> positions;
    std::vector<uint32_t> indices;
    AddQuad(positions, indices, Vec3(-10.0f, 0.0f, -10.0f), Vec3(20.0f, 0.0f, 0.0f),
            Vec3(0.0f, 0.0f, 20.0f));

    raycast::RayScene scene;
    const auto geometry = scene.AddMesh(positions, indices);
    scene.AddInstance(geometry, Mat4(1.0f));
    scene.Commit();

    const SurfaceResolver resolve = [](const raycast::RayHit& hit, const Vec3& origin,
                                       const Vec3& direction) {
        SurfaceHit out;
        out.position = origin + direction * hit.distance;
        out.normal = Vec3(0.0f, 1.0f, 0.0f);
        out.surface = Diffuse(Vec3(1.0f));
        return out;
    };

    Camera camera;
    camera.position = Vec3(0.0f, 1.0f, 0.0f);
    camera.target = Vec3(0.0f, 0.0f, 0.0f);
    camera.focusDistance = 1.0f;
    camera.verticalFov = 20.0f;

    TraceSettings settings;
    settings.width = 8;
    settings.height = 8;
    settings.samplesPerPixel = 400;
    settings.skyRadiance = Vec3(1.0f);

    // **Dibuktikan lebih dulu bahwa lantainya benar-benar kena.** Uji ini
    // membandingkan hasilnya dengan 1,0, dan sebuah acuan yang setiap sinarnya
    // meleset juga menjawab 1,0 — dari langitnya. Tanpa penjaga ini, kamera
    // yang rusak lulus di sini dan baru ketahuan tiga uji kemudian.
    Vec3 probeOrigin;
    Vec3 probeDirection;
    camera.GenerateRay(0.5f, 0.5f, 1.0f, 0.0f, 0.0f, probeOrigin, probeDirection);
    REQUIRE(raycast::Raycast(scene, probeOrigin, probeDirection).hit);

    const Image image = Render(scene, resolve, LightList{}, camera, settings);
    const Vec3 mean = image.Mean();

    // Toleransinya derau Monte Carlo, bukan kelonggaran terhadap kebenaran.
    CHECK(mean.x == doctest::Approx(1.0f).epsilon(0.03));
    CHECK(mean.y == doctest::Approx(1.0f).epsilon(0.03));
    CHECK(mean.z == doctest::Approx(1.0f).epsilon(0.03));
}

TEST_CASE("Albedo separuh memantulkan separuh, dan pantulan ganda mengikutinya") {
    // Lanjutan tungku putih dengan jawaban yang juga diketahui: permukaan
    // tunggal ber-albedo rho di dalam langit seragam L memantulkan rho*L.
    // **Bukan rho*L ditambah sesuatu** — permukaan datar tidak melihat dirinya
    // sendiri, jadi tidak ada pantulan antar-permukaan yang menambahinya.
    std::vector<Vec3> positions;
    std::vector<uint32_t> indices;
    AddQuad(positions, indices, Vec3(-10.0f, 0.0f, -10.0f), Vec3(20.0f, 0.0f, 0.0f),
            Vec3(0.0f, 0.0f, 20.0f));

    raycast::RayScene scene;
    const auto geometry = scene.AddMesh(positions, indices);
    scene.AddInstance(geometry, Mat4(1.0f));
    scene.Commit();

    for (const float albedo : {0.25f, 0.5f, 0.8f}) {
        const SurfaceResolver resolve = [albedo](const raycast::RayHit& hit, const Vec3& origin,
                                                 const Vec3& direction) {
            SurfaceHit out;
            out.position = origin + direction * hit.distance;
            out.normal = Vec3(0.0f, 1.0f, 0.0f);
            out.surface = Diffuse(Vec3(albedo));
            return out;
        };

        Camera camera;
        camera.position = Vec3(0.0f, 1.0f, 0.0f);
        camera.target = Vec3(0.0f, 0.0f, 0.0f);
        camera.focusDistance = 1.0f;
        camera.verticalFov = 20.0f;

        TraceSettings settings;
        settings.width = 8;
        settings.height = 8;
        settings.samplesPerPixel = 400;
        settings.skyRadiance = Vec3(1.0f);

        const Image image = Render(scene, resolve, LightList{}, camera, settings);
        CAPTURE(albedo);
        CHECK(Luminance(image.Mean()) == doctest::Approx(albedo).epsilon(0.04));
    }
}

TEST_CASE("Lampu bidang menerangi lantai sesuai hukum kosinus") {
    // **Jawabannya dihitung lebih dulu, di tangan.** Sebuah lampu kuad kecil
    // pada ketinggian h di atas lantai difus, dilihat tepat di bawahnya,
    // menghasilkan iradiansi mendekati `L * A / h^2` — hampiran sudut kecil —
    // dan lantai memantulkan `rho/pi` dari itu.
    //
    // Uji ini yang membuktikan bahwa sampling lampu, PDF-nya, dan model
    // shading-nya sepakat. Salah satu saja yang meleset menggeser angkanya.
    const float height = 4.0f;
    const float size = 0.4f;
    const float emitted = 50.0f;
    const float albedo = 0.6f;

    std::vector<Vec3> positions;
    std::vector<uint32_t> indices;
    // Lantai.
    AddQuad(positions, indices, Vec3(-20.0f, 0.0f, -20.0f), Vec3(40.0f, 0.0f, 0.0f),
            Vec3(0.0f, 0.0f, 40.0f));
    // **Lampunya ikut menjadi geometri.** Estimator satu-sampel menemukan
    // cahaya dengan *mengenainya*, bukan dengan sinar bayangan terpisah — jadi
    // lampu yang hanya ada di daftar dan tidak ada di adegan tidak akan pernah
    // menerangi apa pun.
    AddQuad(positions, indices, Vec3(-size * 0.5f, height, -size * 0.5f), Vec3(size, 0.0f, 0.0f),
            Vec3(0.0f, 0.0f, size));

    raycast::RayScene scene;
    const auto geometry = scene.AddMesh(positions, indices);
    scene.AddInstance(geometry, Mat4(1.0f));
    scene.Commit();

    const SurfaceResolver resolve = [&](const raycast::RayHit& hit, const Vec3& origin,
                                        const Vec3& direction) {
        SurfaceHit out;
        out.position = origin + direction * hit.distance;
        // Dua segitiga pertama lantai, dua berikutnya lampu.
        const bool isLight = hit.primitive >= 2;
        out.normal = Vec3(0.0f, 1.0f, 0.0f);
        if (isLight) {
            out.surface = Diffuse(Vec3(0.0f));
            out.emission = Vec3(emitted);
        } else {
            out.surface = Diffuse(Vec3(albedo));
        }
        return out;
    };

    QuadLight light;
    light.origin = Vec3(-size * 0.5f, height, -size * 0.5f);
    light.edgeU = Vec3(size, 0.0f, 0.0f);
    light.edgeV = Vec3(0.0f, 0.0f, size);
    light.radiance = Vec3(emitted);
    LightList lights;
    lights.Add(light);

    Camera camera;
    camera.position = Vec3(0.0f, 1.0f, 0.0f);
    camera.target = Vec3(0.0f, 0.0f, 0.0f);
    camera.focusDistance = 1.0f;
    camera.verticalFov = 6.0f;  // sempit: hanya melihat lantai tepat di bawah lampu

    TraceSettings settings;
    settings.width = 4;
    settings.height = 4;
    settings.samplesPerPixel = 900;
    settings.skyRadiance = Vec3(0.0f);

    const Image image = Render(scene, resolve, lights, camera, settings);

    // Iradiansi hampiran sudut kecil, lalu dipantulkan lobe difus.
    const float irradiance = emitted * (size * size) / (height * height);
    const float expected = albedo * irradiance / 3.14159265f;

    CAPTURE(expected);
    CHECK(Luminance(image.Mean()) == doctest::Approx(expected).epsilon(0.08));
}

TEST_CASE("Stratifikasi menurunkan derau dibanding sampel yang tidak ditata") {
    // Sifat yang membuat stratifikasi bukan hiasan: pada jumlah sampel yang
    // sama, sebaran yang ditata menghasilkan varians yang lebih kecil. Diperiksa
    // lewat selisih antar-piksel di adegan yang seharusnya rata sempurna.
    std::vector<Vec3> positions;
    std::vector<uint32_t> indices;
    AddQuad(positions, indices, Vec3(-10.0f, 0.0f, -10.0f), Vec3(20.0f, 0.0f, 0.0f),
            Vec3(0.0f, 0.0f, 20.0f));

    raycast::RayScene scene;
    const auto geometry = scene.AddMesh(positions, indices);
    scene.AddInstance(geometry, Mat4(1.0f));
    scene.Commit();

    const SurfaceResolver resolve = [](const raycast::RayHit& hit, const Vec3& origin,
                                       const Vec3& direction) {
        SurfaceHit out;
        out.position = origin + direction * hit.distance;
        out.normal = Vec3(0.0f, 1.0f, 0.0f);
        out.surface = Diffuse(Vec3(0.5f));
        return out;
    };

    Camera camera;
    camera.position = Vec3(0.0f, 1.0f, 0.0f);
    camera.target = Vec3(0.0f, 0.0f, 0.0f);
    camera.focusDistance = 1.0f;
    camera.verticalFov = 20.0f;

    TraceSettings settings;
    settings.width = 6;
    settings.height = 6;
    settings.skyRadiance = Vec3(1.0f);

    settings.samplesPerPixel = 64;
    const Image few = Render(scene, resolve, LightList{}, camera, settings);
    settings.samplesPerPixel = 1024;
    const Image many = Render(scene, resolve, LightList{}, camera, settings);

    const auto spread = [](const Image& image) {
        float lo = 1e9f;
        float hi = -1e9f;
        for (const Vec3& p : image.pixels) {
            lo = std::min(lo, p.x);
            hi = std::max(hi, p.x);
        }
        return hi - lo;
    };

    // Derau turun ketika sampelnya bertambah — kalau tidak, ada yang bias, dan
    // yang bias tidak akan pernah konvergen berapa pun sampelnya ditambah.
    CHECK(spread(many) < spread(few));
}
