#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Volume/SdfBake.h"
#include "Sim/Volume/VolumeRaymarch.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using namespace sim;
using namespace sim::volume;

namespace {

/// Kubus sumbu-sejajar berpusat di titik asal, sisi `2 * half`. Dua belas
/// segitiga, normal menghadap keluar.
void MakeBox(float half, std::vector<Vec3>& points, std::vector<uint32_t>& indices) {
    points = {{-half, -half, -half}, {half, -half, -half}, {half, half, -half},
              {-half, half, -half},  {-half, -half, half}, {half, -half, half},
              {half, half, half},    {-half, half, half}};
    indices = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
               2, 3, 7, 2, 7, 6, 1, 2, 6, 1, 6, 5, 0, 4, 7, 0, 7, 3};
}

/// Bola UV. Dipakai karena inilah bentuk di mana hampiran kotak paling meleset:
/// di arah diagonal, kotak pembungkusnya menyentuh titik yang permukaan bolanya
/// masih jauh.
void MakeSphere(float radius, int segments, std::vector<Vec3>& points,
                std::vector<uint32_t>& indices) {
    points.clear();
    indices.clear();
    const auto pi = 3.14159265358979323846f;
    for (int y = 0; y <= segments; ++y) {
        const float v = static_cast<float>(y) / static_cast<float>(segments);
        const float theta = v * pi;
        for (int x = 0; x <= segments; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(segments);
            const float phi = u * 2.0f * pi;
            points.push_back(Vec3(radius * std::sin(theta) * std::cos(phi),
                                  radius * std::cos(theta),
                                  radius * std::sin(theta) * std::sin(phi)));
        }
    }
    const auto stride = static_cast<uint32_t>(segments + 1);
    for (uint32_t y = 0; y < static_cast<uint32_t>(segments); ++y) {
        for (uint32_t x = 0; x < static_cast<uint32_t>(segments); ++x) {
            const uint32_t a = y * stride + x;
            const uint32_t b = a + stride;
            indices.insert(indices.end(), {a, a + 1, b, b, a + 1, b + 1});
        }
    }
}

/// Jarak bertanda analitik ke kubus sumbu-sejajar — jawaban yang diketahui.
float BoxDistance(const Vec3& p, float half) {
    const Vec3 d = glm::abs(p) - Vec3(half);
    const float outside = glm::length(glm::max(d, Vec3(0.0f)));
    const float inside = std::min(std::max(d.x, std::max(d.y, d.z)), 0.0f);
    return outside + inside;
}

}  // namespace

TEST_CASE("bake tidak tersedia menolak dengan pesan, bukan grid kosong") {
    if (Available()) {
        CHECK(std::string(BackendVersion()).find("13.") != std::string::npos);
        return;
    }
    std::vector<Vec3> points;
    std::vector<uint32_t> indices;
    MakeBox(1.0f, points, indices);
    SdfGrid grid;
    const SdfBakeResult result = BakeMeshSdf(points, indices, SdfBakeSettings{}, grid);
    CHECK_FALSE(result.ok);
    // Build tanpa OpenVDB harus mengatakan apa yang hilang dan apa gantinya —
    // bukan mengembalikan grid kosong yang lalu terlihat seperti mesh tanpa
    // permukaan.
    CHECK(result.error.find("OpenVDB") != std::string::npos);
    CHECK(grid.Empty());
}

