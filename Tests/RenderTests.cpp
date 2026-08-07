#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Render/FrameGraph.h"
#include "Sim/Render/Frustum.h"

#include <doctest/doctest.h>

#include <algorithm>
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
