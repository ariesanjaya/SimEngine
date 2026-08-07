#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Render/FrameGraph.h"
#include "Sim/Render/Frustum.h"
#include "Sim/Render/Ibl.h"
#include "Sim/Render/LightCluster.h"
#include "Sim/Render/ShadowCascades.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace sim;
using namespace sim::render;

namespace {

ResourceDesc Colour(uint32_t width, uint32_t height, uint32_t format = 1) {
    ResourceDesc desc;
    desc.kind = ResourceKind::Texture;
    desc.width = width;
    desc.height = height;
    desc.format = format;
    return desc;
}

/// Urutan nama pass yang benar-benar dijalankan.
std::vector<std::string> Order(const FrameGraph& graph, const CompiledGraph& compiled) {
    std::vector<std::string> names;
    for (const CompiledPass& pass : compiled.order) {
        names.emplace_back(graph.PassName(pass.pass));
    }
    return names;
}

bool HasBarrier(const CompiledPass& pass, ResourceId resource, Access from, Access to) {
    return std::any_of(pass.barriers.begin(), pass.barriers.end(), [&](const Barrier& barrier) {
        return barrier.resource == resource && barrier.from == from && barrier.to == to;
    });
}

Aabb Box(const Vec3& centre, float half) {
    return Aabb{centre - Vec3(half), centre + Vec3(half)};
}

}  // namespace

// --- frame graph: pembuangan --------------------------------------------------

TEST_CASE("Pass yang hasilnya tidak dibaca siapa pun dibuang") {
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const ResourceId shadow = graph.CreateTexture("shadow", Colour(1024, 1024));
    const ResourceId ssao = graph.CreateTexture("ssao", Colour(640, 360));

    const PassId shadowPass = graph.AddPass("shadow");
    graph.Write(shadowPass, shadow, Access::DepthWrite);

    // SSAO ditulis tapi tidak ada yang membacanya — persis bentuk sebuah fitur
    // yang sedang dimatikan.
    const PassId ssaoPass = graph.AddPass("ssao");
    graph.Write(ssaoPass, ssao, Access::ColorWrite);

    const PassId opaque = graph.AddPass("opaque");
    graph.Read(opaque, shadow, Access::ShaderRead);
    graph.Write(opaque, target, Access::ColorWrite);

    graph.SetOutput(target, Access::Present);
    const CompiledGraph compiled = graph.Compile();

    REQUIRE(compiled.ok);
    CHECK(Order(graph, compiled) == std::vector<std::string>{"shadow", "opaque"});
    REQUIRE(compiled.culled.size() == 1);
    CHECK(compiled.culled[0] == ssaoPass);
}

TEST_CASE("Pembuangan menular ke belakang lewat rantai") {
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const ResourceId a = graph.CreateTexture("a", Colour(64, 64));
    const ResourceId b = graph.CreateTexture("b", Colour(64, 64));

    const PassId first = graph.AddPass("writes-a");
    graph.Write(first, a, Access::ColorWrite);
    const PassId second = graph.AddPass("a-to-b");
    graph.Read(second, a, Access::ShaderRead);
    graph.Write(second, b, Access::ColorWrite);
    const PassId third = graph.AddPass("target");
    graph.Write(third, target, Access::ColorWrite);

    graph.SetOutput(target, Access::Present);
    const CompiledGraph compiled = graph.Compile();

    REQUIRE(compiled.ok);
    // Tidak ada yang membaca b, jadi `a-to-b` mati; dan begitu ia mati, tidak ada
    // lagi yang membaca a, jadi `writes-a` ikut mati. Rantainya harus ikut runtuh
    // seluruhnya — kalau hanya satu tingkat, fitur yang dimatikan tetap membayar
    // pass yang menyiapkannya.
    CHECK(Order(graph, compiled) == std::vector<std::string>{"target"});
    CHECK(compiled.culled.size() == 2);
}

TEST_CASE("Pass efek samping tidak pernah dibuang") {
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const ResourceId scratch = graph.CreateTexture("scratch", Colour(16, 16));

    const PassId readback = graph.AddPass("readback");
    graph.Write(readback, scratch, Access::TransferWrite);
    graph.SetSideEffect(readback);

    const PassId draw = graph.AddPass("draw");
    graph.Write(draw, target, Access::ColorWrite);
    graph.SetOutput(target, Access::Present);

    const CompiledGraph compiled = graph.Compile();
    REQUIRE(compiled.ok);
    CHECK(compiled.culled.empty());
    CHECK(Order(graph, compiled).size() == 2);
}

TEST_CASE("Efek samping menghidupkan pass yang memasoknya") {
    FrameGraph graph;
    const ResourceId source = graph.CreateTexture("source", Colour(16, 16));
    const ResourceId dump = graph.CreateTexture("dump", Colour(16, 16));

    const PassId fill = graph.AddPass("fill");
    graph.Write(fill, source, Access::ColorWrite);
    const PassId copy = graph.AddPass("copy");
    graph.Read(copy, source, Access::TransferRead);
    graph.Write(copy, dump, Access::TransferWrite);
    graph.SetSideEffect(copy);

    const CompiledGraph compiled = graph.Compile();
    REQUIRE(compiled.ok);
    CHECK(Order(graph, compiled) == std::vector<std::string>{"fill", "copy"});
}

// --- frame graph: pemeriksaan --------------------------------------------------

TEST_CASE("Membaca resource yang tidak pernah ditulis adalah galat") {
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const ResourceId ghost = graph.CreateTexture("ghost", Colour(8, 8));

    const PassId pass = graph.AddPass("draw");
    graph.Read(pass, ghost, Access::ShaderRead);
    graph.Write(pass, target, Access::ColorWrite);
    graph.SetOutput(target, Access::Present);

    const CompiledGraph compiled = graph.Compile();
    CHECK(!compiled.ok);
    CHECK(compiled.error.find("ghost") != std::string::npos);
    CHECK(compiled.error.find("draw") != std::string::npos);
}

TEST_CASE("Membaca sebelum penulisnya adalah galat") {
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const ResourceId buffer = graph.CreateTexture("gbuffer", Colour(64, 64));

    const PassId reader = graph.AddPass("lighting");
    graph.Read(reader, buffer, Access::ShaderRead);
    graph.Write(reader, target, Access::ColorWrite);
    const PassId writer = graph.AddPass("gbuffer");
    graph.Write(writer, buffer, Access::ColorWrite);
    graph.SetOutput(target, Access::Present);

    const CompiledGraph compiled = graph.Compile();
    CHECK(!compiled.ok);
    CHECK(compiled.error.find("before anything writes it") != std::string::npos);
}

TEST_CASE("Membaca resource impor tidak menuntut penulis") {
    // Resource impor sudah berisi sesuatu sebelum graph mulai — itulah gunanya.
    FrameGraph graph;
    const ResourceId environment = graph.Import("envmap", Access::ShaderRead);
    const ResourceId target = graph.Import("target", Access::None);

    const PassId pass = graph.AddPass("sky");
    graph.Read(pass, environment, Access::ShaderRead);
    graph.Write(pass, target, Access::ColorWrite);
    graph.SetOutput(target, Access::Present);

    const CompiledGraph compiled = graph.Compile();
    CHECK(compiled.ok);
}

// --- frame graph: barrier ------------------------------------------------------