TEST_CASE("SDF kubus cocok dengan jarak analitik") {
    if (!Available()) {
        return;
    }
    std::vector<Vec3> points;
    std::vector<uint32_t> indices;
    MakeBox(1.0f, points, indices);

    SdfBakeSettings settings;
    settings.voxelSize = 0.05f;
    settings.bandVoxels = 4.0f;

    SdfGrid grid;
    const SdfBakeResult result = BakeMeshSdf(points, indices, settings, grid);
    INFO(result.error);
    REQUIRE(result.ok);
    CHECK(grid.band == doctest::Approx(0.2f));
    CHECK_FALSE(grid.Empty());

    // **Nilainya yang diperiksa, bukan bentuknya.** Grid yang dimensinya benar
    // tapi jaraknya meleset menghasilkan sphere tracing yang menembus dinding,
    // dan itu tidak muncul sebagai galat mana pun.
    //
    // Hanya titik di dalam pita yang punya jarak sejati; di luar itu nilainya
    // jenuh dengan sengaja, dan menuntutnya cocok akan menguji hal yang salah.
    int checked = 0;
    for (float x = -1.4f; x <= 1.4f; x += 0.1f) {
        for (float y = -1.4f; y <= 1.4f; y += 0.1f) {
            for (float z = -1.4f; z <= 1.4f; z += 0.1f) {
                const Vec3 p(x, y, z);
                const float expected = BoxDistance(p, 1.0f);
                if (std::abs(expected) > grid.band * 0.7f) {
                    continue;
                }
                const float actual = grid.SampleLocal(p);
                INFO("titik (" << x << "," << y << "," << z << ")");
                // Toleransi satu voxel: level set menyimpan jarak per voxel dan
                // dibaca trilinear, jadi kesalahan seukuran voxel memang bawaan
                // metodenya — yang tidak boleh adalah lebih dari itu.
                CHECK(std::abs(actual - expected) <= settings.voxelSize);
                ++checked;
            }
        }
    }
    CHECK(checked > 500);
}

TEST_CASE("SDF bola benar di arah diagonal, tempat hampiran kotak meleset") {
    if (!Available()) {
        return;
    }
    // **Inilah yang dibeli bake ini.** `BoxSceneField` mengukur jarak ke kotak
    // pembungkus; untuk bola berjari-jari 1, titik di pojok kotak itu disebut
    // berjarak 0 padahal permukaan bolanya 0,73 jauhnya. Sphere tracing yang
    // percaya angka nol berhenti melangkah dan menggambar dinding yang tidak ada.
    std::vector<Vec3> points;
    std::vector<uint32_t> indices;
    MakeSphere(1.0f, 48, points, indices);

    SdfBakeSettings settings;
    settings.voxelSize = 0.04f;
    settings.bandVoxels = 6.0f;

    SdfGrid grid;
    const SdfBakeResult result = BakeMeshSdf(points, indices, settings, grid);
    INFO(result.error);
    REQUIRE(result.ok);

    // Sepanjang diagonal, jarak sejati ke bola adalah |p| - 1.
    const Vec3 diagonal = glm::normalize(Vec3(1.0f, 1.0f, 1.0f));
    for (const float distance : {-0.15f, -0.05f, 0.05f, 0.15f}) {
        const Vec3 p = diagonal * (1.0f + distance);
        const float actual = grid.SampleLocal(p);
        INFO("jarak sejati " << distance);
        // Bola yang di-tesselasi 48 segmen sedikit lebih kecil dari bola
        // idealnya, jadi toleransinya menampung kesalahan tesselasi di samping
        // kesalahan voxel.
        CHECK(std::abs(actual - distance) <= settings.voxelSize * 2.0f);
    }

    // Dan tandanya benar di kedua sisi — ini yang membedakan medan jarak dari
    // sekadar besaran jarak.
    CHECK(grid.SampleLocal(diagonal * 0.5f) < 0.0f);
    CHECK(grid.SampleLocal(diagonal * 1.1f) > 0.0f);
}

