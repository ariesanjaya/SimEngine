#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Render/FrameGraph.h"
#include "Sim/Render/Frustum.h"
#include "Sim/Render/Ibl.h"
#include "Sim/Render/LightCluster.h"
#include "Sim/Render/ScreenProbe.h"
#include "Sim/Render/ScreenTrace.h"
#include "Sim/Render/SdfClipmap.h"
#include "Sim/Render/SdfVolume.h"
#include "Sim/Render/ShadowAtlas.h"
#include "Sim/Render/TieredTrace.h"
#include "Sim/Render/TraceBackend.h"
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

/// Ubin layar sebuah titik ruang pandang, dihitung **seperti shader
/// menghitungnya**: lewat koordinat framebuffer, yang sumbu Y-nya ke bawah.
///
/// Ditulis begini dengan sengaja. Menghitungnya dari NDC ruang pandang akan
/// menghasilkan test yang konsisten dengan dirinya sendiri dan buta terhadap
/// pembalikan Y — dan itu persis test yang tetap hijau selama cahaya terpotong
/// di batas ubin pada layar sungguhan.
void ScreenTileOf(const Vec3& viewPoint, float tanHalfX, float tanHalfY,
                  const ClusterGridSettings& settings, uint32_t& outX, uint32_t& outY) {
    const float ndcX = viewPoint.x / (tanHalfX * viewPoint.z);
    // Proyeksi membalik Y (`result[1][1] = -f` di PerspectiveReversedZ), jadi
    // gl_FragCoord.y naik ke bawah layar.
    const float ndcY = -viewPoint.y / (tanHalfY * viewPoint.z);
    const float fx = std::clamp(ndcX * 0.5f + 0.5f, 0.0f, 0.999f);
    const float fy = std::clamp(ndcY * 0.5f + 0.5f, 0.0f, 0.999f);
    outX = static_cast<uint32_t>(fx * static_cast<float>(settings.tilesX));
    outY = static_cast<uint32_t>(fy * static_cast<float>(settings.tilesY));
}