TEST_CASE("Barrier hanya muncul saat keadaan benar-benar berpindah") {
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const ResourceId depth = graph.CreateTexture("depth", Colour(800, 600, 2));

    const PassId prepass = graph.AddPass("depth-prepass");
    graph.Write(prepass, depth, Access::DepthWrite);

    const PassId opaque = graph.AddPass("opaque");
    graph.Read(opaque, depth, Access::DepthRead);
    graph.Write(opaque, target, Access::ColorWrite);

    // Dua pembacaan berurutan dengan cara yang sama. Yang kedua tidak boleh
    // menghasilkan barrier apa pun — memancarkannya memang benar, tapi ia
    // memaksa GPU menjalankan keduanya berurutan tanpa ada yang menuntutnya.
    const PassId decals = graph.AddPass("decals");
    graph.Read(decals, depth, Access::DepthRead);
    graph.Write(decals, target, Access::ColorWrite);

    graph.SetOutput(target, Access::Present);
    const CompiledGraph compiled = graph.Compile();
    REQUIRE(compiled.ok);
    REQUIRE(compiled.order.size() == 3);

    CHECK(HasBarrier(compiled.order[0], depth, Access::None, Access::DepthWrite));
    CHECK(HasBarrier(compiled.order[1], depth, Access::DepthWrite, Access::DepthRead));
    CHECK(compiled.order[2].barriers.empty());
}

TEST_CASE("Keluaran dikembalikan ke keadaan yang dijanjikannya") {
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const PassId pass = graph.AddPass("draw");
    graph.Write(pass, target, Access::ColorWrite);
    graph.SetOutput(target, Access::Present);

    const CompiledGraph compiled = graph.Compile();
    REQUIRE(compiled.ok);
    REQUIRE(compiled.order.size() == 1);
    // Dan ia harus SESUDAH pass terakhir, bukan sebelumnya: ditempelkan ke
    // barrier pass itu sendiri, transisi ini berjalan sebelum pass menulis, dan
    // yang diserahkan ke ImGui adalah target yang belum digambar.
    CHECK(!HasBarrier(compiled.order[0], target, Access::ColorWrite, Access::Present));
    REQUIRE(compiled.finalBarriers.size() == 1);
    CHECK(compiled.finalBarriers[0].resource == target);
    CHECK(compiled.finalBarriers[0].from == Access::ColorWrite);
    CHECK(compiled.finalBarriers[0].to == Access::Present);
}

TEST_CASE("Resource impor mulai dari keadaan yang disebutkan pemanggil") {
    FrameGraph graph;
    const ResourceId environment = graph.Import("envmap", Access::ShaderRead);
    const ResourceId target = graph.Import("target", Access::None);
    const PassId pass = graph.AddPass("sky");
    graph.Read(pass, environment, Access::ShaderRead);
    graph.Write(pass, target, Access::ColorWrite);
    graph.SetOutput(target, Access::Present);

    const CompiledGraph compiled = graph.Compile();
    REQUIRE(compiled.ok);
    // Sudah dalam keadaan yang diminta, jadi tidak ada barrier untuknya.
    for (const Barrier& barrier : compiled.order[0].barriers) {
        CHECK(barrier.resource != environment);
    }
}

// --- frame graph: alias memori -------------------------------------------------

TEST_CASE("Dua transien yang umurnya tidak bertumpang tindih berbagi memori") {
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const ResourceId blurA = graph.CreateTexture("blurA", Colour(640, 360));
    const ResourceId blurB = graph.CreateTexture("blurB", Colour(640, 360));
    const ResourceId blurC = graph.CreateTexture("blurC", Colour(640, 360));

    const PassId first = graph.AddPass("h-blur");
    graph.Write(first, blurA, Access::ColorWrite);
    const PassId second = graph.AddPass("v-blur");
    graph.Read(second, blurA, Access::ShaderRead);
    graph.Write(second, blurB, Access::ColorWrite);
    const PassId third = graph.AddPass("downsample");
    graph.Read(third, blurB, Access::ShaderRead);
    graph.Write(third, blurC, Access::ColorWrite);
    const PassId composite = graph.AddPass("composite");
    graph.Read(composite, blurC, Access::ShaderRead);
    graph.Write(composite, target, Access::ColorWrite);
    graph.SetOutput(target, Access::Present);

    const CompiledGraph compiled = graph.Compile();
    REQUIRE(compiled.ok);
    // A mati sesudah pass kedua, jadi C boleh memakai slotnya. Tiga tekstur
    // seukuran layar penuh hanya menuntut dua.
    CHECK(compiled.slotCount == 2);
    CHECK(compiled.memorySlot[blurA] == compiled.memorySlot[blurC]);
    CHECK(compiled.memorySlot[blurA] != compiled.memorySlot[blurB]);
}

TEST_CASE("Transien berbeda bentuk tidak pernah berbagi memori") {
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const ResourceId big = graph.CreateTexture("big", Colour(1920, 1080));
    const ResourceId small = graph.CreateTexture("small", Colour(320, 180));

    const PassId first = graph.AddPass("a");
    graph.Write(first, big, Access::ColorWrite);
    const PassId second = graph.AddPass("b");
    graph.Read(second, big, Access::ShaderRead);
    graph.Write(second, small, Access::ColorWrite);
    const PassId third = graph.AddPass("c");
    graph.Read(third, small, Access::ShaderRead);
    graph.Write(third, target, Access::ColorWrite);
    graph.SetOutput(target, Access::Present);

    const CompiledGraph compiled = graph.Compile();
    REQUIRE(compiled.ok);
    CHECK(compiled.slotCount == 2);
    CHECK(compiled.memorySlot[big] != compiled.memorySlot[small]);
}

TEST_CASE("Keluaran tidak pernah dialias-kan") {
    // Isinya masih dibaca sesudah graph selesai, jadi slot yang bebas di dalam
    // graph belum tentu bebas di luarnya.
    FrameGraph graph;
    const ResourceId result = graph.CreateTexture("result", Colour(256, 256));
    const ResourceId scratch = graph.CreateTexture("scratch", Colour(256, 256));

    const PassId first = graph.AddPass("draw");
    graph.Write(first, result, Access::ColorWrite);
    const PassId second = graph.AddPass("post");
    graph.Read(second, result, Access::ShaderRead);
    graph.Write(second, scratch, Access::ColorWrite);
    const PassId third = graph.AddPass("back");
    graph.Read(third, scratch, Access::ShaderRead);
    graph.Write(third, result, Access::ColorWrite);
    graph.SetOutput(result, Access::Present);

    const CompiledGraph compiled = graph.Compile();
    REQUIRE(compiled.ok);
    CHECK(compiled.memorySlot[result] != compiled.memorySlot[scratch]);
}

TEST_CASE("Resource impor tidak diberi slot memori") {
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const PassId pass = graph.AddPass("draw");
    graph.Write(pass, target, Access::ColorWrite);
    graph.SetOutput(target, Access::Present);

    const CompiledGraph compiled = graph.Compile();
    REQUIRE(compiled.ok);
    CHECK(compiled.memorySlot[target] == kInvalidResource);
    CHECK(compiled.slotCount == 0);
}

TEST_CASE("Graph yang sama dikompilasi dua kali menghasilkan pembagian memori yang sama") {
    // Kolam yang membagikan apa pun yang sedang bebas bergantung pada urutan
    // permintaan; selang umur tidak. "Kadang kehabisan memori" adalah bug yang
    // paling mahal dicari, dan ini yang menutupnya.
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const ResourceId a = graph.CreateTexture("a", Colour(128, 128));
    const ResourceId b = graph.CreateTexture("b", Colour(128, 128));
    const PassId first = graph.AddPass("a");
    graph.Write(first, a, Access::ColorWrite);
    const PassId second = graph.AddPass("b");
    graph.Read(second, a, Access::ShaderRead);
    graph.Write(second, b, Access::ColorWrite);
    const PassId third = graph.AddPass("c");
    graph.Read(third, b, Access::ShaderRead);
    graph.Write(third, target, Access::ColorWrite);
    graph.SetOutput(target, Access::Present);

    const CompiledGraph first_compile = graph.Compile();
    const CompiledGraph second_compile = graph.Compile();
    REQUIRE(first_compile.ok);
    REQUIRE(second_compile.ok);
    CHECK(first_compile.memorySlot == second_compile.memorySlot);
    CHECK(first_compile.slotCount == second_compile.slotCount);
}

// --- frustum -------------------------------------------------------------------

