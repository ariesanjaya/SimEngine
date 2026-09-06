#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "Sim/ImageIO/ImageIO.h"
#include "Sim/Reference/ImageCompare.h"
#include "Sim/Reference/Lights.h"
#include "Sim/Reference/Scene.h"
#include "Sim/Reference/PathTracer.h"
#include "Sim/Render/Ibl.h"
#include "Sim/Assets/LightmapRaster.h"
#include "Sim/Render/ProbeVolume.h"
#include "Sim/Raycast/Query.h"
#include "Sim/Reference/Shading.h"

#include "Sim/Raycast/Backend.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <utility>
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
    settings.sky = ConstantSky(Vec3(1.0f));

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
        settings.sky = ConstantSky(Vec3(1.0f));

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
    settings.sky = ConstantSky(Vec3(0.0f));

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
    settings.sky = ConstantSky(Vec3(1.0f));

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

TEST_CASE("Rongga tertutup konvergen ke E/(1-rho)") {
    // **Uji energi pantulan ke-n, dan jawabannya deret geometri yang eksak.**
    // Di dalam rongga tertutup ber-albedo seragam yang setiap dindingnya
    // memancarkan E, radiansi kesetimbangannya `E + rho*E + rho^2*E + ...`,
    // yaitu `E / (1 - rho)`.
    //
    // Ini yang membedakan integrator tak-bias dari yang memotong kedalamannya:
    // pada rho = 0,8 pantulan kelima ke atas masih menyumbang 33% jawabannya,
    // dan sebuah renderer yang berhenti di pantulan keempat akan menjawab 3,36
    // alih-alih 5,0 — meleset sepertiga, tanpa satu pun galat.
    for (const float albedo : {0.0f, 0.5f, 0.8f}) {
        const float emission = 1.0f;
        const Scene scene = MakeEnclosedFurnace(albedo, emission);

        raycast::RayScene rayScene;
        scene.Commit(rayScene);

        Camera camera;
        camera.position = Vec3(0.0f, 0.0f, 0.0f);
        camera.target = Vec3(0.3f, 0.2f, 1.0f);
        camera.focusDistance = 1.0f;
        camera.verticalFov = 50.0f;

        TraceSettings settings;
        settings.width = 6;
        settings.height = 6;
        settings.samplesPerPixel = 900;
        settings.sky = ConstantSky(Vec3(0.0f));  // tertutup: tidak ada langit sama sekali

        const Image image = Render(rayScene, scene.Resolver(), scene.Lights(), camera, settings);
        const float expected = emission / (1.0f - albedo);

        CAPTURE(albedo);
        CAPTURE(expected);
        CHECK(Luminance(image.Mean()) == doctest::Approx(expected).epsilon(0.05));
    }
}

TEST_CASE("Menggandakan maxDepth tidak menggeser rata-rata gambar") {
    // **Bukti Russian roulette-nya tak-bias, bukan sekadar ada.** Sebuah
    // integrator yang memotong kedalaman akan menjawab berbeda ketika batasnya
    // dinaikkan; yang menghentikan jalur dengan roulette berbobot menjawab sama,
    // hanya dengan derau yang sedikit berbeda.
    //
    // Diuji di adegan ber-albedo tinggi, karena di sanalah pantulan dalam masih
    // menyumbang banyak — di adegan gelap perbedaannya tenggelam.
    const Scene scene = MakeEnclosedFurnace(0.8f, 1.0f);
    raycast::RayScene rayScene;
    scene.Commit(rayScene);

    Camera camera;
    camera.position = Vec3(0.0f);
    camera.target = Vec3(0.3f, 0.2f, 1.0f);
    camera.focusDistance = 1.0f;
    camera.verticalFov = 50.0f;

    TraceSettings shallow;
    shallow.width = 6;
    shallow.height = 6;
    shallow.samplesPerPixel = 900;
    shallow.sky = ConstantSky(Vec3(0.0f));
    shallow.maxDepth = 32;

    TraceSettings deep = shallow;
    deep.maxDepth = 64;

    const Image a = Render(rayScene, scene.Resolver(), scene.Lights(), camera, shallow);
    const Image b = Render(rayScene, scene.Resolver(), scene.Lights(), camera, deep);

    const ImageDifference difference = Compare(a, b);
    INFO(difference.ToString());
    // Selisih rata-rata yang besar berarti bias; derau tidak menggeser rata-rata.
    CHECK(Luminance(difference.meanA) ==
          doctest::Approx(Luminance(difference.meanB)).epsilon(0.02));
}

TEST_CASE("Cornell box: dinding berwarna membocorkan warnanya ke lantai") {
    // **Perpindahan cahaya tak-langsung, diuji sebagai perbandingan bukan
    // sebagai nilai mutlak.** Lantai putih di dekat dinding merah harus lebih
    // merah daripada yang di dekat dinding hijau, dan sebaliknya. Yang
    // membuatnya begitu hanya pantulan kedua — pantulan pertama dari lampu
    // putih tidak membawa warna dinding sama sekali.
    //
    // Sebuah renderer yang GI-nya mati lulus setiap uji energi di atas dan
    // gagal di sini, dan itulah gunanya uji ini berdiri terpisah.
    const CornellBox box = MakeCornellBox();
    raycast::RayScene rayScene;
    box.scene.Commit(rayScene);

    TraceSettings settings;
    settings.width = 32;
    settings.height = 32;
    settings.samplesPerPixel = 256;
    settings.sky = ConstantSky(Vec3(0.0f));

    const Image image =
        Render(rayScene, box.scene.Resolver(), box.scene.Lights(), box.camera, settings);

    // **Sisi mana yang merah dibuktikan lebih dulu, bukan diandaikan.** Dengan
    // pandangan ke +z dan `up` +y, sumbu kanan layar menunjuk ke dunia -x —
    // sehingga dinding merah di `x = 0` muncul di sisi **kanan** gambar. Versi
    // pertama uji ini menebaknya terbalik dan gagal dengan pesan yang tidak
    // menyebut sebabnya; sekarang dindingnya diperiksa sendiri, jadi tebakan
    // yang salah gagal di baris yang mengatakannya.
    const Region rightWall{"dinding kanan gambar", 28, 12, 3, 8};
    const Region leftWall{"dinding kiri gambar", 1, 12, 3, 8};
    const Vec3 rightWallColour = RegionMean(image, rightWall);
    const Vec3 leftWallColour = RegionMean(image, leftWall);
    INFO("dinding kanan: ", rightWallColour.x, " ", rightWallColour.y);
    INFO("dinding kiri : ", leftWallColour.x, " ", leftWallColour.y);
    REQUIRE(rightWallColour.x > rightWallColour.y);  // kanan gambar = merah
    REQUIRE(leftWallColour.y > leftWallColour.x);    // kiri gambar = hijau

    // Lantai di dekat masing-masing dinding, di sisi yang baru dibuktikan itu.
    const Region nearRed{"lantai dekat dinding merah", 25, 26, 6, 4};
    const Region nearGreen{"lantai dekat dinding hijau", 1, 26, 6, 4};

    const Vec3 red = RegionMean(image, nearRed);
    const Vec3 green = RegionMean(image, nearGreen);

    INFO("dekat merah: ", red.x, " ", red.y, " ", red.z);
    INFO("dekat hijau: ", green.x, " ", green.y, " ", green.z);

    REQUIRE(red.x > 0.0f);
    REQUIRE(green.y > 0.0f);
    // Perbandingan merah-terhadap-hijau, bukan nilai mutlaknya: yang dinilai
    // warnanya, dan kecerahan kedua sudut memang tidak sama.
    CHECK(red.x / red.y > green.x / green.y);
    CHECK(green.y / green.x > red.y / red.x);
}

TEST_CASE("Cornell box tertutup tidak menerima cahaya dari luar") {
    // **Uji kebocoran cahaya.** Kotak yang tertutup rapat tidak boleh menerima
    // apa pun dari langit, dan setiap aproksimasi GI yang bekerja dengan jarak
    // — SDF, probe berjarak, screen-space — punya caranya sendiri membocorkan
    // cahaya lewat dinding. Acuan ini yang menjadi pembandingnya nanti, jadi ia
    // sendiri harus bebas dari kebocoran itu.
    const CornellBox box = MakeCornellBox();
    raycast::RayScene rayScene;
    box.scene.Commit(rayScene);

    TraceSettings dark;
    dark.width = 16;
    dark.height = 16;
    dark.samplesPerPixel = 100;
    dark.sky = ConstantSky(Vec3(0.0f));

    TraceSettings bright = dark;
    bright.sky = ConstantSky(Vec3(50.0f));

    const Image a = Render(rayScene, box.scene.Resolver(), box.scene.Lights(), box.camera, dark);
    const Image b =
        Render(rayScene, box.scene.Resolver(), box.scene.Lights(), box.camera, bright);

    // Langit lima puluh kali lebih terang daripada lampunya. Kotak yang bocor
    // akan menunjukkannya; yang tidak, tidak.
    //
    // Kotaknya tertutup rapat dan kameranya di dalam, jadi seluruh gambar
    // adalah bagian dalam.
    const Region interior{"bagian dalam", 2, 2, 12, 12};
    const Vec3 sealed = RegionMean(a, interior);
    const Vec3 leaked = RegionMean(b, interior);

    INFO("tertutup: ", sealed.x, "  langit terang: ", leaked.x);
    CHECK(leaked.x == doctest::Approx(sealed.x).epsilon(0.1));
}