TEST_CASE("Baris ubin nol adalah baris atas layar") {
    // Inti kesalahannya: indeks ubin adalah indeks ubin LAYAR, sedangkan kotak
    // cluster hidup di ruang pandang yang +Y-nya ke atas. Tanpa pembalikan,
    // fragmen mencari lampu di baris yang tercermin — dan yang terlihat adalah
    // cahaya yang terpotong tepat di batas ubin, persegi bertepi tegak lurus
    // yang tidak mungkin dihasilkan geometri mana pun.
    ClusterGridSettings settings;
    settings.tilesY = 8;
    const ClusterGrid grid = MakeGrid(settings);

    const Aabb top = grid.ClusterBounds(0, 0, 5);
    const Aabb bottom = grid.ClusterBounds(0, settings.tilesY - 1, 5);
    CHECK(top.min.y > 0.0f);     // baris atas layar = +Y ruang pandang
    CHECK(bottom.max.y < 0.0f);  // baris bawah layar = -Y ruang pandang
    CHECK(top.min.y > bottom.max.y);
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
                uint32_t x = 0;
                uint32_t y = 0;
                ScreenTileOf(point, tanX, tanY, settings, x, y);
                const uint32_t slice = grid.SliceOf(depth);
                const Aabb box = grid.ClusterBounds(x, y, slice);
                INFO("titik (", point.x, ",", point.y, ",", point.z, ") -> ubin ", x, ",", y);
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

// --- Atlas bayangan point/spot ------------------------------------------------

namespace {

LightInstance MakeSpot(const Vec3& position, float range, const Vec3& direction) {
    LightInstance light;
    light.kind = LightKind::Spot;
    light.position = position;
    light.direction = glm::normalize(direction);
    light.range = range;
    light.cosOuter = std::cos(35.0f * kDegToRad);
    light.cosInner = std::cos(25.0f * kDegToRad);
    return light;
}

LightInstance MakePoint(const Vec3& position, float range) {
    LightInstance light;
    light.kind = LightKind::Point;
    light.position = position;
    light.range = range;
    return light;
}

/// Apakah ada dua ubin yang bertindihan. Inilah properti yang membuat atlas
/// benar; sisanya soal kualitas.
bool TilesOverlap(const ShadowAtlasResult& atlas) {
    for (std::size_t i = 0; i < atlas.entries.size(); ++i) {
        for (std::size_t j = i + 1; j < atlas.entries.size(); ++j) {
            const ShadowAtlasEntry& a = atlas.entries[i];
            const ShadowAtlasEntry& b = atlas.entries[j];
            const bool apart = a.x + a.size <= b.x || b.x + b.size <= a.x ||
                               a.y + a.size <= b.y || b.y + b.size <= a.y;
            if (!apart) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

TEST_CASE("Ubin atlas tidak pernah bertindihan dan tetap di dalam batasnya") {
    ShadowAtlasSettings settings;
    settings.resolution = 2048;
    settings.maxTile = 512;
    settings.minTile = 128;

    // Campuran jarak supaya ukuran ubinnya beragam — di situlah pengalokasi
    // pangkat dua paling mungkin salah.
    std::vector<LightInstance> lights;
    for (int i = 0; i < 12; ++i) {
        const float distance = 2.0f + static_cast<float>(i) * 4.0f;
        lights.push_back(MakePoint(Vec3(distance, 0.0f, 0.0f), 8.0f));
        lights.push_back(MakeSpot(Vec3(0.0f, distance, 0.0f), 12.0f, Vec3(0.0f, -1.0f, 0.0f)));
    }

    const ShadowAtlasResult atlas = AllocateShadowAtlas(lights, Vec3(0.0f), settings);
    REQUIRE(!atlas.entries.empty());
    CHECK(!TilesOverlap(atlas));
    for (const ShadowAtlasEntry& entry : atlas.entries) {
        INFO("ubin ", entry.x, ",", entry.y, " ukuran ", entry.size);
        CHECK(entry.x + entry.size <= settings.resolution);
        CHECK(entry.y + entry.size <= settings.resolution);
        // Keselarasan itu sendiri yang mencegah tindihan; diuji terpisah supaya
        // kegagalannya menunjuk sebabnya, bukan gejalanya.
        CHECK(entry.x % entry.size == 0);
        CHECK(entry.y % entry.size == 0);
    }
}

TEST_CASE("Lampu yang lebih dekat mendapat ubin yang lebih besar") {
    ShadowAtlasSettings settings;
    const std::array<LightInstance, 2> lights{MakeSpot(Vec3(0.0f, 0.0f, 3.0f), 10.0f,
                                                       Vec3(0.0f, 0.0f, -1.0f)),
                                              MakeSpot(Vec3(0.0f, 0.0f, 200.0f), 10.0f,
                                                       Vec3(0.0f, 0.0f, -1.0f))};
    const ShadowAtlasResult atlas = AllocateShadowAtlas(lights, Vec3(0.0f), settings);
    REQUIRE(atlas.entries.size() == 2);

    // Ukuran tetap untuk semua akan memberi lampu di ujung ruangan resolusi
    // yang sama dengan yang memenuhi layar.
    const int32_t nearFirst = atlas.firstEntry[0];
    const int32_t farFirst = atlas.firstEntry[1];
    REQUIRE(nearFirst >= 0);
    REQUIRE(farFirst >= 0);
    CHECK(atlas.entries[static_cast<size_t>(nearFirst)].size >
          atlas.entries[static_cast<size_t>(farFirst)].size);
}

TEST_CASE("Point light memakai enam muka, spot satu") {
    ShadowAtlasSettings settings;
    const std::array<LightInstance, 2> lights{
        MakePoint(Vec3(0.0f, 0.0f, 5.0f), 10.0f),
        MakeSpot(Vec3(5.0f, 0.0f, 0.0f), 10.0f, Vec3(-1.0f, 0.0f, 0.0f))};
    const ShadowAtlasResult atlas = AllocateShadowAtlas(lights, Vec3(0.0f), settings);
    REQUIRE(atlas.entries.size() == 7);

    int pointFaces = 0;
    int spotFaces = 0;
    for (const ShadowAtlasEntry& entry : atlas.entries) {
        (entry.light == 0 ? pointFaces : spotFaces) += 1;
    }
    CHECK(pointFaces == 6);
    CHECK(spotFaces == 1);
}

TEST_CASE("Directional dan yang tidak menjatuhkan bayangan tidak masuk atlas") {
    ShadowAtlasSettings settings;
    LightInstance sun;
    sun.kind = LightKind::Directional;
    LightInstance quiet = MakePoint(Vec3(0.0f, 0.0f, 3.0f), 10.0f);
    quiet.castShadows = false;
    const std::array<LightInstance, 3> lights{sun, quiet,
                                              MakeSpot(Vec3(3.0f, 0.0f, 0.0f), 10.0f,
                                                       Vec3(-1.0f, 0.0f, 0.0f))};

    const ShadowAtlasResult atlas = AllocateShadowAtlas(lights, Vec3(0.0f), settings);
    // Directional punya cascade-nya sendiri; memasukkannya ke atlas berarti
    // membakar tempat untuk bayangan yang sudah digambar di tempat lain.
    CHECK(atlas.firstEntry[0] == -1);
    CHECK(atlas.firstEntry[1] == -1);
    CHECK(atlas.firstEntry[2] >= 0);
    CHECK(atlas.entries.size() == 1);
    // Keduanya memang tidak meminta, jadi tidak ada yang "gagal".
    CHECK(atlas.dropped == 0);
}

TEST_CASE("Atlas yang penuh melaporkan yang tidak kebagian") {
    ShadowAtlasSettings settings;
    settings.resolution = 512;
    settings.maxTile = 256;
    settings.minTile = 256;  // hanya empat ubin muat
    settings.maxFaces = 64;

    std::vector<LightInstance> lights;
    for (int i = 0; i < 10; ++i) {
        lights.push_back(MakeSpot(Vec3(static_cast<float>(i), 0.0f, 2.0f), 10.0f,
                                  Vec3(0.0f, 0.0f, -1.0f)));
    }
    const ShadowAtlasResult atlas = AllocateShadowAtlas(lights, Vec3(0.0f), settings);
    CHECK(atlas.entries.size() == 4);
    // Dilaporkan, bukan didiamkan: lampu yang diam-diam kehilangan bayangannya
    // terlihat sebagai benda yang mengambang.
    CHECK(atlas.dropped == 6);
    CHECK(!TilesOverlap(atlas));
}

TEST_CASE("Point light yang tidak muat seluruh mukanya dibatalkan seluruhnya") {
    ShadowAtlasSettings settings;
    settings.resolution = 512;
    settings.maxTile = 256;
    settings.minTile = 256;  // empat ubin; sebuah point butuh enam
    const std::array<LightInstance, 1> lights{MakePoint(Vec3(0.0f, 0.0f, 2.0f), 10.0f)};

    const ShadowAtlasResult atlas = AllocateShadowAtlas(lights, Vec3(0.0f), settings);
    // Empat dari enam muka menghasilkan bayangan yang berhenti di tengah udara,
    // dan itu lebih buruk daripada tidak ada bayangan sama sekali.
    CHECK(atlas.entries.empty());
    CHECK(atlas.firstEntry[0] == -1);
    CHECK(atlas.dropped == 1);
}

TEST_CASE("Batas jumlah muka membatasi biaya pass, dan urutannya deterministik") {
    ShadowAtlasSettings settings;
    settings.maxFaces = 3;
    std::vector<LightInstance> lights;
    for (int i = 0; i < 8; ++i) {
        lights.push_back(MakeSpot(Vec3(0.0f, 0.0f, 2.0f + static_cast<float>(i)), 10.0f,
                                  Vec3(0.0f, 0.0f, -1.0f)));
    }

    const ShadowAtlasResult first = AllocateShadowAtlas(lights, Vec3(0.0f), settings);
    CHECK(first.entries.size() == 3);
    CHECK(first.dropped == 5);
    // Yang terdekat yang menang — dan lampu 0 memang yang terdekat.
    CHECK(first.firstEntry[0] >= 0);

    // Dijalankan dua kali harus menghasilkan penempatan yang sama persis. Urutan
    // yang bergoyang membuat lampu bergantian kehilangan bayangan, dan kedipan
    // itu jauh lebih mencolok daripada bayangan yang memang tidak ada.
    const ShadowAtlasResult again = AllocateShadowAtlas(lights, Vec3(0.0f), settings);
    REQUIRE(again.entries.size() == first.entries.size());
    for (std::size_t i = 0; i < first.entries.size(); ++i) {
        CHECK(again.entries[i].light == first.entries[i].light);
        CHECK(again.entries[i].x == first.entries[i].x);
        CHECK(again.entries[i].y == first.entries[i].y);
    }
}

TEST_CASE("Keenam muka point light menutupi seluruh arah tanpa NaN") {
    const LightInstance light = MakePoint(Vec3(1.0f, 2.0f, 3.0f), 20.0f);

    // Muka +Y dan -Y memandang lurus sepanjang Y, dan LookAt dengan atas yang
    // sejajar arah pandang menghasilkan matriks yang tidak terdefinisi. Dua dari
    // enam muka lalu berisi NaN, dan bayangannya hilang hanya di atas dan di
    // bawah lampu — gejala yang mudah dikira masalah bias.
    for (uint32_t face = 0; face < 6; ++face) {
        const Mat4 matrix = ShadowFaceMatrix(light, face, 0.05f);
        INFO("muka ", face);
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                CHECK(std::isfinite(matrix[column][row]));
            }
        }
        // Titik satu meter di depan muka ini harus jatuh di dalam kubus satuan.
        const Vec3 ahead = light.position + ShadowFaceDirection(face) * 1.0f;
        const Vec4 clip = matrix * Vec4(ahead, 1.0f);
        REQUIRE(clip.w > 0.0f);
        const Vec3 ndc = Vec3(clip) / clip.w;
        CHECK(std::abs(ndc.x) < 0.01f);
        CHECK(std::abs(ndc.y) < 0.01f);
        CHECK(ndc.z > 0.0f);
        CHECK(ndc.z < 1.0f);
    }
}

TEST_CASE("Frustum spot mengikuti sudut kerucutnya") {
    const LightInstance narrow = MakeSpot(Vec3(0.0f), 20.0f, Vec3(0.0f, 0.0f, -1.0f));
    const Mat4 matrix = ShadowFaceMatrix(narrow, 0, 0.05f);

    // Tepat di sumbu berkas: tengah peta.
    const Vec4 centre = matrix * Vec4(0.0f, 0.0f, -5.0f, 1.0f);
    CHECK(std::abs(centre.x / centre.w) < 0.01f);

    // Di dalam kerucut 35°, masih di dalam peta.
    const float inside = std::tan(30.0f * kDegToRad) * 5.0f;
    const Vec4 near = matrix * Vec4(inside, 0.0f, -5.0f, 1.0f);
    CHECK(std::abs(near.x / near.w) < 1.0f);

    // Jauh di luar kerucut: keluar peta. Frustum yang jauh lebih lebar daripada
    // kerucutnya membuang resolusi pada daerah yang tidak pernah tersinari.
    const float outside = std::tan(60.0f * kDegToRad) * 5.0f;
    const Vec4 far = matrix * Vec4(outside, 0.0f, -5.0f, 1.0f);
    CHECK(std::abs(far.x / far.w) > 1.0f);
}

TEST_CASE("Kepentingan bayangan naik saat lampu mendekat") {
    const LightInstance light = MakePoint(Vec3(0.0f, 0.0f, 0.0f), 10.0f);
    CHECK(ShadowImportance(light, Vec3(0.0f, 0.0f, 100.0f)) <
          ShadowImportance(light, Vec3(0.0f, 0.0f, 30.0f)));
    // Kamera di dalam jangkauannya: lampu itu mengenai seluruh layar.
    CHECK(ShadowImportance(light, Vec3(0.0f, 0.0f, 5.0f)) == doctest::Approx(1.0f));
    // Dan tidak pernah tak hingga meski kamera tepat di posisinya.
    CHECK(std::isfinite(ShadowImportance(light, Vec3(0.0f))));

    LightInstance sun;
    sun.kind = LightKind::Directional;
    CHECK(ShadowImportance(sun, Vec3(0.0f)) == doctest::Approx(0.0f));
}

// --- Pemilih backend trace (GI M0) --------------------------------------------

TEST_CASE("Pemilihan otomatis mengambil ray query bila ada") {
    TraceBackendCaps caps;
    caps.rayQuery = true;
    const TraceBackendSelection selection =
        SelectTraceBackend(caps, TraceBackendPreference::Auto);
    CHECK(selection.kind == TraceBackendKind::RayQuery);
    CHECK(!selection.fellBack);
    CHECK(!selection.reason.empty());

    caps.rayQuery = false;
    const TraceBackendSelection fallback =
        SelectTraceBackend(caps, TraceBackendPreference::Auto);
    CHECK(fallback.kind == TraceBackendKind::Sdf);
    // Bukan penurunan: SDF memang pilihan yang benar di perangkat itu.
    CHECK(!fallback.fellBack);
}

TEST_CASE("Memaksa SDF berlaku justru di perangkat yang punya ray query") {
    // **Ini alasan pemilihnya ada sejak M0, bukan M7.** Mesin pengembangan punya
    // RT core, jadi pemilihan otomatis tidak akan pernah menjalankan jalur SDF —
    // jalur yang justru harus bekerja di setiap GPU. Tanpa paksaan ini, ia
    // berhenti dijalankan siapa pun tanpa ada yang menyadarinya.
    TraceBackendCaps caps;
    caps.rayQuery = true;
    const TraceBackendSelection selection =
        SelectTraceBackend(caps, TraceBackendPreference::ForceSdf);
    CHECK(selection.kind == TraceBackendKind::Sdf);
    CHECK(!selection.fellBack);
}

TEST_CASE("Memaksa ray query di perangkat tanpa dukungan diturunkan, dan disebutkan") {
    TraceBackendCaps caps;
    caps.rayQuery = false;
    const TraceBackendSelection selection =
        SelectTraceBackend(caps, TraceBackendPreference::ForceRayQuery);

    // Diturunkan, bukan ditolak — editor yang menolak jalan tidak bisa dipakai
    // menyunting data. Tapi penurunannya disebutkan, tidak didiamkan: backend
    // yang dipilih diam-diam adalah backend yang tidak ada yang tahu sedang
    // berjalan.
    CHECK(selection.kind == TraceBackendKind::Sdf);
    CHECK(selection.fellBack);
    CHECK(selection.reason.find("Ray query") != std::string::npos);
}

TEST_CASE("Tanpa compute tidak ada backend, dan sebabnya yang disebut") {
    TraceBackendCaps caps;
    caps.compute = false;
    caps.rayQuery = true;
    const TraceBackendSelection selection =
        SelectTraceBackend(caps, TraceBackendPreference::Auto);
    CHECK(selection.kind == TraceBackendKind::Null);
    // Sebab yang sebenarnya, bukan "ray query tidak tersedia" pada perangkat
    // yang masalahnya jauh lebih mendasar.
    CHECK(selection.reason.find("compute") != std::string::npos);
}

TEST_CASE("Backend null selalu meleset dan tidak melangkah") {
    const std::unique_ptr<ITraceBackend> backend = CreateNullTraceBackend();
    REQUIRE(backend != nullptr);
    CHECK(backend->Kind() == TraceBackendKind::Null);

    const TraceResult result =
        backend->Trace(Vec3(0.0f), Vec3(0.0f, 0.0f, 1.0f), 1000.0f);
    CHECK(!result.hit);
    // Nol langkah adalah jawaban yang benar untuk backend yang memang tidak
    // melangkah — heatmap-nya kosong, bukan berisi angka yang mengarang.
    CHECK(result.steps == 0u);
}

TEST_CASE("Setiap nilai enum punya namanya") {
    // Nama dipakai UI dan log. Enum yang bertambah tanpa namanya ikut
    // menghasilkan "None" yang menyesatkan, bukan galat kompilasi.
    for (const TraceBackendKind kind : {TraceBackendKind::Null, TraceBackendKind::Sdf,
                                        TraceBackendKind::RayQuery}) {
        CHECK(std::string(ToString(kind)).size() > 1);
    }
    CHECK(std::string(ToString(TraceBackendKind::Sdf)) != ToString(TraceBackendKind::Null));
    CHECK(std::string(ToString(GiDebugView::MarchSteps)) != ToString(GiDebugView::Off));
    CHECK(std::string(ToString(TraceBackendPreference::ForceSdf)) !=
          ToString(TraceBackendPreference::Auto));
}

// --- SDF clipmap (GI M1) ------------------------------------------------------

namespace {

SdfClipmap MakeClipmap(uint32_t resolution = 128, uint32_t cascades = 3) {
    SdfClipmapSettings settings;
    settings.resolution = resolution;
    settings.cascadeCount = cascades;
    settings.finestVoxelSize = 0.1f;
    settings.voxelScale = 4;
    SdfClipmap clipmap;
    clipmap.Configure(settings);
    return clipmap;
}

/// Apakah dua wilayah bertindihan.
bool RegionsOverlap(const SdfScrollRegion& a, const SdfScrollRegion& b) {
    if (a.cascade != b.cascade) {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (a.max[axis] <= b.min[axis] || b.max[axis] <= a.min[axis]) {
            return false;
        }
    }
    return true;
}

}  // namespace

TEST_CASE("Ukuran kaskade menaik pangkat dua dan menutupi jangkauan yang dijanjikan") {
    const SdfClipmap clipmap = MakeClipmap();
    CHECK(clipmap.VoxelSize(0) == doctest::Approx(0.1f));
    CHECK(clipmap.VoxelSize(1) == doctest::Approx(0.4f));
    CHECK(clipmap.VoxelSize(2) == doctest::Approx(1.6f));
    // 128 voxel × 1,6 m ÷ 2 = ±102,4 m, angka yang disebut rencana.
    CHECK(clipmap.MaxRange() == doctest::Approx(102.4f));
}

TEST_CASE("Pengali voxel dibulatkan ke pangkat dua") {
    // Bukan kerapian: kaskade kasar harus punya kisi yang selaras dengan yang
    // halus, kalau tidak jahitannya bergerak saat kamera maju.
    SdfClipmapSettings settings;
    settings.voxelScale = 3;
    SdfClipmap clipmap;
    clipmap.Configure(settings);
    CHECK(clipmap.Settings().voxelScale == 2u);

    settings.voxelScale = 7;
    clipmap.Configure(settings);
    CHECK(clipmap.Settings().voxelScale == 4u);
}

TEST_CASE("Pembungkusan toroidal benar untuk koordinat negatif") {
    // `%` C++ memberi sisa bertanda: -1 % 128 adalah -1, bukan 127. Indeks
    // negatif membaca di luar tekstur, dan itu tidak muncul sampai kamera
    // melewati titik nol dunia.
    const SdfClipmap clipmap = MakeClipmap(128);
    CHECK(clipmap.WrapAxis(0) == 0u);
    CHECK(clipmap.WrapAxis(127) == 127u);
    CHECK(clipmap.WrapAxis(128) == 0u);
    CHECK(clipmap.WrapAxis(-1) == 127u);
    CHECK(clipmap.WrapAxis(-128) == 0u);
    CHECK(clipmap.WrapAxis(-129) == 127u);
    for (int32_t i = -300; i <= 300; ++i) {
        CHECK(clipmap.WrapAxis(i) < 128u);
    }
}

TEST_CASE("Voxel di sebelah kiri origin tidak tertukar dengan yang di kanan") {
    // Pembagian C++ memotong ke arah nol, jadi -1/128 adalah 0 dan bukan -1.
    // Tanpa pembagian yang membulat ke bawah, voxel di kedua sisi titik nol
    // dunia jatuh ke koordinat yang sama.
    const SdfClipmap clipmap = MakeClipmap();
    CHECK(clipmap.VoxelOf(0, Vec3(0.05f, 0.0f, 0.0f)).x == 0);
    CHECK(clipmap.VoxelOf(0, Vec3(-0.05f, 0.0f, 0.0f)).x == -1);
    CHECK(clipmap.VoxelOf(0, Vec3(-0.15f, 0.0f, 0.0f)).x == -2);
}

TEST_CASE("Penempatan pertama menulis seluruh volume, diam tidak menulis apa pun") {
    SdfClipmap clipmap = MakeClipmap(32, 2);
    const SdfScrollResult first = clipmap.Scroll(Vec3(0.0f));
    CHECK(first.fullRewrite);
    CHECK(first.regions.size() == 2);
    for (const SdfScrollRegion& region : first.regions) {
        CHECK(region.VoxelCount() == 32 * 32 * 32);
    }

    // Kamera diam: tidak ada satu voxel pun yang basi.
    const SdfScrollResult again = clipmap.Scroll(Vec3(0.0f));
    CHECK(!again.fullRewrite);
    CHECK(again.regions.empty());
}

TEST_CASE("Bergerak satu voxel hanya menuliskan satu lempeng tepi") {
    // **Inilah alasan pengalamatan toroidal ada.** Tanpa ini, satu voxel
    // pergerakan berarti 32.768 voxel ditulis ulang di kaskade ini saja.
    SdfClipmap clipmap = MakeClipmap(32, 1);
    clipmap.Scroll(Vec3(0.0f));

    const SdfScrollResult moved = clipmap.Scroll(Vec3(0.1f, 0.0f, 0.0f));
    CHECK(!moved.fullRewrite);
    REQUIRE(moved.regions.size() == 1);
    // Satu lempeng: 1 × 32 × 32.
    CHECK(moved.regions[0].VoxelCount() == 32 * 32);
}

TEST_CASE("Gerak diagonal menghasilkan lempeng yang tidak bertindihan") {
    SdfClipmap clipmap = MakeClipmap(32, 1);
    clipmap.Scroll(Vec3(0.0f));
    const SdfScrollResult moved = clipmap.Scroll(Vec3(0.3f, 0.2f, -0.5f));

    CHECK(!moved.fullRewrite);
    CHECK(moved.regions.size() == 3);
    // Sudutnya tidak boleh ditulis dua kali — bukan demi kebenaran melainkan
    // supaya jumlah voxel yang dilaporkan benar-benar jumlah pekerjaan.
    for (std::size_t i = 0; i < moved.regions.size(); ++i) {
        for (std::size_t j = i + 1; j < moved.regions.size(); ++j) {
            INFO("wilayah ", i, " dan ", j);
            CHECK(!RegionsOverlap(moved.regions[i], moved.regions[j]));
        }
    }

    int64_t total = 0;
    for (const SdfScrollRegion& region : moved.regions) {
        total += region.VoxelCount();
    }
    // 3 + 2 + 5 lempeng dari volume 32³, dipotong supaya tidak tumpang tindih.
    CHECK(total < 32 * 32 * 32);
    CHECK(total > 0);
}

TEST_CASE("Lompatan lebih jauh daripada lebar kaskade menulis ulang seluruhnya") {
    SdfClipmap clipmap = MakeClipmap(32, 1);
    clipmap.Scroll(Vec3(0.0f));
    // 32 voxel × 0,1 m = 3,2 m lebarnya; lompat 100 m.
    const SdfScrollResult jumped = clipmap.Scroll(Vec3(100.0f, 0.0f, 0.0f));
    CHECK(jumped.fullRewrite);
    REQUIRE(jumped.regions.size() == 1);
    // Menuliskan tiga lempeng yang saling tumpang tindih penuh justru lebih
    // mahal daripada menulis volumenya sekali.
    CHECK(jumped.regions[0].VoxelCount() == 32 * 32 * 32);
}

TEST_CASE("Kaskade kasar tidak ikut bergeser untuk gerak yang halus") {
    // Kaskade 1 punya voxel 0,4 m. Bergerak 0,1 m menggeser kaskade 0 satu
    // voxel dan tidak menyentuh kaskade 1 sama sekali — itulah gunanya
    // mengancing tiap kaskade ke ukuran voxelnya sendiri, bukan ke yang
    // terhalus.
    SdfClipmap clipmap = MakeClipmap(32, 2);
    clipmap.Scroll(Vec3(0.0f));
    const SdfScrollResult moved = clipmap.Scroll(Vec3(0.1f, 0.0f, 0.0f));

    REQUIRE(moved.regions.size() == 1);
    CHECK(moved.regions[0].cascade == 0u);
}

TEST_CASE("Pemilihan kaskade mengambil yang terhalus yang memuat titiknya") {
    SdfClipmap clipmap = MakeClipmap(128, 3);
    clipmap.Scroll(Vec3(0.0f));

    // Dekat kamera: kaskade terhalus (±6,4 m).
    CHECK(clipmap.CascadeFor(Vec3(1.0f, 0.0f, 0.0f)) == 0);
    // Di luar kaskade 0 tapi di dalam kaskade 1 (±25,6 m).
    CHECK(clipmap.CascadeFor(Vec3(15.0f, 0.0f, 0.0f)) == 1);
    // Di luar kaskade 1 tapi di dalam kaskade 2 (±102,4 m).
    CHECK(clipmap.CascadeFor(Vec3(60.0f, 0.0f, 0.0f)) == 2);
    // Di luar semuanya.
    CHECK(clipmap.CascadeFor(Vec3(500.0f, 0.0f, 0.0f)) == -1);
}

TEST_CASE("Jarak bertanda bolak-balik lewat penyandian delapan bit") {
    const SdfClipmap clipmap = MakeClipmap();
    for (uint32_t cascade = 0; cascade < 3; ++cascade) {
        const float band = clipmap.BandRadius(cascade);
        for (const float fraction : {-0.9f, -0.5f, 0.0f, 0.25f, 0.9f}) {
            const float distance = band * fraction;
            const float roundTrip =
                clipmap.DecodeDistance(cascade, clipmap.EncodeDistance(cascade, distance));
            INFO("kaskade ", cascade, " jarak ", distance);
            CHECK(roundTrip == doctest::Approx(distance).epsilon(0.01));
        }
        // Nol jarak ada tepat di tengah rentang — itulah yang membuat jarak
        // bertanda terwakili. Menyimpan jarak tak bertanda membuat sphere
        // tracing tidak bisa tahu ia sudah di dalam benda.
        CHECK(clipmap.EncodeDistance(cascade, 0.0f) == doctest::Approx(0.5f));
        // Di luar pita, nilainya jenuh, bukan membungkus.
        CHECK(clipmap.EncodeDistance(cascade, band * 100.0f) == doctest::Approx(1.0f));
        CHECK(clipmap.EncodeDistance(cascade, -band * 100.0f) == doctest::Approx(0.0f));
    }
}

TEST_CASE("Texel yang dipakai ulang setelah bergeser memang texel yang sama") {
    // Inti pengalamatan toroidal: sebuah voxel dunia yang masih berada di dalam
    // kaskade sesudah pergeseran harus memetakan ke texel yang sama persis —
    // kalau tidak, isinya tetap harus ditulis ulang dan penghematannya nol.
    SdfClipmap clipmap = MakeClipmap(32, 1);
    clipmap.Scroll(Vec3(0.0f));

    const glm::ivec3 sample(3, 4, 5);
    const glm::uvec3 before = clipmap.TexelOf(0, sample);
    clipmap.Scroll(Vec3(0.5f, 0.0f, 0.0f));
    const glm::uvec3 after = clipmap.TexelOf(0, sample);
    CHECK(before == after);
}

// --- Pemecahan wilayah yang membungkus, dan sphere tracing ---------------------

namespace {

/// Bola analitik, dipakai sebagai medan jarak yang jawabannya diketahui.
SdfVolume::DistanceField SphereField(const Vec3& centre, float radius) {
    return [centre, radius](const Vec3& p) { return glm::length(p - centre) - radius; };
}

/// Bidang tanah pada y = height.
SdfVolume::DistanceField PlaneField(float height) {
    return [height](const Vec3& p) { return p.y - height; };
}

SdfVolume MakeVolume(uint32_t resolution = 64, uint32_t cascades = 2,
                     float finest = 0.1f) {
    SdfClipmapSettings settings;
    settings.resolution = resolution;
    settings.cascadeCount = cascades;
    settings.finestVoxelSize = finest;
    settings.voxelScale = 4;
    SdfVolume volume;
    volume.Configure(settings);
    return volume;
}

}  // namespace

TEST_CASE("Wilayah yang tidak membungkus tetap satu kotak") {
    SdfClipmap clipmap = MakeClipmap(32, 1);
    clipmap.Scroll(Vec3(1.6f, 1.6f, 1.6f));  // origin voxel = 0

    SdfScrollRegion region;
    region.min = glm::ivec3(4, 4, 4);
    region.max = glm::ivec3(8, 8, 8);
    std::vector<SdfClipmap::TexelBox> boxes;
    clipmap.SplitWrapped(region, boxes);

    REQUIRE(boxes.size() == 1);
    CHECK(boxes[0].min == glm::uvec3(4, 4, 4));
    CHECK(boxes[0].max == glm::uvec3(8, 8, 8));
    CHECK(boxes[0].worldMin == glm::ivec3(4, 4, 4));
}

TEST_CASE("Wilayah yang membelah satu sumbu jadi dua kotak") {
    // Pengalamatan toroidal berarti wilayah yang bersambung di dunia melompati
    // tepi tekstur dan muncul kembali di sisi seberangnya. Menyalinnya sebagai
    // satu kotak akan menimpa texel milik bagian dunia yang sama sekali lain.
    const SdfClipmap clipmap = MakeClipmap(32, 1);
    SdfScrollRegion region;
    region.min = glm::ivec3(30, 0, 0);
    region.max = glm::ivec3(34, 4, 4);
    std::vector<SdfClipmap::TexelBox> boxes;
    clipmap.SplitWrapped(region, boxes);

    REQUIRE(boxes.size() == 2);
    uint32_t total = 0;
    for (const SdfClipmap::TexelBox& box : boxes) {
        total += box.VoxelCount();
        // Tidak satu pun potongan boleh melewati tepi tekstur.
        CHECK(box.max.x <= 32u);
        CHECK(box.max.y <= 32u);
        CHECK(box.max.z <= 32u);
    }
    CHECK(total == 4u * 4u * 4u);
}

TEST_CASE("Wilayah yang membelah ketiga sumbu jadi delapan kotak") {
    const SdfClipmap clipmap = MakeClipmap(32, 1);
    SdfScrollRegion region;
    region.min = glm::ivec3(30, 30, 30);
    region.max = glm::ivec3(34, 34, 34);
    std::vector<SdfClipmap::TexelBox> boxes;
    clipmap.SplitWrapped(region, boxes);

    CHECK(boxes.size() == 8);
    uint32_t total = 0;
    for (const SdfClipmap::TexelBox& box : boxes) {
        total += box.VoxelCount();
    }
    // Delapan potongan yang dijumlahkan tetap wilayah yang sama — tidak ada
    // voxel yang hilang, dan tidak ada yang terhitung dua kali.
    CHECK(total == 4u * 4u * 4u);
}

TEST_CASE("Wilayah selebar tekstur menutupi sumbunya tepat sekali") {
    const SdfClipmap clipmap = MakeClipmap(32, 1);
    SdfScrollRegion region;
    region.min = glm::ivec3(5, 0, 0);
    region.max = glm::ivec3(5 + 40, 2, 2);  // lebih lebar daripada teksturnya
    std::vector<SdfClipmap::TexelBox> boxes;
    clipmap.SplitWrapped(region, boxes);

    uint32_t total = 0;
    for (const SdfClipmap::TexelBox& box : boxes) {
        total += box.VoxelCount();
    }
    // Dijepit ke lebar tekstur: menulis 40 texel pada sumbu selebar 32 hanya
    // berarti delapan di antaranya ditimpa dua kali.
    CHECK(total == 32u * 2u * 2u);
}

TEST_CASE("Volume mengembalikan jarak yang cocok dengan medan analitiknya") {
    SdfVolume volume = MakeVolume(64, 1, 0.1f);
    volume.Clipmap().Scroll(Vec3(0.0f));
    const Vec3 centre(0.0f, 0.0f, 0.0f);
    volume.FillAll(SphereField(centre, 1.0f));

    // Di dekat permukaan, di dalam pita yang disimpan, jaraknya harus cocok
    // dengan jawaban analitiknya dalam batas kuantisasi 8 bit.
    for (const float radius : {0.85f, 0.95f, 1.0f, 1.1f, 1.25f}) {
        const Vec3 point = centre + Vec3(radius, 0.0f, 0.0f);
        float sampled = 0.0f;
        REQUIRE(volume.Sample(point, sampled));
        const float exact = radius - 1.0f;
        INFO("jari-jari ", radius, " sampel ", sampled, " tepat ", exact);
        // Pita 4 voxel = ±0,4 m dipetakan ke 256 langkah → satu langkah ~3 mm.
        CHECK(sampled == doctest::Approx(exact).epsilon(0.05).scale(0.05));
    }
}

TEST_CASE("Di luar pita nilainya jenuh, bukan membungkus") {
    SdfVolume volume = MakeVolume(64, 1, 0.1f);
    volume.Clipmap().Scroll(Vec3(0.0f));
    volume.FillAll(SphereField(Vec3(0.0f), 0.5f));

    float sampled = 0.0f;
    REQUIRE(volume.Sample(Vec3(2.5f, 0.0f, 0.0f), sampled));
    const float band = volume.Clipmap().BandRadius(0);
    // Jauh dari permukaan: jenuh di tepi pita, bukan melompat ke nilai negatif.
    CHECK(sampled == doctest::Approx(band).epsilon(0.02));
    CHECK(sampled > 0.0f);
}

TEST_CASE("Jarak di dalam benda bertanda negatif") {
    // Jarak tak bertanda membuat sphere tracing tidak bisa tahu ia sudah di
    // dalam benda, dan ray yang mulai di dalam dinding tidak pernah keluar.
    SdfVolume volume = MakeVolume(64, 1, 0.1f);
    volume.Clipmap().Scroll(Vec3(0.0f));
    volume.FillAll(SphereField(Vec3(0.0f), 1.0f));

    float sampled = 0.0f;
    REQUIRE(volume.Sample(Vec3(0.8f, 0.0f, 0.0f), sampled));
    CHECK(sampled < 0.0f);
}

TEST_CASE("Pembaruan parsial hanya menulis wilayah yang basi") {
    // Inilah yang membuktikan penghematan toroidal terwujud, bukan sekadar
    // tercatat di komentar.
    SdfVolume volume = MakeVolume(32, 1, 0.1f);
    volume.Fill(volume.Clipmap().Scroll(Vec3(0.0f)), SphereField(Vec3(0.0f), 1.0f));
    const uint64_t initial = volume.WrittenVoxels();
    CHECK(initial == 32u * 32u * 32u);

    volume.ResetWriteCount();
    volume.Fill(volume.Clipmap().Scroll(Vec3(0.1f, 0.0f, 0.0f)), SphereField(Vec3(0.0f), 1.0f));
    // Satu lempeng, bukan seluruh volume.
    CHECK(volume.WrittenVoxels() == 32u * 32u);
}

TEST_CASE("Sphere tracing menemukan bola pada jarak yang benar") {
    SdfVolume volume = MakeVolume(128, 1, 0.05f);
    volume.Clipmap().Scroll(Vec3(0.0f));
    volume.FillAll(SphereField(Vec3(0.0f, 0.0f, 2.0f), 0.5f));

    const std::unique_ptr<ITraceBackend> backend = CreateSdfTraceBackend(volume);
    const TraceResult result = backend->Trace(Vec3(0.0f), Vec3(0.0f, 0.0f, 1.0f), 10.0f);

    REQUIRE(result.hit);
    // Permukaan bola ada di z = 1,5.
    CHECK(result.distance == doctest::Approx(1.5f).epsilon(0.06));
    CHECK(result.position.z == doctest::Approx(1.5f).epsilon(0.06));
    // Sphere tracing, bukan langkah tetap: melintasi 1,5 m dengan voxel 5 cm
    // butuh 30 langkah kalau langkahnya tetap.
    CHECK(result.steps < 20u);
    CHECK(result.steps > 0u);
}

TEST_CASE("Ray yang meleset melaporkan miss, bukan hit palsu") {
    SdfVolume volume = MakeVolume(128, 1, 0.05f);
    volume.Clipmap().Scroll(Vec3(0.0f));
    volume.FillAll(SphereField(Vec3(0.0f, 0.0f, 2.0f), 0.5f));

    const std::unique_ptr<ITraceBackend> backend = CreateSdfTraceBackend(volume);
    // Menyerempet jauh di samping bolanya.
    const TraceResult result = backend->Trace(Vec3(2.0f, 0.0f, 0.0f), Vec3(0.0f, 0.0f, 1.0f),
                                              10.0f);
    CHECK(!result.hit);
    // Tetap melaporkan langkah: heatmap-nya yang menunjukkan ray mana yang mahal.
    CHECK(result.steps > 0u);
}

TEST_CASE("Ray sejajar bidang tanah tidak menembusnya") {
    // Geometri tipis adalah risiko utama SDF. Bidang tak berhingga bukan
    // geometri tipis, tapi ia menguji hal yang sama: sphere tracing yang
    // melangkah lebih jauh daripada jarak yang dijaminnya akan lolos.
    SdfVolume volume = MakeVolume(128, 1, 0.05f);
    volume.Clipmap().Scroll(Vec3(0.0f, -1.0f, 0.0f));
    volume.FillAll(PlaneField(-1.0f));

    const std::unique_ptr<ITraceBackend> backend = CreateSdfTraceBackend(volume);
    const TraceResult result =
        backend->Trace(Vec3(0.0f, 0.5f, 0.0f), glm::normalize(Vec3(0.0f, -1.0f, 0.2f)), 5.0f);
    REQUIRE(result.hit);
    CHECK(result.position.y == doctest::Approx(-1.0f).epsilon(0.1));
}

TEST_CASE("Langkah dibatasi, dan batasnya dilaporkan") {
    SdfVolume volume = MakeVolume(64, 1, 0.1f);
    volume.Clipmap().Scroll(Vec3(0.0f));
    // Medan yang selalu mengembalikan nol jarak: setiap langkah maju
    // sesedikit mungkin. Ini kasus terburuk sphere tracing.
    volume.FillAll([](const Vec3&) { return 0.0f; });

    const std::unique_ptr<ITraceBackend> backend = CreateSdfTraceBackend(volume, 8);
    const TraceResult result = backend->Trace(Vec3(0.0f), Vec3(1.0f, 0.0f, 0.0f), 100.0f);
    // Nol jarak berarti "permukaan di sini", jadi ia hit di langkah pertama.
    // Yang penting: ia tidak pernah melebihi anggarannya.
    CHECK(result.steps <= 8u);
}

TEST_CASE("Medan jarak kotak benar di luar maupun di dalam") {
    // Rumus jarak kotak punya dua suku: yang pertama benar di luar, yang kedua
    // di dalam. Memakai salah satunya saja menghasilkan jarak yang salah tepat
    // di sisi yang lain — dan yang di dalam kotaklah yang menentukan apakah ray
    // yang mulai di dalam dinding bisa keluar.
    MeshInstance box;
    box.transform = Mat4(1.0f);
    box.boundsMin = Vec3(-1.0f);
    box.boundsMax = Vec3(1.0f);
    const std::array<MeshInstance, 1> meshes{box};

    BoxSceneField built;
    built.Build(meshes);
    const auto field = [&built](const Vec3& point) { return built.Distance(point); };

    // Di permukaan.
    CHECK(field(Vec3(1.0f, 0.0f, 0.0f)) == doctest::Approx(0.0f).epsilon(0.001));
    // Di luar, tegak lurus sebuah sisi.
    CHECK(field(Vec3(2.0f, 0.0f, 0.0f)) == doctest::Approx(1.0f).epsilon(0.001));
    // Di luar, di seberang sebuah sudut.
    CHECK(field(Vec3(2.0f, 2.0f, 2.0f)) == doctest::Approx(std::sqrt(3.0f)).epsilon(0.001));
    // Di dalam: negatif, dan besarnya jarak ke sisi terdekat.
    CHECK(field(Vec3(0.0f)) == doctest::Approx(-1.0f).epsilon(0.001));
    CHECK(field(Vec3(0.5f, 0.0f, 0.0f)) == doctest::Approx(-0.5f).epsilon(0.001));
}

TEST_CASE("Skala tak seragam tidak pernah melebih-lebihkan ruang kosong") {
    // Jarak yang diukur di ruang lokal berpadanan dengan antara d·min(skala)
    // dan d·maks(skala) di dunia. Memakai yang terbesar akan membuat sphere
    // tracing melangkah lebih jauh daripada ruang yang benar-benar kosong —
    // dan itu menembus dinding.
    MeshInstance box;
    box.transform = glm::scale(Mat4(1.0f), Vec3(4.0f, 1.0f, 4.0f));
    box.boundsMin = Vec3(-0.5f);
    box.boundsMax = Vec3(0.5f);
    const std::array<MeshInstance, 1> meshes{box};

    BoxSceneField built;
    built.Build(meshes);
    const auto field = [&built](const Vec3& point) { return built.Distance(point); };

    // Kotaknya 4×1×4, jadi permukaan atasnya di y = 0,5. Sebuah titik 1 m di
    // atasnya berjarak tepat 1 m.
    const float above = field(Vec3(0.0f, 1.5f, 0.0f));
    CHECK(above <= 1.0f + 1e-3f);
    CHECK(above > 0.0f);

    // Dan di setiap arah, jaraknya tidak boleh melebihi jarak sesungguhnya ke
    // permukaan terdekat — diuji dengan menembakkan langkah sebesar jarak itu
    // dan memastikan titiknya belum melewati permukaan.
    for (const Vec3 direction : {Vec3(1, 0, 0), Vec3(0, -1, 0), Vec3(0.6f, -0.8f, 0.0f)}) {
        const Vec3 start(0.0f, 2.0f, 0.0f);
        const float distance = field(start);
        const Vec3 stepped = start + glm::normalize(direction) * distance;
        INFO("arah (", direction.x, ",", direction.y, ",", direction.z, ")");
        CHECK(field(stepped) >= -1e-3f);
    }
}

// --- Kriteria selesai M1: depth SDF terhadap kebenaran raster -----------------

namespace {

/// Perpotongan sinar dengan kotak berorientasi — kebenaran analitiknya.
///
/// Ini yang menggantikan "uji visual berdampingan" yang disebut rencana. Depth
/// buffer raster berisi perpotongan sinar dengan segitiga; untuk geometri kotak
/// perpotongan itu punya bentuk tertutup, jadi ia bisa dibandingkan sebagai
/// angka alih-alih sebagai penilaian mata. Test yang bisa gagal sendiri lebih
/// berguna daripada dua tangkapan layar yang harus ditatap orang.
bool RayBoxDistance(const Vec3& origin, const Vec3& direction, const Mat4& model,
                    const Vec3& halfExtent, float& outDistance) {
    const Mat4 inverse = glm::inverse(model);
    const Vec3 localOrigin = Vec3(inverse * Vec4(origin, 1.0f));
    const Vec3 localDirection = Vec3(inverse * Vec4(direction, 0.0f));

    float tMin = -std::numeric_limits<float>::max();
    float tMax = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(localDirection[axis]) < 1e-8f) {
            if (std::abs(localOrigin[axis]) > halfExtent[axis]) {
                return false;
            }
            continue;
        }
        const float inverseDirection = 1.0f / localDirection[axis];
        float near = (-halfExtent[axis] - localOrigin[axis]) * inverseDirection;
        float far = (halfExtent[axis] - localOrigin[axis]) * inverseDirection;
        if (near > far) {
            std::swap(near, far);
        }
        tMin = std::max(tMin, near);
        tMax = std::min(tMax, far);
        if (tMin > tMax) {
            return false;
        }
    }
    if (tMax < 0.0f) {
        return false;
    }
    // Jarak diukur di ruang lokal, dan modelnya berskala seragam pada test ini,
    // jadi t yang sama berlaku di dunia.
    outDistance = tMin >= 0.0f ? tMin : tMax;
    return true;
}

}  // namespace

TEST_CASE("Depth sphere tracing cocok dengan perpotongan analitiknya") {
    // Adegan: sebuah kotak 2×2×2 di depan kamera, dan bidang tanah.
    MeshInstance box;
    box.transform = glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, 4.0f));
    box.boundsMin = Vec3(-1.0f);
    box.boundsMax = Vec3(1.0f);
    const std::array<MeshInstance, 1> meshes{box};

    SdfClipmapSettings settings;
    settings.resolution = 128;
    settings.cascadeCount = 2;
    settings.finestVoxelSize = 0.05f;
    SdfVolume volume;
    volume.Configure(settings);
    volume.Clipmap().Scroll(Vec3(0.0f, 0.0f, 3.0f));

    BoxSceneField field;
    field.Build(meshes);
    volume.FillAll(field);

    const std::unique_ptr<ITraceBackend> backend = CreateSdfTraceBackend(volume, 96);
    const Mat4 model = glm::scale(glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, 4.0f)), Vec3(2.0f));

    const float voxel = volume.Clipmap().VoxelSize(0);
    int compared = 0;
    float worst = 0.0f;

    // Kisi sinar yang menyapu permukaan depan kotak.
    for (const float x : {-0.6f, -0.3f, 0.0f, 0.3f, 0.6f}) {
        for (const float y : {-0.6f, -0.3f, 0.0f, 0.3f, 0.6f}) {
            const Vec3 origin(0.0f, 0.0f, 0.5f);
            const Vec3 direction = glm::normalize(Vec3(x, y, 3.0f));

            float analytic = 0.0f;
            const bool analyticHit =
                RayBoxDistance(origin, direction, model, Vec3(0.5f), analytic);
            const TraceResult traced = backend->Trace(origin, direction, 20.0f);

            INFO("sinar (", x, ",", y, ")");
            // Sepakat soal kena atau tidaknya lebih dulu — selisih jarak pada
            // sinar yang salah satunya bilang meleset tidak berarti apa-apa.
            REQUIRE(traced.hit == analyticHit);
            if (!analyticHit) {
                continue;
            }
            const float error = std::abs(traced.distance - analytic);
            worst = std::max(worst, error);
            ++compared;
            // Dua voxel: satu untuk kuantisasi 8-bit, satu untuk ambang berhenti
            // sphere tracing yang memang setengah voxel di depan permukaan.
            CHECK(error < voxel * 2.0f);
        }
    }
    REQUIRE(compared >= 20);
    INFO("selisih terburuk ", worst, " m pada voxel ", voxel, " m");
    CHECK(worst < voxel * 2.0f);
}