TEST_CASE("bake menolak masukan yang tidak masuk akal alih-alih menebak") {
    std::vector<Vec3> points;
    std::vector<uint32_t> indices;
    MakeBox(1.0f, points, indices);
    SdfGrid grid;

    SUBCASE("tanpa segitiga") {
        CHECK_FALSE(BakeMeshSdf({}, {}, SdfBakeSettings{}, grid).ok);
    }

    SUBCASE("jumlah indeks bukan kelipatan tiga") {
        std::vector<uint32_t> broken{0, 1, 2, 3};
        const SdfBakeResult result = BakeMeshSdf(points, broken, SdfBakeSettings{}, grid);
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("multiple of 3") != std::string::npos);
    }

    SUBCASE("voxel nol") {
        SdfBakeSettings settings;
        settings.voxelSize = 0.0f;
        CHECK_FALSE(BakeMeshSdf(points, indices, settings, grid).ok);
    }

    SUBCASE("mesh raksasa ditolak sebelum memakan memori") {
        if (!Available()) {
            return;
        }
        // Penjaganya menghitung dari kotak batas sebelum membake. Tanpa itu,
        // mesh 200 m pada voxel 1 cm akan mencoba mengalokasikan ratusan gigabyte
        // sebelum ada yang sempat menolaknya.
        std::vector<Vec3> huge;
        std::vector<uint32_t> hugeIndices;
        MakeBox(100.0f, huge, hugeIndices);
        SdfBakeSettings settings;
        settings.voxelSize = 0.01f;
        const SdfBakeResult result = BakeMeshSdf(huge, hugeIndices, settings, grid);
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("coarser voxel size") != std::string::npos);
    }

    SUBCASE("indeks di luar jangkauan") {
        std::vector<uint32_t> broken{0, 1, 999};
        const SdfBakeResult result = BakeMeshSdf(points, broken, SdfBakeSettings{}, grid);
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("out of range") != std::string::npos);
    }
}

// --- V2a: impor .vdb ---------------------------------------------------------

namespace {

std::filesystem::path VolumeDir() { return std::filesystem::path(SIM_VOLUME_DIR); }

/// Isi fixture-nya, sebagai rumus.
///
/// `sphere-fog.vdb` diisi fungsi analitik, bukan hasil simulasi:
///   density(p)     = max(0, 1 - |p|)  untuk |p| <= 1
///   temperature(p) = |p|              untuk |p| <= 1
///
/// **Berkasnya memang ditulis OpenVDB** — format VDB terlalu rumit untuk
/// disusun tangan seperti yang dilakukan fixture EXR dan TIFF. Yang menjaga uji
/// ini tetap berarti adalah bahwa yang dibandingkan tetap rumus di atas, bukan
/// keluaran pembacanya sendiri: pembaca yang salah menempatkan titik asal atau
/// salah menskalakan voxel akan meleset dari rumus ini walau berkasnya utuh.
float ExpectedDensity(const Vec3& p) {
    const float r = glm::length(p);
    return r <= 1.0f ? 1.0f - r : 0.0f;
}

}  // namespace

TEST_CASE("daftar format volume mengikuti backend yang terbangun") {
    const auto readable = ReadableExtensions();
    if (Available()) {
        REQUIRE(readable.size() == 1);
        CHECK(readable[0] == ".vdb");
        CHECK(CanRead(".vdb"));
        // Besar-kecil huruf tidak membedakan; berkas dari Windows kerap `.VDB`.
        CHECK(CanRead(".VDB"));
    } else {
        // Tanpa OpenVDB daftarnya kosong — dan itu yang membuat Asset Browser
        // tidak menawarkan impor untuk berkas yang tidak bisa dibacanya.
        CHECK(readable.empty());
        CHECK_FALSE(CanRead(".vdb"));
    }
    CHECK_FALSE(CanRead(".png"));
}

TEST_CASE("grid di dalam berkas .vdb bisa didaftar") {
    if (!Available()) {
        return;
    }
    std::vector<std::string> names;
    const SdfBakeResult result = ListVdbGrids(VolumeDir() / "sphere-fog.vdb", names);
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(names.size() == 2);
    // Panel impor menawarkan pilihan alih-alih menebak: berkas asap sungguhan
    // membawa density, temperature, dan velocity sekaligus.
    CHECK(std::find(names.begin(), names.end(), "density") != names.end());
    CHECK(std::find(names.begin(), names.end(), "temperature") != names.end());
}