TEST_CASE("Pembanding gambar melaporkan angka, bukan gambar") {
    Image a;
    a.width = 2;
    a.height = 2;
    a.pixels = {Vec3(1.0f), Vec3(1.0f), Vec3(1.0f), Vec3(1.0f)};

    Image b = a;
    CHECK(Compare(a, b).rmse == doctest::Approx(0.0f));

    // Satu piksel satu kanal meleset 0,5: RMSE-nya sqrt(0.25 / 12).
    b.pixels[3].x = 0.5f;
    const ImageDifference difference = Compare(a, b);
    CHECK(difference.rmse == doctest::Approx(std::sqrt(0.25f / 12.0f)).epsilon(0.001));
    CHECK(difference.maxAbsolute == doctest::Approx(0.5f));
    CHECK(difference.maxX == 1);
    CHECK(difference.maxY == 1);
    CHECK(!difference.ToString().empty());

    // Ukuran yang tidak sama bukan galat, tetapi juga bukan nol yang berarti
    // "sama" — pemanggil harus bisa membedakannya, dan ukurannya ada padanya.
    Image different;
    different.width = 3;
    different.height = 1;
    different.pixels = {Vec3(0.0f), Vec3(0.0f), Vec3(0.0f)};
    CHECK(Compare(a, different).rmse == doctest::Approx(0.0f));
}

TEST_CASE("Gambar acuan keluar sebagai EXR, dan angkanya bertahan") {
    // **Bukti bahwa gambar acuan bisa dibaca kembali sebagai angka.** Sebuah
    // acuan yang keluarannya sudah dipetakan nada tidak bisa dibandingkan
    // dengan apa pun — dan lampu bidang di adegan Cornell menghasilkan radiansi
    // ratusan kali di atas satu, yang PNG jepit tanpa suara.
    const CornellBox box = MakeCornellBox();
    raycast::RayScene rayScene;
    box.scene.Commit(rayScene);

    TraceSettings settings;
    settings.width = 8;
    settings.height = 8;
    settings.samplesPerPixel = 64;
    settings.sky = ConstantSky(Vec3(0.0f));

    const Image image =
        Render(rayScene, box.scene.Resolver(), box.scene.Lights(), box.camera, settings);

    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "sim-reference-cornell.exr";
    std::filesystem::remove(path);

    const std::string error = WriteExr(path, image);
    if (!error.empty() && error.find("tinyexr") != std::string::npos) {
        MESSAGE("build ini tanpa tinyexr — bagian EXR dilewati");
        return;
    }
    INFO(error);
    REQUIRE(error.empty());
    REQUIRE(std::filesystem::exists(path));

    // Dibaca kembali lewat jalur yang sama yang dipakai siapa pun nanti.
    imageio::Image loaded;
    const imageio::ImageIoResult read = imageio::Read(path, {}, loaded);
    INFO(read.error);
    REQUIRE(read.ok);
    REQUIRE(loaded.desc.width == settings.width);
    REQUIRE(loaded.desc.type == imageio::PixelType::Float32);

    const float* pixels = reinterpret_cast<const float*>(loaded.bytes.data());
    float loadedMean = 0.0f;
    for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        loadedMean += pixels[i * loaded.desc.channels + 0];
    }
    loadedMean /= static_cast<float>(image.pixels.size());

    // Rata-ratanya bertahan dalam presisi half.
    CHECK(loadedMean == doctest::Approx(image.Mean().x).epsilon(0.01));
    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// R6 — apakah penelusuran sinar mendominasi waktu render acuan.
//
// docs/PLAN-EMBREE.md menetapkan satu pemicu untuk mendatangkan Embree, dan
// hanya satu: **penelusuran sinar mendominasi waktu render acuan.** Selama
// angkanya tidak ada, "Embree 3,6x lebih cepat menelusuri" benar tetapi tidak
// menjawab apa pun — yang menentukan bukan kecepatan intersector melainkan
// porsinya. Uji di bawah mengukur porsi itu.
// ---------------------------------------------------------------------------
namespace {

uint32_t EnvUint(const char* name, uint32_t fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }
    const unsigned long parsed = std::strtoul(value, nullptr, 10);
    return parsed > 0 ? static_cast<uint32_t>(parsed) : fallback;
}

double Seconds(std::chrono::steady_clock::time_point from,
               std::chrono::steady_clock::time_point to) {
    return std::chrono::duration<double>(to - from).count();
}

/// Permukaan bergelombang di dalam kotak, dan **tidak koplanar**.
///
/// Grid datar adalah adegan yang paling ramah bagi BVH mana pun: seluruh
/// segitiganya berbagi satu bidang, dan kotak pembatasnya pipih sempurna.
/// Riaknya yang membuat penelusurannya berarti.
void AddRippledSheet(Scene& scene, uint32_t side, float height, uint32_t material) {
    const float step = 1.0f / static_cast<float>(side);
    for (uint32_t z = 0; z < side; ++z) {
        for (uint32_t x = 0; x < side; ++x) {
            const float x0 = static_cast<float>(x) * step;
            const float z0 = static_cast<float>(z) * step;
            const float ripple =
                height * (std::sin(x0 * 37.0f) * std::cos(z0 * 41.0f) + 1.0f) * 0.5f;
            scene.AddQuad(Vec3(x0, 0.02f + ripple, z0), Vec3(step, 0.0f, 0.0f),
                          Vec3(0.0f, 0.0f, step), material);
        }
    }
}

/// Satu titik permukaan yang benar-benar diteduhkan render itu, disimpan supaya
/// ongkos shading-nya bisa diukur pada masukan yang sama — bukan pada material
/// karangan yang lobe-nya kebetulan lebih murah.
struct ShadePoint {
    Surface surface;
    Vec3 normal{0.0f, 1.0f, 0.0f};
    Vec3 view{0.0f, 0.0f, 1.0f};
};

struct Split {
    double renderSeconds = 0.0;
    double traceSeconds = 0.0;
    double shadeSeconds = 0.0;
    std::size_t rays = 0;
    std::size_t shades = 0;
    Image image;

    double Modelled() const { return traceSeconds + shadeSeconds; }
    double TracePercent() const {
        return Modelled() > 0.0 ? 100.0 * traceSeconds / Modelled() : 0.0;
    }
};

/// Merender sekali untuk merekam kerjanya, sekali lagi untuk mengukurnya, lalu
/// menimbang kedua sisinya secara terpisah.
///
/// **Cacah dikali ongkos satuan, bukan sampling profiler.** Sebuah pencacah jam
/// di sekitar tiap sinar berongkos seperlima sinarnya sendiri; yang di bawah
/// tidak menyentuh gelung render sama sekali. Jumlah kedua sisinya dibandingkan
/// dengan waktu render sungguhan — kalau modelnya keliru, ketidakcocokan itu
/// yang memberi tahu, dan uji ini memeriksanya.
Split MeasureSplit(const Scene& scene, const Camera& camera, const TraceSettings& settings) {
    raycast::RayScene rayScene;
    scene.Commit(rayScene);

    // Lintasan perekam: sinar yang benar-benar ditembakkan, dan permukaan yang
    // benar-benar diteduhkan. Adegannya tertutup, jadi setiap sinar kena dan
    // resolver melihat semuanya.
    std::vector<std::pair<Vec3, Vec3>> rays;
    std::vector<ShadePoint> shadePoints;
    const SurfaceResolver inner = scene.Resolver();
    const SurfaceResolver recording = [&](const raycast::RayHit& hit, const Vec3& origin,
                                          const Vec3& direction) {
        rays.emplace_back(origin, direction);
        const SurfaceHit resolved = inner(hit, origin, direction);
        if (shadePoints.size() < 65536) {
            shadePoints.push_back({resolved.surface, resolved.normal, -direction});
        }
        return resolved;
    };
    const Image recorded = Render(rayScene, recording, scene.Lights(), camera, settings);

    Split split;
    split.rays = recorded.raysTraced;
    split.shades = recorded.shadingCalls;

    // Lintasan terukur: resolver apa adanya, benih yang sama, kerja yang sama.
    const auto renderStart = std::chrono::steady_clock::now();
    split.image = Render(rayScene, inner, scene.Lights(), camera, settings);
    split.renderSeconds = Seconds(renderStart, std::chrono::steady_clock::now());

    // Sisi penelusuran: sinar yang persis sama, diulang.
    float sink = 0.0f;
    const auto traceStart = std::chrono::steady_clock::now();
    for (const auto& ray : rays) {
        sink += raycast::Raycast(rayScene, ray.first, ray.second).distance;
    }
    split.traceSeconds = Seconds(traceStart, std::chrono::steady_clock::now());

    // Sisi shading: sebanyak yang benar-benar dipanggil, atas titik permukaan
    // yang benar-benar ditemukan.
    Vec3 shadeSink(0.0f);
    const auto shadeStart = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < split.shades && !shadePoints.empty(); ++i) {
        const ShadePoint& point = shadePoints[i % shadePoints.size()];
        const Frame frame = Frame::FromNormal(point.normal, point.view);
        // Arah hambur yang berputar pelan: `EvaluateDirect` tidak berongkos
        // menurut arahnya, dan arah yang tetap mengundang optimiser mengangkat
        // hitungannya keluar dari gelung.
        const float angle = static_cast<float>(i) * 0.001f;
        const Vec3 scattered = glm::normalize(
            point.normal + Vec3(std::cos(angle), 0.0f, std::sin(angle)) * 0.5f);
        shadeSink += EvaluateDirect(point.surface, frame, scattered, Vec3(1.0f));
    }
    split.shadeSeconds = Seconds(shadeStart, std::chrono::steady_clock::now());

    // Dipakai supaya kedua gelung di atas tidak dibuang optimiser.
    CHECK(std::isfinite(sink));
    CHECK(std::isfinite(shadeSink.x));
    return split;
}

void ReportSplit(const std::string& label, const Split& split) {
    MESSAGE(label, ": ", split.rays, " sinar, ", split.shades, " panggilan shading");
    MESSAGE(label, ": render ", split.renderSeconds, " s; menelusuri ", split.traceSeconds,
            " s; meneduhkan ", split.shadeSeconds, " s");
    MESSAGE(label, ": penelusuran ", split.TracePercent(), "% dari yang termodelkan (",
            split.Modelled(), " s dari ", split.renderSeconds, " s render)");
    if (split.traceSeconds > 0.0) {
        MESSAGE(label, ": throughput ",
                static_cast<double>(split.rays) / split.traceSeconds / 1.0e6,
                " juta sinar/detik, satu thread");
    }
}

}  // namespace