TEST_CASE("Sinar yang melewati sisi kotak sepakat soal meleset") {
    MeshInstance box;
    box.transform = glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, 4.0f));
    box.boundsMin = Vec3(-0.5f);
    box.boundsMax = Vec3(0.5f);
    const std::array<MeshInstance, 1> meshes{box};

    SdfClipmapSettings settings;
    settings.resolution = 128;
    settings.cascadeCount = 1;
    settings.finestVoxelSize = 0.05f;
    SdfVolume volume;
    volume.Configure(settings);
    volume.Clipmap().Scroll(Vec3(0.0f, 0.0f, 3.0f));

    BoxSceneField field;
    field.Build(meshes);
    volume.FillAll(field);

    const std::unique_ptr<ITraceBackend> backend = CreateSdfTraceBackend(volume, 96);
    // Jauh di samping kotaknya: harus meleset, bukan mengenai sesuatu yang tidak
    // ada. Permukaan hantu adalah gejala paling khas medan jarak yang salah
    // disandikan.
    const TraceResult wide =
        backend->Trace(Vec3(0.0f, 0.0f, 0.5f), glm::normalize(Vec3(1.0f, 0.0f, 1.0f)), 20.0f);
    CHECK(!wide.hit);
}


// --- M2: lapis screen-space -------------------------------------------------