TEST_CASE("Frustum memuat yang di depan dan membuang yang di belakang") {
    const Mat4 view = LookAt(Vec3(0.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, -1.0f));
    const Mat4 proj = PerspectiveReversedZ(60.0f * kDegToRad, 16.0f / 9.0f, 0.1f, 100.0f);
    const Frustum frustum(proj * view);

    CHECK(frustum.Contains(Vec3(0.0f, 0.0f, -10.0f)));
    CHECK(!frustum.Contains(Vec3(0.0f, 0.0f, 10.0f)));    // di belakang
    CHECK(!frustum.Contains(Vec3(0.0f, 0.0f, -1000.0f))); // di luar bidang jauh
    CHECK(!frustum.Contains(Vec3(500.0f, 0.0f, -10.0f))); // jauh di samping
}

TEST_CASE("Frustum reversed-Z membatasi volume yang sama dengan yang biasa") {
    // Yang berubah pada reversed-Z adalah isi matriksnya; volume yang dibatasi
    // keenam bidangnya tetap sama. Kalau tidak, mengganti proyeksi akan
    // mengubah objek mana yang terlihat — dan itu bukan perubahan presisi
    // melainkan perubahan gambar.
    const Mat4 view = LookAt(Vec3(2.0f, 3.0f, 6.0f), Vec3(0.0f, 0.0f, 0.0f));
    const Mat4 standard = Perspective(50.0f * kDegToRad, 4.0f / 3.0f, 0.1f, 200.0f);
    const Mat4 reversed = PerspectiveReversedZ(50.0f * kDegToRad, 4.0f / 3.0f, 0.1f, 200.0f);
    const Frustum a(standard * view);
    const Frustum b(reversed * view);

    int agree = 0;
    int total = 0;
    for (int x = -60; x <= 60; x += 7) {
        for (int y = -60; y <= 60; y += 7) {
            for (int z = -60; z <= 60; z += 7) {
                const Vec3 point(static_cast<float>(x), static_cast<float>(y),
                                 static_cast<float>(z));
                ++total;
                if (a.Contains(point) == b.Contains(point)) {
                    ++agree;
                }
            }
        }
    }
    CHECK(agree == total);
}