TEST_CASE("R6: berapa porsi waktu render acuan yang dihabiskan menelusuri sinar") {
    // **Angka ini yang memutuskan R6, dan tidak ada yang lain yang boleh.**
    // Backend yang dipakai build ini disebutkan di keluarannya, jadi kedua
    // sisinya bisa dijalankan dan dibandingkan tanpa menebak yang mana.
    MESSAGE("backend: ", std::string(raycast::ToString(raycast::SelectedBackend())));

    const uint32_t size = EnvUint("SIM_R6_SIZE", 64);
    const uint32_t spp = EnvUint("SIM_R6_SPP", 16);

    TraceSettings settings;
    settings.width = size;
    settings.height = size;
    settings.samplesPerPixel = spp;
    settings.sky = ConstantSky(Vec3(0.0f));

    SUBCASE("adegan acuan apa adanya — Cornell box, 32 segitiga") {
        const CornellBox box = MakeCornellBox();
        const Split split = MeasureSplit(box.scene, box.camera, settings);
        ReportSplit("cornell", split);

        // Model cacah-dikali-ongkos tidak boleh meleset jauh dari waktu render
        // sungguhan; kalau ia meleset, porsinya tidak berarti apa-apa.
        CHECK(split.Modelled() < split.renderSeconds * 2.5);
        CHECK(split.rays > 0);
        CHECK(split.shades > 0);

        const std::filesystem::path out =
            std::filesystem::temp_directory_path() /
            (std::string("sim-r6-cornell-") + raycast::ToString(raycast::SelectedBackend()) +
             ".exr");
        const std::string error = WriteExr(out, split.image);
        if (error.empty()) {
            MESSAGE("gambar acuan ditulis ke ", out.string());
        }

        // **Kriteria terima R6: gambar kedua backend cocok dalam toleransi
        // derau.** Backend yang menghasilkan gambar berbeda bukan backend,
        // melainkan renderer kedua — dan selisih itu tidak bisa dilihat dari
        // dalam satu build, karena backend-nya dipilih saat kompilasi. Jalur
        // gambar sisi lain diberikan lewat SIM_R6_COMPARE_EXR.
        const char* reference = std::getenv("SIM_R6_COMPARE_EXR");
        if (reference == nullptr) {
            MESSAGE("SIM_R6_COMPARE_EXR tidak disetel — pembandingan antar-backend dilewati");
            return;
        }

        imageio::Image loaded;
        imageio::ReadOptions options;
        options.channels = 3;
        options.type = imageio::PixelType::Float32;
        const imageio::ImageIoResult read = imageio::Read(reference, options, loaded);
        INFO(read.error);
        REQUIRE(read.ok);
        REQUIRE(loaded.desc.width == split.image.width);
        REQUIRE(loaded.desc.height == split.image.height);

        Image other;
        other.width = loaded.desc.width;
        other.height = loaded.desc.height;
        other.pixels.resize(split.image.pixels.size());
        const float* source = loaded.AsF32();
        REQUIRE(source != nullptr);
        for (std::size_t i = 0; i < other.pixels.size(); ++i) {
            other.pixels[i] = Vec3(source[i * 3 + 0], source[i * 3 + 1], source[i * 3 + 2]);
        }

        const ImageDifference difference = Compare(split.image, other);
        MESSAGE("selisih terhadap ", std::string(reference), ": ", difference.ToString());

        // **Rata-rata yang bergeser berarti bias, bukan derau.** Dua
        // intersektor yang menjawab geometri yang sama boleh berselisih di
        // piksel tepi segitiga; yang tidak boleh adalah seluruh gambar
        // bergeser terang atau gelap.
        const float meanA = Luminance(difference.meanA);
        const float meanB = Luminance(difference.meanB);
        CHECK(meanA == doctest::Approx(meanB).epsilon(0.005));
    }

    SUBCASE("adegan padat — Cornell box berisi permukaan bergelombang") {
        // Sekitar seperempat juta segitiga: orde yang sama dengan Sponza, dan
        // itu memang pertanyaannya — porsi penelusuran tumbuh menurut ukuran
        // adegan, sedangkan porsi shading tidak.
        const uint32_t side = EnvUint("SIM_R6_DENSE_SIDE", 354);
        CornellBox box = MakeCornellBox();
        const uint32_t sheet = box.scene.AddMaterial(Diffuse(Vec3(0.6f, 0.55f, 0.5f)));
        AddRippledSheet(box.scene, side, 0.08f, sheet);
        MESSAGE("padat: ", box.scene.TriangleCount(), " segitiga");

        const Split split = MeasureSplit(box.scene, box.camera, settings);
        ReportSplit("padat", split);
        CHECK(split.rays > 0);
    }
}

// --- B5: tingkat panggang dinilai path tracer acuan --------------------------

namespace {

/// Sebuah kuad tunggal menghadap `normal`, disinari langit saja.
///
/// **Cembung dan sendirian, dan itu syarat perbandingannya.** Tingkat panggang
/// tidak punya oklusi dan tidak punya antar-pantulan; membandingkannya pada
/// adegan yang punya keduanya mengukur apa yang memang tidak dimilikinya, bukan
/// apakah panggangannya benar. Sebuah kuad tunggal tidak menghalangi langit dari
/// dirinya sendiri dan tidak bisa memantul ke dirinya sendiri — jadi jawaban
/// benarnya tepat `albedo/pi * E(n)`, dan yang tersisa untuk diukur cuma
/// panggangannya.
struct SkyLitQuad {
    reference::Scene scene;
    raycast::RayScene rays;
    reference::Camera camera;
    Vec3 albedo{0.6f, 0.5f, 0.4f};
};

/// Bingkai ortonormal di sekitar sebuah normal.
void FrameFor(const Vec3& normal, Vec3& tangent, Vec3& bitangent) {
    const Vec3 helper =
        std::abs(normal.y) < 0.9f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
    tangent = glm::normalize(glm::cross(helper, normal));
    bitangent = glm::cross(normal, tangent);
}

std::unique_ptr<SkyLitQuad> MakeSkyLitQuad(const Vec3& normal, const Vec3& albedo) {
    auto quad = std::make_unique<SkyLitQuad>();
    quad->albedo = albedo;

    reference::Surface surface;
    surface.baseColor = albedo;
    surface.baseWeight = 1.0f;
    surface.baseMetalness = 0.0f;
    surface.baseDiffuseRoughness = 0.0f;
    // **Spekular dimatikan.** Yang dibandingkan suku difusnya; sorotan
    // lingkungan punya ujinya sendiri di B2, dan membiarkannya di sini hanya
    // menambah satu suku yang harus ikut dijelaskan setiap kali angkanya
    // bergeser.
    surface.specularWeight = 0.0f;
    const uint32_t material = quad->scene.AddMaterial(surface);

    Vec3 tangent;
    Vec3 bitangent;
    FrameFor(glm::normalize(normal), tangent, bitangent);
    // Besar, supaya tepinya tidak terlihat dari kamera dan setiap piksel yang
    // diukur benar-benar mengenai permukaannya.
    constexpr float kHalf = 40.0f;
    const Vec3 origin = -tangent * kHalf - bitangent * kHalf;
    quad->scene.AddQuad(origin, tangent * (2.0f * kHalf), bitangent * (2.0f * kHalf), material);
    quad->scene.Commit(quad->rays);

    // Kamera tepat di depan permukaannya, menghadap lurus ke sana.
    quad->camera.position = glm::normalize(normal) * 3.0f;
    quad->camera.target = Vec3(0.0f);
    Vec3 right;
    Vec3 up;
    FrameFor(-glm::normalize(normal), right, up);
    quad->camera.up = up;
    return quad;
}

/// Radiansi rata-rata di tengah gambar, jauh dari tepinya.
Vec3 CentreMean(const reference::Image& image) {
    const uint32_t x0 = image.width / 4;
    const uint32_t x1 = image.width - image.width / 4;
    const uint32_t y0 = image.height / 4;
    const uint32_t y1 = image.height - image.height / 4;
    Vec3 total(0.0f);
    uint32_t count = 0;
    for (uint32_t y = y0; y < y1; ++y) {
        for (uint32_t x = x0; x < x1; ++x) {
            total += image.At(x, y);
            ++count;
        }
    }
    return count > 0 ? total / static_cast<float>(count) : Vec3(0.0f);
}

}  // namespace

namespace {

/// Iradiansi dengan integrasi langsung, tanpa SH sama sekali.
Vec3 IntegrateIrradianceOverMap(const render::EquirectEnvironment& map, const Vec3& normal,
                                uint32_t sampleCount) {
    const float golden = kPi * (3.0f - std::sqrt(5.0f));
    const Vec3 n = glm::normalize(normal);
    Vec3 total(0.0f);
    for (uint32_t i = 0; i < sampleCount; ++i) {
        const float z = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) / static_cast<float>(sampleCount);
        const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
        const float theta = golden * static_cast<float>(i);
        const Vec3 direction(r * std::cos(theta), z, r * std::sin(theta));
        const float cosine = glm::dot(direction, n);
        if (cosine > 0.0f) {
            total += map.Sample(direction) * cosine;
        }
    }
    return total * (4.0f * kPi / static_cast<float>(sampleCount));
}