namespace {

/// Depth buffer sintetis: sebuah bidang tegak lurus sumbu Z pada `planeZ`,
/// dirasterkan dengan menembakkan satu sinar per piksel.
///
/// **Dibangun dari perpotongan analitik, bukan dari rasterizer.** Yang diuji di
/// sini adalah penelusurnya; depth buffer yang dihasilkan jalur lain akan
/// membuat kegagalan mereka tampak sebagai kegagalan penelusur.
std::vector<float> RasterizePlane(const Mat4& viewProj, const Mat4& invViewProj, uint32_t width,
                                  uint32_t height, float planeZ, const Vec2& halfExtent) {
    std::vector<float> depth(static_cast<std::size_t>(width) * height, 0.0f);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const Vec2 uv((static_cast<float>(x) + 0.5f) / static_cast<float>(width),
                          (static_cast<float>(y) + 0.5f) / static_cast<float>(height));
            const Vec4 ndc(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, 0.5f, 1.0f);
            const Vec4 point = invViewProj * ndc;
            const Vec3 world = Vec3(point) / point.w;
            // Kamera di titik asal menghadap -Z, jadi sinar melalui piksel ini
            // adalah arah ke titik yang barusan dibalik proyeksinya.
            const Vec3 ray = glm::normalize(world);
            if (std::abs(ray.z) < 1e-6f) {
                continue;
            }
            const float t = planeZ / ray.z;
            if (t <= 0.0f) {
                continue;
            }
            const Vec3 hit = ray * t;
            if (std::abs(hit.x) > halfExtent.x || std::abs(hit.y) > halfExtent.y) {
                continue;  // di luar bidang: langit, depth reversed-Z nol
            }
            const Vec4 clip = viewProj * Vec4(hit, 1.0f);
            depth[static_cast<std::size_t>(y) * width + x] = clip.z / clip.w;
        }
    }
    return depth;
}

}  // namespace