TEST_CASE("Reversed-Z menaruh presisi di tempat yang membutuhkannya") {
    const Mat4 standard = Perspective(60.0f * kDegToRad, 1.0f, 0.1f, 1000.0f);
    const Mat4 reversed = PerspectiveReversedZ(60.0f * kDegToRad, 1.0f, 0.1f, 1000.0f);
    const auto depthAt = [](const Mat4& proj, float distance) {
        const Vec4 clip = proj * Vec4(0.0f, 0.0f, -distance, 1.0f);
        return clip.z / clip.w;
    };

    // Batasnya: near dan far saling tukar.
    CHECK(depthAt(standard, 0.1f) == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(depthAt(standard, 1000.0f) == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(depthAt(reversed, 0.1f) == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(depthAt(reversed, 1000.0f) == doctest::Approx(0.0f).epsilon(0.001));

    // Dan inilah gunanya: pada jarak jauh, dua objek yang terpisah satu meter
    // memisah jauh lebih lebar dalam nilai depth reversed-Z. Float menyimpan
    // angka rapat di dekat nol, dan di sanalah reversed-Z menaruh yang jauh.
    const float standardGap = std::abs(depthAt(standard, 500.0f) - depthAt(standard, 501.0f));
    const float reversedGap = std::abs(depthAt(reversed, 500.0f) - depthAt(reversed, 501.0f));
    CHECK(reversedGap == doctest::Approx(standardGap).epsilon(0.01));
    // Jaraknya sama dalam nilai mutlak — yang berbeda adalah *di mana* nilai itu
    // jatuh. Reversed-Z menaruhnya di dekat nol, tempat float paling rapat.
    CHECK(depthAt(reversed, 500.0f) < 0.01f);
    CHECK(depthAt(standard, 500.0f) > 0.99f);
}

TEST_CASE("Bidang jauh tak hingga tetap membuang yang di belakang kamera") {
    const Mat4 view = LookAt(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f));
    const Frustum frustum(PerspectiveReversedZInfinite(60.0f * kDegToRad, 1.0f, 0.1f) * view);
    CHECK(frustum.Contains(Vec3(0.0f, 0.0f, -5.0f)));
    CHECK(frustum.Contains(Vec3(0.0f, 0.0f, -50000.0f)));  // tidak ada bidang jauh
    CHECK(!frustum.Contains(Vec3(0.0f, 0.0f, 5.0f)));
}

TEST_CASE("Kotak yang hanya menyentuh tepi tetap dianggap terlihat") {
    const Mat4 view = LookAt(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f));
    const Frustum frustum(PerspectiveReversedZ(90.0f * kDegToRad, 1.0f, 0.1f, 100.0f) * view);

    // Pusatnya di luar kerucut pandang, tapi sebagian kotaknya masuk. Membuangnya
    // berarti objek yang terlihat separuh menghilang di tepi layar.
    CHECK(!frustum.Contains(Vec3(12.0f, 0.0f, -10.0f)));
    CHECK(frustum.Intersects(Box(Vec3(12.0f, 0.0f, -10.0f), 3.0f)));
    // Yang benar-benar jauh tetap dibuang.
    CHECK(!frustum.Intersects(Box(Vec3(60.0f, 0.0f, -10.0f), 3.0f)));
}

TEST_CASE("Kotak yang diputar diuji lewat kotak dunia yang memuatnya") {
    const Aabb local{Vec3(-1.0f, -0.1f, -0.1f), Vec3(1.0f, 0.1f, 0.1f)};
    const Mat4 rotate = glm::mat4_cast(glm::angleAxis(kHalfPi, Vec3(0.0f, 0.0f, 1.0f)));
    const Aabb world = TransformAabb(local, rotate);

    // Batang sepanjang X yang diputar 90° menjadi batang sepanjang Y.
    CHECK(world.max.y == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(world.max.x == doctest::Approx(0.1f).epsilon(0.001));
}

TEST_CASE("Menyaring kotak mengembalikan indeks yang lolos saja") {
    const Mat4 view = LookAt(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f));
    const Frustum frustum(PerspectiveReversedZ(60.0f * kDegToRad, 1.0f, 0.1f, 100.0f) * view);

    const std::vector<Aabb> boxes{
        Box(Vec3(0.0f, 0.0f, -10.0f), 1.0f),     // di depan
        Box(Vec3(0.0f, 0.0f, 10.0f), 1.0f),      // di belakang
        Box(Vec3(0.0f, 0.0f, -50.0f), 1.0f),     // di depan, jauh
        Box(Vec3(0.0f, 0.0f, -5000.0f), 1.0f),   // di luar bidang jauh
    };
    std::vector<uint32_t> visible;
    CHECK(CullAabbs(frustum, boxes, visible) == 2u);
    CHECK(visible == std::vector<uint32_t>{0u, 2u});
}

// --- Cascade bayangan --------------------------------------------------------

namespace {

Camera TestCamera(const Vec3& position = Vec3(0.0f, 2.0f, 0.0f),
                  const Quat& rotation = Quat(1.0f, 0.0f, 0.0f, 0.0f)) {
    Camera camera;
    camera.position = position;
    camera.rotation = rotation;
    camera.fovYRadians = 60.0f * kDegToRad;
    camera.nearZ = 0.1f;
    camera.farZ = 1000.0f;
    return camera;
}

/// Kedelapan sudut irisan frustum, dalam ruang dunia.
std::array<Vec3, 8> SliceCorners(const Camera& camera, float aspect, float nearDistance,
                                 float farDistance) {
    const float tanY = std::tan(camera.fovYRadians * 0.5f);
    const float tanX = tanY * aspect;
    const Vec3 forward = camera.Forward();
    const Vec3 right = camera.Right();
    const Vec3 up = camera.Up();

    std::array<Vec3, 8> corners{};
    int at = 0;
    for (const float depth : {nearDistance, farDistance}) {
        for (const float sy : {-1.0f, 1.0f}) {
            for (const float sx : {-1.0f, 1.0f}) {
                corners[static_cast<size_t>(at++)] = camera.position + forward * depth +
                                                     right * (sx * tanX * depth) +
                                                     up * (sy * tanY * depth);
            }
        }
    }
    return corners;
}

}  // namespace

TEST_CASE("Belahan cascade menaik dan berakhir tepat di jarak maksimum") {
    CascadeSettings settings;
    settings.count = 4;
    settings.maxDistance = 150.0f;
    std::array<float, kMaxCascades> splits{};
    ComputeCascadeSplits(settings, 0.1f, splits);

    CHECK(splits[0] < splits[1]);
    CHECK(splits[1] < splits[2]);
    CHECK(splits[2] < splits[3]);
    // Yang terakhir dipaksa tepat: selisih sepersekian meter di sini berarti
    // pita sempit yang tidak ditutupi cascade mana pun, dan yang terlihat di
    // sana adalah bayangan yang hilang mendadak.
    CHECK(splits[3] == doctest::Approx(150.0f));
}

TEST_CASE("splitLambda memilih antara pembagian seragam dan logaritmik") {
    CascadeSettings uniform;
    uniform.count = 4;
    uniform.maxDistance = 100.0f;
    uniform.splitLambda = 0.0f;
    std::array<float, kMaxCascades> uniformSplits{};
    ComputeCascadeSplits(uniform, 0.1f, uniformSplits);
    // Seragam: tiap cascade menutupi seperempat jaraknya.
    CHECK(uniformSplits[0] == doctest::Approx(25.075f).epsilon(0.01));

    CascadeSettings logarithmic = uniform;
    logarithmic.splitLambda = 1.0f;
    std::array<float, kMaxCascades> logSplits{};
    ComputeCascadeSplits(logarithmic, 0.1f, logSplits);
    // Logaritmik menaruh belahan pertama jauh lebih dekat — itulah gunanya, dan
    // juga alasan ia tidak dipakai murni.
    CHECK(logSplits[0] < 1.0f);
    CHECK(logSplits[0] < uniformSplits[0] * 0.1f);
}

TEST_CASE("Bola pembatas benar-benar memuat seluruh sudut irisan") {
    const Camera camera = TestCamera();
    const float aspect = 16.0f / 9.0f;
    for (const auto [from, to] : {std::pair{0.1f, 10.0f}, std::pair{10.0f, 40.0f},
                                  std::pair{40.0f, 150.0f}, std::pair{0.1f, 0.11f}}) {
        const BoundingSphere sphere = FrustumSliceSphere(camera, aspect, from, to);
        INFO("irisan ", from, "..", to);
        for (const Vec3& corner : SliceCorners(camera, aspect, from, to)) {
            CHECK(glm::length(corner - sphere.centre) <= sphere.radius + 1e-3f);
        }
    }
}

TEST_CASE("Jari-jari bola tidak berubah saat kamera berputar") {
    // **Inilah yang membuat tepi bayangan tidak berkilat.** Kotak yang dipas
    // ketat berubah ukuran mengikuti orientasi kamera, dan ukuran yang berubah
    // berarti dunia-per-texel yang berubah — tepi bayangan lalu bergetar setiap
    // kali kamera bergerak sedikit.
    const float aspect = 16.0f / 9.0f;
    const float reference =
        FrustumSliceSphere(TestCamera(), aspect, 10.0f, 40.0f).radius;

    for (const float yaw : {0.3f, 1.1f, 2.7f, -0.8f}) {
        for (const float pitch : {0.0f, 0.4f, -1.2f}) {
            const Quat rotation = glm::angleAxis(yaw, Vec3(0.0f, 1.0f, 0.0f)) *
                                  glm::angleAxis(pitch, Vec3(1.0f, 0.0f, 0.0f));
            const Camera rotated = TestCamera(Vec3(0.0f, 2.0f, 0.0f), rotation);
            INFO("yaw ", yaw, " pitch ", pitch);
            CHECK(FrustumSliceSphere(rotated, aspect, 10.0f, 40.0f).radius ==
                  doctest::Approx(reference));
        }
    }
}

TEST_CASE("Kisi texel cascade terkunci ke kisi dunia yang tetap") {
    // Pengancingan texel adalah separuh kedua dari anti-kilat; bola menangani
    // rotasi, ini menangani pergeseran. Keduanya sering dikira satu perbaikan
    // yang sama, dan yang memakai salah satunya saja tetap melihat kilatan.
    CascadeSettings settings;
    settings.count = 1;
    settings.resolution = 512;
    settings.maxDistance = 50.0f;
    const Vec3 toLight = glm::normalize(Vec3(0.3f, 0.8f, 0.5f));
    const float aspect = 16.0f / 9.0f;

    // Kisi yang sama dengan yang dipakai implementasinya: rotasi cahaya saja,
    // titik asal di origin dunia.
    const Mat4 lightGrid = LookAt(Vec3(0.0f), -toLight, Vec3(0.0f, 1.0f, 0.0f));

    // **Hanya X dan Y yang dikancing, dan itu memang cukup.** Z di ruang cahaya
    // menunjuk sepanjang arah cahaya; menggesernya memindahkan rentang
    // kedalaman, bukan kisi texel. Mengukur pergeseran di ruang dunia — yang
    // mencampur ketiganya — akan membuat pengancingan yang benar terlihat
    // gagal. Itu yang mula-mula saya ukur.
    const auto originXy = [&lightGrid](const Cascade& cascade) {
        const Mat4 inverse = glm::inverse(cascade.viewProjection);
        const Vec3 world = Vec3(inverse * Vec4(0.0f, 0.0f, 0.5f, 1.0f));
        const Vec3 inLight = Vec3(lightGrid * Vec4(world, 1.0f));
        return Vec2(inLight.x, inLight.y);
    };

    const CascadeSet base = ComputeCascades(TestCamera(), aspect, toLight, settings);
    const float texel = base.cascades[0].texelWorldSize;
    REQUIRE(texel > 0.0f);
    const Vec2 reference = originXy(base.cascades[0]);

    int identical = 0;
    Vec2 previous = reference;
    for (int step = 0; step <= 40; ++step) {
        const float offset = static_cast<float>(step) * texel * 0.1f;
        const CascadeSet moved = ComputeCascades(
            TestCamera(Vec3(offset, 2.0f, 0.0f)), aspect, toLight, settings);
        const Vec2 xy = originXy(moved.cascades[0]);

        // Invarian yang sebenarnya: titik asal cascade selalu jatuh tepat pada
        // kelipatan texel dari kisi dunia yang tetap. Itulah yang membuat
        // sebuah texel bayangan menutupi bagian dunia yang sama dari frame ke
        // frame, dan tepinya berhenti bergetar.
        INFO("langkah ", step);
        CHECK(std::abs(xy.x / texel - std::round(xy.x / texel)) < 0.02f);
        CHECK(std::abs(xy.y / texel - std::round(xy.y / texel)) < 0.02f);

        if (glm::length(xy - previous) < texel * 0.02f) {
            ++identical;
        }
        previous = xy;
    }
    // 41 sampel yang seluruhnya hanya membentang beberapa texel harus banyak
    // yang kembar. Kalau setiap langkah menghasilkan nilai baru, pengancingannya
    // tidak mengancing apa pun.
    //
    // Dihitung terhadap sampel SEBELUMNYA, bukan terhadap sampel pertama:
    // kalau yang pertama kebetulan jatuh dekat batas texel, kelompok pertamanya
    // pendek dan hitungannya bilang "gagal" pada pengancingan yang bekerja.
    CHECK(identical > 30);
}

TEST_CASE("Arah cahaya nol tidak menghasilkan NaN") {
    CascadeSettings settings;
    const CascadeSet set = ComputeCascades(TestCamera(), 1.6f, Vec3(0.0f), settings);
    for (int i = 0; i < set.count; ++i) {
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                CHECK(std::isfinite(set.cascades[static_cast<size_t>(i)]
                                        .viewProjection[column][row]));
            }
        }
    }
}