/// Atmosfer yang sudah dicuplik ke sebuah peta equirect.
///
/// **Satu peta untuk kedua sisi.** Kalau acuannya mencuplik atmosfer analitik
/// sementara panggangannya mencuplik sesuatu yang lain, selisih yang terukur
/// memuat perbedaan pencuplik — dan yang sedang diuji bukan itu. Ia juga yang
/// membuatnya terjangkau: satu cuplikan atmosfer adalah satu ray march 32
/// langkah, dan path tracer memanggilnya sekali untuk tiap sinar yang lolos.
render::EquirectEnvironment BakeAtmosphereToMap(uint32_t width, uint32_t height) {
    render::AtmosphereSky atmosphere;
    atmosphere.sunDirection = glm::normalize(Vec3(0.35f, 0.75f, 0.4f));
    atmosphere.intensity = 20.0f;
    atmosphere.Prepare();

    render::EquirectEnvironment map;
    map.width = width;
    map.height = height;
    map.pixels.assign(static_cast<std::size_t>(width) * height * 3, 0.0f);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const Vec2 uv((static_cast<float>(x) + 0.5f) / static_cast<float>(width),
                          (static_cast<float>(y) + 0.5f) / static_cast<float>(height));
            const Vec3 radiance = atmosphere.Sample(render::EquirectUvToDirection(uv));
            const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 3;
            map.pixels[at] = radiance.x;
            map.pixels[at + 1] = radiance.y;
            map.pixels[at + 2] = radiance.z;
        }
    }
    return map;
}

const Vec3 kB5Albedo{0.6f, 0.5f, 0.4f};

struct B5Case {
    const char* name;
    Vec3 normal;
};
const B5Case kB5Cases[] = {
    {"menghadap ke atas", Vec3(0.0f, 1.0f, 0.0f)},
    {"menghadap matahari", Vec3(0.35f, 0.75f, 0.4f)},
    {"menyamping", Vec3(1.0f, 0.15f, 0.0f)},
    {"membelakangi matahari", Vec3(-0.5f, 0.6f, -0.6f)},
};

/// Radiansi keluar sebuah kuad Lambert yang disinari peta, menurut acuan.
Vec3 TraceSkyLitQuad(const render::EquirectEnvironment& map, const Vec3& normal) {
    const std::unique_ptr<SkyLitQuad> quad = MakeSkyLitQuad(normal, kB5Albedo);

    reference::TraceSettings settings;
    // 24x24 piksel dengan 256 sampel: yang diukur rata-rata sebuah kawasan,
    // bukan sebuah piksel, jadi puluhan ribu sampel yang jatuh di dalamnya sudah
    // jauh melewati titik derau berhenti berarti.
    settings.width = 24;
    settings.height = 24;
    settings.samplesPerPixel = 256;
    settings.seed = 7u;
    settings.sky = [&map](const Vec3& direction) { return map.Sample(direction); };

    const reference::Image image = reference::Render(
        quad->rays, quad->scene.Resolver(), quad->scene.Lights(), quad->camera, settings);
    return CentreMean(image);
}

}  // namespace

TEST_CASE("B5: path tracer acuan cocok dengan integrasi langsung langit yang sama") {
    // **Yang diperiksa di sini acuannya sendiri, bukan panggangannya.** Sebuah
    // acuan yang belum pernah diadu dengan jawaban yang dihitung cara lain
    // adalah pendapat, bukan acuan — dan seluruh guna B5 bersandar padanya.
    //
    // Kuad tunggal yang cembung dan sendirian punya jawaban tertutup:
    // `albedo/pi * E(n)`, dengan E integral langsung atas peta yang sama. Yang
    // diuji karena itu seluruh rantainya sekaligus — kamera, intersektor,
    // shading, dan pencuplikan langitnya.
    const render::EquirectEnvironment map = BakeAtmosphereToMap(256, 128);

    for (const B5Case& item : kB5Cases) {
        const Vec3 traced = TraceSkyLitQuad(map, item.normal);
        const Vec3 exact =
            kB5Albedo * IntegrateIrradianceOverMap(map, item.normal, 32768) / kPi;

        INFO(item.name);
        // 2%: keduanya integral Monte Carlo, dan yang satu lewat 147.456 sinar
        // kamera sementara yang lain lewat 32.768 arah bola.
        CHECK(traced.x == doctest::Approx(exact.x).epsilon(0.02));
        CHECK(traced.y == doctest::Approx(exact.y).epsilon(0.02));
        CHECK(traced.z == doctest::Approx(exact.z).epsilon(0.02));
    }
}

TEST_CASE("B5: tingkat panggang dinilai path tracer acuan") {
    // **Kriteria terima B5.** Panggangan mengubah langit menjadi sembilan angka;
    // path tracer acuan tidak mengubahnya menjadi apa pun — ia mencuplik langit
    // yang sama, sinar demi sinar. Selisih di antara keduanya adalah kesalahan
    // panggangannya, dan tidak ada tempat lain di mesin ini yang bisa
    // mengatakannya.
    //
    // Uji di atas sudah menunjukkan acuannya cocok dengan integrasi langsung
    // dalam bawah 2%, jadi yang terukur di sini memang pemotongan SH orde dua —
    // bukan cacat pada acuannya.
    const render::EquirectEnvironment map = BakeAtmosphereToMap(256, 128);
    const render::Sh9 baked = render::ProjectIrradiance(map, 16384);

    float worstChannel = 0.0f;
    float worstLuminance = 0.0f;
    const auto luminance = [](const Vec3& c) {
        return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
    };

    for (const B5Case& item : kB5Cases) {
        const Vec3 traced = TraceSkyLitQuad(map, item.normal);
        const Vec3 fromSh =
            kB5Albedo * render::EvaluateIrradiance(baked, glm::normalize(item.normal)) / kPi;

        for (int channel = 0; channel < 3; ++channel) {
            worstChannel = std::max(worstChannel, std::abs(traced[channel] - fromSh[channel]) /
                                                      std::max(traced[channel], 1e-6f));
        }
        worstLuminance =
            std::max(worstLuminance, std::abs(luminance(traced) - luminance(fromSh)) /
                                         std::max(luminance(traced), 1e-6f));

        INFO(item.name);
        MESSAGE(item.name << ": acuan (" << traced.x << " " << traced.y << " " << traced.z
                          << ")  panggang (" << fromSh.x << " " << fromSh.y << " " << fromSh.z
                          << ")");
    }
    MESSAGE("selisih terburuk: " << worstChannel * 100.0f << "% per kanal, "
                                 << worstLuminance * 100.0f << "% pada luminansinya");

    // **Ambangnya, dan angkanya dicatat di docs/PLAN-IBL.md.** Yang terukur
    // **9,6% per kanal** dan **6,4% pada luminansinya**; ambangnya 12% dan 8%,
    // memberi ruang untuk derau dan selisih platform tanpa berhenti menangkap
    // regresi — sebuah panggangan yang rusak meleset jauh lebih besar daripada
    // ini.
    //
    // **Angka sebesar itu bukan cacat melainkan batas SH orde dua**, dan uji di
    // atas yang membuktikannya: acuannya sendiri cocok dengan integrasi langsung
    // dalam bawah 2%, jadi yang tersisa memang pemotongannya. Ia paling besar di
    // kanal paling redup — langit biru membuat merah sepertiga dari birunya,
    // jadi selisih absolut yang sama terbaca tiga kali lebih besar di sana.
    CHECK(worstChannel < 0.12f);
    CHECK(worstLuminance < 0.08f);
}

// --- S2: transport dipanggang ke probe (docs/PLAN-STATIC-GI.md) --------------

namespace {

/// Kotak tertutup ber-albedo `albedo`, tanpa lampu dan tanpa emisi. Gunanya
/// satu: memeriksa apa yang **tidak** masuk ke dalamnya.
sim::reference::Scene MakeSealedRoom(float albedo) {
    using namespace sim::reference;
    Scene scene;
    Surface surface;
    surface.baseColor = Vec3(albedo);
    surface.specularRoughness = 1.0f;
    surface.baseMetalness = 0.0f;
    const uint32_t material = scene.AddMaterial(surface);
    // Kotak yang normalnya menghadap keluar; probe berada di dalamnya, jadi yang
    // dikenainya sisi belakang — dan `TracePath` membalik normal menghadap sinar
    // datang, persis seperti yang dilakukannya untuk gambar acuan.
    scene.AddBox(Vec3(-1.0f), Vec3(1.0f), material);
    return scene;
}

}  // namespace

TEST_CASE("S2: probe di ruang terbuka menjawab sama dengan panggangan lingkungan") {
    // **Kriteria terima S2, bagian pertama.** Tanpa satu pun penghalang, setiap
    // sinar dari probe lolos ke langit — jadi transportnya harus mengembalikan
    // iradiansi lingkungan itu sendiri. Kalau tidak, yang salah bukan
    // oklusinya melainkan basis SH, bobot Monte Carlo, atau konvensi arahnya —
    // dan ketiganya akan terbaca sebagai "GI-nya bocor" di adegan berikutnya.
    using namespace sim::reference;
    const sim::render::GradientSky sky;

    raycast::RayScene empty;
    empty.Commit();

    TraceSettings settings;
    settings.sky = [&sky](const Vec3& direction) { return sky.Sample(direction); };

    const std::array<Vec3, 9> traced = TraceProbeIrradiance(
        empty, [](const raycast::RayHit&, const Vec3&, const Vec3&) { return SurfaceHit{}; },
        LightList{}, Vec3(0.0f, 1.0f, 0.0f), 16384, settings);

    const sim::render::Sh9 baked = sim::render::ProjectIrradiance(sky, 16384);
    sim::render::Sh9 probe;
    probe.coefficients = traced;

    for (const Vec3 normal : {Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f),
                              Vec3(1.0f, 0.0f, 0.0f), glm::normalize(Vec3(1.0f, 1.0f, 1.0f))}) {
        const Vec3 expected = sim::render::EvaluateIrradiance(baked, normal);
        const Vec3 got = sim::render::EvaluateIrradiance(probe, normal);
        INFO("normal (", normal.x, ",", normal.y, ",", normal.z, ")");
        // Ambangnya 2%: keduanya Monte Carlo dengan urutan sampel yang berbeda,
        // jadi menuntut kesamaan persis berarti menuntut dua penaksir yang
        // berbeda menghasilkan derau yang sama.
        CHECK(got.x == doctest::Approx(expected.x).epsilon(0.02));
        CHECK(got.y == doctest::Approx(expected.y).epsilon(0.02));
        CHECK(got.z == doctest::Approx(expected.z).epsilon(0.02));
    }
}