TEST_CASE("Mip HiZ menyimpan permukaan terdekat, bukan terjauh") {
    // Reversed-Z: depth terbesar berarti paling dekat. Mip yang menyimpan yang
    // terkecil membuat tiap sel melaporkan permukaan terjauhnya sebagai
    // penghalang, dan tiap sinar menembus geometri yang justru paling dekat.
    const std::vector<float> depth{0.1f, 0.9f, 0.2f, 0.3f,
                                   0.4f, 0.5f, 0.6f, 0.7f,
                                   0.0f, 0.0f, 0.0f, 0.0f,
                                   0.0f, 0.0f, 0.0f, 0.0f};
    HiZPyramid pyramid;
    pyramid.Build(4, 4, depth);

    REQUIRE(pyramid.LevelCount() == 3);
    CHECK(pyramid.Size(1) == glm::uvec2(2, 2));
    CHECK(pyramid.Size(2) == glm::uvec2(1, 1));

    CHECK(pyramid.At(1, 0, 0) == doctest::Approx(0.9f));
    CHECK(pyramid.At(1, 1, 0) == doctest::Approx(0.7f));
    CHECK(pyramid.At(2, 0, 0) == doctest::Approx(0.9f));
}

TEST_CASE("Mip HiZ tidak membuang baris terakhir dari ukuran ganjil") {
    // Ukuran tiap tingkat mengikuti aturan mip Vulkan — dibulatkan ke bawah —
    // karena bentuk yang membulatkannya ke atas menghasilkan satu tingkat lebih
    // banyak daripada yang boleh dimiliki sebuah image, dan `vkCreateImage`
    // menolaknya. Barisnya tetap tidak boleh hilang: yang berubah bukan
    // ukurannya melainkan cakupannya, dan texel terakhir merangkum sisa
    // barisnya.
    std::vector<float> depth(9, 0.1f);
    depth[8] = 0.95f;  // pojok kanan bawah, satu-satunya texel di baris ganjil
    HiZPyramid pyramid;
    pyramid.Build(3, 3, depth);

    REQUIRE(pyramid.LevelCount() == 2);
    CHECK(pyramid.Size(1) == glm::uvec2(1, 1));
    CHECK(pyramid.At(1, 0, 0) == doctest::Approx(0.95f));
    // Dan jumlah tingkatnya persis yang diizinkan Vulkan untuk ukuran itu.
    CHECK(HiZPyramid::LevelsFor(3, 3) == 2);
    CHECK(HiZPyramid::LevelsFor(1280, 768) == 11);
}