TEST_CASE("Matahari tepat di puncak tetap menghasilkan matriks yang sah") {
    // Y dunia dan arah pandang cahaya sejajar di sini, dan LookAt biasa
    // menghasilkan matriks yang tidak terdefinisi. Tengah hari bukan kasus aneh.
    CascadeSettings settings;
    const CascadeSet set = ComputeCascades(TestCamera(), 1.6f, Vec3(0.0f, 1.0f, 0.0f), settings);
    REQUIRE(set.count > 0);
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            CHECK(std::isfinite(set.cascades[0].viewProjection[column][row]));
        }
    }
}

TEST_CASE("Pemilihan cascade dan pita campurnya") {
    CascadeSettings settings;
    settings.count = 3;
    settings.maxDistance = 120.0f;
    settings.blendFraction = 0.2f;
    const CascadeSet set =
        ComputeCascades(TestCamera(), 1.6f, glm::normalize(Vec3(0.0f, 1.0f, 0.3f)), settings);
    REQUIRE(set.count == 3);

    // Di tengah cascade pertama: tanpa campuran.
    const CascadeChoice inside = ChooseCascade(set, set.cascades[0].blendBegin * 0.5f);
    CHECK(inside.index == 0);
    CHECK(inside.blendWeight == doctest::Approx(0.0f));

    // Tepat di batas: sepenuhnya sudah cascade berikutnya.
    const CascadeChoice edge = ChooseCascade(set, set.cascades[0].splitFar);
    CHECK(edge.index == 0);
    CHECK(edge.blendWeight == doctest::Approx(1.0f));

    // Di dalam pita: di antara keduanya.
    const float middle = (set.cascades[0].blendBegin + set.cascades[0].splitFar) * 0.5f;
    const CascadeChoice band = ChooseCascade(set, middle);
    CHECK(band.index == 0);
    CHECK(band.blendWeight > 0.4f);
    CHECK(band.blendWeight < 0.6f);

    // Cascade terakhir tidak punya penerus, jadi tidak ada yang bisa dicampur.
    CHECK(set.cascades[2].blendBegin == doctest::Approx(set.cascades[2].splitFar));
    const CascadeChoice beyond = ChooseCascade(set, 500.0f);
    CHECK(beyond.index == 2);
    CHECK(beyond.blendWeight == doctest::Approx(0.0f));
}

// --- Clustered light culling -------------------------------------------------

namespace {

ClusterGrid MakeGrid(const ClusterGridSettings& settings, float nearZ = 0.1f,
                     float farZ = 200.0f) {
    ClusterGrid grid;
    grid.Build(settings, 60.0f * kDegToRad, 16.0f / 9.0f, nearZ, farZ);
    return grid;
}

ClusterLight PointAt(const Vec3& position, float range) {
    ClusterLight light;
    light.type = ClusterLightType::Point;
    light.position = position;
    light.range = range;
    return light;
}

}  // namespace

TEST_CASE("Irisan kedalaman eksponensial dan konsisten dengan batasnya") {
    const ClusterGridSettings settings;
    const ClusterGrid grid = MakeGrid(settings, 0.1f, 200.0f);

    CHECK(grid.SliceOf(0.1f) == 0u);
    CHECK(grid.SliceOf(0.05f) == 0u);  // di depan bidang dekat, dijepit
    CHECK(grid.SliceOf(200.0f) == settings.slices - 1);
    CHECK(grid.SliceOf(5000.0f) == settings.slices - 1);

    // Setiap kedalaman harus jatuh ke irisan yang batasnya memuatnya. Rumus
    // irisan dan rumus batas ditulis terpisah, dan justru selisih satu di
    // antaranya yang membuat lampu hilang tepat pada jarak tertentu.
    for (const float depth : {0.2f, 1.0f, 7.5f, 33.0f, 120.0f, 199.0f}) {
        const uint32_t slice = grid.SliceOf(depth);
        const Vec2 bounds = grid.SliceBounds(slice);
        INFO("kedalaman ", depth, " irisan ", slice);
        CHECK(depth >= bounds.x - 1e-3f);
        CHECK(depth <= bounds.y + 1e-3f);
    }

    // Eksponensial: irisan pertama jauh lebih tipis daripada yang terakhir.
    const Vec2 first = grid.SliceBounds(0);
    const Vec2 last = grid.SliceBounds(settings.slices - 1);
    CHECK((first.y - first.x) < (last.y - last.x) * 0.01f);
}

TEST_CASE("Kotak cluster memuat titik yang dipetakan ke cluster itu") {
    ClusterGridSettings settings;
    settings.tilesX = 8;
    settings.tilesY = 6;
    settings.slices = 12;
    const ClusterGrid grid = MakeGrid(settings);

    const float tanY = std::tan(30.0f * kDegToRad);
    const float tanX = tanY * 16.0f / 9.0f;

    for (const float depth : {0.5f, 3.0f, 25.0f, 150.0f}) {
        for (const float ndcX : {-0.9f, -0.2f, 0.35f, 0.85f}) {
            for (const float ndcY : {-0.75f, 0.1f, 0.6f}) {
                const Vec3 point(ndcX * tanX * depth, ndcY * tanY * depth, depth);
                const auto x = static_cast<uint32_t>((ndcX * 0.5f + 0.5f) *
                                                     static_cast<float>(settings.tilesX));
                const auto y = static_cast<uint32_t>((ndcY * 0.5f + 0.5f) *
                                                     static_cast<float>(settings.tilesY));
                const uint32_t slice = grid.SliceOf(depth);
                const Aabb box = grid.ClusterBounds(std::min(x, settings.tilesX - 1),
                                                   std::min(y, settings.tilesY - 1), slice);
                INFO("titik (", point.x, ",", point.y, ",", point.z, ")");
                CHECK(SphereIntersectsAabb(point, 1e-4f, box));
            }
        }
    }
}

TEST_CASE("Uji bola-kotak menolak bola yang lewat di dekat sudut") {
    // Inilah alasan cluster diuji sebagai kotak, bukan sebagai enam bidang:
    // bola ini berada di sisi dalam bidang X dan bidang Y sekaligus, jadi uji
    // bidang menyatakannya di dalam — padahal ia tidak menyentuh kotaknya.
    Aabb box;
    box.min = Vec3(0.0f);
    box.max = Vec3(1.0f);
    CHECK(!SphereIntersectsAabb(Vec3(-1.0f, -1.0f, 0.5f), 1.2f, box));
    CHECK(SphereIntersectsAabb(Vec3(-1.0f, -1.0f, 0.5f), 1.5f, box));
    CHECK(SphereIntersectsAabb(Vec3(0.5f, 0.5f, 0.5f), 0.01f, box));
}