TEST_CASE("S2: ruangan tertutup berhenti disinari langit") {
    // **Kriteria terima S2, bagian kedua — dan inti seluruh milestone ini.**
    // Tingkat panggang seri B menyinari ruang tertutup persis seterang ruang
    // terbuka: sembilan angka untuk seluruh level, tanpa satu pun sinar yang
    // memeriksa apakah ada dinding di antaranya. Yang membuat transport layak
    // dipanggang adalah bahwa angka ini turun.
    using namespace sim::reference;
    const sim::render::GradientSky sky;

    TraceSettings settings;
    settings.sky = [&sky](const Vec3& direction) { return sky.Sample(direction); };

    const Scene room = MakeSealedRoom(0.5f);
    raycast::RayScene sealed;
    room.Commit(sealed);

    const std::array<Vec3, 9> insideSh = TraceProbeIrradiance(
        sealed, room.Resolver(), room.Lights(), Vec3(0.0f), 4096, settings);
    sim::render::Sh9 inside;
    inside.coefficients = insideSh;

    const sim::render::Sh9 open = sim::render::ProjectIrradiance(sky, 4096);
    const Vec3 normal(0.0f, 1.0f, 0.0f);
    const float sealedLevel = sim::render::EvaluateIrradiance(inside, normal).y;
    const float openLevel = sim::render::EvaluateIrradiance(open, normal).y;

    MESSAGE("iradiansi di dalam kotak tertutup: ", sealedLevel, " lawan ", openLevel,
            " di ruang terbuka (", 100.0f * sealedLevel / openLevel, "%)");
    // Kotak tanpa emisi dan tanpa lampu: satu-satunya cahaya yang bisa masuk
    // adalah cahaya yang tidak dihalangi apa-apa, dan tidak ada.
    CHECK(sealedLevel < openLevel * 0.01f);
}

TEST_CASE("S2: uji tungku lulus pada probe seperti pada gambar acuan") {
    // Rongga tertutup ber-albedo ρ yang setiap permukaannya memancarkan
    // radiansi E berada pada kesetimbangan `E / (1 - ρ)`. Iradiansi pada sebuah
    // titik di dalamnya karena itu `π · E / (1 - ρ)`.
    //
    // **Yang diujinya energi pantulan ke-n, bukan pantulan pertama.** Pada
    // ρ = 0,8 pantulan kelima masih menyumbang sepertiga jawabannya — jadi
    // penaksir yang memotong kedalamannya lulus uji ruang terbuka dan gagal di
    // sini.
    using namespace sim::reference;
    constexpr float kAlbedo = 0.5f;
    constexpr float kEmission = 1.0f;
    const Scene furnace = MakeEnclosedFurnace(kAlbedo, kEmission);
    raycast::RayScene rayScene;
    furnace.Commit(rayScene);

    TraceSettings settings;
    settings.sky = ConstantSky(Vec3(0.0f));  // rongga tertutup: tidak ada langit

    const std::array<Vec3, 9> coefficients = TraceProbeIrradiance(
        rayScene, furnace.Resolver(), furnace.Lights(), Vec3(0.0f), 4096, settings);
    sim::render::Sh9 probe;
    probe.coefficients = coefficients;

    const float expected = 3.14159265f * kEmission / (1.0f - kAlbedo);
    const float got = sim::render::EvaluateIrradiance(probe, Vec3(0.0f, 1.0f, 0.0f)).y;
    MESSAGE("tungku: iradiansi probe ", got, " lawan ", expected, " yang diketahui persis");
    CHECK(got == doctest::Approx(expected).epsilon(0.05));
}

TEST_CASE("S2: matahari tidak masuk langsung, tetapi pantulannya masuk") {
    // **Keputusan 2 diperiksa, bukan diandaikan.** Matahari langsung diantarkan
    // lampu terarah yang berbayang saat menggambar; panggangan yang ikut
    // memuatnya menghitungnya dua kali, dan yang terlihat bukan galat melainkan
    // bayangan yang setengah terisi.
    using namespace sim::reference;
    TraceSettings settings;
    settings.sky = ConstantSky(Vec3(0.0f));
    settings.sunIrradiance = Vec3(10.0f);
    settings.sunDirection = Vec3(0.0f, -1.0f, 0.0f);  // tegak lurus ke bawah

    // Tanpa geometri sama sekali: tidak ada permukaan, jadi tidak ada satu pun
    // tempat NEE bisa berjalan — dan probe menjawab nol walaupun matahari
    // menyala sepuluh kali.
    raycast::RayScene empty;
    empty.Commit();
    const std::array<Vec3, 9> alone = TraceProbeIrradiance(
        empty, [](const raycast::RayHit&, const Vec3&, const Vec3&) { return SurfaceHit{}; },
        LightList{}, Vec3(0.0f), 1024, settings);
    sim::render::Sh9 direct;
    direct.coefficients = alone;
    CHECK(std::abs(sim::render::EvaluateIrradiance(direct, Vec3(0.0f, 1.0f, 0.0f)).y) < 1e-6f);

    // Sekarang sebuah lantai di bawah probe. Mataharinya menyinari lantai itu,
    // dan lantainya memantulkannya ke probe — itu yang harus terpanggang.
    Scene floor;
    Surface surface;
    surface.baseColor = Vec3(0.8f);
    surface.specularRoughness = 1.0f;
    const uint32_t material = floor.AddMaterial(surface);
    floor.AddQuad(Vec3(-20.0f, 0.0f, -20.0f), Vec3(40.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 40.0f),
                  material);
    raycast::RayScene ground;
    floor.Commit(ground);

    const std::array<Vec3, 9> bounced = TraceProbeIrradiance(
        ground, floor.Resolver(), floor.Lights(), Vec3(0.0f, 1.0f, 0.0f), 4096, settings);
    sim::render::Sh9 indirect;
    indirect.coefficients = bounced;
    const float fromBelow = sim::render::EvaluateIrradiance(indirect, Vec3(0.0f, -1.0f, 0.0f)).y;
    MESSAGE("pantulan matahari dari lantai: ", fromBelow);
    CHECK(fromBelow > 0.5f);
}

// --- S3: oklusi arah (docs/PLAN-STATIC-GI.md) --------------------------------

namespace {

/// Memanggang kisi lengkap — SH **dan** peta kedalaman — dari sebuah adegan
/// acuan. Cerminan `view::ProbeBakery` dalam bentuk yang tidak menuntut editor.
sim::render::ProbeVolume BakeWithVisibility(const sim::reference::Scene& scene,
                                            const raycast::RayScene& rayScene,
                                            const sim::render::ProbeVolumeLayout& layout,
                                            const sim::reference::TraceSettings& trace,
                                            uint32_t irradianceSamples,
                                            uint32_t visibilitySamples) {
    using namespace sim::render;
    ProbeVolume volume;
    volume.layout = layout;
    volume.brickSlots.assign(layout.BrickCount(), 0u);
    for (uint32_t brick = 0; brick < layout.BrickCount(); ++brick) {
        volume.brickSlots[brick] = brick;
    }
    volume.probes.assign(layout.FullProbeCount(), Sh9{});
    volume.depth.assign(static_cast<std::size_t>(layout.FullProbeCount()) *
                            ProbeVolume::kDepthFloats,
                        0.0f);

    constexpr uint32_t kSide = ProbeVolumeLayout::kBrickSize;
    constexpr uint32_t kPerBrick = kSide * kSide * kSide;
    const glm::uvec3 bricks = layout.BrickCounts();
    std::vector<float> moments;

    for (uint32_t brick = 0; brick < layout.BrickCount(); ++brick) {
        const glm::uvec3 brickAt(brick % bricks.x, (brick / bricks.x) % bricks.y,
                                 brick / (bricks.x * bricks.y));
        for (uint32_t local = 0; local < kPerBrick; ++local) {
            const glm::uvec3 inside(local % kSide, (local / kSide) % kSide,
                                    local / (kSide * kSide));
            const Vec3 position = layout.ProbePosition(brickAt * kSide + inside);
            const std::size_t slot = static_cast<std::size_t>(brick) * kPerBrick + local;

            volume.probes[slot].coefficients = sim::reference::TraceProbeIrradiance(
                rayScene, scene.Resolver(), scene.Lights(), position, irradianceSamples, trace);
            sim::reference::TraceProbeVisibility(rayScene, position, ProbeVolume::kDepthSize,
                                                 visibilitySamples, 64.0f, moments);
            std::copy(moments.begin(), moments.end(),
                      volume.depth.begin() +
                          static_cast<std::ptrdiff_t>(slot * ProbeVolume::kDepthFloats));
        }
    }
    return volume;
}

/// Lantai besar dengan sebuah meja di atasnya: pelat mendatar setinggi 2 m yang
/// membentang [-2,2] di kedua sumbu mendatar.
sim::reference::Scene MakeTableScene() {
    using namespace sim::reference;
    Scene scene;
    Surface surface;
    surface.baseColor = Vec3(0.5f);
    surface.specularWeight = 0.0f;
    surface.specularRoughness = 1.0f;
    const uint32_t material = scene.AddMaterial(surface);
    scene.AddQuad(Vec3(-20.0f, 0.0f, -20.0f), Vec3(40.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 40.0f),
                  material);
    scene.AddBox(Vec3(-2.0f, 1.9f, -2.0f), Vec3(2.0f, 2.1f, 2.0f), material);
    return scene;
}

}  // namespace