TEST_CASE("Penelusuran screen-space menemukan dinding di depan kamera") {
    constexpr uint32_t kWidth = 64;
    constexpr uint32_t kHeight = 64;
    const Mat4 view = LookAt(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 1.0f, 0.0f));
    const Mat4 proj = PerspectiveReversedZ(60.0f * kDegToRad, 1.0f, 0.1f, 100.0f);
    ScreenTraceView screen;
    screen.viewProj = proj * view;
    screen.invViewProj = glm::inverse(screen.viewProj);

    const std::vector<float> depth =
        RasterizePlane(screen.viewProj, screen.invViewProj, kWidth, kHeight, -10.0f,
                       Vec2(100.0f));
    HiZPyramid pyramid;
    pyramid.Build(kWidth, kHeight, depth);

    ScreenTraceSettings settings;
    settings.thickness = 1.0f;

    // Sinar dari titik di depan dinding, menuju dinding.
    const ScreenTraceResult straight = TraceScreenSpace(
        pyramid, screen, Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 0.0f, -1.0f), 30.0f, settings);
    REQUIRE(straight.hit);
    CHECK(straight.distance == doctest::Approx(9.0f).epsilon(0.02));
    CHECK(straight.steps <= settings.maxSteps);

    // Miring, tapi tetap mengenai dinding yang sama: jaraknya lebih jauh persis
    // sebesar 1/cos.
    const Vec3 slanted = glm::normalize(Vec3(0.3f, 0.0f, -1.0f));
    const ScreenTraceResult oblique =
        TraceScreenSpace(pyramid, screen, Vec3(0.0f, 0.0f, -1.0f), slanted, 30.0f, settings);
    REQUIRE(oblique.hit);
    CHECK(oblique.distance == doctest::Approx(9.0f / slanted.z * -1.0f).epsilon(0.05));
}