TEST_CASE("nilai .vdb cocok dengan fungsi yang mengisinya") {
    if (!Available()) {
        return;
    }
    VdbLoadSettings settings;
    settings.gridName = "density";

    VolumeGrid grid;
    const SdfBakeResult result = LoadVdb(VolumeDir() / "sphere-fog.vdb", settings, grid);
    INFO(result.error);
    REQUIRE(result.ok);

    CHECK(grid.name == "density");
    CHECK(grid.voxelSize == doctest::Approx(0.1f));
    CHECK(grid.background == doctest::Approx(0.0f));
    CHECK_FALSE(grid.Empty());
    // Bola berjari-jari 1 pada voxel 0,1 menempati sekitar 21 voxel per sisi.
    CHECK(grid.sizeX >= 20);
    CHECK(grid.sizeX <= 23);
    CHECK(grid.sizeX == grid.sizeY);
    CHECK(grid.sizeY == grid.sizeZ);

    // Puncaknya di pusat bernilai 1, tepinya nol.
    CHECK(grid.maxValue == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(grid.minValue == doctest::Approx(0.0f).epsilon(0.02));

    // **Nilainya yang diperiksa terhadap rumusnya**, di pusat voxel supaya yang
    // diuji bukan interpolasinya. Titik asal yang tergeser satu voxel, atau
    // ukuran voxel yang salah, akan meleset di sini walau bentuknya benar.
    int checked = 0;
    for (uint32_t z = 2; z < grid.sizeZ - 2; z += 3) {
        for (uint32_t y = 2; y < grid.sizeY - 2; y += 3) {
            for (uint32_t x = 2; x < grid.sizeX - 2; x += 3) {
                const Vec3 p = grid.origin + Vec3(static_cast<float>(x), static_cast<float>(y),
                                                  static_cast<float>(z)) *
                                                 grid.voxelSize;
                INFO("voxel (" << x << "," << y << "," << z << ")");
                CHECK(grid.SampleLocal(p) == doctest::Approx(ExpectedDensity(p)).epsilon(0.02));
                ++checked;
            }
        }
    }
    CHECK(checked > 100);

    // Di luar bola nilainya nol — dan itu yang membuat raymarch bisa berhenti
    // lebih awal alih-alih menjejaki kabut yang tidak ada.
    CHECK(grid.SampleLocal(Vec3(5.0f, 0.0f, 0.0f)) == doctest::Approx(0.0f));
}

TEST_CASE("grid dipilih menurut namanya, bukan urutannya") {
    if (!Available()) {
        return;
    }
    VdbLoadSettings settings;
    settings.gridName = "temperature";

    VolumeGrid grid;
    REQUIRE(LoadVdb(VolumeDir() / "sphere-fog.vdb", settings, grid).ok);
    CHECK(grid.name == "temperature");

    // temperature(p) = |p| — kebalikan bentuk density, jadi keliru memilih grid
    // akan terlihat seketika alih-alih lolos sebagai angka yang mirip.
    const Vec3 p = grid.origin + Vec3(static_cast<float>(grid.sizeX / 2)) * grid.voxelSize;
    CHECK(grid.SampleLocal(p) == doctest::Approx(glm::length(p)).epsilon(0.05));
    CHECK(grid.maxValue > 0.9f);
}

TEST_CASE("berkas volume yang bermasalah ditolak dengan pesan yang berguna") {
    VolumeGrid grid;

    SUBCASE("berkas tidak ada") {
        const SdfBakeResult result = LoadVdb(VolumeDir() / "hilang.vdb", VdbLoadSettings{}, grid);
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("hilang.vdb") != std::string::npos);
    }

    SUBCASE("nama grid yang tidak ada menyebut yang ada") {
        if (!Available()) {
            return;
        }
        VdbLoadSettings settings;
        settings.gridName = "smoke";
        const SdfBakeResult result = LoadVdb(VolumeDir() / "sphere-fog.vdb", settings, grid);
        CHECK_FALSE(result.ok);
        // Menyebutkan yang ada, bukan hanya yang tidak ada: yang dicari biasanya
        // salah satu dari daftar itu dengan nama yang sedikit berbeda.
        CHECK(result.error.find("density") != std::string::npos);
        CHECK(result.error.find("temperature") != std::string::npos);
    }

    SUBCASE("volume yang terlalu besar ditolak sebelum dipadatkan") {
        if (!Available()) {
            return;
        }
        VdbLoadSettings settings;
        settings.maxVoxels = 100;  // fixture-nya sekitar 21³ = 9261
        const SdfBakeResult result = LoadVdb(VolumeDir() / "sphere-fog.vdb", settings, grid);
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("over the limit") != std::string::npos);
    }
}