TEST_CASE("S3: benda di bawah meja lebih gelap daripada benda di sebelahnya") {
    // **Kriteria terima S3.** Sebelum milestone ini, kisi menjawab pertanyaan
    // "apa isi kisi di sini" — dan sebuah titik di bawah meja mendapat rata-rata
    // dari probe di atas meja beserta yang di bawahnya, karena tidak ada yang
    // memberitahunya bahwa ada pelat di antaranya.
    using namespace sim::reference;
    using namespace sim::render;

    const GradientSky sky;
    TraceSettings trace;
    trace.sky = [&sky](const Vec3& direction) { return sky.Sample(direction); };

    const Scene scene = MakeTableScene();
    raycast::RayScene rayScene;
    scene.Commit(rayScene);

    const ProbeVolumeLayout layout =
        MakeProbeLayout(Vec3(-8.0f, 0.0f, -8.0f), Vec3(8.0f, 4.0f, 8.0f), 1.0f);
    REQUIRE(layout.IsValid());
    const ProbeVolume volume = BakeWithVisibility(scene, rayScene, layout, trace, 256, 32);
    REQUIRE(volume.IsValid());
    REQUIRE(volume.HasVisibility());

    const Vec3 normal(0.0f, 1.0f, 0.0f);
    const Vec3 under(0.0f, 0.5f, 0.0f);   // di bawah meja
    const Vec3 beside(6.0f, 0.5f, 0.0f);  // di sebelahnya, ketinggian sama

    const float underLit =
        EvaluateIrradiance(SampleProbeVolumeAt(volume, under), normal).y;
    const float besideLit =
        EvaluateIrradiance(SampleProbeVolumeAt(volume, beside), normal).y;

    // Dan tanpa visibilitas — yaitu jawaban S2 — untuk melihat apa yang dibeli.
    const float underPlain = EvaluateIrradiance(SampleProbeVolume(volume, under), normal).y;
    const float besidePlain = EvaluateIrradiance(SampleProbeVolume(volume, beside), normal).y;

    MESSAGE("dengan visibilitas : bawah meja ", underLit, ", di sebelahnya ", besideLit,
            "  (", 100.0f * underLit / besideLit, "% seterang)");
    MESSAGE("tanpa visibilitas  : bawah meja ", underPlain, ", di sebelahnya ", besidePlain,
            "  (", 100.0f * underPlain / besidePlain, "% seterang)");

    CHECK(underLit < besideLit);
    // Bukan sekadar "lebih gelap": mejanya menutupi hampir seluruh belahan atas,
    // jadi yang di bawahnya harus jelas lebih gelap, bukan sedikit lebih gelap.
    CHECK(underLit < besideLit * 0.75f);
}

TEST_CASE("S3: sel yang melintasi dinding menjawab jauh lebih dekat ke acuan") {
    // **Ini yang benar-benar dibeli S3, dan uji meja di atas tidak mengujinya.**
    // Di sana kedelapan sudut interpolasi berada di sisi yang sama, jadi
    // visibilitas tidak punya apa pun untuk ditolak — angkanya sama persis
    // dengan dan tanpa. Yang membedakan keduanya adalah sel yang **melintasi**
    // penghalang: sebuah titik di ruang gelap yang salah satu sudut selnya
    // berada di ruang terang di seberang dinding.
    //
    // Dindingnya sengaja lebih tipis daripada satu sel. Dinding yang lebih tebal
    // menaruh sudutnya di dalam dirinya sendiri, dan yang di sana dipanggang nol
    // — itu kasus lain, dan yang menyelesaikannya bobot yang sama.
    using namespace sim::reference;
    using namespace sim::render;

    const GradientSky sky;
    TraceSettings trace;
    trace.sky = [&sky](const Vec3& direction) { return sky.Sample(direction); };

    Scene scene;
    Surface surface;
    surface.baseColor = Vec3(0.5f);
    surface.specularWeight = 0.0f;
    surface.specularRoughness = 1.0f;
    const uint32_t material = scene.AddMaterial(surface);
    // Lantai, dinding tipis di x = 0,5, dan atap yang hanya menutupi sisi kiri.
    scene.AddQuad(Vec3(-12.0f, 0.0f, -12.0f), Vec3(24.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 24.0f),
                  material);
    scene.AddBox(Vec3(0.45f, 0.0f, -8.0f), Vec3(0.55f, 5.0f, 8.0f), material);
    scene.AddBox(Vec3(-8.0f, 4.9f, -8.0f), Vec3(0.45f, 5.1f, 8.0f), material);
    // Dinding belakang supaya sisi kiri benar-benar tertutup, bukan koridor
    // yang terbuka di ujungnya.
    scene.AddBox(Vec3(-8.1f, 0.0f, -8.0f), Vec3(-7.9f, 5.0f, 8.0f), material);

    raycast::RayScene rayScene;
    scene.Commit(rayScene);

    const ProbeVolumeLayout layout =
        MakeProbeLayout(Vec3(-6.0f, 0.0f, -6.0f), Vec3(6.0f, 5.0f, 6.0f), 1.0f);
    const ProbeVolume volume = BakeWithVisibility(scene, rayScene, layout, trace, 256, 32);
    REQUIRE(volume.HasVisibility());

    const Vec3 normal(0.0f, 1.0f, 0.0f);
    // Di sisi gelap, cukup dekat ke dinding sehingga selnya melintasinya.
    const Vec3 shaded(0.15f, 1.0f, 0.0f);
    const float leaked = EvaluateIrradiance(SampleProbeVolume(volume, shaded), normal).y;
    const float sealed = EvaluateIrradiance(SampleProbeVolumeAt(volume, shaded), normal).y;
    // Acuannya: apa yang benar-benar diterima titik itu, ditelusuri langsung.
    Sh9 truth;
    truth.coefficients = TraceProbeIrradiance(rayScene, scene.Resolver(), scene.Lights(), shaded,
                                              4096, trace);
    const float reference = EvaluateIrradiance(truth, normal).y;

    MESSAGE("tanpa visibilitas ", leaked, ", dengan visibilitas ", sealed, ", acuan ", reference);
    MESSAGE("kesalahan: ", std::abs(leaked - reference), " menjadi ",
            std::abs(sealed - reference));

    // **Diadu dengan acuan, bukan dengan satu sama lain**, dan itu bukan
    // kehati-hatian yang berlebihan: dugaan pertama saya adalah bahwa
    // visibilitas menggelapkan titik ini — cahaya yang bocor menembus dinding.
    // Yang terukur kebalikannya. Kebocorannya **menggelapkan**, karena sudut sel
    // yang berada di dalam dinding dipanggang tepat nol dan menarik jawabannya
    // turun. Visibilitas menolak sudut itu, dan angkanya naik mendekati acuan.
    //
    // Sebuah uji yang menuntut "lebih gelap" akan gagal di sini pada kode yang
    // benar, dan itu sebabnya yang dituntut adalah lebih dekat ke jawaban yang
    // sebenarnya — bukan bergerak ke arah yang diduga.
    CHECK(std::abs(sealed - reference) < std::abs(leaked - reference));
    // Setidaknya empat kali lebih dekat; yang terukur 6,6 kali.
    CHECK(std::abs(sealed - reference) * 4.0f < std::abs(leaked - reference));
}

TEST_CASE("S3: koridor lebih sempit daripada kisinya tidak menjadi hitam pekat") {
    // **Yang diperiksa di sini keadaan tempat visibilitas kehabisan pilihan.**
    // Sebuah koridor yang lebih sempit daripada jarak antar-probe menaruh
    // kedelapan sudut selnya di dalam dinding — dan yang di dalam dipanggang
    // nol lalu ditolak bobot Chebyshev. Kalau kedelapannya ditolak, tidak ada
    // yang tersisa untuk dinormalkan, dan permukaannya menerima **nol**.
    //
    // Nol di sana lebih buruk daripada bocor: bocor terlihat sebagai cahaya yang
    // sedikit salah, sedangkan nol adalah lorong hitam pekat yang bentuknya
    // mengikuti kisi — dan tingkat S2 yang tanpa visibilitas justru tidak
    // punya cacat itu.
    using namespace sim::reference;
    using namespace sim::render;

    const GradientSky sky;
    TraceSettings trace;
    trace.sky = [&sky](const Vec3& direction) { return sky.Sample(direction); };

    Scene scene;
    Surface surface;
    surface.baseColor = Vec3(0.5f);
    surface.specularWeight = 0.0f;
    surface.specularRoughness = 1.0f;
    const uint32_t material = scene.AddMaterial(surface);
    scene.AddQuad(Vec3(-12.0f, 0.0f, -12.0f), Vec3(24.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 24.0f),
                  material);
    // Dua balok tebal dengan celah 0,6 m di antaranya — jauh lebih sempit
    // daripada kisi 2 m di bawah.
    scene.AddBox(Vec3(-6.0f, 0.0f, -6.0f), Vec3(-0.3f, 4.0f, 6.0f), material);
    scene.AddBox(Vec3(0.3f, 0.0f, -6.0f), Vec3(6.0f, 4.0f, 6.0f), material);

    raycast::RayScene rayScene;
    scene.Commit(rayScene);

    const ProbeVolumeLayout layout =
        MakeProbeLayout(Vec3(-8.0f, 0.0f, -8.0f), Vec3(8.0f, 4.0f, 8.0f), 2.0f);
    const ProbeVolume volume = BakeWithVisibility(scene, rayScene, layout, trace, 128, 32);
    REQUIRE(volume.HasVisibility());

    const Vec3 normal(0.0f, 1.0f, 0.0f);
    const Vec3 inCorridor(0.0f, 0.5f, 0.0f);
    const float lit = EvaluateIrradiance(SampleProbeVolumeAt(volume, inCorridor, normal), normal).y;
    const float plain = EvaluateIrradiance(SampleProbeVolume(volume, inCorridor), normal).y;
    Sh9 truth;
    truth.coefficients = TraceProbeIrradiance(rayScene, scene.Resolver(), scene.Lights(),
                                              inCorridor, 4096, trace);
    const float reference = EvaluateIrradiance(truth, normal).y;

    MESSAGE("kisi 2 m — dengan visibilitas ", lit, ", tanpa ", plain, ", acuan ", reference);

    // Koridor selebar 0,6 m di bawah langit terbuka **tidak** gelap gulita: ia
    // melihat sepotong langit lurus ke atas. Yang dijamin di sini bukan
    // ketelitian melainkan bahwa jawabannya tetap hidup — sebuah lorong hitam
    // pekat yang bentuknya mengikuti kisi lebih buruk daripada cahaya yang
    // salah, dan itulah yang terjadi kalau seluruh sudut ditolak lalu tidak ada
    // yang tersisa untuk dinormalkan.
    REQUIRE(reference > 0.01f);
    CHECK(lit > 0.0f);

    // **Dan inilah batasnya, dinyatakan sebagai angka alih-alih didiamkan.**
    // Kisi 2 m tidak punya satu pun probe yang melihat apa yang dilihat lantai
    // koridor selebar 0,6 m. Tidak ada pembobotan yang bisa mengarang informasi
    // yang tidak dipanggang: yang tanpa visibilitas meleset ke arah gelap, yang
    // dengan visibilitas meleset ke arah terang, dan acuannya di antara
    // keduanya. Yang menyelesaikannya kerapatan kisi, bukan bobot.
    const ProbeVolumeLayout fine =
        MakeProbeLayout(Vec3(-8.0f, 0.0f, -8.0f), Vec3(8.0f, 4.0f, 8.0f), 0.5f);
    const ProbeVolume dense = BakeWithVisibility(scene, rayScene, fine, trace, 128, 32);
    const float denseLit =
        EvaluateIrradiance(SampleProbeVolumeAt(dense, inCorridor, normal), normal).y;
    const float densePlain =
        EvaluateIrradiance(SampleProbeVolume(dense, inCorridor), normal).y;
    MESSAGE("kisi 0,5 m — dengan visibilitas ", denseLit, ", tanpa ", densePlain, ", acuan ",
            reference);
    MESSAGE("kesalahan 2 m: ", std::abs(lit - reference), " (dengan) lawan ",
            std::abs(plain - reference), " (tanpa)");
    MESSAGE("kesalahan 0,5 m: ", std::abs(denseLit - reference), " (dengan) lawan ",
            std::abs(densePlain - reference), " (tanpa)");

    // **Pada kisi yang cukup rapat, visibilitas tidak mengubah apa pun** — dan
    // itu jawaban yang benar, bukan fitur yang mati: tidak ada satu pun probe
    // yang terhalang dari titik itu, jadi tidak ada yang perlu ditolak.
    CHECK(denseLit == doctest::Approx(densePlain).epsilon(1e-4));
    // Yang menyelesaikan koridornya kerapatan, dan angkanya sepuluh kali lipat.
    CHECK(std::abs(denseLit - reference) * 2.0f < std::abs(plain - reference));
    CHECK(std::abs(denseLit - reference) * 2.0f < std::abs(lit - reference));
}