TEST_CASE("Sinar yang keluar layar dilaporkan sebagai tidak tahu, bukan sebagai kosong") {
    // Membedakan keduanya adalah seluruh gunanya jenjang: yang keluar layar
    // harus diteruskan ke SDF, sedangkan yang benar-benar kosong sudah merupakan
    // jawaban. Menyamakannya menghasilkan lubang gelap tepat di tepi layar.
    constexpr uint32_t kSize = 64;
    const Mat4 view = LookAt(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 1.0f, 0.0f));
    const Mat4 proj = PerspectiveReversedZ(60.0f * kDegToRad, 1.0f, 0.1f, 100.0f);
    ScreenTraceView screen;
    screen.viewProj = proj * view;
    screen.invViewProj = glm::inverse(screen.viewProj);

    const std::vector<float> depth = RasterizePlane(screen.viewProj, screen.invViewProj, kSize,
                                                    kSize, -10.0f, Vec2(100.0f));
    HiZPyramid pyramid;
    pyramid.Build(kSize, kSize, depth);

    // Menjauh dari kamera ke samping: keluar layar sebelum mengenai apa pun.
    const ScreenTraceResult sideways =
        TraceScreenSpace(pyramid, screen, Vec3(0.0f, 0.0f, -1.0f),
                         glm::normalize(Vec3(1.0f, 0.0f, -0.05f)), 30.0f, ScreenTraceSettings{});
    CHECK(!sideways.hit);
    CHECK(sideways.leftScreen);

    // Ke belakang kamera: tidak punya proyeksi yang berarti sama sekali.
    const ScreenTraceResult behind =
        TraceScreenSpace(pyramid, screen, Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 0.0f, 1.0f), 30.0f,
                         ScreenTraceSettings{});
    CHECK(!behind.hit);
    CHECK(behind.leftScreen);
}

TEST_CASE("Sinar yang lewat di belakang dinding tidak mengenai siluetnya") {
    // Depth buffer hanya menyimpan permukaan terdepan. Tanpa uji ketebalan,
    // setiap sinar yang lewat jauh di belakang sebuah benda melaporkan kena di
    // tepinya — cacat khas penelusuran screen-space.
    constexpr uint32_t kSize = 128;
    const Mat4 view = LookAt(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 1.0f, 0.0f));
    const Mat4 proj = PerspectiveReversedZ(60.0f * kDegToRad, 1.0f, 0.1f, 100.0f);
    ScreenTraceView screen;
    screen.viewProj = proj * view;
    screen.invViewProj = glm::inverse(screen.viewProj);

    // Papan sempit di tengah layar, pada z = -5.
    const std::vector<float> depth =
        RasterizePlane(screen.viewProj, screen.invViewProj, kSize, kSize, -5.0f, Vec2(1.0f));
    HiZPyramid pyramid;
    pyramid.Build(kSize, kSize, depth);

    ScreenTraceSettings thin;
    thin.thickness = 0.25f;
    // Mulai dari sisi kanan papan, jauh di belakangnya, mengarah melintas.
    const ScreenTraceResult behind = TraceScreenSpace(
        pyramid, screen, Vec3(2.5f, 0.0f, -20.0f), glm::normalize(Vec3(-1.0f, 0.0f, 0.0f)),
        10.0f, thin);
    CHECK(!behind.hit);

    // Ketebalan yang sangat besar membuat papan yang sama menjadi penghalang.
    // Uji ini yang membuktikan gagalnya yang di atas memang karena ketebalan,
    // bukan karena sinarnya tidak pernah sampai ke sana.
    ScreenTraceSettings thick;
    thick.thickness = 100.0f;
    const ScreenTraceResult swallowed = TraceScreenSpace(
        pyramid, screen, Vec3(2.5f, 0.0f, -20.0f), glm::normalize(Vec3(-1.0f, 0.0f, 0.0f)),
        10.0f, thick);
    CHECK(swallowed.hit);
}

TEST_CASE("Jenjang menjawab dengan lapis yang benar") {
    constexpr uint32_t kSize = 64;
    const Mat4 view = LookAt(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 1.0f, 0.0f));
    const Mat4 proj = PerspectiveReversedZ(60.0f * kDegToRad, 1.0f, 0.1f, 100.0f);
    ScreenTraceView screen;
    screen.viewProj = proj * view;
    screen.invViewProj = glm::inverse(screen.viewProj);

    const std::vector<float> depth =
        RasterizePlane(screen.viewProj, screen.invViewProj, kSize, kSize, -6.0f, Vec2(2.0f));
    HiZPyramid pyramid;
    pyramid.Build(kSize, kSize, depth);

    // SDF berisi kotak yang sama sekali berbeda tempatnya: di samping kamera,
    // jauh di luar layar. Dengan begitu, lapis mana yang menjawab bisa dibaca
    // dari lapisnya sendiri, bukan disimpulkan dari jaraknya.
    MeshInstance box;
    box.transform = glm::scale(glm::translate(Mat4(1.0f), Vec3(6.0f, 0.0f, 0.0f)), Vec3(2.0f));
    box.boundsMin = Vec3(-1.0f);
    box.boundsMax = Vec3(1.0f);
    const std::array<MeshInstance, 1> meshes{box};

    SdfClipmapSettings clipmap;
    clipmap.resolution = 64;
    clipmap.cascadeCount = 2;
    clipmap.finestVoxelSize = 0.1f;
    SdfVolume volume;
    volume.Configure(clipmap);
    volume.Clipmap().Scroll(Vec3(3.0f, 0.0f, 0.0f));
    BoxSceneField field;
    field.Build(meshes);
    volume.FillAll(field);

    TieredTraceSettings settings;
    settings.screen.thickness = 1.0f;
    const std::unique_ptr<ITraceBackend> tiered =
        CreateTieredTraceBackend(pyramid, screen, volume, settings);
    CHECK(tiered->Kind() == TraceBackendKind::Sdf);

    // Ke depan, ke papan yang terlihat: dijawab layar.
    const TraceResult onScreen =
        tiered->Trace(Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 0.0f, -1.0f), 30.0f);
    CHECK(onScreen.hit);
    CHECK(onScreen.layer == TraceLayer::Screen);

    // Ke samping, keluar layar, ke kotak yang hanya ada di SDF: dijawab SDF.
    const TraceResult offScreen =
        tiered->Trace(Vec3(3.0f, 0.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f), 20.0f);
    CHECK(offScreen.hit);
    CHECK(offScreen.layer == TraceLayer::Sdf);

    // Lurus ke atas: tidak ada apa pun di kedua lapis. `Sky` adalah jawaban,
    // bukan ketiadaan jawaban.
    const TraceResult nothing =
        tiered->Trace(Vec3(3.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 20.0f);
    CHECK(!nothing.hit);
    CHECK(nothing.layer == TraceLayer::Sky);
}

TEST_CASE("Mematikan lapis layar membuat jawabannya turun ke SDF") {
    // Bukan tombol kualitas: ia alat untuk melihat berapa banyak yang sebenarnya
    // dijawab lapis pertama. Alat itu hanya berarti kalau mematikannya
    // benar-benar mengubah lapis yang menjawab.
    constexpr uint32_t kSize = 64;
    const Mat4 view = LookAt(Vec3(0.0f), Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 1.0f, 0.0f));
    const Mat4 proj = PerspectiveReversedZ(60.0f * kDegToRad, 1.0f, 0.1f, 100.0f);
    ScreenTraceView screen;
    screen.viewProj = proj * view;
    screen.invViewProj = glm::inverse(screen.viewProj);

    MeshInstance box;
    box.transform = glm::scale(glm::translate(Mat4(1.0f), Vec3(0.0f, 0.0f, -4.0f)), Vec3(2.0f));
    box.boundsMin = Vec3(-1.0f);
    box.boundsMax = Vec3(1.0f);
    const std::array<MeshInstance, 1> meshes{box};

    SdfClipmapSettings clipmap;
    clipmap.resolution = 64;
    clipmap.cascadeCount = 2;
    clipmap.finestVoxelSize = 0.1f;
    SdfVolume volume;
    volume.Configure(clipmap);
    volume.Clipmap().Scroll(Vec3(0.0f, 0.0f, -3.0f));
    BoxSceneField field;
    field.Build(meshes);
    volume.FillAll(field);

    // Kotaknya berskala dua DAN kotak batasnya selebar dua, dan `BoxSceneField`
    // mengalikan keduanya — jadi sisi depannya di z = -2, bukan -3. Papan
    // rasternya diletakkan di sana supaya kedua lapis benar-benar menggambarkan
    // permukaan yang sama.
    const std::vector<float> depth =
        RasterizePlane(screen.viewProj, screen.invViewProj, kSize, kSize, -2.0f, Vec2(2.0f));
    HiZPyramid pyramid;
    pyramid.Build(kSize, kSize, depth);

    TieredTraceSettings withScreen;
    withScreen.screen.thickness = 1.0f;
    const TraceResult layered =
        CreateTieredTraceBackend(pyramid, screen, volume, withScreen)
            ->Trace(Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 0.0f, -1.0f), 20.0f);
    REQUIRE(layered.hit);
    CHECK(layered.layer == TraceLayer::Screen);

    TieredTraceSettings sdfOnly = withScreen;
    sdfOnly.screenEnabled = false;
    const TraceResult direct = CreateTieredTraceBackend(pyramid, screen, volume, sdfOnly)
                                   ->Trace(Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 0.0f, -1.0f),
                                           20.0f);
    REQUIRE(direct.hit);
    CHECK(direct.layer == TraceLayer::Sdf);
    // Keduanya menemukan permukaan depan yang sama. Selisihnya paling banyak
    // satu voxel SDF — dan itulah gunanya membandingkannya: dua lapis yang
    // menjawab jarak yang berbeda untuk permukaan yang sama adalah dua lapis
    // yang akan berkedip bergantian saat kamera bergerak.
    CHECK(direct.distance == doctest::Approx(layered.distance).epsilon(0.2));
}

// --- M3: screen probe -------------------------------------------------------