TEST_CASE("Kerucut spot menghitung bola yang menyerempet tepinya") {
    const Vec3 apex(0.0f);
    const Vec3 direction(0.0f, 0.0f, 1.0f);
    const float range = 20.0f;
    const float cosOuter = std::cos(15.0f * kDegToRad);

    // Di dalam berkas.
    CHECK(ConeIntersectsSphere(apex, direction, range, cosOuter, Vec3(0.0f, 0.0f, 10.0f), 0.1f));
    // Di belakang lampu.
    CHECK(!ConeIntersectsSphere(apex, direction, range, cosOuter, Vec3(0.0f, 0.0f, -10.0f), 0.1f));
    // Lebih jauh daripada jangkauannya.
    CHECK(!ConeIntersectsSphere(apex, direction, range, cosOuter, Vec3(0.0f, 0.0f, 40.0f), 0.5f));
    // Melenceng jauh dari berkasnya.
    CHECK(!ConeIntersectsSphere(apex, direction, range, cosOuter, Vec3(15.0f, 0.0f, 10.0f), 0.5f));

    // Pusatnya tepat di luar berkas, tapi bolanya menyerempet masuk. Tanpa
    // penggeseran puncak, hanya pusat bola yang diuji — dan lampu sorot akan
    // memotong benda tepat di tepi berkasnya.
    const float justOutside = std::tan(16.0f * kDegToRad) * 10.0f;
    CHECK(!ConeIntersectsSphere(apex, direction, range, cosOuter,
                                Vec3(justOutside, 0.0f, 10.0f), 0.01f));
    CHECK(ConeIntersectsSphere(apex, direction, range, cosOuter, Vec3(justOutside, 0.0f, 10.0f),
                               1.0f));
    // Bola yang menelan puncaknya selalu memotong.
    CHECK(ConeIntersectsSphere(apex, direction, range, cosOuter, Vec3(0.5f, 0.0f, -0.2f), 2.0f));
}

TEST_CASE("Lampu hanya masuk ke cluster yang benar-benar dikenainya") {
    ClusterGridSettings settings;
    settings.tilesX = 8;
    settings.tilesY = 6;
    settings.slices = 12;
    const ClusterGrid grid = MakeGrid(settings);

    // Lampu kecil di sumbu pandang pada kedalaman 10.
    const std::array<ClusterLight, 1> lights{PointAt(Vec3(0.0f, 0.0f, 10.0f), 1.0f)};
    const ClusterAssignment assignment =
        AssignLights(grid, Mat4(1.0f), lights, settings);

    uint32_t touched = 0;
    for (uint32_t i = 0; i < grid.ClusterCount(); ++i) {
        touched += assignment.ranges[i].count;
    }
    CHECK(touched > 0);
    // Kalau ia masuk ke sebagian besar cluster, penyaringannya tidak menyaring.
    CHECK(touched < grid.ClusterCount() / 4);

    // Cluster tempat lampunya berada pasti termuat.
    const uint32_t slice = grid.SliceOf(10.0f);
    const uint32_t centre = grid.IndexOf(settings.tilesX / 2, settings.tilesY / 2, slice);
    CHECK(assignment.LightsOf(centre).size() == 1);
}

TEST_CASE("Lampu di belakang kamera tidak masuk cluster mana pun") {
    const ClusterGridSettings settings;
    const ClusterGrid grid = MakeGrid(settings);
    const std::array<ClusterLight, 1> lights{PointAt(Vec3(0.0f, 0.0f, -50.0f), 5.0f)};
    const ClusterAssignment assignment = AssignLights(grid, Mat4(1.0f), lights, settings);

    for (uint32_t i = 0; i < grid.ClusterCount(); ++i) {
        CHECK(assignment.ranges[i].count == 0);
    }
}

TEST_CASE("Daftar cluster dipotong dan pemotongannya dilaporkan") {
    ClusterGridSettings settings;
    settings.tilesX = 4;
    settings.tilesY = 4;
    settings.slices = 4;
    settings.maxLightsPerCluster = 3;
    const ClusterGrid grid = MakeGrid(settings);

    // Sepuluh lampu raksasa yang semuanya mengenai semua cluster.
    std::vector<ClusterLight> lights;
    for (int i = 0; i < 10; ++i) {
        lights.push_back(PointAt(Vec3(0.0f, 0.0f, 20.0f), 10000.0f));
    }
    const ClusterAssignment assignment = AssignLights(grid, Mat4(1.0f), lights, settings);

    // Dipotong, bukan dibiarkan tumbuh: satu cluster buruk tidak boleh
    // menentukan biaya seluruh frame.
    for (uint32_t i = 0; i < grid.ClusterCount(); ++i) {
        CHECK(assignment.ranges[i].count <= 3u);
    }
    // Dan pemotongannya dilaporkan, bukan diam-diam.
    CHECK(assignment.overflowed == grid.ClusterCount());
}

TEST_CASE("Matriks pandang memindahkan lampu, arahnya tanpa translasi") {
    ClusterGridSettings settings;
    settings.tilesX = 8;
    settings.tilesY = 6;
    settings.slices = 12;
    const ClusterGrid grid = MakeGrid(settings);

    // Kamera digeser 100 m di sumbu X; lampu ikut digeser sama banyak, jadi
    // hasilnya harus sama persis dengan kamera di origin.
    const std::array<ClusterLight, 1> atOrigin{PointAt(Vec3(0.0f, 0.0f, 10.0f), 2.0f)};
    const std::array<ClusterLight, 1> shifted{PointAt(Vec3(100.0f, 0.0f, 10.0f), 2.0f)};
    const Mat4 view = glm::translate(Mat4(1.0f), Vec3(-100.0f, 0.0f, 0.0f));

    const ClusterAssignment a = AssignLights(grid, Mat4(1.0f), atOrigin, settings);
    const ClusterAssignment b = AssignLights(grid, view, shifted, settings);
    for (uint32_t i = 0; i < grid.ClusterCount(); ++i) {
        CHECK(a.ranges[i].count == b.ranges[i].count);
    }
}

// --- IBL ---------------------------------------------------------------------

namespace {

/// Lingkungan yang jawabannya diketahui secara analitis.
class ConstantEnvironment final : public IEnvironmentSampler {
public:
    explicit ConstantEnvironment(const Vec3& radiance) : radiance_(radiance) {}
    Vec3 Sample(const Vec3&) const override { return radiance_; }

private:
    Vec3 radiance_;
};

/// Setengah bola terang di arah tertentu, setengah lagi gelap.
class HemisphereEnvironment final : public IEnvironmentSampler {
public:
    explicit HemisphereEnvironment(const Vec3& up) : up_(glm::normalize(up)) {}
    Vec3 Sample(const Vec3& direction) const override {
        return glm::dot(direction, up_) > 0.0f ? Vec3(1.0f) : Vec3(0.0f);
    }

private:
    Vec3 up_;
};

}  // namespace

TEST_CASE("Urutan Hammersley deterministik dan mengisi kotak satuan") {
    // Deterministik: LUT yang berbeda antar-jalan membuat perbandingan gambar
    // tidak bisa dipakai sebagai test, dan cache apa pun yang menyimpannya tidak
    // pernah sah. Alasan yang sama dengan penabur vegetasi E7.4.
    CHECK(Hammersley(0, 16).x == doctest::Approx(0.0f));
    CHECK(Hammersley(0, 16).y == doctest::Approx(0.0f));
    CHECK(Hammersley(8, 16).y == doctest::Approx(0.0625f));
    CHECK(Hammersley(1, 16).y == doctest::Approx(0.5f));

    float minX = 1.0f;
    float maxX = 0.0f;
    float minY = 1.0f;
    float maxY = 0.0f;
    for (uint32_t i = 0; i < 256; ++i) {
        const Vec2 point = Hammersley(i, 256);
        CHECK(point.x >= 0.0f);
        CHECK(point.x < 1.0f);
        CHECK(point.y >= 0.0f);
        CHECK(point.y < 1.0f);
        minX = std::min(minX, point.x);
        maxX = std::max(maxX, point.x);
        minY = std::min(minY, point.y);
        maxY = std::max(maxY, point.y);
    }
    CHECK(minX < 0.01f);
    CHECK(maxX > 0.99f);
    CHECK(minY < 0.01f);
    CHECK(maxY > 0.99f);
}

TEST_CASE("Sampel GGX berkumpul di sekitar normal dan melebar dengan kekasaran") {
    const Vec3 normal(0.0f, 0.0f, 1.0f);
    const auto spread = [&normal](float roughness) {
        float total = 0.0f;
        for (uint32_t i = 0; i < 512; ++i) {
            const Vec3 half = ImportanceSampleGgx(Hammersley(i, 512), normal, roughness);
            CHECK(glm::dot(half, normal) >= -1e-4f);
            CHECK(std::abs(glm::length(half) - 1.0f) < 1e-3f);
            total += std::acos(std::clamp(glm::dot(half, normal), -1.0f, 1.0f));
        }
        return total / 512.0f;
    };
    const float tight = spread(0.05f);
    const float loose = spread(0.8f);
    CHECK(tight < 0.05f);
    CHECK(loose > tight * 5.0f);
}