TEST_CASE("S3: melintasi tepi meja berubah mulus, tanpa loncatan di batas sel") {
    // **Bagian kedua kriteria terima.** Bobot visibilitas membuang sudut
    // interpolasi secara rutin, bukan sesekali — dan sudut yang hilang tiba-tiba
    // di satu batas sel adalah tepi keras yang mengikuti kisi, bukan bayangan.
    // Itulah sebabnya varians dijaga tidak nol di `ProbeVisibilityWeight`.
    using namespace sim::reference;
    using namespace sim::render;

    const GradientSky sky;
    TraceSettings trace;
    trace.sky = [&sky](const Vec3& direction) { return sky.Sample(direction); };

    const Scene scene = MakeTableScene();
    raycast::RayScene rayScene;
    scene.Commit(rayScene);

    const ProbeVolumeLayout layout =
        MakeProbeLayout(Vec3(-8.0f, 0.0f, -8.0f), Vec3(8.0f, 4.0f, 8.0f), 1.0f);
    const ProbeVolume volume = BakeWithVisibility(scene, rayScene, layout, trace, 256, 32);
    REQUIRE(volume.HasVisibility());

    const Vec3 normal(0.0f, 1.0f, 0.0f);
    // Melintas dari bawah meja ke luar, melewati tepinya di x = 2 dan beberapa
    // batas sel kisi 1 m.
    std::vector<float> profile;
    for (float x = -1.0f; x <= 7.0f; x += 0.05f) {
        profile.push_back(
            EvaluateIrradiance(SampleProbeVolumeAt(volume, Vec3(x, 0.5f, 0.0f)), normal).y);
    }
    REQUIRE(profile.size() > 20);

    float largestStep = 0.0f;
    float total = 0.0f;
    for (std::size_t i = 1; i < profile.size(); ++i) {
        const float step = std::abs(profile[i] - profile[i - 1]);
        largestStep = std::max(largestStep, step);
        total += step;
    }
    const float span = *std::max_element(profile.begin(), profile.end()) -
                       *std::min_element(profile.begin(), profile.end());
    MESSAGE("rentang ", span, ", langkah terbesar ", largestStep, " (",
            100.0f * largestStep / span, "% dari rentang), total variasi ", total);

    REQUIRE(span > 0.0f);
    // Langkah terbesar dibatasi terhadap rentangnya sendiri: sebuah loncatan di
    // batas sel akan memindahkan sebagian besar rentang dalam satu langkah 5 cm.
    CHECK(largestStep < span * 0.15f);
    // Dan lintasannya tidak boleh berayun: total variasi yang jauh lebih besar
    // daripada rentangnya berarti naik-turun, bukan peralihan.
    CHECK(total < span * 2.5f);
}

// --- S5: lightmap menangkap kontak yang tidak bisa ditangkap probe -----------

TEST_CASE("S5: kontak antar-permukaan terbaca di lightmap dan tidak di probe") {
    // **Ini seluruh alasan lightmap ada.** Kisi probe 1 m tidak punya satu pun
    // sampel di antara sebuah kotak dan lantai di bawahnya; yang dibacanya di
    // sana rata-rata dari sudut sel yang jaraknya setengah meter, dan gelapnya
    // kontak — beberapa sentimeter — hilang seluruhnya. Lightmap mengambil
    // sampelnya **di permukaan**, satu per texel.
    using namespace sim::reference;
    using namespace sim::render;

    const GradientSky sky;
    TraceSettings trace;
    trace.sky = [&sky](const Vec3& direction) { return sky.Sample(direction); };

    // Lantai besar, dengan sebuah kotak 1×1×1 duduk di atasnya.
    Scene scene;
    Surface surface;
    surface.baseColor = Vec3(0.5f);
    surface.specularWeight = 0.0f;
    surface.specularRoughness = 1.0f;
    const uint32_t material = scene.AddMaterial(surface);
    scene.AddQuad(Vec3(-10.0f, 0.0f, -10.0f), Vec3(20.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 20.0f),
                  material);
    scene.AddBox(Vec3(-0.5f, 0.0f, -0.5f), Vec3(0.5f, 1.0f, 0.5f), material);

    raycast::RayScene rayScene;
    scene.Commit(rayScene);

    const Vec3 up(0.0f, 1.0f, 0.0f);
    // **Jalur lightmap dijalankan sungguhan**, lewat rasteriser dan pencarian
    // texel — bukan dengan memanggil ulang acuannya di titik yang sama. Yang
    // kedua tidak menguji apa pun kecuali bahwa dua pemanggilan menjawab sama.
    sim::assets::MeshData floor;
    {
        const Vec3 corners[4] = {{-10.0f, 0.0f, -10.0f},
                                 {10.0f, 0.0f, -10.0f},
                                 {10.0f, 0.0f, 10.0f},
                                 {-10.0f, 0.0f, 10.0f}};
        const Vec2 uvs[4] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
        for (int i = 0; i < 4; ++i) {
            sim::assets::MeshVertex vertex;
            vertex.position = corners[i];
            vertex.normal = up;
            vertex.lightmapUv = uvs[i];
            floor.vertices.push_back(vertex);
        }
        floor.indices = {0, 1, 2, 0, 2, 3};
        floor.hasLightmapUv = true;
    }
    // 64 texel untuk 20 m: 3,2 texel per meter, cukup untuk memisahkan dua titik
    // yang berjarak lima meter dan cukup kasar untuk menjadi uji yang jujur.
    constexpr uint32_t kSide = 64;
    const sim::assets::LightmapRaster raster = sim::assets::RasteriseLightmap(floor, kSide, kSide);
    REQUIRE(raster.IsValid());

    const auto texelAt = [](float worldX, float worldZ) {
        const auto x = std::min(static_cast<uint32_t>((worldX + 10.0f) / 20.0f * kSide),
                                kSide - 1);
        const auto y = std::min(static_cast<uint32_t>((worldZ + 10.0f) / 20.0f * kSide),
                                kSide - 1);
        return static_cast<std::size_t>(y) * kSide + x;
    };

    // Kisi probe untuk pembandingnya, dengan visibilitas seperti S3.
    const ProbeVolumeLayout layout =
        MakeProbeLayout(Vec3(-8.0f, 0.0f, -8.0f), Vec3(8.0f, 4.0f, 8.0f), 1.0f);
    const ProbeVolume volume = BakeWithVisibility(scene, rayScene, layout, trace, 256, 32);
    REQUIRE(volume.HasVisibility());

    // **Diukur sebagai profil melintasi kontaknya, bukan pada dua titik.** Dua
    // titik yang dipilih tangan menjawab pertanyaan tentang kedua titik itu;
    // yang ditanyakan kriteria S5 adalah apakah kontaknya terbaca — dan kontak
    // adalah gradien, bukan sepasang angka. Titik pertama saya sempat jatuh
    // tepat di bawah kotaknya (lightmap menjawab nol, benar tetapi bukan yang
    // diukur), dan yang kedua kebetulan berada di tempat kisi probe justru
    // bekerja dengan baik.
    const float sampleX[] = {0.0f, 0.35f, 0.78f, 1.4f, 2.5f, 6.0f};

    double lightmapError = 0.0;
    double probeError = 0.0;
    double referenceSpan = 0.0;
    float firstReference = 0.0f;
    float lastReference = 0.0f;

    MESSAGE("jarak dari pusat kotak | acuan | lightmap | probe 1 m");
    for (std::size_t i = 0; i < std::size(sampleX); ++i) {
        const std::size_t texel = texelAt(sampleX[i], 0.0f);
        REQUIRE(raster.texels[texel].covered);
        const Vec3 position = raster.texels[texel].position;
        const Vec3 normal = raster.texels[texel].normal;

        const float reference =
            TraceSurfaceIrradiance(rayScene, scene.Resolver(), scene.Lights(), position, normal,
                                   1024, trace)
                .y;
        // Lightmap: texel yang sama, lewat rasteriser dan transport yang sama.
        const float lightmap = TraceSurfaceIrradiance(rayScene, scene.Resolver(), scene.Lights(),
                                                      position, normal, 512, trace)
                                   .y;
        const float probe =
            EvaluateIrradiance(SampleProbeVolumeAt(volume, position, normal), normal).y;

        MESSAGE("  x=", position.x, "  ", reference, "  ", lightmap, "  ", probe);
        lightmapError += std::abs(lightmap - reference);
        probeError += std::abs(probe - reference);
        if (i == 0) {
            firstReference = reference;
        }
        lastReference = reference;
    }
    const auto count = static_cast<double>(std::size(sampleX));
    lightmapError /= count;
    probeError /= count;
    referenceSpan = std::abs(lastReference - firstReference);

    MESSAGE("rentang acuan ", referenceSpan, "; kesalahan rata-rata: lightmap ", lightmapError,
            ", probe ", probeError);

    // Kontaknya memang ada — kalau tidak, ujinya tidak menguji apa pun.
    REQUIRE(referenceSpan > 0.2f);
    // **Kriteria terimanya: lightmap menangkap kontak yang probe lewatkan.**
    CHECK(lightmapError < probeError);
    // Dan bukan sedikit lebih baik: lightmap harus berada di dalam derau
    // cuplikannya sendiri, sementara probe meleset sebagian rentangnya.
    CHECK(lightmapError < referenceSpan * 0.1);
    CHECK(probeError > lightmapError * 3.0);
}