TEST_CASE("Pemetaan oktahedral bolak-balik tanpa kehilangan arah") {
    // Yang diuji bukan rumusnya melainkan sifat yang dipakai: setiap arah punya
    // satu titik di kotak, dan titik itu mengembalikan arah yang sama. Pemetaan
    // yang benar di setengah bola atas tapi salah di bawah adalah kesalahan
    // paling umum di sini, dan gejalanya cahaya yang datang dari arah yang
    // terbalik — bukan cahaya yang hilang.
    const std::array<Vec3, 10> directions{Vec3(0, 1, 0),   Vec3(0, -1, 0), Vec3(1, 0, 0),
                                          Vec3(-1, 0, 0),  Vec3(0, 0, 1),  Vec3(0, 0, -1),
                                          glm::normalize(Vec3(1, 1, 1)),
                                          glm::normalize(Vec3(-1, 2, -3)),
                                          glm::normalize(Vec3(0.2f, -0.9f, 0.4f)),
                                          glm::normalize(Vec3(-0.7f, -0.1f, -0.7f))};
    for (const Vec3& direction : directions) {
        const Vec2 uv = OctEncode(direction);
        CHECK(uv.x >= -1e-5f);
        CHECK(uv.x <= 1.0f + 1e-5f);
        CHECK(uv.y >= -1e-5f);
        CHECK(uv.y <= 1.0f + 1e-5f);
        const Vec3 back = OctDecode(uv);
        CHECK(glm::dot(back, direction) == doctest::Approx(1.0f).epsilon(0.001));
    }
}

TEST_CASE("Kisi probe menutupi ubin tepi yang hanya terisi sebagian") {
    // Dibulatkan ke bawah, sepotong tepi kanan dan bawah layar tidak punya probe
    // sama sekali — dan tepi layar justru tempat penelusuran screen-space paling
    // sering menyerah ke SDF, yaitu tempat probe paling dibutuhkan.
    ProbeGridSettings settings;
    settings.tileSize = 16;
    ProbeGrid grid;
    grid.Configure(100, 40, settings);

    CHECK(grid.Counts() == glm::uvec2(7, 3));
    CHECK(grid.ProbeCount() == 21);
    CHECK(grid.RaysPerProbe() == 16);

    // Probe ubin terakhir tetap mengambil depth dari piksel yang benar-benar
    // ada: pusat ubin itu di x = 104, jauh di luar layar selebar 100.
    const Vec2 last = grid.ProbePixel(6, 2);
    CHECK(last.x <= 99.5f);
    CHECK(last.y <= 39.5f);
    CHECK(last.x > 96.0f);
}

TEST_CASE("Enam belas ray probe menutupi seluruh sel oktahedral") {
    // Satu ray per sel, jadi tidak ada bagian bola yang tidak tersampel pada
    // sebuah frame. Arah yang diacak bebas tanpa stratifikasi bisa meninggalkan
    // separuh bola kosong, dan yang kosong itu muncul sebagai iradiansi yang
    // berdenyut walaupun adegannya diam.
    ProbeGridSettings settings;
    settings.raysPerAxis = 4;

    std::array<int, 16> hits{};
    for (uint32_t ray = 0; ray < 16; ++ray) {
        const Vec3 direction = ProbeRayDirection(ray, 3, 17, settings);
        CHECK(glm::length(direction) == doctest::Approx(1.0f).epsilon(0.001));
        const Vec2 uv = OctEncode(direction);
        const auto cx = static_cast<uint32_t>(std::min(uv.x * 4.0f, 3.999f));
        const auto cy = static_cast<uint32_t>(std::min(uv.y * 4.0f, 3.999f));
        ++hits[cy * 4 + cx];
    }
    for (const int count : hits) {
        CHECK(count == 1);
    }
}

TEST_CASE("Arah probe bergeser antar-frame dan antar-probe") {
    // Enam belas arah tetap adalah pola, bukan derau — dan pola tidak hilang
    // oleh akumulasi berapa pun lamanya. Jitter yang sama di seluruh probe sama
    // buruknya: polanya lalu terlihat sebagai kisi ubin.
    ProbeGridSettings settings;

    const Vec3 frame0 = ProbeRayDirection(5, 0, 9, settings);
    const Vec3 frame1 = ProbeRayDirection(5, 1, 9, settings);
    CHECK(glm::dot(frame0, frame1) < 0.9999f);

    const Vec3 probeA = ProbeRayDirection(5, 0, 9, settings);
    const Vec3 probeB = ProbeRayDirection(5, 0, 10, settings);
    CHECK(glm::dot(probeA, probeB) < 0.9999f);

    // Tapi tetap deterministik: dipanggil dua kali dengan angka yang sama, hasil
    // yang sama. Test yang hasilnya berubah tiap dijalankan bukan test.
    CHECK(glm::dot(probeA, ProbeRayDirection(5, 0, 9, settings)) ==
          doctest::Approx(1.0f).epsilon(1e-6));
}

TEST_CASE("Radiance seragam menghasilkan iradiansi pi kali radiance-nya") {
    // Uji tungku: ∫cos θ dω atas setengah bola persis π, jadi radiance seragam L
    // harus menghasilkan iradiansi πL untuk normal mana pun. Ini yang menangkap
    // faktor normalisasi yang salah — 2π alih-alih 4π, atau pembagi yang lupa —
    // dan kesalahan seperti itu muncul sebagai adegan yang seluruhnya terlalu
    // terang atau terlalu gelap, yang paling mudah dikira masalah eksposur.
    ProbeGridSettings settings;
    std::vector<ProbeRay> rays(16);
    const Vec3 radiance(0.4f, 0.6f, 0.8f);

    for (const Vec3 normal :
         {Vec3(0, 1, 0), Vec3(1, 0, 0), glm::normalize(Vec3(1, 2, 3))}) {
        // Dirata-rata atas banyak frame: enam belas arah saja terlalu sedikit
        // untuk sebuah penaksir Monte Carlo, dan yang diuji di sini nilai yang
        // dituju penaksirnya, bukan derau satu frame.
        Vec3 total(0.0f);
        constexpr uint32_t kFrames = 256;
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            for (uint32_t i = 0; i < rays.size(); ++i) {
                rays[i].direction = ProbeRayDirection(i, frame, 0, settings);
                rays[i].radiance = radiance;
            }
            total += IntegrateIrradiance(rays.data(), static_cast<uint32_t>(rays.size()), normal);
        }
        const Vec3 average = total / static_cast<float>(kFrames);
        const Vec3 expected = radiance * 3.14159265358979323846f;
        CHECK(average.x == doctest::Approx(expected.x).epsilon(0.05));
        CHECK(average.y == doctest::Approx(expected.y).epsilon(0.05));
        CHECK(average.z == doctest::Approx(expected.z).epsilon(0.05));
    }
}

TEST_CASE("Akumulasi menyatu dan tetap merespons perubahan") {
    // Rata-rata sejati atas seluruh riwayat berhenti merespons setelah beberapa
    // detik. Membatasi jumlah sampelnya membuat bobot frame terbaru tidak pernah
    // turun di bawah 1/maxFrames — jadi responsnya punya batas atas yang bisa
    // disebut, dan itulah yang diuji di sini.
    constexpr uint32_t kMax = 12;
    Vec3 value(0.0f);
    for (uint32_t frame = 0; frame < 64; ++frame) {
        value = AccumulateProbe(value, Vec3(1.0f), frame, kMax);
    }
    CHECK(value.x == doctest::Approx(1.0f).epsilon(0.01));

    // Lampu dimatikan: harus turun mendekati nol dalam beberapa puluh frame,
    // bukan bertahan selamanya.
    for (uint32_t frame = 0; frame < 64; ++frame) {
        value = AccumulateProbe(value, Vec3(0.0f), frame + 64, kMax);
    }
    CHECK(value.x < 0.01f);
}

TEST_CASE("Probe di seberang dinding tidak menyumbang ke piksel") {
    // Yang membedakan "lantai yang sama" dari "terpisah dinding" adalah jarak ke
    // bidang piksel, bukan jarak antar-titik. Memakai jarak lurus membuat cahaya
    // merembes menembus sudut ruangan — cacat yang paling sering dikira
    // kebocoran denoiser.
    ProbeFilterSettings filter;
    const Vec3 pixelPosition(0.0f);
    const Vec3 pixelNormal(0.0f, 1.0f, 0.0f);

    ProbeSurface sameFloor;
    sameFloor.valid = true;
    sameFloor.normal = pixelNormal;
    sameFloor.position = Vec3(3.0f, 0.0f, -2.0f);  // jauh, tapi sebidang
    CHECK(ProbeWeight(sameFloor, pixelPosition, pixelNormal, filter) > 0.9f);

    ProbeSurface aboveFloor = sameFloor;
    aboveFloor.position = Vec3(0.05f, 0.5f, 0.0f);  // dekat, tapi di atas bidang
    CHECK(ProbeWeight(aboveFloor, pixelPosition, pixelNormal, filter) == 0.0f);

    ProbeSurface wall = sameFloor;
    wall.normal = Vec3(1.0f, 0.0f, 0.0f);
    wall.position = pixelPosition;
    CHECK(ProbeWeight(wall, pixelPosition, pixelNormal, filter) == 0.0f);

    ProbeSurface sky;
    sky.valid = false;
    CHECK(ProbeWeight(sky, pixelPosition, pixelNormal, filter) == 0.0f);
}

TEST_CASE("Koordinat ubin berpusat di probe, bukan di pojok ubin") {
    // Tanpa pergeseran setengah ubin, seluruh interpolasi bergeser — dan
    // pergeseran itu terlihat sebagai cahaya yang selalu meleset ke satu arah,
    // gejala yang mudah dikira masalah penempatan probe.
    ProbeGridSettings settings;
    settings.tileSize = 16;
    ProbeGrid grid;
    grid.Configure(64, 64, settings);

    // Piksel tepat di probe (1,1), yaitu pusat ubin kedua: 16 + 8 = 24.
    const Vec2 atProbe = grid.TileCoordinate(Vec2(24.0f, 24.0f));
    CHECK(atProbe.x == doctest::Approx(1.0f));
    CHECK(atProbe.y == doctest::Approx(1.0f));

    // Dan tepat di tengah antara dua probe.
    const Vec2 between = grid.TileCoordinate(Vec2(32.0f, 24.0f));
    CHECK(between.x == doctest::Approx(1.5f));
}