// --- V2a: acuan raymarch CPU -------------------------------------------------

namespace {

/// Kubus kerapatan seragam, untuk membandingkan dengan jawaban analitik.
VolumeGrid MakeUniformGrid(float density, uint32_t side, float voxelSize) {
    VolumeGrid grid;
    grid.name = "uniform";
    grid.voxelSize = voxelSize;
    grid.background = 0.0f;
    grid.sizeX = side;
    grid.sizeY = side;
    grid.sizeZ = side;
    grid.origin = Vec3(-static_cast<float>(side - 1) * 0.5f * voxelSize);
    grid.values.assign(grid.VoxelCount(), density);
    grid.minValue = density;
    grid.maxValue = density;
    return grid;
}

}  // namespace

TEST_CASE("ray yang meleset dari volume tidak menjejaki apa pun") {
    const VolumeGrid grid = MakeUniformGrid(1.0f, 20, 0.1f);
    RaymarchSettings settings;

    // Lewat jauh di samping kubusnya.
    const RaymarchResult result =
        Raymarch(grid, Vec3(-10.0f, 5.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), settings);

    // Latar terlihat utuh, dan **tidak satu langkah pun diambil**. Inilah yang
    // membuat volume murah: kebanyakan ray memang tidak menyentuhnya.
    CHECK(result.transmittance == doctest::Approx(1.0f));
    CHECK(result.steps == 0);
    CHECK(glm::length(result.scattered) == doctest::Approx(0.0f));
}

TEST_CASE("medium seragam mengikuti Beer-Lambert") {
    // **Jawaban analitiknya diketahui**: T = exp(-sigma * L). Uji ini yang
    // membedakan integrasi yang benar dari integrasi yang kebetulan
    // menghasilkan gambar yang masuk akal.
    const VolumeGrid grid = MakeUniformGrid(1.0f, 41, 0.1f);  // kubus 4 m
    RaymarchSettings settings;
    settings.extinction = 1.0f;
    settings.stepSize = 0.05f;
    settings.scatterAlbedo = Vec3(0.0f);  // penyerapan murni; hamburan diuji terpisah
    settings.minTransmittance = 0.0f;     // tanpa penghentian dini
    settings.maxSteps = 40;               // 40 * 0,05 = 2 m, seluruhnya di dalam kubus

    // Bermula di dalam kubus supaya seluruh jalurnya melewati kerapatan 1
    // penuh — tanpa landai setengah voxel di tepinya.
    const RaymarchResult result =
        Raymarch(grid, Vec3(-1.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), settings);

    CHECK(result.steps == 40);
    CHECK(result.transmittance == doctest::Approx(std::exp(-2.0f)).epsilon(0.01));
}

TEST_CASE("hasilnya tidak bergantung pada besar langkah") {
    // **Sifat yang paling mudah hilang, dan paling mahal ketika hilang.**
    // Integrasi naif (`radiance * step`) membuat hasilnya berubah bersama besar
    // langkah — sehingga menaikkan kualitas mengubah kecerahan seluruh adegan,
    // dan setiap preset kualitas harus dikalibrasi ulang sendiri-sendiri.
    const VolumeGrid grid = MakeUniformGrid(0.8f, 41, 0.1f);

    RaymarchSettings coarse;
    coarse.stepSize = 0.1f;
    coarse.minTransmittance = 0.0f;
    coarse.maxSteps = 2048;

    RaymarchSettings fine = coarse;
    fine.stepSize = 0.0125f;  // delapan kali lebih halus

    const Vec3 from(-4.0f, 0.0f, 0.0f);
    const Vec3 direction(1.0f, 0.0f, 0.0f);
    const RaymarchResult a = Raymarch(grid, from, direction, coarse);
    const RaymarchResult b = Raymarch(grid, from, direction, fine);

    INFO("kasar T=" << a.transmittance << " halus T=" << b.transmittance);
    CHECK(a.transmittance == doctest::Approx(b.transmittance).epsilon(0.02));
    // Dan cahaya terhamburnya juga, yang justru bagian yang paling mudah
    // bergeser bersama besar langkah.
    CHECK(a.scattered.x == doctest::Approx(b.scattered.x).epsilon(0.03));
    CHECK(a.scattered.y == doctest::Approx(b.scattered.y).epsilon(0.03));
}