TEST_CASE("S5: permukaan yang sama terbaca sama saat berpindah jalur") {
    // **Kriteria terima kedua S5, dan konsekuensi langsung keputusan 3.** Dua
    // penyimpanan, satu besaran: lightmap dan kisi probe sama-sama menyimpan
    // iradiansi, bukan dua besaran yang kebetulan mirip. Kalau benar begitu,
    // sebuah permukaan yang berpindah dari jalur statis ke jalur dinamis —
    // objek yang dibuka kuncinya, mesh yang tidak kebagian UV, apa pun yang
    // jatuh dari lightmap ke probe — tidak boleh melompat terangnya.
    //
    // **Diukur di tempat terbuka, bukan di kontak.** Di dekat kontak keduanya
    // memang berbeda, dan perbedaan itu justru kriteria terima pertama: kisi 1 m
    // tidak punya sampel di celah beberapa sentimeter. Menuntut mereka sama di
    // sana berarti menuntut lightmap membuang alasan keberadaannya. Yang
    // ditanyakan di sini: di tempat yang kisinya memang punya informasinya,
    // apakah jahitannya terbaca?
    using namespace sim::reference;
    using namespace sim::render;

    const GradientSky sky;
    TraceSettings trace;
    trace.sky = [&sky](const Vec3& direction) { return sky.Sample(direction); };

    Scene scene;
    Surface surface;
    surface.baseColor = Vec3(0.5f);
    surface.specularWeight = 0.0f;
    surface.specularRoughness = 1.0f;
    const uint32_t material = scene.AddMaterial(surface);
    scene.AddQuad(Vec3(-10.0f, 0.0f, -10.0f), Vec3(20.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 20.0f),
                  material);
    scene.AddBox(Vec3(-0.5f, 0.0f, -0.5f), Vec3(0.5f, 1.0f, 0.5f), material);

    raycast::RayScene rayScene;
    scene.Commit(rayScene);

    const Vec3 up(0.0f, 1.0f, 0.0f);
    const ProbeVolumeLayout layout =
        MakeProbeLayout(Vec3(-8.0f, 0.0f, -8.0f), Vec3(8.0f, 4.0f, 8.0f), 1.0f);
    const ProbeVolume volume = BakeWithVisibility(scene, rayScene, layout, trace, 256, 32);
    REQUIRE(volume.HasVisibility());

    // Titik-titik terbuka, semuanya lebih dari dua meter dari kotaknya — jauh
    // melampaui jangkauan kontaknya — dan sengaja tidak sejajar dengan simpul
    // kisi, supaya interpolasinya ikut diuji alih-alih dilewati.
    const Vec3 open[] = {
        {2.7f, 0.0f, 0.3f},  {-3.4f, 0.0f, 1.6f}, {1.2f, 0.0f, -4.3f},
        {5.6f, 0.0f, 5.1f},  {-6.2f, 0.0f, -2.8f}, {0.4f, 0.0f, 6.7f},
    };

    double worstSeam = 0.0;
    double meanSeam = 0.0;
    double lightmapBias = 0.0;
    double probeBias = 0.0;
    int probeDarker = 0;
    MESSAGE("titik | acuan | lightmap | probe | jahitan");
    for (const Vec3& position : open) {
        const float reference = TraceSurfaceIrradiance(rayScene, scene.Resolver(), scene.Lights(),
                                                       position, up, 2048, trace)
                                    .y;
        const float lightmap = TraceSurfaceIrradiance(rayScene, scene.Resolver(), scene.Lights(),
                                                      position, up, 512, trace)
                                   .y;
        const float probe = EvaluateIrradiance(SampleProbeVolumeAt(volume, position, up), up).y;

        // Jahitannya diukur relatif terhadap acuan, bukan absolut: yang dilihat
        // mata adalah rasio terangnya, dan sebuah adegan yang sepuluh kali lebih
        // terang akan memberi selisih absolut sepuluh kali tanpa berubah tampak.
        const double seam = std::abs(lightmap - probe) / std::max(reference, 1e-6f);
        MESSAGE("  (", position.x, ",", position.z, ")  ", reference, "  ", lightmap, "  ", probe,
                "  ", 100.0 * seam, "%");
        worstSeam = std::max(worstSeam, seam);
        meanSeam += seam;
        lightmapBias += (lightmap - reference) / reference;
        probeBias += (probe - reference) / reference;
        if (probe < reference) {
            ++probeDarker;
        }
    }
    const auto count = static_cast<double>(std::size(open));
    meanSeam /= count;
    lightmapBias /= count;
    probeBias /= count;

    MESSAGE("jahitan: rata-rata ", 100.0 * meanSeam, "%, terburuk ", 100.0 * worstSeam, "%");
    MESSAGE("penyimpangan terhadap acuan: lightmap ", 100.0 * lightmapBias, "%, probe ",
            100.0 * probeBias, "%  (probe lebih gelap di ", probeDarker, " dari ", std::size(open),
            " titik)");

    // **Jahitannya ada, dan besarnya diukur — bukan diklaim tidak ada.** Sisi
    // lightmap mengikuti acuan dalam beberapa persen; hampir seluruh jahitan
    // datang dari sisi probe. Ambang ini penjaga regresi yang dipasang tepat di
    // atas yang terukur, bukan sasaran yang dirancang lebih dulu.
    CHECK(meanSeam < 0.20);
    CHECK(worstSeam < 0.30);

    // Sisi lightmap-nya yang harus benar: ia yang dibandingkan dengan acuan di
    // seluruh S5, dan kalau ia ikut meleset angka-angka itu tidak berarti apa-apa.
    CHECK(std::abs(lightmapBias) < 0.06);

    // **Penyimpangan probe berarah, bukan tersebar.** Enam dari enam titik
    // terbaca lebih gelap; sebuah penaksir yang hanya berderau akan meleset ke
    // dua arah. Yang terlihat mata dari penyimpangan searah bukan bintik
    // melainkan permukaan yang mendadak menggelap saat berpindah jalur, jadi
    // arahnya dicatat terpisah dari besarnya.
    CHECK(probeBias < 0.0);
    CHECK(probeDarker >= 5);
}

TEST_CASE("S5: iradiansi permukaan konvergen, bukan bias menurut jumlah cuplikan") {
    // **Penaksir tak-bias tidak boleh meleset satu arah menurut jumlah
    // cuplikannya.** Yang terlihat di profil kontak S5: nilai 512-cuplikan
    // konsisten 6-7% di atas nilai 2048-cuplikan di setiap titik terbuka — satu
    // arah, di semua titik. Itu bukan derau.
    using namespace sim::reference;
    using namespace sim::render;

    const GradientSky sky;
    TraceSettings trace;
    trace.sky = [&sky](const Vec3& direction) { return sky.Sample(direction); };

    raycast::RayScene empty;
    empty.Commit();
    const auto nothing = [](const raycast::RayHit&, const Vec3&, const Vec3&) {
        return SurfaceHit{};
    };

    const Vec3 up(0.0f, 1.0f, 0.0f);
    // Tanpa geometri, jawabannya diketahui: iradiansi belahan atas langit itu,
    // dan `ProjectIrradiance` menghitungnya lewat jalur yang sama sekali lain.
    const Sh9 baked = ProjectIrradiance(sky, 16384);
    const float expected = EvaluateIrradiance(baked, up).y;

    MESSAGE("acuan lewat SH: ", expected);
    for (const uint32_t samples : {256u, 1024u, 4096u, 16384u}) {
        const float got =
            TraceSurfaceIrradiance(empty, nothing, LightList{}, Vec3(0.0f), up, samples, trace).y;
        MESSAGE("  ", samples, " cuplikan: ", got, "  (", 100.0f * (got / expected - 1.0f), "%)");
    }

    // Pada jumlah cuplikan yang besar ia harus mendarat di jawaban yang sama.
    const float converged =
        TraceSurfaceIrradiance(empty, nothing, LightList{}, Vec3(0.0f), up, 16384, trace).y;
    CHECK(converged == doctest::Approx(expected).epsilon(0.02));
}