TEST_CASE("Sampel GGX tetap sah pada normal yang sejajar sumbu bantu") {
    // Bingkai yang selalu memakai satu sumbu tetap menghasilkan cross product
    // nol tepat ketika normal sejajar dengannya — dan arah itu bukan kasus
    // langka melainkan arah "atas", yang muncul pada setiap permukaan datar.
    for (const Vec3 normal : {Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 0.0f, -1.0f),
                              Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f)}) {
        const Vec3 half = ImportanceSampleGgx(Vec2(0.3f, 0.4f), normal, 0.4f);
        INFO("normal (", normal.x, ",", normal.y, ",", normal.z, ")");
        CHECK(std::isfinite(half.x));
        CHECK(std::isfinite(half.y));
        CHECK(std::isfinite(half.z));
        CHECK(std::abs(glm::length(half) - 1.0f) < 1e-3f);
    }
}

TEST_CASE("Suku DFG: cermin sempurna memberi F0 apa adanya") {
    // Pada kekasaran nol, F0 * scale + bias harus mengembalikan F0 — permukaan
    // cermin memantulkan tepat Fresnel-nya.
    const DfgTerms terms = IntegrateDfg(1.0f, 0.0f, 2048);
    CHECK(terms.scale == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(terms.bias == doctest::Approx(0.0f).epsilon(0.02));
}

TEST_CASE("Suku DFG kehilangan energi seiring kekasaran, tidak pernah menambah") {
    // GGX hamburan tunggal memang kehilangan energi pada permukaan kasar —
    // itulah yang nanti dikembalikan oleh kompensasi multi-scatter. Yang tidak
    // boleh terjadi adalah sebaliknya: BRDF yang memantulkan lebih dari yang
    // diterimanya membuat pantulan bertingkat menyala makin terang.
    float previous = 2.0f;
    for (const float roughness : {0.05f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f}) {
        const DfgTerms terms = IntegrateDfg(0.8f, roughness, 2048);
        const float total = terms.scale + terms.bias;
        INFO("kekasaran ", roughness, " total ", total);
        CHECK(total > 0.0f);
        CHECK(total <= 1.0f + 1e-3f);
        CHECK(total < previous + 1e-3f);
        previous = total;
    }
}

TEST_CASE("Suku DFG tidak pernah negatif untuk seluruh isi LUT") {
    const DfgLut lut = BakeDfgLut(32, 256);
    REQUIRE(lut.size == 32);
    for (uint32_t y = 0; y < lut.size; ++y) {
        for (uint32_t x = 0; x < lut.size; ++x) {
            const DfgTerms terms = lut.At(x, y);
            INFO("texel ", x, ",", y);
            CHECK(terms.scale >= 0.0f);
            CHECK(terms.bias >= 0.0f);
            CHECK(terms.scale <= 1.0f + 1e-3f);
            CHECK(terms.bias <= 1.0f + 1e-3f);
            CHECK(std::isfinite(terms.scale));
            CHECK(std::isfinite(terms.bias));
        }
    }
}

TEST_CASE("Pembacaan LUT bilinear cocok dengan texel di tengahnya") {
    const DfgLut lut = BakeDfgLut(16, 128);
    // Tengah texel — di sanalah nilai bakarnya berada, dan di situlah
    // interpolasi harus mengembalikannya persis. Membakar di tepi texel akan
    // membuat separuh texel pertama dan terakhir mewakili nilai di luar rentang.
    for (uint32_t i = 0; i < lut.size; ++i) {
        const float coordinate = (static_cast<float>(i) + 0.5f) / static_cast<float>(lut.size);
        const DfgTerms sampled = lut.Sample(coordinate, coordinate);
        const DfgTerms exact = lut.At(i, i);
        INFO("texel ", i);
        CHECK(sampled.scale == doctest::Approx(exact.scale).epsilon(0.001));
        CHECK(sampled.bias == doctest::Approx(exact.bias).epsilon(0.001));
    }
    // Di luar rentang dijepit, bukan dibungkus.
    CHECK(lut.Sample(-1.0f, -1.0f).scale == doctest::Approx(lut.At(0, 0).scale));
    CHECK(lut.Sample(2.0f, 2.0f).scale ==
          doctest::Approx(lut.At(lut.size - 1, lut.size - 1).scale));
}

TEST_CASE("Irradiance lingkungan konstan sama dengan pi kali radiance-nya") {
    // Uji tungku putih. Radiance L merata dari segala arah menghasilkan
    // irradiance pi*L pada normal mana pun — dan angka pi itulah yang hilang
    // kalau konvolusi lobe kosinus dilewatkan.
    const ConstantEnvironment environment(Vec3(0.5f, 0.25f, 1.0f));
    const Sh9 sh = ProjectIrradiance(environment, 8192);

    for (const Vec3 normal : {Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f),
                              Vec3(0.0f, 0.0f, -1.0f), glm::normalize(Vec3(1.0f, 1.0f, 1.0f))}) {
        const Vec3 irradiance = EvaluateIrradiance(sh, normal);
        INFO("normal (", normal.x, ",", normal.y, ",", normal.z, ")");
        CHECK(irradiance.x == doctest::Approx(kPi * 0.5f).epsilon(0.02));
        CHECK(irradiance.y == doctest::Approx(kPi * 0.25f).epsilon(0.02));
        CHECK(irradiance.z == doctest::Approx(kPi * 1.0f).epsilon(0.02));
    }
}

TEST_CASE("Irradiance mengikuti arah setengah bola yang terang") {
    const HemisphereEnvironment environment(Vec3(0.0f, 1.0f, 0.0f));
    const Sh9 sh = ProjectIrradiance(environment, 16384);

    const Vec3 up = EvaluateIrradiance(sh, Vec3(0.0f, 1.0f, 0.0f));
    const Vec3 side = EvaluateIrradiance(sh, Vec3(1.0f, 0.0f, 0.0f));
    const Vec3 down = EvaluateIrradiance(sh, Vec3(0.0f, -1.0f, 0.0f));

    CHECK(up.x > side.x);
    CHECK(side.x > down.x);
    // Menghadap lurus ke langit terang menerima hampir seluruh pi.
    CHECK(up.x == doctest::Approx(kPi).epsilon(0.06));
    // Menyamping menerima kira-kira separuhnya.
    CHECK(side.x == doctest::Approx(kPi * 0.5f).epsilon(0.08));
    // Dan tidak ada yang negatif, meski orde dua tidak bisa mewakili tepi tajam.
    CHECK(down.x >= 0.0f);
}

TEST_CASE("Prefilter lingkungan konstan mengembalikan konstanta itu") {
    // Kalau pembobotannya salah, hasil ini bergeser — dan pada lingkungan
    // sungguhan pergeserannya tidak bisa dibedakan dari "memang begitu
    // rupanya".
    const ConstantEnvironment environment(Vec3(0.3f, 0.6f, 0.9f));
    for (const float roughness : {0.0f, 0.1f, 0.35f, 0.7f, 1.0f}) {
        const Vec3 filtered =
            PrefilterSpecular(environment, glm::normalize(Vec3(0.2f, 0.9f, -0.3f)), roughness);
        INFO("kekasaran ", roughness);
        CHECK(filtered.x == doctest::Approx(0.3f).epsilon(0.001));
        CHECK(filtered.y == doctest::Approx(0.6f).epsilon(0.001));
        CHECK(filtered.z == doctest::Approx(0.9f).epsilon(0.001));
    }
}