TEST_CASE("penjejakan berhenti begitu latar tidak lagi terlihat") {
    const VolumeGrid grid = MakeUniformGrid(5.0f, 81, 0.1f);
    RaymarchSettings settings;
    settings.extinction = 4.0f;
    settings.stepSize = 0.05f;
    settings.maxSteps = 4096;
    settings.minTransmittance = 0.01f;

    const RaymarchResult result =
        Raymarch(grid, Vec3(-4.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), settings);

    CHECK(result.transmittance < settings.minTransmittance);
    // Medium setebal ini menjadi buram jauh sebelum ujung kubusnya; menjejaki
    // sisanya hanya membayar langkah yang tidak mengubah satu piksel pun.
    CHECK(result.steps < 200);
}

TEST_CASE("volume yang lebih tebal lebih menghalangi, dan energinya tidak dikarang") {
    RaymarchSettings settings;
    settings.stepSize = 0.05f;
    settings.minTransmittance = 0.0f;
    settings.maxSteps = 4096;

    float previous = 1.0f;
    for (const float density : {0.25f, 0.5f, 1.0f, 2.0f}) {
        const VolumeGrid grid = MakeUniformGrid(density, 41, 0.1f);
        const RaymarchResult result =
            Raymarch(grid, Vec3(-4.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), settings);
        INFO("kerapatan " << density);
        CHECK(result.transmittance < previous);
        previous = result.transmittance;

        // Albedo 0,8 berarti sebagian energi terserap; yang keluar tidak pernah
        // melebihi yang masuk. Integrasi yang salah tanda atau salah bagi akan
        // melanggar ini jauh sebelum ia terlihat sebagai gambar yang aneh.
        CHECK(result.scattered.x <= 1.0f + 1e-4f);
        CHECK(result.scattered.x >= 0.0f);
        CHECK(result.scattered.x + result.transmittance <= 1.0f + 1e-4f);
    }
}

TEST_CASE("asap dari berkas dijejaki dan menghalangi paling banyak di tengahnya") {
    if (!Available()) {
        return;
    }
    VdbLoadSettings load;
    load.gridName = "density";
    VolumeGrid grid;
    REQUIRE(LoadVdb(VolumeDir() / "sphere-fog.vdb", load, grid).ok);

    RaymarchSettings settings;
    settings.extinction = 8.0f;
    settings.stepSize = 0.01f;
    settings.minTransmittance = 0.0f;
    settings.maxSteps = 4096;

    // Melalui pusat bola: jalur terpanjang lewat kerapatan tertinggi.
    const RaymarchResult centre =
        Raymarch(grid, Vec3(-3.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), settings);
    // Menyerempet tepinya: jalur pendek lewat kerapatan yang hampir nol.
    const RaymarchResult edge =
        Raymarch(grid, Vec3(-3.0f, 0.95f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), settings);

    INFO("tengah T=" << centre.transmittance << " tepi T=" << edge.transmittance);
    CHECK(centre.transmittance < edge.transmittance);
    // Lewat pusat: kerapatan puncak 1 sepanjang dua meter — nyaris buram.
    CHECK(centre.transmittance < 0.01f);
    // Menyerempet di y = 0,95: talinya cuma 0,62 m dan seluruhnya melewati
    // pinggiran tempat kerapatannya di bawah 0,05, jadi ia sebagian besar tetap
    // tembus pandang. Ambangnya 0,7 dan bukan 0,9 karena begitulah hitungannya
    // — bukan karena hasilnya dilonggarkan sampai lulus.
    CHECK(edge.transmittance > 0.7f);
    // Dan yang menyerempet tetap menjejaki sesuatu — bukan meleset sama sekali.
    CHECK(edge.steps > 0);
}