TEST_CASE("Prefilter kekasaran nol adalah pengambilan tunggal yang tajam") {
    // Tanpa cabang khusus, integralnya menyebar sampel di sekitar arah pantul
    // karena kekasarannya dijepit ke batas bawah — dan pantulan tajam jadi
    // selalu sedikit buram.
    const HemisphereEnvironment environment(Vec3(0.0f, 1.0f, 0.0f));
    const Vec3 justAbove = glm::normalize(Vec3(1.0f, 0.02f, 0.0f));
    CHECK(PrefilterSpecular(environment, justAbove, 0.0f).x == doctest::Approx(1.0f));

    // Sedikit kasar sudah mulai mencampur sisi gelapnya.
    const float blurred = PrefilterSpecular(environment, justAbove, 0.5f, 1024).x;
    CHECK(blurred < 0.95f);
    CHECK(blurred > 0.3f);
}

TEST_CASE("Pemetaan mip ke kekasaran linear dan menutupi kedua ujungnya") {
    CHECK(RoughnessForMip(0, 6) == doctest::Approx(0.0f));
    CHECK(RoughnessForMip(5, 6) == doctest::Approx(1.0f));
    CHECK(RoughnessForMip(3, 6) == doctest::Approx(0.6f));
    // Satu mip berarti tidak ada rentang untuk dipetakan.
    CHECK(RoughnessForMip(0, 1) == doctest::Approx(0.0f));
}

TEST_CASE("Konvensi kedalaman cascade: mendekati cahaya berarti depth mengecil") {
    // Test ini ada karena shader-nya bergantung pada konvensi ini dan tidak bisa
    // menyatakannya sendiri. Ortografik glm, pembalikan Y untuk Vulkan, dan
    // LookAt tangan-kanan bertemu di satu matriks — dan kalau salah satunya
    // berlawanan arah, yang terlihat adalah adegan yang seluruhnya gelap tanpa
    // satu pun pesan galat.
    CascadeSettings settings;
    settings.count = 1;
    settings.maxDistance = 40.0f;
    const Vec3 toLight = glm::normalize(Vec3(-0.4f, 0.8f, 0.45f));
    const Camera camera = TestCamera(Vec3(0.0f, 2.0f, 0.0f));
    const CascadeSet set = ComputeCascades(camera, 16.0f / 9.0f, toLight, settings);
    REQUIRE(set.count == 1);

    const BoundingSphere sphere = FrustumSliceSphere(camera, 16.0f / 9.0f, camera.nearZ, 40.0f);
    const auto project = [&set](const Vec3& world) {
        const Vec4 clip = set.cascades[0].viewProjection * Vec4(world, 1.0f);
        return Vec3(clip) / clip.w;
    };

    const Vec3 centre = project(sphere.centre);
    INFO("pusat -> (", centre.x, ",", centre.y, ",", centre.z, ")");
    CHECK(centre.x >= -1.0f);
    CHECK(centre.x <= 1.0f);
    CHECK(centre.y >= -1.0f);
    CHECK(centre.y <= 1.0f);
    CHECK(centre.z > 0.0f);
    CHECK(centre.z < 1.0f);

    // Bergerak ke arah cahaya harus mengurangi depth — itulah yang membuat
    // perbandingan LESS_OR_EQUAL berarti "tidak ada yang menghalangi".
    const Vec3 nearer = project(sphere.centre + toLight * 2.0f);
    CHECK(nearer.z < centre.z);
    const Vec3 farther = project(sphere.centre - toLight * 2.0f);
    CHECK(farther.z > centre.z);

    // Seluruh isi bola harus muat di dalam kubus satuan.
    for (const Vec3 offset : {Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 1, 0), Vec3(0, -1, 0),
                              Vec3(0, 0, 1), Vec3(0, 0, -1)}) {
        const Vec3 point = project(sphere.centre + offset * sphere.radius);
        INFO("tepi bola -> (", point.x, ",", point.y, ",", point.z, ")");
        CHECK(std::abs(point.x) <= 1.001f);
        CHECK(std::abs(point.y) <= 1.001f);
        CHECK(point.z >= 0.0f);
        CHECK(point.z <= 1.0f);
    }
}

TEST_CASE("Langit prosedural: gradien, tanah, dan cakram matahari") {
    GradientSky sky;
    sky.zenith = Vec3(0.0f, 0.0f, 1.0f);
    sky.horizon = Vec3(0.5f);
    sky.ground = Vec3(0.0f);
    sky.sunDirection = Vec3(0.0f, 1.0f, 0.0f);
    sky.sunRadiance = Vec3(10.0f);

    // Zenit memuat matahari, jadi ia langit biru ditambah cakramnya.
    const Vec3 up = sky.Sample(Vec3(0.0f, 1.0f, 0.0f));
    CHECK(up.z > 10.0f);
    // Sedikit di luar cakram: langit saja.
    const Vec3 nearSun = sky.Sample(glm::normalize(Vec3(0.2f, 1.0f, 0.0f)));
    CHECK(nearSun.z < 2.0f);
    CHECK(nearSun.z > 0.5f);
    // Ke bawah: tanah, dan tidak ada matahari di sana.
    const Vec3 down = sky.Sample(Vec3(0.0f, -1.0f, 0.0f));
    CHECK(down.x < 0.05f);
    CHECK(down.z < 0.05f);
    // Cakrawala berada di antara keduanya.
    const Vec3 side = sky.Sample(Vec3(1.0f, 0.0f, 0.0f));
    CHECK(side.x == doctest::Approx(0.5f));
}

TEST_CASE("Arah muka cubemap menutupi keenam sumbu dan ternormalisasi") {
    // Tengah tiap muka harus menunjuk tepat ke sumbunya. Konvensi V yang
    // terbalik hanya salah pada dua muka, dan itu jauh lebih membingungkan
    // daripada terbalik seluruhnya — jadi diuji per muka, bukan sekali.
    const Vec3 expected[6]{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int face = 0; face < kCubeFaceCount; ++face) {
        const Vec3 centre = CubeFaceDirection(face, 0.5f, 0.5f);
        INFO("muka ", face);
        CHECK(glm::length(centre - expected[static_cast<size_t>(face)]) < 1e-4f);
    }

    // Setiap texel di setiap muka harus ternormalisasi, dan keenam muka bersama
    // harus menutupi seluruh bola — diuji dengan memeriksa bahwa setiap arah
    // sumbu ditemukan oleh salah satu muka.
    for (int face = 0; face < kCubeFaceCount; ++face) {
        for (const float u : {0.02f, 0.5f, 0.98f}) {
            for (const float v : {0.02f, 0.5f, 0.98f}) {
                const Vec3 d = CubeFaceDirection(face, u, v);
                CHECK(std::abs(glm::length(d) - 1.0f) < 1e-4f);
            }
        }
    }
}

TEST_CASE("Prefilter mip 0 lingkungan konstan sama dengan konstantanya") {
    // Rantai mip peta prefilter bukan penyaringan biasa: tiap mip dibakar
    // dengan kekasaran yang berbeda, bukan dikecilkan dari mip sebelumnya.
    // Yang dijamin di sini: pada lingkungan konstan seluruh mip harus sama,
    // karena tidak ada yang bisa dirata-ratakan.
    const ConstantEnvironment environment(Vec3(0.25f, 0.5f, 0.75f));
    for (uint32_t mip = 0; mip < 5; ++mip) {
        const float roughness = RoughnessForMip(mip, 5);
        for (int face = 0; face < kCubeFaceCount; ++face) {
            const Vec3 direction = CubeFaceDirection(face, 0.5f, 0.5f);
            const Vec3 filtered = PrefilterSpecular(environment, direction, roughness, 64);
            INFO("mip ", mip, " muka ", face);
            CHECK(filtered.x == doctest::Approx(0.25f).epsilon(0.002));
            CHECK(filtered.z == doctest::Approx(0.75f).epsilon(0.002));
        }
    }
}
