#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/RHI/Ktx2.h"
#include "Sim/RHI/Pipeline.h"

#include <cstring>
#include <fstream>
#include "Sim/Render/FrameGraph.h"
#include "Sim/Render/DrawRun.h"
#include "Sim/Render/Frustum.h"
#include "Sim/Render/Ibl.h"
#include "Sim/Render/LightCluster.h"
#include "Sim/Render/Denoise.h"
#include "Sim/Render/RadianceCache.h"
#include "Sim/Render/ScreenProbe.h"
#include "Sim/Render/ScreenTrace.h"
#include "Sim/Render/SdfClipmap.h"
#include "Sim/Render/SdfVolume.h"
#include "Sim/Render/VolumeTexture.h"
#include "Sim/Render/ShadowAtlas.h"
#include "Sim/Render/TimeOfDay.h"
#include "Sim/Render/ToneMap.h"
#include "Sim/Render/Atmosphere.h"
#include "Sim/Render/CloudNoise.h"
#include "Sim/Render/Bloom.h"
#include "Sim/Render/TieredTrace.h"
#include "Sim/Render/TraceBackend.h"
#include "Sim/Render/ShadowCascades.h"

// Diminta sendiri: Render memakai Sim::ImageIO secara PRIVATE, jadi headernya
// tidak sampai ke sini lewat Ibl.h. Yang dibutuhkan uji ini cuma kueri
// kapabilitas — kriteria terima I2 berbunyi berbeda tergantung backend yang
// terbangun, dan uji yang tidak bisa membedakan keduanya akan melewat diam di
// salah satunya.
#include "Sim/ImageIO/ImageIO.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>
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

// --- SplitRuns (G1) ----------------------------------------------------------
//
// Yang diuji di sini adalah satu-satunya hal yang membuat penyaringan per muka
// bayangan mungkin tanpa mengunggah ulang apa pun. Salahnya tidak muncul sebagai
// galat melainkan sebagai instance yang digambar di tempat instance lain — sebab
// `first` sebuah ruas adalah indeks ke dalam buffer bersama.

namespace {

std::vector<sim::render::DrawRun> SplitWith(std::span<const sim::render::DrawRun> source,
                                            const std::vector<bool>& keep) {
    std::vector<sim::render::DrawRun> out;
    sim::render::SplitRuns(source, [&keep](uint32_t index) {
        return index < keep.size() && keep[index];
    }, out);
    return out;
}

}  // namespace

TEST_CASE("SplitRuns memecah satu ruas menjadi rentang yang bersambung saja") {
    using sim::render::DrawRun;
    // Satu ruas berisi enam instance, indeks 0..5. Yang lolos: 0,1, lalu 4.
    const std::vector<DrawRun> source{DrawRun{7, 2, 3, false, 0, 6}};
    const std::vector<bool> keep{true, true, false, false, true, false};

    const std::vector<DrawRun> out = SplitWith(source, keep);
    REQUIRE(out.size() == 2);
    CHECK(out[0].first == 0);
    CHECK(out[0].count == 2);
    CHECK(out[1].first == 4);
    CHECK(out[1].count == 1);

    // Kunci ruas ikut apa adanya. Kalau tidak, pecahan sebuah ruas akan
    // digambar lewat pipeline atau material milik ruas lain.
    for (const DrawRun& piece : out) {
        CHECK(piece.mesh == 7);
        CHECK(piece.partColorFirst == 2);
        CHECK(piece.partColorCount == 3);
        CHECK(piece.skinned == false);
    }
}

TEST_CASE("SplitRuns menghormati offset ruas asal") {
    using sim::render::DrawRun;
    // **Ruas kedua tidak mulai dari nol**, dan inilah kesalahan yang paling
    // mudah dibuat: memakai offset di dalam ruas sebagai indeks instance.
    // Hasilnya instance milik ruas pertama yang digambar dengan material ruas
    // kedua — tanpa satu pun galat validation layer.
    const std::vector<DrawRun> source{DrawRun{1, 0, 1, false, 0, 3},
                                      DrawRun{2, 1, 1, true, 3, 3}};
    const std::vector<bool> keep{false, false, false, false, true, true};

    const std::vector<DrawRun> out = SplitWith(source, keep);
    REQUIRE(out.size() == 1);
    CHECK(out[0].mesh == 2);
    CHECK(out[0].skinned == true);
    CHECK(out[0].first == 4);
    CHECK(out[0].count == 2);
}

TEST_CASE("SplitRuns menjatuhkan ruas yang seluruh isinya tersaring") {
    using sim::render::DrawRun;
    const std::vector<DrawRun> source{DrawRun{1, 0, 1, false, 0, 2},
                                      DrawRun{2, 0, 1, false, 2, 2}};
    const std::vector<bool> keep{false, false, true, true};

    const std::vector<DrawRun> out = SplitWith(source, keep);
    REQUIRE(out.size() == 1);
    CHECK(out[0].mesh == 2);
    CHECK(out[0].first == 2);
    CHECK(out[0].count == 2);
}

TEST_CASE("SplitRuns yang meloloskan semuanya mengembalikan daftar yang setara") {
    using sim::render::DrawRun;
    // Bukan sekadar kelengkapan: inilah jalur yang berlaku saat tidak ada yang
    // tersaring, dan ia harus tetap satu panggilan gambar per ruas — bukan satu
    // per instance, yang akan menukar penghematan GPU dengan biaya CPU yang
    // lebih besar daripada yang dihemat.
    const std::vector<DrawRun> source{DrawRun{1, 0, 1, false, 0, 4}};
    const std::vector<bool> keep{true, true, true, true};

    const std::vector<DrawRun> out = SplitWith(source, keep);
    REQUIRE(out.size() == 1);
    CHECK(out[0].first == 0);
    CHECK(out[0].count == 4);
}

TEST_CASE("SplitRuns atas daftar kosong menghasilkan daftar kosong") {
    std::vector<sim::render::DrawRun> out{sim::render::DrawRun{}};
    sim::render::SplitRuns(std::span<const sim::render::DrawRun>{},
                           [](uint32_t) { return true; }, out);
    // Dikosongkan, bukan dibiarkan berisi sisa pemanggilan sebelumnya — vektor
    // ini memang dipakai ulang antar-muka bayangan.
    CHECK(out.empty());
}

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

// --- frame graph: barrier compute (G3) ------------------------------------------
//
// Bagian yang paling mudah salah di seluruh fondasi compute, dan yang paling
// layak diuji tanpa GPU: salahnya tidak muncul sebagai galat melainkan sebagai
// hasil yang berubah-ubah antar-jalan pada mesin orang lain.

TEST_CASE("Tulisan compute lalu pembacaan tekstur menghasilkan satu transisi") {
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const ResourceId storage = graph.Import("gradient", Access::ShaderRead);

    const PassId fill = graph.AddPass("compute-gradient");
    graph.Write(fill, storage, Access::ShaderWrite);
    const PassId blit = graph.AddPass("compute-gradient-blit");
    graph.Read(blit, storage, Access::ShaderRead);
    graph.Write(blit, target, Access::ColorWrite);
    graph.SetOutput(target, Access::Present);

    const CompiledGraph compiled = graph.Compile();
    REQUIRE(compiled.ok);
    REQUIRE(compiled.order.size() == 2);
    // Masuk ke GENERAL sebelum dispatch, keluar lagi sebelum yang membacanya
    // sebagai tekstur. Dua-duanya, bukan salah satu: descriptor storage image
    // dan descriptor tekstur menjanjikan layout yang berbeda, dan yang
    // menjanjikan layout selain yang sedang berlaku sedang membaca dengan cara
    // yang tidak sah.
    CHECK(HasBarrier(compiled.order[0], storage, Access::ShaderRead, Access::ShaderWrite));
    CHECK(HasBarrier(compiled.order[1], storage, Access::ShaderWrite, Access::ShaderRead));
}

TEST_CASE("Dua dispatch yang menulis storage yang sama tetap dipisahkan barrier") {
    // Keadaannya tidak berpindah — ShaderWrite lalu ShaderWrite — jadi aturan
    // "barrier hanya saat keadaan berpindah" akan melewatkannya. Dan justru di
    // sini ia tidak boleh dilewatkan: Vulkan tidak menjanjikan urutan apa pun
    // antara dua perintah, jadi dispatch kedua bisa berjalan berbarengan dengan
    // yang pertama dan membaca sebagian tulisannya.
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const ResourceId storage = graph.Import("scratch", Access::None);

    const PassId first = graph.AddPass("clear");
    graph.Write(first, storage, Access::ShaderWrite);
    const PassId second = graph.AddPass("accumulate");
    graph.Write(second, storage, Access::ShaderWrite);
    const PassId reader = graph.AddPass("resolve");
    graph.Read(reader, storage, Access::ShaderRead);
    graph.Write(reader, target, Access::ColorWrite);
    graph.SetOutput(target, Access::Present);

    const CompiledGraph compiled = graph.Compile();
    REQUIRE(compiled.ok);
    REQUIRE(compiled.order.size() == 3);
    CHECK(HasBarrier(compiled.order[0], storage, Access::None, Access::ShaderWrite));
    CHECK(HasBarrier(compiled.order[1], storage, Access::ShaderWrite, Access::ShaderWrite));
    CHECK(HasBarrier(compiled.order[2], storage, Access::ShaderWrite, Access::ShaderRead));
}

TEST_CASE("Dua dispatch yang hanya membaca tidak dipisahkan apa pun") {
    // Pasangan uji di atas. Tanpa yang ini, "selalu pancarkan barrier" akan
    // lulus keduanya — dan barrier di antara dua pembacaan adalah dua pass yang
    // dipaksa berurutan tanpa ada yang menuntutnya.
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const ResourceId source = graph.Import("hiz", Access::ShaderRead);

    const PassId first = graph.AddPass("trace-a");
    graph.Read(first, source, Access::ShaderRead);
    graph.Write(first, target, Access::ColorWrite);
    const PassId second = graph.AddPass("trace-b");
    graph.Read(second, source, Access::ShaderRead);
    graph.Write(second, target, Access::ColorWrite);
    graph.SetOutput(target, Access::Present);

    const CompiledGraph compiled = graph.Compile();
    REQUIRE(compiled.ok);
    REQUIRE(compiled.order.size() == 2);
    CHECK(compiled.order[1].barriers.empty());
}

TEST_CASE("Pass compute yang keluarannya tidak dibaca siapa pun tetap dibuang") {
    // Pass compute adalah pass biasa, dan itu termasuk soal pembuangan. Kalau
    // tidak, setiap fitur berbasis compute yang dimatikan tetap membayar
    // dispatch-nya.
    FrameGraph graph;
    const ResourceId target = graph.Import("target", Access::None);
    const ResourceId storage = graph.CreateTexture("scratch", Colour(64, 64));

    const PassId dispatch = graph.AddPass("compute");
    graph.Write(dispatch, storage, Access::ShaderWrite);
    const PassId draw = graph.AddPass("draw");
    graph.Write(draw, target, Access::ColorWrite);
    graph.SetOutput(target, Access::Present);

    const CompiledGraph compiled = graph.Compile();
    REQUIRE(compiled.ok);
    CHECK(Order(graph, compiled) == std::vector<std::string>{"draw"});
    REQUIRE(compiled.culled.size() == 1);
    CHECK(compiled.culled[0] == dispatch);
}

TEST_CASE("Kisi cluster melaporkan angka yang sudah dijepit, bukan yang diberikan") {
    // Jalur GPU membangun kotak cluster dari angka-angka ini, jalur CPU dari
    // yang tersimpan di dalam kisi. Selisih di antara keduanya menghasilkan
    // lampu yang hilang hanya pada kamera yang ekstrem — bentuk kesalahan yang
    // tidak pernah muncul di adegan uji mana pun.
    ClusterGridSettings settings;
    ClusterGrid grid;
    // Nilai yang seluruhnya di luar rentang: fov nol, aspect nol, near nol, dan
    // far yang lebih kecil daripada near.
    grid.Build(settings, 0.0f, 0.0f, 0.0f, -5.0f);

    CHECK(grid.NearZ() > 0.0f);
    CHECK(grid.FarZ() > grid.NearZ());
    CHECK(grid.TanHalfX() > 0.0f);
    CHECK(grid.TanHalfY() > 0.0f);

    // Dan pada kamera biasa, angkanya memang angka kamera itu.
    grid.Build(settings, 1.0f, 16.0f / 9.0f, 0.1f, 300.0f);
    CHECK(grid.NearZ() == doctest::Approx(0.1f));
    CHECK(grid.FarZ() == doctest::Approx(300.0f));
    CHECK(grid.TanHalfY() == doctest::Approx(std::tan(0.5f)));
    CHECK(grid.TanHalfX() == doctest::Approx(std::tan(0.5f) * 16.0f / 9.0f));
    // Batas irisan pertama dan terakhir harus bersandar pada angka yang sama —
    // shader menurunkan kotak clusternya dari keduanya.
    CHECK(grid.SliceBounds(0).x == doctest::Approx(grid.NearZ()));
    CHECK(grid.SliceBounds(grid.Slices() - 1).y == doctest::Approx(grid.FarZ()));
}

// --- RHI: pembagian kerja dispatch (G3) ------------------------------------------

TEST_CASE("GroupCount membulatkan ke atas dan tidak pernah meluap") {
    using sim::rhi::GroupCount;
    // Kelipatan pas: tidak ada grup tambahan.
    CHECK(GroupCount(64, 8) == 8);
    // Sisa satu piksel tetap menuntut satu grup penuh — dan itu sebabnya setiap
    // kernel harus membuang invocation di luar batas.
    CHECK(GroupCount(65, 8) == 9);
    CHECK(GroupCount(1, 8) == 1);
    // Nol elemen berarti nol grup, bukan satu. `vkCmdDispatch(0, ...)` sah dan
    // tidak mengerjakan apa pun; satu grup atas data kosong adalah pembacaan di
    // luar batas.
    CHECK(GroupCount(0, 8) == 0);
    // Bentuk `(count + groupSize - 1) / groupSize` meluap di sini dan
    // menghasilkan nol grup — yaitu dispatch yang diam-diam tidak mengerjakan
    // apa pun.
    CHECK(GroupCount(0xFFFFFFFFu, 8) == 0x20000000u);
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

// --- Sinar yang berangkat DARI permukaan -------------------------------------
//
// **Seluruh uji trace di atas menembak dari ruang kosong. Sinar probe tidak.**
// `gi_probe.frag.slang` merekonstruksi titik asal probe dari depth buffer, jadi
// ia duduk tepat di permukaan — dan `traceTiered` meneruskan titik itu apa
// adanya ke `traceSdf`, yang mulai menapak pada `travelled = 0`. Titik di
// permukaan punya jarak nol, dan ambang berhentinya setengah voxel.
//
// Itulah kasus yang tidak pernah diuji, dan itulah kasus yang dipakai setiap
// sinar probe.

TEST_CASE("Sinar SDF dari titik di permukaan tidak mengenai permukaannya sendiri") {
    // Lantai cornell.simlevel: 10×0,2×10 berpusat di y = −0,1, jadi permukaan
    // atasnya tepat di y = 0.
    MeshInstance floor;
    floor.transform = glm::translate(Mat4(1.0f), Vec3(0.0f, -0.1f, 0.0f));
    floor.boundsMin = Vec3(-5.0f, -0.1f, -5.0f);
    floor.boundsMax = Vec3(5.0f, 0.1f, 5.0f);
    const std::array<MeshInstance, 1> meshes{floor};

    SdfClipmapSettings settings;
    settings.resolution = 128;
    settings.cascadeCount = 2;
    settings.finestVoxelSize = 0.05f;
    SdfVolume volume;
    volume.Configure(settings);
    volume.Clipmap().Scroll(Vec3(0.0f, 0.0f, 0.0f));

    BoxSceneField field;
    field.Build(meshes);
    volume.FillAll(field);

    const std::unique_ptr<ITraceBackend> backend = CreateSdfTraceBackend(volume, 96);
    const float voxel = volume.Clipmap().VoxelSize(0);
    const Vec3 surface(0.0f, 0.0f, 0.0f);
    const Vec3 normal(0.0f, 1.0f, 0.0f);

    // Enam belas arah, sama banyaknya dengan sinar satu probe per frame.
    const auto selfHitsFrom = [&](const Vec3& origin) {
        int count = 0;
        for (int i = 0; i < 16; ++i) {
            const float phi = 6.2831853f * static_cast<float>(i) / 16.0f;
            const float theta = 0.6f;  // 34° dari vertikal: masih jelas menjauh
            const Vec3 direction(std::sin(theta) * std::cos(phi), std::cos(theta),
                                 std::sin(theta) * std::sin(phi));
            const TraceResult traced = backend->Trace(origin, direction, 20.0f);
            if (traced.hit && traced.distance < voxel) {
                ++count;
            }
        }
        return count;
    };

    SUBCASE("tanpa bias, seluruhnya mengenai dirinya sendiri") {
        // **Bukan cacat penelusurnya.** Titik di permukaan memang berjarak nol,
        // dan menjawab "kena" adalah jawaban yang jujur. Yang salah adalah
        // memberinya titik asal itu — dan itulah yang dilakukan pass probe
        // sebelum `normalBiasVoxels` ada.
        const TraceResult up = backend->Trace(surface, normal, 20.0f);
        INFO("hit=", up.hit, " layer=", static_cast<int>(up.layer), " (",
             std::string(ToString(up.layer)), ") distance=", up.distance,
             " steps=", up.steps);
        CHECK(up.hit);
        CHECK(up.distance < voxel * 0.5f);
        CHECK(selfHitsFrom(surface) == 16);
    }

    SUBCASE("dengan bias normal bawaan, tidak satu pun") {
        // Memaku `ProbeGridSettings::normalBiasVoxels`: nilainya harus cukup
        // melewati ambang berhenti sphere tracing, dan test inilah yang
        // menyatakan seberapa cukup.
        const ProbeGridSettings probeSettings;
        const float bias = voxel * probeSettings.normalBiasVoxels;
        const Vec3 origin = surface + normal * bias;

        const TraceResult up = backend->Trace(origin, normal, 20.0f);
        INFO("bias=", bias, " m (", probeSettings.normalBiasVoxels, " voxel), hit=", up.hit,
             " distance=", up.distance);
        CHECK_FALSE(up.hit);
        CHECK(selfHitsFrom(origin) == 0);
    }
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

TEST_CASE("SH probe menjawab uji tungku yang sama dengan integrasi langsung") {
    // Dua jalur menuju satu angka: sampel diintegrasikan langsung, atau
    // diproyeksikan ke SH lalu dievaluasi. Keduanya harus sepakat pada medan
    // yang cukup halus — dan radiance seragam adalah yang paling halus.
    // Berselisih di sini berarti faktor konvolusi cosinus-nya salah, dan
    // gejalanya adegan yang terlalu terang atau terlalu gelap secara merata.
    ProbeGridSettings settings;
    std::vector<ProbeRay> rays(16);
    const Vec3 radiance(0.5f, 0.25f, 0.75f);

    ProbeSh accumulated;
    constexpr uint32_t kFrames = 256;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        for (uint32_t i = 0; i < rays.size(); ++i) {
            rays[i].direction = ProbeRayDirection(i, frame, 0, settings);
            rays[i].radiance = radiance;
        }
        accumulated = AddProbeSh(
            accumulated,
            BlendProbeSh(ProjectProbeSh(rays.data(), static_cast<uint32_t>(rays.size())),
                         1.0f / static_cast<float>(kFrames)));
    }

    for (const Vec3 normal : {Vec3(0, 1, 0), Vec3(-1, 0, 0), glm::normalize(Vec3(2, -1, 1))}) {
        const Vec3 irradiance = EvaluateProbeSh(accumulated, normal);
        CHECK(irradiance.x == doctest::Approx(radiance.x * 3.14159265f).epsilon(0.05));
        CHECK(irradiance.y == doctest::Approx(radiance.y * 3.14159265f).epsilon(0.05));
        CHECK(irradiance.z == doctest::Approx(radiance.z * 3.14159265f).epsilon(0.05));
    }
}

TEST_CASE("SH probe membawa arah, bukan hanya terang rata-rata") {
    // Kalau ia hanya membawa orde nol, permukaan yang menghadap ke cahaya dan
    // yang membelakanginya akan sama terangnya — dan color bleeding, yang
    // seluruhnya soal arah, tidak akan pernah muncul.
    ProbeGridSettings settings;
    std::vector<ProbeRay> rays(16);
    const Vec3 lit(0.0f, 1.0f, 0.0f);

    ProbeSh accumulated;
    constexpr uint32_t kFrames = 256;
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        for (uint32_t i = 0; i < rays.size(); ++i) {
            rays[i].direction = ProbeRayDirection(i, frame, 0, settings);
            // Setengah bola atas terang, bawahnya gelap.
            rays[i].radiance = rays[i].direction.y > 0.0f ? Vec3(1.0f) : Vec3(0.0f);
        }
        accumulated = AddProbeSh(
            accumulated,
            BlendProbeSh(ProjectProbeSh(rays.data(), static_cast<uint32_t>(rays.size())),
                         1.0f / static_cast<float>(kFrames)));
    }

    const Vec3 up = EvaluateProbeSh(accumulated, lit);
    const Vec3 down = EvaluateProbeSh(accumulated, -lit);
    CHECK(up.x > down.x * 3.0f);
    CHECK(down.x >= 0.0f);
}

// --- M4: hash grid radiance cache -------------------------------------------

TEST_CASE("Kunci cache membedakan arah, bukan hanya posisi") {
    // Sebuah titik di lantai memancarkan radiansi yang sangat berbeda ke atas
    // dan ke samping. Menyimpan satu angka per posisi berarti merata-ratakan
    // keduanya, dan hasilnya cahaya yang bocor menembus permukaan tipis — sisi
    // gelap sebuah dinding menerima rata-rata sisi terangnya.
    RadianceCacheSettings settings;
    settings.capacity = 1u << 12;
    RadianceCache cache;
    cache.Configure(settings);

    const Vec3 point(1.3f, 0.4f, -2.2f);
    const Vec3 camera(0.0f);
    const RadianceCacheKey up = cache.KeyFor(point, Vec3(0, 1, 0), camera);
    const RadianceCacheKey down = cache.KeyFor(point, Vec3(0, -1, 0), camera);
    const RadianceCacheKey side = cache.KeyFor(point, Vec3(1, 0, 0), camera);

    CHECK(up.cell == down.cell);
    CHECK(up.face != down.face);
    CHECK(up.face != side.face);

    cache.Insert(up, Vec3(1.0f, 0.0f, 0.0f), 0);
    cache.Insert(down, Vec3(0.0f, 1.0f, 0.0f), 0);

    Vec3 value;
    REQUIRE(cache.Query(up, value));
    CHECK(value.x == doctest::Approx(1.0f));
    REQUIRE(cache.Query(down, value));
    CHECK(value.y == doctest::Approx(1.0f));
    // Yang belum pernah diisi menjawab "tidak tahu", bukan hitam.
    CHECK(!cache.Query(side, value));
}

TEST_CASE("Sel membesar mengikuti jarak ke kamera") {
    // Detail radiansi pada jarak lima puluh meter tidak terlihat sama sekali,
    // dan menyimpannya sehalus yang di depan mata menghabiskan seluruh cache
    // untuk hal yang tidak ada yang melihat.
    RadianceCacheSettings settings;
    settings.capacity = 1u << 12;
    settings.cellSize = 0.25f;
    settings.lodDistance = 8.0f;
    RadianceCache cache;
    cache.Configure(settings);

    const Vec3 camera(0.0f);
    const Vec3 normal(0.0f, 1.0f, 0.0f);
    CHECK(cache.KeyFor(Vec3(1.0f, 0.0f, 0.0f), normal, camera).level == 0);
    CHECK(cache.KeyFor(Vec3(12.0f, 0.0f, 0.0f), normal, camera).level == 1);
    CHECK(cache.KeyFor(Vec3(40.0f, 0.0f, 0.0f), normal, camera).level == 3);

    // Tingkatnya ikut ke kunci: sel halus dan sel kasar yang kebetulan
    // bertumpuk tidak boleh berbagi entri, karena isinya beda arti.
    const RadianceCacheKey near = cache.KeyFor(Vec3(1.0f, 0.0f, 0.0f), normal, camera);
    const RadianceCacheKey far = cache.KeyFor(Vec3(1.0f, 0.0f, 0.0f), normal, Vec3(100.0f, 0, 0));
    CHECK(near.level != far.level);
}

TEST_CASE("Hash menyebar sel bertetangga ke slot yang berjauhan") {
    // Dua sel bertetangga berbeda satu pada satu sumbu. Tanpa pencampuran bit
    // keduanya mendarat di slot bertetangga — dan probing linear lalu langsung
    // menabrak tetangganya sendiri, yang membuat cache penuh jauh sebelum
    // entrinya habis.
    RadianceCacheSettings settings;
    settings.capacity = 1u << 16;
    settings.maxProbe = 4;
    RadianceCache cache;
    cache.Configure(settings);

    // Satu blok 20x20x20 sel bertetangga, satu arah: 8000 kunci ke 65536 slot.
    uint32_t inserted = 0;
    for (int z = 0; z < 20; ++z) {
        for (int y = 0; y < 20; ++y) {
            for (int x = 0; x < 20; ++x) {
                RadianceCacheKey key;
                key.cell = {x, y, z};
                key.face = 2;
                if (cache.Insert(key, Vec3(static_cast<float>(x)), 0)) {
                    ++inserted;
                }
            }
        }
    }
    // Dengan beban 12% dan empat langkah probing, hampir semuanya harus muat.
    CHECK(inserted > 7900);
    CHECK(cache.DroppedSamples() < 100);

    // Dan yang masuk harus bisa dibaca kembali apa adanya.
    RadianceCacheKey probe;
    probe.cell = {7, 11, 13};
    probe.face = 2;
    Vec3 value;
    REQUIRE(cache.Query(probe, value));
    CHECK(value.x == doctest::Approx(7.0f));
}

TEST_CASE("Uji tungku: albedo satu di bawah cahaya seragam tidak menggelap") {
    // **Kriteria selesai M4.** Sebuah adegan dengan albedo 1,0 di bawah
    // pencahayaan seragam L harus tetap L setelah berapa pun pantulan: setiap
    // permukaan memantulkan seluruh yang diterimanya. Yang menggelapkan adalah
    // faktor yang hilang — pembagi pi yang lupa, iradiansi yang dikira radiansi,
    // atau entri cache yang belum terisi dianggap hitam.
    //
    // Yang diuji di sini lingkarannya, bukan salah satu bagiannya: radiansi
    // masuk ke cache, dibaca kembali sebagai radiansi permukaan, lalu masuk lagi
    // — persis jalur yang dipakai multi-bounce.
    RadianceCacheSettings settings;
    settings.capacity = 1u << 14;
    settings.accumulationFrames = 32;
    RadianceCache cache;
    cache.Configure(settings);

    ProbeGridSettings probeSettings;
    const Vec3 camera(0.0f);
    const float uniform = 0.7f;

    // Sepuluh permukaan tersebar, masing-masing menghadap arah berbeda.
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    for (int i = 0; i < 10; ++i) {
        const float angle = static_cast<float>(i) * 0.7f;
        positions.push_back(Vec3(std::cos(angle) * 3.0f, static_cast<float>(i) * 0.4f,
                                 std::sin(angle) * 3.0f));
        normals.push_back(glm::normalize(Vec3(std::cos(angle), 0.4f, std::sin(angle))));
    }

    // Frame pertama: hanya cahaya langsung yang seragam. Sesudahnya setiap
    // permukaan mengambil radiansinya dari cache — itulah pantulan berikutnya.
    std::vector<ProbeRay> rays(16);
    for (uint32_t frame = 0; frame < 200; ++frame) {
        for (std::size_t i = 0; i < positions.size(); ++i) {
            for (uint32_t r = 0; r < rays.size(); ++r) {
                rays[r].direction =
                    ProbeRayDirection(r, frame, static_cast<uint32_t>(i), probeSettings);
                // Radiansi yang datang: dari cache kalau sudah tahu, dari cahaya
                // langsung kalau belum. Yang tidak diketahui **dibuang**, bukan
                // dihitung nol — pelajaran yang sama dengan sinar SDF di M3.
                Vec3 incoming;
                const RadianceCacheKey key =
                    cache.KeyFor(positions[i] + rays[r].direction, -rays[r].direction, camera);
                rays[r].radiance = cache.Query(key, incoming) ? incoming : Vec3(uniform);
            }
            const Vec3 irradiance =
                IntegrateIrradiance(rays.data(), static_cast<uint32_t>(rays.size()), normals[i]);
            // Albedo 1,0: radiansi keluar = iradiansi / pi. Faktor pi inilah
            // yang paling sering hilang, dan hilangnya terlihat sebagai adegan
            // yang menggelap tiap pantulan — bukan sebagai galat.
            const Vec3 outgoing = irradiance / 3.14159265358979323846f;
            cache.Insert(cache.KeyFor(positions[i], normals[i], camera), outgoing, frame);
        }
    }

    // Tidak menggelap, dan tidak meledak.
    for (std::size_t i = 0; i < positions.size(); ++i) {
        Vec3 value;
        REQUIRE(cache.Query(cache.KeyFor(positions[i], normals[i], camera), value));
        CHECK(value.x == doctest::Approx(uniform).epsilon(0.1));
        CHECK(value.y == doctest::Approx(uniform).epsilon(0.1));
        CHECK(value.z == doctest::Approx(uniform).epsilon(0.1));
    }
}

TEST_CASE("Entri basi direbut, dan yang masih terpakai tidak") {
    // Tanpa daur ulang, kamera yang berkeliling meninggalkan entri untuk setiap
    // tempat yang pernah dilihatnya — entri yang tidak pernah dibaca lagi tapi
    // tetap memakai slotnya, sampai seluruh cache penuh oleh masa lalu.
    // Gejalanya GI yang bekerja saat editor baru dibuka lalu diam-diam berhenti
    // bekerja setelah beberapa menit menjelajah.
    RadianceCacheSettings settings;
    settings.capacity = 8;
    settings.maxProbe = 8;   // seluruh cache terjangkau probing
    settings.staleFrames = 10;
    RadianceCache cache;
    cache.Configure(settings);

    // Isi penuh pada frame nol.
    for (int i = 0; i < 8; ++i) {
        RadianceCacheKey key;
        key.cell = {i, 0, 0};
        REQUIRE(cache.Insert(key, Vec3(static_cast<float>(i)), 0));
    }
    CHECK(cache.LiveEntries() == 8);

    // Frame 5: belum basi, jadi kunci baru harus terbuang, bukan merebut.
    RadianceCacheKey fresh;
    fresh.cell = {100, 0, 0};
    CHECK(!cache.Insert(fresh, Vec3(9.0f), 5));
    CHECK(cache.DroppedSamples() == 1);
    CHECK(cache.EvictedEntries() == 0);

    // Frame 20: seluruhnya sudah basi, jadi slotnya boleh direbut.
    CHECK(cache.Insert(fresh, Vec3(9.0f), 20));
    CHECK(cache.EvictedEntries() == 1);
    Vec3 value;
    REQUIRE(cache.Query(fresh, value));
    CHECK(value.x == doctest::Approx(9.0f));

    // Dan entri yang terus disegarkan tidak pernah direbut walaupun waktu
    // berjalan jauh — itu yang membedakan daur ulang dari sekadar kedaluwarsa.
    RadianceCacheKey kept;
    kept.cell = {3, 0, 0};
    for (uint32_t frame = 20; frame < 200; ++frame) {
        REQUIRE(cache.Insert(kept, Vec3(3.0f), frame));
    }
    REQUIRE(cache.Query(kept, value));
    CHECK(value.x == doctest::Approx(3.0f));
}

// --- M5: denoise & temporal -------------------------------------------------

TEST_CASE("Reproyeksi menemukan titik dunia di kisi frame sebelumnya") {
    // Inilah yang membuat riwayat bertahan saat kamera bergerak. Sampai M4
    // riwayat probe dibuang seluruhnya begitu kamera berpindah: riwayatnya
    // terikat ke piksel, dan piksel yang sama menunjuk permukaan berbeda.
    const glm::uvec2 viewport(640, 480);
    const Mat4 proj = PerspectiveReversedZ(60.0f * kDegToRad, 4.0f / 3.0f, 0.1f, 100.0f);
    const Mat4 previous =
        proj * LookAt(Vec3(0.0f, 0.0f, 5.0f), Vec3(0.0f), Vec3(0.0f, 1.0f, 0.0f));

    // Titik di pusat pandangan frame lalu jatuh di tengah layar, yaitu ubin
    // ke-(20, 15) dari kisi 16 piksel.
    const ProbeReprojection centre = ReprojectProbe(Vec3(0.0f), previous, viewport, 16);
    REQUIRE(centre.onScreen);
    CHECK(centre.tile.x == doctest::Approx(19.5f).epsilon(0.01));
    CHECK(centre.tile.y == doctest::Approx(14.5f).epsilon(0.01));

    // Titik jauh di samping tidak ada di layar frame lalu — riwayatnya tidak
    // ada, dan itu berbeda dari riwayat yang bernilai nol.
    CHECK(!ReprojectProbe(Vec3(50.0f, 0.0f, 0.0f), previous, viewport, 16).onScreen);
    // Dan yang di belakang kamera juga tidak: koordinat layarnya adalah
    // pantulan titik itu di seberang layar.
    CHECK(!ReprojectProbe(Vec3(0.0f, 0.0f, 20.0f), previous, viewport, 16).onScreen);
}

TEST_CASE("Kernel a-trous menjumlah satu, jadi bidang rata tetap rata") {
    // Jumlah bobot yang bukan satu membuat penyaring mengubah terang gambarnya,
    // bukan hanya menghaluskannya — dan perubahan itu bertambah tiap lintasan.
    float total = 0.0f;
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            total += AtrousKernel(dx, dy);
        }
    }
    CHECK(total == doctest::Approx(1.0f).epsilon(1e-5));
    // Puncaknya di tengah, dan meluruh mulus — kernel kotak menyebarkan tepi
    // menjadi tangga selebar kernelnya.
    CHECK(AtrousKernel(0, 0) > AtrousKernel(1, 0));
    CHECK(AtrousKernel(1, 0) > AtrousKernel(2, 0));
    CHECK(AtrousKernel(3, 0) == 0.0f);
}

TEST_CASE("A-trous menghaluskan derau tapi tidak melintasi permukaan") {
    // Dua bidang sejajar berjarak satu meter, masing-masing berisi nilai yang
    // berbeda plus derau. Penyaringnya harus menghapus deraunya tanpa
    // mencampurkan kedua bidang — pencampuran itulah yang terlihat sebagai
    // cahaya yang merembes menembus sudut ruangan.
    const glm::uvec2 size(32, 32);
    const std::size_t count = 32u * 32u;
    std::vector<Vec3> input(count);
    std::vector<Vec3> positions(count);
    std::vector<Vec3> normals(count, Vec3(0.0f, 1.0f, 0.0f));
    std::vector<uint8_t> valid(count, 1);
    std::vector<Vec3> output(count);

    for (uint32_t y = 0; y < size.y; ++y) {
        for (uint32_t x = 0; x < size.x; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * size.x + x;
            // Separuh kiri di bidang y = 0, separuh kanan di bidang y = 1.
            const bool upper = x >= 16;
            positions[i] = Vec3(static_cast<float>(x) * 0.05f, upper ? 1.0f : 0.0f,
                                static_cast<float>(y) * 0.05f);
            // Derau bergantian +/- 0,4 di sekitar nilai dasarnya.
            const float noise = ((x + y) % 2 == 0) ? 0.4f : -0.4f;
            input[i] = Vec3((upper ? 2.0f : 1.0f) + noise);
        }
    }

    AtrousSettings settings;
    AtrousPass(input, positions, normals, valid, size, 1, settings, output);

    // Di tengah tiap bidang deraunya harus hilang.
    const std::size_t left = 16u * 32u + 6u;
    const std::size_t right = 16u * 32u + 25u;
    CHECK(output[left].x == doctest::Approx(1.0f).epsilon(0.1));
    CHECK(output[right].x == doctest::Approx(2.0f).epsilon(0.1));

    // Dan tepat di kedua sisi batas, nilainya tidak boleh tercampur: yang di
    // bidang bawah tetap dekat 1, yang di atas tetap dekat 2.
    const std::size_t belowEdge = 16u * 32u + 15u;
    const std::size_t aboveEdge = 16u * 32u + 16u;
    CHECK(output[belowEdge].x < 1.5f);
    CHECK(output[aboveEdge].x > 1.5f);
}

TEST_CASE("Jendela akumulasi menentukan waktu respons, dan angkanya bisa disebut") {
    // **Kriteria selesai M5 diubah menjadi angka di sini.** Rencana menuntut GI
    // merespons lampu dinyalakan-matikan di bawah 200 ms; pada 60 Hz itu dua
    // belas frame. Rata-rata berjalan meluruh eksponensial, jadi jendela yang
    // panjang tidak akan pernah memenuhinya berapa pun bagusnya penyaring
    // spasialnya — dan itu fakta yang lebih baik diketahui sebelum satu shader
    // pun ditulis.
    CHECK(FramesToRespond(1, 0.9f) == 1);

    const uint32_t sixteen = FramesToRespond(16, 0.9f);
    const uint32_t five = FramesToRespond(5, 0.9f);
    CHECK(sixteen > five);
    // Jendela 16 frame — yang dipakai M3 dan M4 — jauh melewati dua belas.
    CHECK(sixteen > 12);
    // Jendela lima frame masuk anggaran.
    CHECK(five <= 12);
}

TEST_CASE("Penjepitan riwayat memutus pertukaran respons lawan derau") {
    // Jendela pendek merespons cepat tapi berderau; jendela panjang sebaliknya.
    // Penjepitan memutus pertukaran itu: selama sampel barunya sejalan dengan
    // tetangganya, riwayat panjang dipertahankan; begitu adegannya benar-benar
    // berubah, riwayat yang sudah tidak sejalan dijepit masuk dalam satu frame.
    const Vec3 mean(1.0f);
    const Vec3 deviation(0.1f);

    // Riwayat yang masih sejalan tidak disentuh.
    const Vec3 agreeing(1.05f);
    CHECK(ClampHistory(agreeing, mean, deviation, 2.0f).x == doctest::Approx(1.05f));

    // Riwayat yang jauh di luar rentang dijepit ke tepinya — bukan dibuang,
    // karena membuangnya mengembalikan seluruh derau frame tunggal.
    const Vec3 stale(5.0f);
    const Vec3 clamped = ClampHistory(stale, mean, deviation, 2.0f);
    CHECK(clamped.x == doctest::Approx(1.2f));

    // Dan lampu yang dimatikan: riwayat terang, sekitarnya sudah gelap.
    const Vec3 dark(0.0f);
    const Vec3 offClamped = ClampHistory(Vec3(1.0f), dark, Vec3(0.02f), 2.0f);
    CHECK(offClamped.x == doctest::Approx(0.04f));
}

// --- M6: integrasi ke shading OpenPBR ---------------------------------------

TEST_CASE("Uji white furnace lulus untuk seluruh rentang roughness dan metalness") {
    // **Kriteria selesai M6.** Di dalam tungku putih — lingkungan yang
    // memancarkan radiansi seragam L dari segala arah — permukaan ber-albedo 1,0
    // harus memantulkan tepat L, berapa pun kekasaran dan metalness-nya. Yang
    // membuatnya gagal adalah energi yang hilang, dan energi yang hilang tidak
    // pernah muncul sebagai galat: ia muncul sebagai logam kasar yang tampak
    // abu-abu kotor, dan tidak ada penyetelan material yang bisa
    // memperbaikinya.
    const DfgLut lut = BakeDfgLut(64, 1024);

    for (const float roughness : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f}) {
        for (const float metalness : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
            for (const float nDotV : {0.15f, 0.5f, 0.95f}) {
                const DfgTerms dfg = lut.Sample(nDotV, roughness);
                // Albedo putih, jadi f0 logamnya juga putih; dielektriknya 0,04.
                const Vec3 white(1.0f);
                const Vec3 f0 = glm::mix(Vec3(0.04f), white, metalness);
                const EnergyTerms terms = SplitEnergy(dfg, f0);

                // Di dalam tungku, radiansi spekular dan iradiansi/pi keduanya L.
                // Totalnya karena itu jumlah ketiga suku dikali albedo untuk yang
                // difus — dan albedo-nya satu.
                const Vec3 total = terms.singleScatter + terms.multiScatter +
                                   terms.diffuse * white;
                CHECK(total.x == doctest::Approx(1.0f).epsilon(0.02));
                CHECK(total.y == doctest::Approx(1.0f).epsilon(0.02));
                CHECK(total.z == doctest::Approx(1.0f).epsilon(0.02));
            }
        }
    }
}

TEST_CASE("Logam putih tidak menyisakan energi untuk difus") {
    // Aturan "metal mengambil dari lapis spekular, bukan iradiansi difus" tidak
    // ditulis sebagai cabang terpisah — ia akibat langsung dari pembagian
    // energinya. Menuliskannya sebagai cabang berarti dua tempat yang bisa
    // berselisih; membiarkannya jatuh dari rumus berarti hanya satu.
    const DfgLut lut = BakeDfgLut(64, 1024);
    for (const float roughness : {0.05f, 0.4f, 1.0f}) {
        const EnergyTerms metal = SplitEnergy(lut.Sample(0.6f, roughness), Vec3(1.0f));
        CHECK(metal.diffuse.x == doctest::Approx(0.0f).epsilon(0.01));

        // Sedangkan dielektrik menyisakan hampir seluruhnya.
        const EnergyTerms dielectric = SplitEnergy(lut.Sample(0.6f, roughness), Vec3(0.04f));
        CHECK(dielectric.diffuse.x > 0.85f);
    }
}

TEST_CASE("Kompensasi mengembalikan energi yang hilang, bukan menambah energi baru") {
    // Dua arah kesalahan, dan keduanya sama buruk: tanpa kompensasi permukaan
    // kasar menggelap, dengan kompensasi yang berlebihan ia bersinar sendiri.
    // Yang dikembalikan harus persis yang hilang.
    const DfgLut lut = BakeDfgLut(64, 1024);

    // Pada cermin sempurna tidak ada yang hilang, jadi tidak ada yang perlu
    // dikembalikan.
    const EnergyTerms mirror = SplitEnergy(lut.Sample(0.7f, 0.0f), Vec3(1.0f));
    CHECK(mirror.multiScatter.x == doctest::Approx(0.0f).epsilon(0.02));

    // Pada permukaan paling kasar, yang dikembalikan justru yang paling banyak.
    const DfgTerms rough = lut.Sample(0.7f, 1.0f);
    const EnergyTerms compensated = SplitEnergy(rough, Vec3(1.0f));
    CHECK(compensated.multiScatter.x > 0.5f);
    // Dan pantulan tunggalnya memang jauh dari satu — itu defisit yang ditutup.
    CHECK(rough.scale + rough.bias < 0.5f);
}

// --- E8.8: Time of day ------------------------------------------------------

TEST_CASE("Matahari terbit di timur dan terbenam di barat") {
    // Yang diminta editor Time-of-Day adalah matahari yang benar-benar berputar.
    // Arah terbitnya ditentukan lintang dan musim, dan busur yang digambar
    // tangan tidak bisa menyatakan keduanya.
    SunPlacement placement;
    placement.latitudeDegrees = 0.0f;
    placement.dayOfYear = 80;  // ekuinoks Maret: deklinasi hampir nol
    placement.northOffsetDegrees = 0.0f;

    placement.hour = 6.0f;
    const SunPosition sunrise = ComputeSunPosition(placement);
    CHECK(sunrise.direction.x > 0.99f);                    // +X adalah timur
    CHECK(std::abs(sunrise.altitude) < 0.05f);             // di horizon

    placement.hour = 12.0f;
    const SunPosition noon = ComputeSunPosition(placement);
    CHECK(noon.direction.y > 0.99f);                       // tepat di atas
    CHECK(noon.altitude > 1.5f);

    placement.hour = 18.0f;
    const SunPosition sunset = ComputeSunPosition(placement);
    CHECK(sunset.direction.x < -0.99f);                    // barat
    CHECK(std::abs(sunset.altitude) < 0.05f);

    placement.hour = 0.0f;
    CHECK(ComputeSunPosition(placement).altitude < -1.5f); // di bawah kaki
}

TEST_CASE("Lintang dan musim menentukan arah tengah hari") {
    // Di belahan utara matahari tengah hari condong ke selatan, di selatan ke
    // utara. Ini yang membuat bayangan tengah hari jatuh ke arah yang benar
    // untuk lokasi adegannya — dan yang tidak bisa dinyatakan busur mana pun
    // yang digambar untuk satu tempat saja.
    SunPlacement north;
    north.latitudeDegrees = 52.0f;  // Amsterdam
    north.dayOfYear = 80;
    north.hour = 12.0f;
    // Utara adegan adalah −Z, jadi condong ke selatan berarti +Z.
    CHECK(ComputeSunPosition(north).direction.z > 0.5f);

    SunPlacement south = north;
    south.latitudeDegrees = -33.0f;  // Sydney
    CHECK(ComputeSunPosition(south).direction.z < -0.3f);

    // Dan musim menggesernya: di lintang utara, matahari Juni lebih tinggi
    // daripada matahari Desember.
    SunPlacement june = north;
    june.dayOfYear = 172;
    SunPlacement december = north;
    december.dayOfYear = 355;
    CHECK(ComputeSunPosition(june).altitude > ComputeSunPosition(december).altitude + 0.5f);
}

TEST_CASE("Utara adegan bisa diputar tanpa memutar adegannya") {
    // Adegan jarang dibangun menghadap utara sungguhan, dan memutar seluruh
    // adegan agar cocok jauh lebih mahal daripada memutar mataharinya.
    SunPlacement placement;
    placement.latitudeDegrees = 0.0f;
    placement.dayOfYear = 80;
    placement.hour = 6.0f;

    const SunPosition unrotated = ComputeSunPosition(placement);
    placement.northOffsetDegrees = 90.0f;
    const SunPosition rotated = ComputeSunPosition(placement);

    // Ketinggiannya tidak boleh berubah — memutar utara memutar kompas, bukan
    // memindahkan matahari naik atau turun.
    CHECK(rotated.altitude == doctest::Approx(unrotated.altitude).epsilon(0.01));
    // Tapi arahnya berputar seperempat lingkaran.
    CHECK(std::abs(glm::dot(Vec3(rotated.direction.x, 0.0f, rotated.direction.z),
                            Vec3(unrotated.direction.x, 0.0f, unrotated.direction.z))) < 0.05f);
}

TEST_CASE("Kurva harian siklis melintasi tengah malam") {
    // Kurva yang berhenti di jam 24 menjadikan tengah malam sebuah loncatan —
    // dan tengah malam justru saat yang pasti dilewati setiap siklus
    // siang-malam. Ruas dari kunci terakhir ke kunci pertama itulah yang paling
    // mudah terlupa.
    TimeOfDayCurve curve({
        {6.0f, Vec3(1.0f, 0.0f, 0.0f)},
        {18.0f, Vec3(0.0f, 1.0f, 0.0f)},
    });

    // Tepat di kunci: nilainya persis nilai kuncinya.
    CHECK(curve.Evaluate(6.0f).x == doctest::Approx(1.0f));
    CHECK(curve.Evaluate(18.0f).y == doctest::Approx(1.0f));
    // Di tengah siang: separuh jalan.
    CHECK(curve.Evaluate(12.0f).x == doctest::Approx(0.5f));

    // Melintasi tengah malam: dari jam 18 ke jam 6 berikutnya lewat jam 24,
    // yaitu dua belas jam. Jam 0 tepat separuh jalan.
    const Vec3 midnight = curve.Evaluate(0.0f);
    CHECK(midnight.x == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(midnight.y == doctest::Approx(0.5f).epsilon(0.02));
    // Dan tepat sesudah tengah malam nilainya melanjutkan arah yang sama, bukan
    // melompat balik.
    CHECK(curve.Evaluate(1.0f).x > midnight.x);
    CHECK(curve.Evaluate(23.0f).x < midnight.x);

    // Jam di luar rentang dibungkus, bukan dijepit.
    CHECK(curve.Evaluate(30.0f).x == doctest::Approx(curve.Evaluate(6.0f).x));
    CHECK(curve.Evaluate(-6.0f).x == doctest::Approx(curve.Evaluate(18.0f).x));
}

TEST_CASE("Kunci pada jam yang sama diganti, bukan ditumpuk") {
    // Dua kunci pada jam yang sama membuat hasilnya bergantung urutan
    // penyisipan — dan itu tidak terlihat di mana pun kecuali pada berkas yang
    // disimpan lalu dibuka kembali.
    TimeOfDayCurve curve;
    curve.Set(8.0f, Vec3(1.0f));
    curve.Set(8.0f, Vec3(2.0f));
    CHECK(curve.KeyCount() == 1);
    CHECK(curve.Evaluate(8.0f).x == doctest::Approx(2.0f));

    // Dan urutannya tetap terjaga berapa pun urutan penyisipannya.
    curve.Set(3.0f, Vec3(0.0f));
    curve.Set(20.0f, Vec3(9.0f));
    REQUIRE(curve.KeyCount() == 3);
    CHECK(curve.Key(0).hour < curve.Key(1).hour);
    CHECK(curve.Key(1).hour < curve.Key(2).hour);

    // Kurva kosong mengembalikan nilai mundurnya, bukan nol diam-diam.
    TimeOfDayCurve empty;
    CHECK(empty.Evaluate(12.0f, Vec3(7.0f)).x == doctest::Approx(7.0f));
}

TEST_CASE("Jam siklus berjalan hanya saat diputar, dan membungkus di tengah malam") {
    // Jam terpisah dari kurvanya: kurva adalah data yang disunting dan disimpan,
    // jam adalah keadaan yang berjalan.
    TimeOfDayClock dayClock;
    dayClock.SetHour(23.5f);
    dayClock.SetSpeed(1.0f);  // satu jam permainan per detik

    // Berhenti: menyeret slider jam tidak boleh dilawan siklusnya sendiri.
    dayClock.Advance(10.0f);
    CHECK(dayClock.Hour() == doctest::Approx(23.5f));

    dayClock.SetPlaying(true);
    dayClock.Advance(1.0f);
    CHECK(dayClock.Hour() == doctest::Approx(0.5f));  // membungkus, bukan 24,5

    dayClock.SetHour(30.0f);
    CHECK(dayClock.Hour() == doctest::Approx(6.0f));
}

TEST_CASE("Matahari diredam saat terbit dan terbenam, bukan diputus mendadak") {
    // Matahari yang dimatikan tepat saat menyentuh horizon membuat seluruh
    // adegan berkedip dalam satu frame — dan kedipan itu jatuh persis pada saat
    // yang paling diperhatikan orang.
    const TimeOfDayPreset preset = TimeOfDayPreset::Default();
    SunPlacement placement;
    placement.latitudeDegrees = 0.0f;
    placement.dayOfYear = 80;

    placement.hour = 12.0f;
    const TimeOfDayState noon = EvaluateTimeOfDay(preset, placement);
    CHECK(noon.daylight == doctest::Approx(1.0f));
    CHECK(noon.sunRadiance.x > 3.0f);

    placement.hour = 0.0f;
    const TimeOfDayState midnight = EvaluateTimeOfDay(preset, placement);
    CHECK(midnight.daylight == doctest::Approx(0.0f));
    CHECK(midnight.sunRadiance.x == doctest::Approx(0.0f));

    // Tepat di horizon: separuh, bukan nol dan bukan penuh.
    placement.hour = 6.0f;
    const TimeOfDayState sunrise = EvaluateTimeOfDay(preset, placement);
    CHECK(sunrise.daylight > 0.3f);
    CHECK(sunrise.daylight < 0.7f);

    // Dan peredamannya monoton: tidak ada saat di mana matahari terbit membuat
    // adegan lebih gelap daripada semenit sebelumnya.
    float previous = -1.0f;
    for (float hour = 5.6f; hour <= 6.4f; hour += 0.05f) {
        placement.hour = hour;
        // Bukan `daylight`: itu nama global POSIX di <time.h>, dan -Wshadow
        // menangkapnya hanya di Release.
        const float level = EvaluateTimeOfDay(preset, placement).daylight;
        CHECK(level >= previous - 1e-4f);
        previous = level;
    }
}

// --- E8.8: operator nada dan eksposur ----------------------------------------

TEST_CASE("kurva ACES monoton, mulai dari nol, dan menyisakan ruang di atas putih") {
    using namespace sim::render;

    CHECK(AcesCurve(0.0f) == 0.0f);

    // Monoton di seluruh rentang yang dipakai. Kurva nada yang berbalik arah
    // membuat bagian yang lebih terang digambar lebih gelap — dan itu terlihat
    // sebagai pita gelap di tengah sorotan, bukan sebagai galat.
    float previous = -1.0f;
    for (float v = 0.0f; v < 40.0f; v += 0.01f) {
        const float mapped = AcesCurve(v);
        CHECK(mapped >= previous - 1e-6f);
        previous = mapped;
    }

    // Titik putihnya jauh di atas 1. Inilah yang membedakan operator nada dari
    // pemotongan: ada rentang di atas putih layar yang masih terbedakan.
    const float white = AcesWhitePoint();
    CHECK(white > 20.0f);
    CHECK(white < 30.0f);
    CHECK(AcesCurve(white) == doctest::Approx(1.0f).epsilon(0.001f));
    CHECK(AcesCurve(white * 0.5f) < 0.999f);
}

TEST_CASE("ACES menahan rona sorotan berwarna yang akan dipatahkan pemotongan") {
    using namespace sim::render;

    // Matahari jingga jauh di atas putih. Pemotongan per kanal akan mendorong
    // hijau dan biru sampai bertemu merah — jingga menjadi putih. ACES boleh
    // memutihkannya, tapi urutan kanalnya harus bertahan.
    const Vec3 orange(12.0f, 5.0f, 1.0f);
    const Vec3 mapped = AcesToneMap(orange);
    CHECK(mapped.x > mapped.y);
    CHECK(mapped.y > mapped.z);
    CHECK(mapped.x <= 1.0f);

    // Pembanding: pemotongan mentah menyamakan merah dan hijau, yaitu persis
    // pergeseran rona yang dihindari.
    const Vec3 clipped = glm::clamp(orange, Vec3(0.0f), Vec3(1.0f));
    CHECK(clipped.x == doctest::Approx(clipped.y));

    // Abu-abu tetap abu-abu: matriks yang tertranspos akan mewarnainya.
    const Vec3 grey = AcesToneMap(Vec3(0.5f));
    CHECK(grey.x == doctest::Approx(grey.y).epsilon(0.002f));
    CHECK(grey.y == doctest::Approx(grey.z).epsilon(0.002f));
}

TEST_CASE("eksposur otomatis tidak bergantung skala adegan") {
    using namespace sim::render;

    // **Ini kriteria terimanya.** Adegan seragam berluminansi berapa pun, sesudah
    // diukur dan dikalikan, harus mendarat di angka yang sama. Kalau tidak,
    // menaikkan seluruh lampu sepuluh kali lipat akan mengubah gambar — dan
    // eksposur yang bergantung skala terlihat benar pada satu adegan saja.
    for (const float luminance : {0.001f, 0.05f, 0.18f, 1.0f, 40.0f, 5000.0f}) {
        const float exposure = AutoExposure(luminance);
        CHECK(luminance * exposure == doctest::Approx(1.0f / 9.6f).epsilon(1e-4f));
    }

    // Kompensasi bekerja dalam stop: +1 stop tepat dua kali lebih terang.
    const float base = AutoExposure(0.18f, 0.0f);
    CHECK(AutoExposure(0.18f, 1.0f) == doctest::Approx(base * 2.0f).epsilon(1e-4f));
    CHECK(AutoExposure(0.18f, -2.0f) == doctest::Approx(base * 0.25f).epsilon(1e-4f));
}

TEST_CASE("rantai reduksi log-luminansi memulihkan rata-rata geometrik") {
    using namespace sim::render;
    using Reducer = LogLuminanceReducer;

    CHECK(Reducer::LevelCount() == 8);  // 256 -> 1

    // Rata-rata 2×2 berjenjang atas petak pangkat dua sama persis dengan
    // rata-rata datarnya. Kalau tidak, eksposur akan bergantung pada di mana
    // sebuah piksel terang kebetulan berada, bukan pada seberapa terang ia.
    std::vector<float> level(16 * 16);
    for (std::size_t i = 0; i < level.size(); ++i) {
        level[i] = static_cast<float>(i % 37) * 0.1f - 1.0f;
    }
    double flat = 0.0;
    for (const float value : level) {
        flat += static_cast<double>(value);
    }
    flat /= static_cast<double>(level.size());

    std::vector<float> reduced = level;
    for (uint32_t size = 16; size > 1; size /= 2) {
        reduced = Reducer::ReduceLevel(reduced, size);
    }
    CHECK(reduced.size() == 1);
    CHECK(reduced.front() == doctest::Approx(static_cast<float>(flat)).epsilon(1e-5f));

    // Gambar seragam kembali sebagai luminansinya sendiri, pada ukuran viewport
    // yang genap maupun ganjil. Ukuran ganjil adalah alasan rantainya berangkat
    // dari petak tetap: eksposur yang bergeser saat pemisah dock digeser tidak
    // akan pernah dicurigai siapa pun.
    for (const float luminance : {0.02f, 0.18f, 4.0f, 250.0f}) {
        for (const auto size : {std::pair<uint32_t, uint32_t>{64, 64},
                                std::pair<uint32_t, uint32_t>{101, 57}}) {
            const std::vector<Vec3> pixels(
                static_cast<std::size_t>(size.first) * size.second, Vec3(luminance));
            const float measured = Reducer::Reduce(pixels, size.first, size.second);
            const float expected = luminance * (0.2126f + 0.7152f + 0.0722f);
            CHECK(measured == doctest::Approx(expected).epsilon(0.02f));
        }
    }
}

TEST_CASE("pengukuran luminansi tahan piksel hitam dan sorotan tunggal") {
    using namespace sim::render;
    using Reducer = LogLuminanceReducer;

    // Piksel hitam tidak menghasilkan NaN. log2(0) adalah negatif tak hingga,
    // dan satu saja sudah cukup meracuni seluruh rata-rata — yang muncul sebagai
    // layar hitam atau putih penuh, bukan sebagai galat.
    std::vector<Vec3> withBlack(64 * 64, Vec3(0.5f));
    withBlack[100] = Vec3(0.0f);
    const float measured = Reducer::Reduce(withBlack, 64, 64);
    CHECK(std::isfinite(measured));
    CHECK(measured > 0.0f);
    CHECK(measured == doctest::Approx(Luminance(Vec3(0.5f))).epsilon(0.05f));

    // Satu sorotan sangat terang di antara ribuan piksel redup hampir tidak
    // menggeser pengukuran — inilah yang tidak dimiliki rata-rata aritmetik.
    std::vector<Vec3> withSpike(64 * 64, Vec3(0.1f));
    withSpike[500] = Vec3(10000.0f);
    const float spiked = Reducer::Reduce(withSpike, 64, 64);
    const float clean = Reducer::Reduce(std::vector<Vec3>(64 * 64, Vec3(0.1f)), 64, 64);
    CHECK(spiked < clean * 1.05f);

    // Pembanding: rata-rata aritmetik atas gambar yang sama meleset lebih dari
    // dua kali lipat, dan itu terlihat sebagai seluruh adegan yang menggelap.
    double arithmetic = 0.0;
    for (const Vec3& pixel : withSpike) {
        arithmetic += static_cast<double>(Luminance(pixel));
    }
    arithmetic /= static_cast<double>(withSpike.size());
    CHECK(arithmetic > clean * 2.0);
}

TEST_CASE("adaptasi eksposur mengejar sasaran dengan dua tetapan waktu") {
    using namespace sim::render;

    // Frame pertama tidak beradaptasi — ia mendarat. Memulai dari nilai lain
    // berarti setiap kali viewport dibuka adegannya menyala dari gelap.
    ExposureAdaptation adaptation(0.5f, 2.0f);
    CHECK_FALSE(adaptation.Ready());
    CHECK(adaptation.Update(0.25f, 0.016f) == doctest::Approx(0.25f));
    CHECK(adaptation.Ready());

    // Satu tetapan waktu menempuh 63,2% jaraknya, menurut definisinya.
    ExposureAdaptation brighten(0.5f, 2.0f);
    brighten.Reset(1.0f);
    // Sasaran lebih kecil berarti adegan menjadi lebih terang → tetapan cepat.
    brighten.Update(0.0f, 0.5f);
    CHECK(brighten.Current() == doctest::Approx(1.0f - 0.6321f).epsilon(0.01f));

    ExposureAdaptation darken(0.5f, 2.0f);
    darken.Reset(0.0f);
    darken.Update(1.0f, 2.0f);
    CHECK(darken.Current() == doctest::Approx(0.6321f).epsilon(0.01f));

    // Dan arah terang memang lebih cepat daripada arah gelap pada langkah waktu
    // yang sama — satu tetapan waktu memaksa memilih antara kedipan dan
    // keterlambatan.
    ExposureAdaptation a(0.5f, 2.0f);
    a.Reset(1.0f);
    a.Update(0.0f, 0.1f);
    ExposureAdaptation b(0.5f, 2.0f);
    b.Reset(0.0f);
    b.Update(1.0f, 0.1f);
    CHECK(1.0f - a.Current() > b.Current());
}

// --- E8.8: bloom ------------------------------------------------------------

TEST_CASE("gambar konstan lewat seluruh rantai bloom tanpa berubah") {
    using namespace sim::render;

    // **Ini kriteria terimanya.** Bloom yang mengubah energi gambar akan diukur
    // eksposur otomatis, yang lalu menurunkan eksposur, yang menurunkan bloom —
    // keduanya saling mengejar dan yang terlihat adalah kecerahan yang bergoyang
    // pelan tanpa sebab yang kelihatan.
    CHECK(BloomChain::DownsampleWeightSum() == doctest::Approx(1.0f).epsilon(1e-6f));

    BloomLevel flat;
    flat.width = 64;
    flat.height = 64;
    flat.pixels.assign(64 * 64, Vec3(2.0f));

    // Turun sekali: konstan tetap konstan, dengan maupun tanpa pembobotan Karis.
    for (const bool karis : {false, true}) {
        const BloomLevel half = BloomChain::Downsample(flat, karis);
        CHECK(half.width == 32);
        for (const Vec3& pixel : half.pixels) {
            CHECK(pixel.x == doctest::Approx(2.0f).epsilon(1e-4f));
        }
    }

    // Naik kembali: tapis tendanya juga berjumlah satu.
    BloomLevel target;
    target.width = 8;
    target.height = 8;
    target.pixels.assign(64, Vec3(0.0f));
    BloomLevel source;
    source.width = 4;
    source.height = 4;
    source.pixels.assign(16, Vec3(3.0f));
    BloomChain::UpsampleInto(target, source, 1.0f);
    for (const Vec3& pixel : target.pixels) {
        CHECK(pixel.x == doctest::Approx(3.0f).epsilon(1e-4f));
    }

    // Dan rantai penuhnya, dengan ambang di bawah nilainya supaya seluruh gambar
    // ikut berpendar.
    BloomSettings settings;
    settings.threshold = 0.0f;
    settings.knee = 0.01f;
    const BloomLevel bloom = BloomChain::Build(flat, settings);
    CHECK(bloom.width == flat.width);
    float lowest = 1e9f;
    float highest = -1e9f;
    for (const Vec3& pixel : bloom.pixels) {
        lowest = std::min(lowest, pixel.x);
        highest = std::max(highest, pixel.x);
    }
    CHECK(lowest == doctest::Approx(2.0f).epsilon(0.01f));
    CHECK(highest == doctest::Approx(2.0f).epsilon(0.01f));
}

TEST_CASE("ambang bloom berlutut lembut, monoton, dan nol di bawahnya") {
    using namespace sim::render;

    constexpr float kThreshold = 1.0f;
    constexpr float kKnee = 0.5f;

    // Jauh di bawah ambang dikurangi lutut: tidak ada sumbangan sama sekali.
    CHECK(BloomChain::SoftThreshold(0.2f, kThreshold, kKnee) == doctest::Approx(0.0f));

    // Jauh di atas: sumbangannya luminansi dikurangi ambang.
    const float high = 8.0f;
    CHECK(BloomChain::SoftThreshold(high, kThreshold, kKnee) * high ==
          doctest::Approx(high - kThreshold).epsilon(1e-3f));

    // Dan di antaranya monoton serta menyambung — ambang tajam membuat permukaan
    // yang luminansinya melintasi ambang berkedip antara berpendar dan tidak.
    float previous = 0.0f;
    for (float luminance = 0.0f; luminance < 4.0f; luminance += 0.005f) {
        const float contribution =
            BloomChain::SoftThreshold(luminance, kThreshold, kKnee) * luminance;
        CHECK(contribution >= previous - 1e-5f);
        CHECK(contribution - previous < 0.02f);  // tanpa loncatan
        previous = contribution;
    }
}

TEST_CASE("pembobotan Karis menjinakkan satu piksel yang jauh lebih terang") {
    using namespace sim::render;

    // Satu piksel sangat terang di tengah lautan gelap. Tanpa pembobotan, ia
    // mendominasi seluruh kelompok dan muncul sebagai bintik berpendar yang
    // berkedip begitu kamera bergeser satu piksel.
    BloomLevel spike;
    spike.width = 16;
    spike.height = 16;
    spike.pixels.assign(256, Vec3(0.05f));
    spike.At(8, 8) = Vec3(500.0f);

    const BloomLevel plain = BloomChain::Downsample(spike, /*karis=*/false);
    const BloomLevel tamed = BloomChain::Downsample(spike, /*karis=*/true);

    float plainPeak = 0.0f;
    float tamedPeak = 0.0f;
    for (std::size_t i = 0; i < plain.pixels.size(); ++i) {
        plainPeak = std::max(plainPeak, plain.pixels[i].x);
        tamedPeak = std::max(tamedPeak, tamed.pixels[i].x);
    }
    CHECK(plainPeak > 50.0f);
    CHECK(tamedPeak < 1.0f);

    // Tapi ia tidak dihilangkan: yang dilakukan pembobotan adalah menurunkan
    // pengaruh, bukan memotong energi seperti penjepitan nilai maksimum.
    CHECK(tamedPeak > 0.05f);
}

TEST_CASE("campuran bloom menambahkan, dan pada kekuatan nol tidak mengubah apa pun") {
    using namespace sim::render;

    BloomLevel scene;
    scene.width = 4;
    scene.height = 4;
    scene.pixels.assign(16, Vec3(1.0f, 2.0f, 3.0f));
    BloomLevel bloom = scene;
    bloom.pixels.assign(16, Vec3(9.0f));

    // **Ditambahkan, bukan dipadu.** Rantainya berambang, jadi di seluruh bagian
    // yang tidak berpendar isinya nol — memadu dengan nol menggelapkan setiap
    // piksel yang tidak berpendar, dan yang terlihat adalah seluruh adegan yang
    // meredup begitu bloom dinyalakan. Bentuk pertama saya memakai paduan, dan
    // inilah yang terukur di editor: latar 62/255 menjadi 46/255 tanpa satu pun
    // halo yang muncul.
    const BloomLevel none = BloomChain::Composite(scene, bloom, 0.0f);
    CHECK(none.pixels.front().y == doctest::Approx(2.0f));
    const BloomLevel half = BloomChain::Composite(scene, bloom, 0.5f);
    CHECK(half.pixels.front().y == doctest::Approx(2.0f + 4.5f));

    // Dan bagian yang tidak berpendar sama sekali tidak tersentuh.
    BloomLevel dark = bloom;
    dark.pixels.assign(16, Vec3(0.0f));
    const BloomLevel untouched = BloomChain::Composite(scene, dark, 0.5f);
    CHECK(untouched.pixels.front().x == doctest::Approx(1.0f));
    CHECK(untouched.pixels.front().y == doctest::Approx(2.0f));
    CHECK(untouched.pixels.front().z == doctest::Approx(3.0f));
}

// --- E8.8: atmosfer Bruneton-Hillaire ---------------------------------------

TEST_CASE("kedalaman optik zenit numerik cocok dengan hitungan analitiknya") {
    using namespace sim::render;

    const AtmosphereParameters atmosphere;
    // **Dua cara menghitung angka yang sama.** Untuk lapisan eksponensial,
    // integral kerapatan sepanjang zenit tepat sama dengan tinggi skalanya;
    // untuk ozon, luas tendanya. Angka yang hanya dihitung satu cara adalah
    // angka yang tidak pernah diperiksa — dan seluruh warna langit bergantung
    // pada angka ini.
    const Vec3 origin(0.0f, 0.0f, atmosphere.bottomRadius);
    const Vec3 zenith(0.0f, 0.0f, 1.0f);
    const Vec3 numeric = IntegrateOpticalDepth(atmosphere, origin, zenith, 4096);
    const Vec3 analytic = AnalyticZenithOpticalDepth(atmosphere);

    CHECK(numeric.x == doctest::Approx(analytic.x).epsilon(0.005f));
    CHECK(numeric.y == doctest::Approx(analytic.y).epsilon(0.005f));
    CHECK(numeric.z == doctest::Approx(analytic.z).epsilon(0.005f));

    // Dan angkanya memang seperti bumi: zenit meneruskan sekitar 94% merah dan
    // 76% biru. Biru lebih banyak dihamburkan keluar — itulah sebabnya langit
    // biru dan matahari di zenit sedikit menguning.
    const Vec3 transmittance = Transmittance(atmosphere, origin, zenith, 4096);
    CHECK(transmittance.x == doctest::Approx(0.940f).epsilon(0.01f));
    CHECK(transmittance.y == doctest::Approx(0.868f).epsilon(0.01f));
    CHECK(transmittance.z == doctest::Approx(0.762f).epsilon(0.01f));
    CHECK(transmittance.x > transmittance.y);
    CHECK(transmittance.y > transmittance.z);
}

TEST_CASE("jalur panjang di dekat horizon memerahkan cahaya yang lewat") {
    using namespace sim::render;

    const AtmosphereParameters atmosphere;
    const Vec3 origin(0.0f, 0.0f, atmosphere.bottomRadius + 0.001f);

    // Matahari terbenam: jalurnya jauh lebih panjang, dan biru habis lebih dulu.
    // Inilah pernyataan numerik dari "matahari terbenam itu merah".
    float previousRatio = 0.0f;
    float previousLuminance = 1e9f;
    for (const float elevation : {60.0f, 30.0f, 10.0f, 2.0f, 0.5f}) {
        const float radians = elevation * 3.14159265f / 180.0f;
        const Vec3 direction(std::cos(radians), 0.0f, std::sin(radians));
        const Vec3 t = Transmittance(atmosphere, origin, direction, 4096);

        // Makin rendah, makin merah — nisbah merah terhadap biru naik terus.
        const float ratio = t.x / std::max(t.z, 1e-6f);
        CHECK(ratio > previousRatio);
        previousRatio = ratio;

        // Dan makin rendah, makin redup seluruhnya.
        CHECK(t.y < previousLuminance);
        previousLuminance = t.y;
    }
    // Di 0,5° di atas horizon, biru tinggal sebagian kecil dari merah.
    CHECK(previousRatio > 3.0f);
}

TEST_CASE("pemetaan uv LUT transmitansi membalik dengan tepat") {
    using namespace sim::render;

    const AtmosphereParameters atmosphere;
    // **Pemetaan yang tidak membalik tidak menghasilkan galat apa pun**, hanya
    // langit yang warnanya masuk akal di tempat yang salah — dan "masuk akal di
    // tempat yang salah" adalah persis yang tidak terlihat saat memandanginya.
    // Baris paling atas dilewati, dan itu bukan kelonggaran melainkan
    // singularitas yang nyata: di sana pemandangnya tepat di puncak atmosfer,
    // jadi setiap arah ke atas menempuh jarak nol dan seluruh baris memetakan ke
    // satu titik. Pemetaannya memang tidak injektif di sana — acuan Bruneton pun
    // begitu — dan LUT-nya tidak pernah dibaca di baris itu.
    for (int yi = 0; yi < 16; ++yi) {
        for (int xi = 0; xi <= 16; ++xi) {
            const Vec2 uv(static_cast<float>(xi) / 16.0f, static_cast<float>(yi) / 16.0f);
            const TransmittanceParams params = UvToTransmittanceParams(atmosphere, uv);
            CHECK(params.radius >= atmosphere.bottomRadius - 1e-2f);
            CHECK(params.radius <= atmosphere.topRadius + 1e-2f);
            const Vec2 back = TransmittanceParamsToUv(atmosphere, params);
            CHECK(back.x == doctest::Approx(uv.x).epsilon(0.002f));
            CHECK(back.y == doctest::Approx(uv.y).epsilon(0.002f));
        }
    }
}

TEST_CASE("pemetaan uv LUT sky-view membalik dan memadat di dekat horizon") {
    using namespace sim::render;

    const AtmosphereParameters atmosphere;
    const Vec2 dimensions(192.0f, 108.0f);
    const float viewHeight = atmosphere.bottomRadius + 0.5f;

    for (int yi = 1; yi < 108; yi += 7) {
        for (int xi = 1; xi < 192; xi += 11) {
            const Vec2 uv((static_cast<float>(xi) + 0.5f) / dimensions.x,
                          (static_cast<float>(yi) + 0.5f) / dimensions.y);
            const SkyViewParams params =
                UvToSkyViewParams(atmosphere, uv, viewHeight, dimensions);
            const float beta = std::asin(atmosphere.bottomRadius / viewHeight);
            const bool intersectsGround = params.viewZenithAngle > 3.14159265f - beta;
            const Vec2 back = SkyViewParamsToUv(atmosphere, params, viewHeight,
                                                intersectsGround, dimensions);
            CHECK(back.x == doctest::Approx(uv.x).epsilon(0.002f));
            CHECK(back.y == doctest::Approx(uv.y).epsilon(0.002f));
        }
    }

    // Dan pemadatannya nyata: sepuluh persen texel teratas dekat horizon
    // mencakup jauh lebih sedikit sudut daripada sepuluh persen di tengah.
    const auto span = [&](float from, float to) {
        return UvToSkyViewParams(atmosphere, Vec2(0.5f, to), viewHeight, dimensions)
                   .viewZenithAngle -
               UvToSkyViewParams(atmosphere, Vec2(0.5f, from), viewHeight, dimensions)
                   .viewZenithAngle;
    };
    CHECK(std::abs(span(0.45f, 0.55f)) < std::abs(span(0.05f, 0.15f)));
}

TEST_CASE("pemetaan slice aerial perspective membalik dan memadat di dekat kamera") {
    using namespace sim::render;

    constexpr uint32_t kSlices = 32;
    constexpr float kMaxKm = 4.0f;

    // **Pemetaan yang meleset tidak menghasilkan galat apa pun**, hanya kabut
    // yang pekatnya benar pada jarak yang salah — dan itu tampak seperti pilihan
    // seni yang buruk, bukan seperti bug.
    for (uint32_t slice = 0; slice < kSlices; ++slice) {
        const float distance = AerialSliceDistance(slice, kSlices, kMaxKm);
        const float coord = AerialDistanceToSliceCoord(distance, kSlices, kMaxKm);
        const float expected = (static_cast<float>(slice) + 0.5f) / static_cast<float>(kSlices);
        CHECK(coord == doctest::Approx(expected).epsilon(0.001f));
    }

    // Dan pemadatannya nyata: delapan slice pertama menampung jauh lebih sedikit
    // jarak daripada delapan slice terakhir. Kabut yang berarti ada di dekat
    // kamera; sebaran seragam akan menghabiskan resolusinya di kejauhan yang
    // isinya sudah nyaris tak berubah.
    const float nearSpan = AerialSliceDistance(7, kSlices, kMaxKm) -
                           AerialSliceDistance(0, kSlices, kMaxKm);
    const float farSpan = AerialSliceDistance(31, kSlices, kMaxKm) -
                          AerialSliceDistance(24, kSlices, kMaxKm);
    CHECK(nearSpan < farSpan * 0.2f);
}

TEST_CASE("aerial perspective menyambung tanpa jahitan: 0→D sama dengan 0→d lalu d→D") {
    using namespace sim::render;

    // **Ini uji yang menentukan.** LUT aerial perspective berhenti di jarak
    // maksimumnya, dan yang berada di baliknya diwarnai pass langit. Kalau
    // integral yang dipotong dua tidak sama dengan integral utuh, kedua bagian
    // itu tidak akan bertemu — dan yang terlihat adalah garis jahitan tepat di
    // tempat terrain menyentuh langit, yaitu tempat yang paling diperhatikan
    // orang. Sifat yang menjamin sambungannya: transmitansi mengalikan, dan
    // hamburan yang di belakang harus lewat transmitansi yang di depan.
    const AtmosphereParameters atmosphere;
    const Vec3 origin(0.0f, 0.0f, atmosphere.bottomRadius + 0.5f);
    const Vec3 direction = glm::normalize(Vec3(1.0f, 0.0f, 0.08f));
    const Vec3 sun = glm::normalize(Vec3(0.4f, 0.0f, 0.9f));

    constexpr float kTotal = 40.0f;
    constexpr float kSplit = 12.0f;

    const AerialSample whole =
        IntegrateAerialPerspective(atmosphere, origin, direction, sun, kTotal, 512);
    const AerialSample front =
        IntegrateAerialPerspective(atmosphere, origin, direction, sun, kSplit, 154);
    const AerialSample back = IntegrateAerialPerspective(
        atmosphere, origin + direction * kSplit, direction, sun, kTotal - kSplit, 358);

    const Vec3 chainedTransmittance = front.transmittance * back.transmittance;
    const Vec3 chainedInscatter = front.inscatter + front.transmittance * back.inscatter;

    CHECK(chainedTransmittance.x == doctest::Approx(whole.transmittance.x).epsilon(0.002f));
    CHECK(chainedTransmittance.z == doctest::Approx(whole.transmittance.z).epsilon(0.002f));
    CHECK(chainedInscatter.x == doctest::Approx(whole.inscatter.x).epsilon(0.01f));
    CHECK(chainedInscatter.z == doctest::Approx(whole.inscatter.z).epsilon(0.01f));
}

TEST_CASE("aerial perspective tidak menyentuh apa pun pada jarak nol") {
    using namespace sim::render;

    // Kabut yang sudah pekat pada jarak nol adalah kabut yang menyelimuti benda
    // yang dipegang kamera. Cacat ini muncul dari pemetaan slice yang bergeser
    // setengah texel, dan ia terlihat pada setiap adegan sekaligus — sehingga
    // tampak seperti "renderer-nya memang begitu".
    const AtmosphereParameters atmosphere;
    const Vec3 origin(0.0f, 0.0f, atmosphere.bottomRadius + 0.2f);
    const Vec3 direction = glm::normalize(Vec3(1.0f, 0.0f, 0.1f));
    const Vec3 sun = glm::normalize(Vec3(0.3f, 0.0f, 0.95f));
    const Vec3 scene(0.4f, 0.5f, 0.6f);

    const AerialSample zero =
        IntegrateAerialPerspective(atmosphere, origin, direction, sun, 0.0f, 64);
    const Vec3 composited = ApplyAerialPerspective(scene, zero);
    CHECK(composited.x == doctest::Approx(scene.x));
    CHECK(composited.y == doctest::Approx(scene.y));
    CHECK(composited.z == doctest::Approx(scene.z));
}

TEST_CASE("udara memakan biru dan mengembalikannya sebagai kabut biru") {
    using namespace sim::render;

    // Dua efek yang berlawanan arah, dan keduanya harus ada. Transmitansi
    // **memerahkan** apa yang lewat — biru dihamburkan keluar lebih dulu.
    // Hamburan masuk **membirukan** apa yang ada di depannya — biru itu
    // dihamburkan masuk dari arah lain. Yang membuat gunung jauh tampak biru
    // adalah suku kedua yang menang atas suku pertama; sebuah implementasi yang
    // hanya punya salah satunya akan tampak masuk akal sendirian.
    const AtmosphereParameters atmosphere;
    const Vec3 origin(0.0f, 0.0f, atmosphere.bottomRadius + 0.1f);
    const Vec3 direction = glm::normalize(Vec3(1.0f, 0.0f, 0.02f));
    const Vec3 sun = glm::normalize(Vec3(0.2f, 0.3f, 0.93f));

    Vec3 previousTransmittance(2.0f);
    Vec3 previousInscatter(-1.0f);
    for (const float distance : {0.5f, 2.0f, 8.0f, 32.0f}) {
        const AerialSample sample =
            IntegrateAerialPerspective(atmosphere, origin, direction, sun, distance, 256);

        // Makin jauh, makin sedikit yang lewat — dan biru berkurang lebih cepat.
        CHECK(sample.transmittance.z < sample.transmittance.x);
        CHECK(sample.transmittance.x < previousTransmittance.x);
        CHECK(sample.inscatter.z > previousInscatter.z);
        previousTransmittance = sample.transmittance;
        previousInscatter = sample.inscatter;
    }

    // Dan kabut yang ditambahkannya biru: pada jarak jauh, biru jauh melebihi
    // merah.
    const AerialSample far =
        IntegrateAerialPerspective(atmosphere, origin, direction, sun, 32.0f, 256);
    CHECK(far.inscatter.z > far.inscatter.x * 2.0f);

    // Permukaan hitam yang jauh karena itu tidak hitam melainkan biru — persis
    // yang membuat gunung di kejauhan tampak biru.
    const Vec3 black = ApplyAerialPerspective(Vec3(0.0f), far);
    CHECK(black.z > black.x);
    CHECK(black.z > 0.0f);
}

// --- E8.8: peta lingkungan equirectangular -----------------------------------

TEST_CASE("pemetaan equirect membalik dengan tepat di kedua arah") {
    using namespace sim::render;

    // **Pemetaan yang tidak membalik tidak menghasilkan galat apa pun**, hanya
    // langit yang isinya benar di tempat yang salah: matahari di HDRI muncul di
    // arah yang berbeda dari matahari yang menerangi adegan, dan yang terlihat
    // adalah bayangan yang "arahnya aneh" alih-alih peta yang terpasang
    // terbalik.
    for (int yi = 1; yi < 32; ++yi) {
        for (int xi = 0; xi < 32; ++xi) {
            const Vec2 uv((static_cast<float>(xi) + 0.5f) / 32.0f,
                          (static_cast<float>(yi) + 0.5f) / 32.0f);
            const Vec3 direction = EquirectUvToDirection(uv);
            CHECK(glm::length(direction) == doctest::Approx(1.0f).epsilon(1e-4f));
            const Vec2 back = DirectionToEquirectUv(direction);
            CHECK(back.x == doctest::Approx(uv.x).epsilon(1e-3f));
            CHECK(back.y == doctest::Approx(uv.y).epsilon(1e-3f));
        }
    }

    // Dan arah-arah yang bisa diperiksa tangan. v = 0 adalah zenit, v = 1 nadir,
    // dan u = 0,5 adalah −Z yaitu arah pandang bawaan kamera.
    CHECK(EquirectUvToDirection(Vec2(0.5f, 0.0f)).y == doctest::Approx(1.0f));
    CHECK(EquirectUvToDirection(Vec2(0.5f, 1.0f)).y == doctest::Approx(-1.0f));
    const Vec3 forward = EquirectUvToDirection(Vec2(0.5f, 0.5f));
    CHECK(forward.z == doctest::Approx(-1.0f).epsilon(1e-4f));
    CHECK(DirectionToEquirectUv(Vec3(0.0f, 1.0f, 0.0f)).y == doctest::Approx(0.0f));
    CHECK(DirectionToEquirectUv(Vec3(0.0f, 0.0f, -1.0f)).x == doctest::Approx(0.5f));
}

TEST_CASE("peta equirect membungkus di U dan menjepit di V") {
    using namespace sim::render;

    // Dua texel dengan warna berbeda di ujung kiri dan kanan baris yang sama.
    EquirectEnvironment environment;
    environment.width = 4;
    environment.height = 2;
    environment.pixels.assign(4 * 2 * 3, 0.0f);
    const auto set = [&](uint32_t x, uint32_t y, const Vec3& color) {
        const std::size_t at = (static_cast<std::size_t>(y) * 4 + x) * 3;
        environment.pixels[at] = color.x;
        environment.pixels[at + 1] = color.y;
        environment.pixels[at + 2] = color.z;
    };
    for (uint32_t y = 0; y < 2; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            set(x, y, Vec3(0.5f));
        }
    }
    set(0, 0, Vec3(1.0f, 0.0f, 0.0f));
    set(3, 0, Vec3(0.0f, 0.0f, 1.0f));

    // **U membungkus.** Tepat di u = 0 texel pertama dan texel terakhir saling
    // bertetangga, jadi cuplikan di sana harus memadu keduanya. Sampler yang
    // menjepit mengembalikan texel pertama apa adanya — dan yang terlihat adalah
    // jahitan tegak selebar satu texel yang membelah langit dari zenit ke nadir.
    const Vec3 seam = environment.SampleUv(Vec2(0.0f, 0.25f));
    CHECK(seam.x == doctest::Approx(0.5f).epsilon(0.01f));
    CHECK(seam.z == doctest::Approx(0.5f).epsilon(0.01f));

    // **V menjepit.** Di atas baris pertama tidak ada apa-apa; membungkusnya
    // akan mengambil warna dari kutub seberang, yaitu langit yang tiba-tiba
    // menjadi tanah tepat di zenit.
    const Vec3 top = environment.SampleUv(Vec2(0.125f, 0.0f));
    CHECK(top.x == doctest::Approx(1.0f));
    const Vec3 above = environment.SampleUv(Vec2(0.125f, -0.5f));
    CHECK(above.x == doctest::Approx(top.x));

    // Peta kosong menjawab hitam alih-alih membaca larik kosong.
    const EquirectEnvironment empty;
    CHECK(empty.IsValid() == false);
    CHECK(empty.Sample(Vec3(0.0f, 1.0f, 0.0f)).x == doctest::Approx(0.0f));
}

TEST_CASE("peta equirect adalah IEnvironmentSampler yang utuh") {
    using namespace sim::render;

    // **Sifat yang membuatnya berguna melampaui skybox:** ia antarmuka yang
    // sama dengan `GradientSky`, jadi seluruh rantai IBL menerimanya tanpa satu
    // baris pun berubah. Yang diuji di sini bukan warnanya melainkan bahwa
    // rantai itu benar-benar jalan di atasnya.
    EquirectEnvironment environment;
    environment.width = 8;
    environment.height = 4;
    environment.pixels.assign(8 * 4 * 3, 0.0f);
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            // Terang di atas, gelap di bawah — lingkungan yang iradiansinya
            // punya arah yang jelas.
            const float value = y < 2 ? 4.0f : 0.25f;
            const std::size_t at = (static_cast<std::size_t>(y) * 8 + x) * 3;
            environment.pixels[at] = value;
            environment.pixels[at + 1] = value;
            environment.pixels[at + 2] = value;
        }
    }

    const Sh9 sh = ProjectIrradiance(environment, 4096);
    const Vec3 up = EvaluateIrradiance(sh, Vec3(0.0f, 1.0f, 0.0f));
    const Vec3 down = EvaluateIrradiance(sh, Vec3(0.0f, -1.0f, 0.0f));
    CHECK(up.y > down.y);
    CHECK(down.y >= 0.0f);
}

TEST_CASE("peta lingkungan HDR dimuat dari berkas") {
    using namespace sim::render;

    // Titik dekode ini dialihkan lewat `Sim::ImageIO` di I0, dan sebelumnya
    // tidak ada satu pun uji yang menyentuhnya — jadi "masih bekerja persis
    // seperti sebelumnya" adalah klaim yang tidak bisa diperiksa siapa pun.
    // Berkasnya dibuat enkoder di luar mesin ini; isinya diterangkan di
    // Tests/ImageIOTests.cpp.
    const std::filesystem::path path =
        std::filesystem::path(SIM_IMAGE_DIR) / "gradient.hdr";
    const EquirectEnvironment environment = LoadHdrEquirect(path);

    REQUIRE(environment.IsValid());
    CHECK(environment.width == 8);
    CHECK(environment.height == 4);
    // Tiga kanal, bukan empat: berkas HDR tidak punya alfa, dan kanal keempat
    // yang diarang akan menambah sepertiga memori tanpa satu bit informasi.
    CHECK(environment.pixels.size() == 8u * 4u * 3u);

    // piksel(x,y) = (x/8, y/4, 0.5), dan eksponen RGBE-nya dipatok sehingga
    // nilainya tepat. Yang dijaga di sini adalah bahwa tidak ada gamma yang
    // diterapkan diam-diam: 0.875 yang menjadi 0.73 tidak akan terlihat sebagai
    // galat mana pun — hanya sebagai langit yang salah terang.
    CHECK(environment.pixels[0] == doctest::Approx(0.0f).epsilon(0.0001));
    CHECK(environment.pixels[2] == doctest::Approx(0.5f).epsilon(0.0001));
    const std::size_t last = (3u * 8u + 7u) * 3u;
    CHECK(environment.pixels[last] == doctest::Approx(7.0f / 8.0f).epsilon(0.0001));
    CHECK(environment.pixels[last + 1] == doctest::Approx(3.0f / 4.0f).epsilon(0.0001));

    // Berkas yang tidak ada mengembalikan lingkungan tidak valid, bukan crash.
    CHECK_FALSE(LoadHdrEquirect(std::filesystem::path(SIM_IMAGE_DIR) / "hilang.hdr").IsValid());
}

TEST_CASE("EXR menghasilkan lingkungan yang sama dengan HDR dari sumber yang sama") {
    using namespace sim::render;

    // **Kriteria terima I2, dan bentuknya sengaja bukan "menghasilkan gambar".**
    // Dua berkas ini berisi nilai yang sama persis, ditulis tangan oleh dua
    // penulis yang berbeda — RGBE untuk `.hdr`, dan penulis EXR mandiri untuk
    // `.exr` (lihat catatannya di Tests/ImageIOTests.cpp). Yang diuji adalah
    // bahwa jalur muatnya sampai pada angka yang sama lewat keduanya.
    const std::filesystem::path dir(SIM_IMAGE_DIR);
    const EquirectEnvironment fromHdr = LoadHdrEquirect(dir / "gradient.hdr");
    REQUIRE(fromHdr.IsValid());

    if (!sim::imageio::CanRead(".exr")) {
        // Tanpa backend EXR, `.exr` memang tidak ada di daftar format — dan
        // yang dituntut kriteria terimanya justru itu: ditolak, bukan dimuat
        // separuh. Asset Browser pun tidak menawarkannya.
        CHECK_FALSE(LoadHdrEquirect(dir / "gradient.exr").IsValid());
        MESSAGE("build ini tanpa tinyexr — .exr ditolak sebagaimana mestinya");
        return;
    }

    const EquirectEnvironment fromExr = LoadHdrEquirect(dir / "gradient.exr");
    REQUIRE(fromExr.IsValid());

    CHECK(fromExr.width == fromHdr.width);
    CHECK(fromExr.height == fromHdr.height);
    REQUIRE(fromExr.pixels.size() == fromHdr.pixels.size());

    // Toleransinya ditulis, bukan dikira-kira. Keduanya menyimpan nilai yang
    // tepat pada float32 — RGBE dengan eksponen yang dipatok, EXR sebagai float
    // penuh — jadi tidak ada alasan keduanya berbeda sama sekali. Toleransi
    // longgar di sini akan menyembunyikan persis kesalahan yang dicari.
    for (std::size_t i = 0; i < fromHdr.pixels.size(); ++i) {
        CHECK(fromExr.pixels[i] == doctest::Approx(fromHdr.pixels[i]).epsilon(0.0001));
    }

    // Dan iradiansinya ikut sama, karena itulah yang benar-benar dipakai IBL.
    const Sh9 hdrSh = ProjectIrradiance(fromHdr, 1024);
    const Sh9 exrSh = ProjectIrradiance(fromExr, 1024);
    for (const Vec3& direction : {Vec3(0.0f, 1.0f, 0.0f), Vec3(1.0f, 0.0f, 0.0f),
                                  Vec3(0.0f, -1.0f, 0.0f)}) {
        const Vec3 fromHdrIrradiance = EvaluateIrradiance(hdrSh, direction);
        const Vec3 fromExrIrradiance = EvaluateIrradiance(exrSh, direction);
        CHECK(fromExrIrradiance.x == doctest::Approx(fromHdrIrradiance.x).epsilon(0.0001));
        CHECK(fromExrIrradiance.y == doctest::Approx(fromHdrIrradiance.y).epsilon(0.0001));
        CHECK(fromExrIrradiance.z == doctest::Approx(fromHdrIrradiance.z).epsilon(0.0001));
    }
}

TEST_CASE("EXR yang bukan peta RGB ditolak") {
    using namespace sim::render;

    if (!sim::imageio::CanRead(".exr")) {
        return;  // tanpa tinyexr tidak ada `.exr` untuk ditolak dengan alasan ini
    }

    // Bentuknya sama persis dengan HDRI — 8x4, tiga kanal float. Yang
    // membedakannya cuma nama kanalnya, dan itu satu-satunya yang bisa
    // membedakan render multi-channel dari peta lingkungan.
    const std::filesystem::path path =
        std::filesystem::path(SIM_IMAGE_DIR) / "aov-bukan-lingkungan.exr";
    REQUIRE(std::filesystem::exists(path));
    CHECK_FALSE(LoadHdrEquirect(path).IsValid());

    // Dan yang dibaca lewat lapisan I/O tetap sah — yang menolak adalah aturan
    // "ini bukan peta lingkungan", bukan dekodernya. Membedakan keduanya
    // penting: berkasnya baik-baik saja, pemakaiannya yang salah.
    sim::imageio::Image image;
    REQUIRE(sim::imageio::Read(path, sim::imageio::ReadOptions{}, image).ok);
    REQUIRE(image.desc.channelNames.size() == 3);
    CHECK(image.desc.channelNames[0] != "R");
}

// --- E8.8: derau awan volumetrik ---------------------------------------------

TEST_CASE("derau awan menyambung di ketiga tepinya") {
    using namespace sim::render;

    // **Ini sifat yang menentukan seluruhnya.** Lapisan awan membentang puluhan
    // kilometer dan volume deraunya beberapa puluh texel, jadi ia diulang
    // berkali-kali di setiap arah. Derau yang tidak menyambung menaruh tepi
    // tajam pada setiap batas pengulangan — dan yang terlihat bukan derau yang
    // salah melainkan **kisi garis lurus di langit**, teratur sempurna, yang
    // tidak mungkin dikira awan oleh siapa pun. Cacat ini juga tidak muncul
    // sebagai galat apa pun: tiap texel di dalam volumenya benar.
    for (int i = 0; i < 24; ++i) {
        const float a = static_cast<float>(i) / 24.0f;
        const float b = static_cast<float>((i * 7) % 24) / 24.0f;

        // Worley dan Perlin, digeser satu periode penuh di tiap sumbu.
        const Vec3 base(a, b, 0.37f);
        for (const Vec3& shift : {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f),
                                  Vec3(0.0f, 0.0f, 1.0f)}) {
            CHECK(CloudNoise::Worley(base, 4, 1234) ==
                  doctest::Approx(CloudNoise::Worley(base + shift, 4, 1234)).epsilon(1e-4f));
            CHECK(CloudNoise::Perlin(base, 4, 1234) ==
                  doctest::Approx(CloudNoise::Perlin(base + shift, 4, 1234)).epsilon(1e-4f));
            // Dan FBM-nya juga — tiap oktafnya melipatduakan frekuensi, dan
            // frekuensi yang tetap bilangan bulat itulah syarat agar jumlahnya
            // ikut menyambung. Faktor pecahan menghasilkan oktaf yang
            // masing-masing rapi tapi jumlahnya tidak menyambung sama sekali.
            CHECK(CloudNoise::WorleyFbm(base, 4, 3, 99) ==
                  doctest::Approx(CloudNoise::WorleyFbm(base + shift, 4, 3, 99)).epsilon(1e-4f));
            CHECK(CloudNoise::PerlinWorley(base, 4, 55) ==
                  doctest::Approx(CloudNoise::PerlinWorley(base + shift, 4, 55)).epsilon(1e-4f));
        }
    }
}

TEST_CASE("derau awan berada di 0..1 dan benar-benar berderau") {
    using namespace sim::render;

    // Sebuah "derau" yang ternyata tetapan tidak menghasilkan galat apa pun,
    // hanya langit yang tertutup rata atau kosong sama sekali — dan keduanya
    // mudah dikira pilihan pengaturan.
    float lowest = 2.0f;
    float highest = -1.0f;
    double sum = 0.0;
    int count = 0;
    for (int z = 0; z < 12; ++z) {
        for (int y = 0; y < 12; ++y) {
            for (int x = 0; x < 12; ++x) {
                const Vec3 p((x + 0.5f) / 12.0f, (y + 0.5f) / 12.0f, (z + 0.5f) / 12.0f);
                const float value = CloudNoise::PerlinWorley(p, 4, 2024);
                CHECK(value >= 0.0f);
                CHECK(value <= 1.0f);
                lowest = std::min(lowest, value);
                highest = std::max(highest, value);
                sum += value;
                ++count;
            }
        }
    }
    CHECK(highest - lowest > 0.4f);
    const double mean = sum / count;
    CHECK(mean > 0.05);
    CHECK(mean < 0.95);
}

TEST_CASE("Worley bernilai nol tepat di titik fiturnya, dan naik menjauhinya") {
    using namespace sim::render;

    // Worley adalah jarak ke titik fitur terdekat. Kalau ia tidak pernah
    // menyentuh nol, titik fiturnya tidak berada di tempat yang dikira
    // pencariannya — cacat yang muncul sebagai awan yang seluruhnya sama
    // rapatnya, bukan sebagai galat.
    float smallest = 1.0f;
    for (int i = 0; i < 4096; ++i) {
        const Vec3 p(static_cast<float>(i % 16) / 16.0f,
                     static_cast<float>((i / 16) % 16) / 16.0f,
                     static_cast<float>(i / 256) / 16.0f);
        smallest = std::min(smallest, CloudNoise::Worley(p, 4, 5150));
    }
    CHECK(smallest < 0.1f);

    // Dan benih yang berbeda menghasilkan bidang yang berbeda — kalau tidak,
    // volume bentuk dan volume rincian akan mengikis dirinya sendiri dengan
    // pola yang sama persis, yang meniadakan seluruh guna keduanya terpisah.
    int different = 0;
    for (int i = 0; i < 64; ++i) {
        const Vec3 p(static_cast<float>(i) / 64.0f, 0.3f, 0.7f);
        if (std::abs(CloudNoise::Worley(p, 4, 1) - CloudNoise::Worley(p, 4, 2)) > 0.01f) {
            ++different;
        }
    }
    CHECK(different > 50);
}

TEST_CASE("gradien ketinggian awan nol di kedua ujung lapisannya") {
    using namespace sim::render;

    // **Nol di alas juga, bukan hanya di puncak.** Lapisan yang dipotong rata di
    // bawah memperlihatkan alasnya sebagai bidang datar sempurna yang membentang
    // sampai horizon, dan tidak ada yang lebih cepat memberi tahu mata bahwa
    // langitnya palsu.
    CHECK(CloudHeightGradient(1.5f, 1.5f, 4.0f) == doctest::Approx(0.0f));
    CHECK(CloudHeightGradient(4.0f, 1.5f, 4.0f) == doctest::Approx(0.0f));
    CHECK(CloudHeightGradient(1.0f, 1.5f, 4.0f) == doctest::Approx(0.0f));
    CHECK(CloudHeightGradient(9.0f, 1.5f, 4.0f) == doctest::Approx(0.0f));

    // Di dalamnya positif, dan puncaknya berada di bawah tengah — dasar kumulus
    // hampir rata sementara puncaknya berjumbai panjang.
    float peakAt = 0.0f;
    float peak = 0.0f;
    for (int i = 1; i < 100; ++i) {
        const float fraction = static_cast<float>(i) / 100.0f;
        const float value = CloudHeightGradient(1.5f + fraction * 2.5f, 1.5f, 4.0f);
        CHECK(value > 0.0f);
        CHECK(value <= 1.0f);
        if (value > peak) {
            peak = value;
            peakAt = fraction;
        }
    }
    CHECK(peakAt < 0.5f);

    // Lapisan yang tebalnya nol atau terbalik tidak boleh membagi dengan nol.
    CHECK(CloudHeightGradient(2.0f, 3.0f, 3.0f) == doctest::Approx(0.0f));
    CHECK(CloudHeightGradient(2.0f, 4.0f, 1.0f) == doctest::Approx(0.0f));
}

TEST_CASE("volume derau awan terisi dan tiap kanalnya berbeda") {
    using namespace sim::render;

    // Kecil, karena yang diuji bentuknya bukan kualitasnya.
    const CloudNoiseVolume shape = BuildCloudShapeVolume(8, 4242);
    CHECK(shape.size == 8);
    CHECK(shape.texels.size() == 8u * 8u * 8u * 4u);

    // Empat kanal yang isinya sama adalah empat kanal yang tiga di antaranya
    // terbuang — dan bobot yang disetel pemakai lalu tidak berpengaruh apa pun,
    // yang tampak seperti slider yang rusak.
    for (uint32_t channel = 1; channel < 4; ++channel) {
        int different = 0;
        for (uint32_t z = 0; z < 8; ++z) {
            for (uint32_t y = 0; y < 8; ++y) {
                for (uint32_t x = 0; x < 8; ++x) {
                    if (std::abs(shape.At(x, y, z, 0) - shape.At(x, y, z, channel)) > 0.02f) {
                        ++different;
                    }
                }
            }
        }
        CHECK(different > 256);
    }

    const CloudNoiseVolume detail = BuildCloudDetailVolume(8, 4242);
    CHECK(detail.size == 8);
    CHECK(detail.texels.size() == 8u * 8u * 8u * 4u);

    // Di luar batas mengembalikan nol alih-alih membaca memori tetangga.
    // **Dijaga per sumbu, bukan pada indeks datarnya:** indeks datar dari x yang
    // melewati tepi tetap jatuh di dalam larik, hanya di baris berikutnya.
    CHECK(shape.At(8, 0, 0, 0) == doctest::Approx(0.0f));
    CHECK(shape.At(0, 8, 0, 0) == doctest::Approx(0.0f));
    CHECK(shape.At(0, 0, 8, 0) == doctest::Approx(0.0f));
    CHECK(shape.At(0, 0, 0, 4) == doctest::Approx(0.0f));
}

TEST_CASE("tiap kanal volume derau meregang ke seluruh rentangnya") {
    using namespace sim::render;

    // **Bukan penghalusan, dan ini terukur.** Sebelum peregangan ada, kanal
    // gabungan volume bentuk membentang 0,09..0,71 dengan median 0,24 — dan
    // cakupan, yang berupa ambang, karena itu memotong di tempat yang hanya
    // dilewati 0,2% volume. Yang terlihat bukan derau yang salah melainkan
    // langit yang cerah, yang tidak bisa dibedakan dari sakelar yang mati.
    const CloudNoiseVolume shape = BuildCloudShapeVolume(16, 777);
    for (uint32_t channel = 0; channel < 4; ++channel) {
        float lowest = 2.0f;
        float highest = -1.0f;
        double sum = 0.0;
        for (uint32_t z = 0; z < 16; ++z) {
            for (uint32_t y = 0; y < 16; ++y) {
                for (uint32_t x = 0; x < 16; ++x) {
                    const float value = shape.At(x, y, z, channel);
                    lowest = std::min(lowest, value);
                    highest = std::max(highest, value);
                    sum += value;
                }
            }
        }
        CHECK(lowest <= 1.0f / 255.0f);
        CHECK(highest >= 254.0f / 255.0f);
        // Dan pusatnya berada di tengah rentangnya, bukan menempel di salah satu
        // ujung — ambang yang berguna menuntut nilai di kedua sisinya.
        const double mean = sum / (16 * 16 * 16);
        CHECK(mean > 0.2);
        CHECK(mean < 0.8);
    }
}

TEST_CASE("fungsi fase berintegral satu atas bola") {
    using namespace sim::render;

    // **Fase yang tidak berintegral satu memindahkan energi masuk atau keluar
    // dari adegan pada setiap peristiwa hamburan** — dan pada ratusan langkah
    // raymarch, selisih satu persen menjadi langit yang terlalu terang atau
    // terlalu gelap tanpa satu pun bagian yang tampak salah sendiri.
    constexpr int kSteps = 20000;
    double rayleigh = 0.0;
    double mie = 0.0;
    for (int i = 0; i < kSteps; ++i) {
        const double u = (static_cast<double>(i) + 0.5) / kSteps;
        const double cosTheta = 1.0 - 2.0 * u;  // seragam dalam cos, seperti bola
        rayleigh += RayleighPhase(static_cast<float>(cosTheta));
        mie += MiePhase(static_cast<float>(cosTheta), 0.8f);
    }
    // dΩ = 2π d(cosθ), dan langkahnya 2/kSteps dalam cosθ.
    const double scale = 2.0 * 3.14159265358979 * 2.0 / kSteps;
    CHECK(rayleigh * scale == doctest::Approx(1.0).epsilon(0.001));
    CHECK(mie * scale == doctest::Approx(1.0).epsilon(0.01));

    // Mie sangat condong ke depan: ke arah matahari ia puluhan kali fase
    // Rayleigh (34x pada g = 0,8), dan itulah lingkar terang di sekitar matahari.
    CHECK(MiePhase(1.0f, 0.8f) > RayleighPhase(1.0f) * 30.0f);
    // Rayleigh simetris ke depan dan ke belakang; Mie tidak.
    CHECK(RayleighPhase(1.0f) == doctest::Approx(RayleighPhase(-1.0f)));
    CHECK(MiePhase(1.0f, 0.8f) > MiePhase(-1.0f, 0.8f) * 100.0f);
}

// --- V1: SDF hasil bake mengisi clipmap --------------------------------------

namespace {

/// Grid SDF bola, diisi dari rumus analitik alih-alih dibake.
///
/// **Sengaja tanpa OpenVDB.** Yang diuji di sini adalah `BakedSceneField` —
/// transformasi, pembuangan, dan penggabungan batas bawahnya — bukan bakernya.
/// Memisahkan keduanya membuat uji ini berjalan di setiap konfigurasi, dan
/// membuat kegagalannya menunjuk satu tersangka.
sim::SdfGrid MakeSphereGrid(float radius, float voxelSize, float bandVoxels) {
    sim::SdfGrid grid;
    grid.voxelSize = voxelSize;
    grid.band = bandVoxels * voxelSize;
    const float reach = radius + grid.band + voxelSize;
    const auto side = static_cast<uint32_t>(std::ceil(2.0f * reach / voxelSize)) + 1;
    grid.sizeX = side;
    grid.sizeY = side;
    grid.sizeZ = side;
    grid.origin = Vec3(-static_cast<float>(side - 1) * 0.5f * voxelSize);
    grid.distances.resize(grid.VoxelCount());
    for (uint32_t z = 0; z < side; ++z) {
        for (uint32_t y = 0; y < side; ++y) {
            for (uint32_t x = 0; x < side; ++x) {
                const Vec3 p = grid.origin + Vec3(static_cast<float>(x), static_cast<float>(y),
                                                  static_cast<float>(z)) *
                                                 voxelSize;
                const float d = glm::length(p) - radius;
                grid.distances[(static_cast<std::size_t>(z) * side + y) * side + x] =
                    std::clamp(d, -grid.band, grid.band);
            }
        }
    }
    return grid;
}

}  // namespace

TEST_CASE("SDF hasil bake mengalahkan hampiran kotak di arah diagonal") {
    using namespace sim::render;

    // **Inilah alasan bake itu ada.** Bola berjari-jari 1 di dalam kotak
    // pembungkus 2x2x2: di arah diagonal, jarak ke kotaknya nol tepat di titik
    // yang permukaan bolanya masih 0,73 jauhnya. Sphere tracing yang percaya
    // angka nol berhenti melangkah dan menggambar dinding yang tidak ada.
    MeshInstance instance;
    instance.transform = Mat4(1.0f);
    instance.boundsMin = Vec3(-1.0f);
    instance.boundsMax = Vec3(1.0f);

    const sim::SdfGrid grid = MakeSphereGrid(1.0f, 0.05f, 6.0f);
    const sim::SdfGrid* grids[]{&grid};

    BakedSceneField baked;
    baked.Build(std::span<const MeshInstance>(&instance, 1),
                std::span<const sim::SdfGrid* const>(grids, 1));
    REQUIRE(baked.BakedCount() == 1);

    BoxSceneField boxes;
    boxes.Build(std::span<const MeshInstance>(&instance, 1));

    const Vec3 corner = glm::normalize(Vec3(1.0f, 1.0f, 1.0f)) * 1.2f;
    const float trueDistance = glm::length(corner) - 1.0f;  // 0,2
    const float fromBaked = baked.Distance(corner);
    const float fromBoxes = boxes.Distance(corner);

    INFO("sejati " << trueDistance << ", bake " << fromBaked << ", kotak " << fromBoxes);
    CHECK(fromBaked == doctest::Approx(trueDistance).epsilon(0.05));
    // Kotaknya menyebut titik itu **di dalam** — nilainya negatif — padahal ia
    // jelas di luar bolanya. Itu bukan sekadar kurang teliti; tandanya salah.
    CHECK(fromBoxes < fromBaked);
    CHECK(std::abs(fromBoxes - trueDistance) > std::abs(fromBaked - trueDistance));
}

TEST_CASE("medan bake mencampur mesh ter-bake dan yang belum") {
    using namespace sim::render;

    // Adegan nyata selalu campuran: kubus satuan bawaan, mesh yang gagal
    // dibake, mesh yang terlalu besar untuk voxel sehalus itu. Yang punya grid
    // memakai grid; sisanya tetap memakai kotak, dan yang keluar tetap satu
    // medan jarak.
    MeshInstance sphere;
    sphere.boundsMin = Vec3(-1.0f);
    sphere.boundsMax = Vec3(1.0f);

    MeshInstance box;
    box.transform = glm::translate(Mat4(1.0f), Vec3(6.0f, 0.0f, 0.0f));
    box.boundsMin = Vec3(-1.0f);
    box.boundsMax = Vec3(1.0f);

    const std::array<MeshInstance, 2> meshes{sphere, box};
    const sim::SdfGrid grid = MakeSphereGrid(1.0f, 0.05f, 6.0f);
    const sim::SdfGrid* grids[]{&grid, nullptr};

    BakedSceneField field;
    field.Build(meshes, std::span<const sim::SdfGrid* const>(grids, 2));
    // Satu ter-bake, satu tidak — dan keduanya tetap ikut menyumbang.
    CHECK(field.BakedCount() == 1);

    // Dekat bola: jawaban bola, dari grid.
    CHECK(field.Distance(Vec3(1.2f, 0.0f, 0.0f)) == doctest::Approx(0.2f).epsilon(0.05));
    // Dekat kotak: jawaban kotak, dari jalur lama.
    CHECK(field.Distance(Vec3(4.8f, 0.0f, 0.0f)) == doctest::Approx(0.2f).epsilon(0.05));
    // Di dalam bola tandanya negatif.
    CHECK(field.Distance(Vec3(0.0f)) < 0.0f);
}

TEST_CASE("grid yang jenuh tidak menciptakan dinding hantu") {
    using namespace sim::render;

    // **Jebakan yang paling mudah terlewat.** Level set hanya menyimpan jarak
    // yang tepat di dekat permukaan; di luar pita nilainya jenuh. Dipakai apa
    // adanya, sebuah titik lima meter dari mesh akan dilaporkan berjarak
    // selebar pita — dan karena `Row` mengambil `min`, angka kecil palsu itu
    // menjadi dinding di seluruh kotak bake.
    MeshInstance instance;
    instance.boundsMin = Vec3(-1.0f);
    instance.boundsMax = Vec3(1.0f);

    const sim::SdfGrid grid = MakeSphereGrid(1.0f, 0.05f, 4.0f);
    const sim::SdfGrid* grids[]{&grid};

    BakedSceneField field;
    field.Build(std::span<const MeshInstance>(&instance, 1),
                std::span<const sim::SdfGrid* const>(grids, 1));

    // Pitanya cuma 0,2 — jauh lebih sempit daripada jarak ini.
    CHECK(grid.band == doctest::Approx(0.2f));
    for (const float distance : {3.0f, 6.0f, 12.0f}) {
        const float reported = field.Distance(Vec3(distance, 0.0f, 0.0f));
        INFO("pada jarak " << distance << " m dilaporkan " << reported);
        // Tidak pernah melebih-lebihkan — itu arah yang menembus dinding...
        CHECK(reported <= distance - 1.0f + 0.05f);
        // ...tapi juga tidak jatuh ke lebar pita, yang akan jadi dinding hantu.
        CHECK(reported > grid.band);
    }
}

TEST_CASE("clipmap terisi dari SDF hasil bake cocok dengan jarak analitik") {
    using namespace sim::render;

    MeshInstance instance;
    instance.boundsMin = Vec3(-1.0f);
    instance.boundsMax = Vec3(1.0f);

    const sim::SdfGrid grid = MakeSphereGrid(1.0f, 0.05f, 8.0f);
    const sim::SdfGrid* grids[]{&grid};

    BakedSceneField field;
    field.Build(std::span<const MeshInstance>(&instance, 1),
                std::span<const sim::SdfGrid* const>(grids, 1));

    SdfClipmapSettings settings;
    settings.resolution = 64;
    settings.cascadeCount = 1;
    settings.finestVoxelSize = 0.05f;

    SdfVolume volume;
    volume.Configure(settings);
    // Memusatkan kaskade di titik asal. Tanpa ini kaskadenya belum punya posisi
    // dan `Sample` menolak hampir setiap titik — persis yang terjadi saat uji
    // ini pertama ditulis.
    volume.Clipmap().Scroll(Vec3(0.0f));
    volume.FillAll(field);

    // Yang diuji: nilai yang benar-benar tersimpan di clipmap — lewat
    // penyandian 8-bit dan pembacaan trilinearnya — bukan medannya saja.
    int checked = 0;
    for (const float radius : {0.85f, 0.95f, 1.05f, 1.15f}) {
        for (const Vec3& direction : {Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f),
                                      glm::normalize(Vec3(1.0f, 1.0f, 1.0f))}) {
            const Vec3 p = direction * radius;
            float sampled = 0.0f;
            if (!volume.Sample(p, sampled)) {
                continue;
            }
            const float expected = radius - 1.0f;
            INFO("jari-jari " << radius);
            // Penyandian 8-bit di atas pita 4 voxel (0,2 m) berlangkah sekitar
            // 1,6 mm, jadi toleransinya didominasi voxel grid-nya, bukan
            // penyandiannya.
            CHECK(std::abs(sampled - expected) <= 0.03f);
            ++checked;
        }
    }
    CHECK(checked >= 8);
}

// --- V2b: penyandian volume untuk tekstur 3D ---------------------------------

namespace {

/// Grid dengan gradien yang diketahui, untuk memeriksa round-trip penyandian.
sim::VolumeGrid MakeRampVolume(float low, float high, uint32_t side) {
    sim::VolumeGrid grid;
    grid.name = "ramp";
    grid.voxelSize = 0.1f;
    grid.origin = Vec3(0.0f);
    grid.sizeX = side;
    grid.sizeY = side;
    grid.sizeZ = side;
    grid.values.resize(static_cast<std::size_t>(side) * side * side);
    const auto last = static_cast<float>(grid.values.size() - 1);
    for (std::size_t i = 0; i < grid.values.size(); ++i) {
        grid.values[i] = low + (high - low) * (static_cast<float>(i) / last);
    }
    grid.minValue = low;
    grid.maxValue = high;
    return grid;
}

}  // namespace

TEST_CASE("penyandian volume memakai rentang gridnya sendiri") {
    using namespace sim::render;

    // **Grid asap tipis yang tidak pernah melewati 0,05.** Dinormalkan dengan
    // 0..1 yang diandaikan, seluruhnya akan jatuh ke texel 0 sampai 13 — asap
    // yang praktis menghilang. Dinormalkan dengan rentangnya sendiri, seluruh
    // 256 tingkat terpakai.
    const sim::VolumeGrid grid = MakeRampVolume(0.0f, 0.05f, 8);

    std::vector<std::byte> bytes;
    VolumeTextureDesc desc;
    REQUIRE(EncodeVolume(grid, VolumeTextureFormat::R8Unorm, bytes, desc));

    CHECK(desc.sizeX == 8);
    CHECK(desc.scale == doctest::Approx(0.05f));
    CHECK(desc.bias == doctest::Approx(0.0f));
    CHECK(bytes.size() == 8u * 8u * 8u);

    // Ujung bawah dan ujung atas benar-benar menyentuh kedua ujung rentang
    // texel — itulah tanda normalisasinya memakai rentang yang tepat.
    CHECK(static_cast<uint32_t>(bytes.front()) == 0);
    CHECK(static_cast<uint32_t>(bytes.back()) == 255);
}

TEST_CASE("round-trip penyandian volume tetap di dalam kuantisasinya") {
    using namespace sim::render;

    const sim::VolumeGrid grid = MakeRampVolume(-40.0f, 120.0f, 12);  // gaya grid suhu

    for (const auto format : {VolumeTextureFormat::R8Unorm, VolumeTextureFormat::R16Unorm}) {
        std::vector<std::byte> bytes;
        VolumeTextureDesc desc;
        REQUIRE(EncodeVolume(grid, format, bytes, desc));
        CHECK(bytes.size() == desc.ByteCount());

        const float levels = format == VolumeTextureFormat::R16Unorm ? 65535.0f : 255.0f;
        const float tolerance = (grid.maxValue - grid.minValue) / levels;

        for (uint32_t z = 0; z < grid.sizeZ; z += 3) {
            for (uint32_t y = 0; y < grid.sizeY; y += 3) {
                for (uint32_t x = 0; x < grid.sizeX; x += 3) {
                    const float expected = grid.At(static_cast<int32_t>(x), static_cast<int32_t>(y),
                                                   static_cast<int32_t>(z));
                    const float decoded = DecodeTexel(desc, bytes, x, y, z);
                    INFO("format " << (format == VolumeTextureFormat::R16Unorm ? "R16" : "R8")
                                   << " voxel (" << x << "," << y << "," << z << ")");
                    // Setengah tingkat, karena kuantisasinya membulatkan alih-alih
                    // memotong. Pemotongan akan menggeser seluruh volume ke bawah.
                    CHECK(std::abs(decoded - expected) <= tolerance * 0.5f + 1e-4f);
                }
            }
        }
    }
}

TEST_CASE("volume yang seluruhnya bernilai sama tidak menghasilkan bukan-angka") {
    using namespace sim::render;

    // Rentang nol. Dibagi apa adanya, setiap texel menjadi bukan-angka — dan
    // bukan-angka di tekstur 3D menyebar ke seluruh gambar lewat interpolasi.
    sim::VolumeGrid grid = MakeRampVolume(0.7f, 0.7f, 4);
    grid.minValue = 0.7f;
    grid.maxValue = 0.7f;

    std::vector<std::byte> bytes;
    VolumeTextureDesc desc;
    REQUIRE(EncodeVolume(grid, VolumeTextureFormat::R8Unorm, bytes, desc));

    CHECK(desc.scale == doctest::Approx(0.0f));
    CHECK(desc.bias == doctest::Approx(0.7f));
    for (uint32_t i = 0; i < 4; ++i) {
        const float decoded = DecodeTexel(desc, bytes, i, i, i);
        CHECK(std::isfinite(decoded));
        CHECK(decoded == doctest::Approx(0.7f));
    }
}

TEST_CASE("grid kosong ditolak, bukan menghasilkan tekstur nol") {
    using namespace sim::render;
    sim::VolumeGrid empty;
    std::vector<std::byte> bytes;
    VolumeTextureDesc desc;
    CHECK_FALSE(EncodeVolume(empty, VolumeTextureFormat::R8Unorm, bytes, desc));
    CHECK(bytes.empty());
    CHECK(desc.ByteCount() == 0);
}

// ============================================================================
// T0 — pembaca KTX2
// ============================================================================

namespace {

/// Menyusun sebuah berkas KTX2 **dari tata letak spesifikasinya**, bukan lewat
/// penulis buatan sendiri.
///
/// Bedanya menentukan. Round-trip terhadap penulis sendiri hanya membuktikan
/// konsistensi dengan diri sendiri — ia lulus walaupun kedua sisinya salah
/// membaca spesifikasi dengan cara yang sama. Yang di sini menuliskan offset
/// medannya satu per satu, sehingga yang diuji adalah pembacanya terhadap tata
/// letak yang tertulis, bukan terhadap tebakan yang sama.
std::vector<uint8_t> MakeKtx2(uint32_t format, uint32_t width, uint32_t height,
                              const std::vector<uint32_t>& levelSizes,
                              uint32_t supercompression = 0, uint32_t faceCount = 1,
                              uint32_t layerCount = 0, uint32_t pixelDepth = 0) {
    const std::size_t levelCount = levelSizes.size();
    const std::size_t headerBytes = 80;
    const std::size_t indexBytes = levelCount * 24;
    std::size_t payload = 0;
    for (const uint32_t size : levelSizes) {
        payload += size;
    }

    std::vector<uint8_t> bytes(headerBytes + indexBytes + payload, 0);
    const auto put32 = [&](std::size_t at, uint32_t value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };
    const auto put64 = [&](std::size_t at, uint64_t value) {
        std::memcpy(bytes.data() + at, &value, sizeof(value));
    };

    std::memcpy(bytes.data(), sim::rhi::kKtx2Identifier, 12);
    put32(12, format);
    put32(16, 1);  // typeSize
    put32(20, width);
    put32(24, height);
    put32(28, pixelDepth);
    put32(32, layerCount);
    put32(36, faceCount);
    put32(40, static_cast<uint32_t>(levelCount));
    put32(44, supercompression);

    std::size_t offset = headerBytes + indexBytes;
    for (std::size_t level = 0; level < levelCount; ++level) {
        const std::size_t at = headerBytes + level * 24;
        put64(at, offset);
        put64(at + 8, levelSizes[level]);
        put64(at + 16, levelSizes[level]);
        // Isi yang bisa dibedakan antar level, supaya `LevelBytes` yang menunjuk
        // level yang salah terlihat.
        for (uint32_t i = 0; i < levelSizes[level]; ++i) {
            bytes[offset + i] = static_cast<uint8_t>(level + 1);
        }
        offset += levelSizes[level];
    }
    return bytes;
}

/// `VK_FORMAT_BC7_SRGB_BLOCK`, menurut nomor yang ditetapkan Vulkan.
constexpr uint32_t kBc7Srgb = 146;

}  // namespace

TEST_CASE("T0: KTX2 BC7 berantai mip terbaca beserta ukuran tiap levelnya") {
    using namespace sim::rhi;

    // 16x16 BC7: blok 4x4 berukuran 16 byte, jadi level 0 = 16 blok = 256 byte,
    // level 1 = 64, dan seterusnya. Angkanya dihitung tangan, bukan diambil dari
    // yang dihasilkan kode yang sedang diuji.
    const std::vector<uint32_t> sizes = {256, 64, 16, 16, 16};
    const std::vector<uint8_t> file = MakeKtx2(kBc7Srgb, 16, 16, sizes);

    Ktx2Texture texture;
    const Ktx2Result result = ReadKtx2(file, texture);
    INFO(result.error);
    REQUIRE(result.ok);
    CHECK(texture.IsValid());
    CHECK(texture.format == kBc7Srgb);
    CHECK(texture.width == 16);
    CHECK(texture.height == 16);
    REQUIRE(texture.levels.size() == 5);

    for (std::size_t level = 0; level < sizes.size(); ++level) {
        INFO("level " << level);
        CHECK(texture.levels[level].length == sizes[level]);
        CHECK(texture.LevelBytes(level).size() == sizes[level]);
        // Isinya benar-benar level itu, bukan tetangganya.
        CHECK(texture.LevelBytes(level)[0] == static_cast<uint8_t>(level + 1));
    }

    // Ukuran per level menyusut separuh dan berhenti di satu — bukan nol, yang
    // akan membuat `vkCmdCopyBufferToImage` menolak seluruh unggahan.
    CHECK(texture.levels[0].width == 16);
    CHECK(texture.levels[1].width == 8);
    CHECK(texture.levels[4].width == 1);
    CHECK(texture.levels[4].height == 1);

    // Level di luar batas menjawab kosong, bukan membaca memori tetangga.
    CHECK(texture.LevelBytes(99).empty());
}

TEST_CASE("T0: tekstur tidak persegi berhenti di satu, bukan di nol") {
    using namespace sim::rhi;

    // **Sisi yang pendek habis lebih dulu.** Pada 16x4, level 2 sudah 4x1 dan
    // level 3 seharusnya 2x1 — bukan 2x0. Ukuran nol membuat
    // `vkCmdCopyBufferToImage` menolak seluruh unggahan, dan yang terlihat
    // adalah tekstur yang hilang tanpa satu pun pesan tentang mip.
    //
    // Kasus persegi tidak bisa menangkap ini: pada 16x16 kelima levelnya
    // berhenti tepat di 1x1 dengan sendirinya.
    const std::vector<uint8_t> file = MakeKtx2(kBc7Srgb, 16, 4, {16, 16, 16, 16, 16});

    Ktx2Texture texture;
    const Ktx2Result result = ReadKtx2(file, texture);
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(texture.levels.size() == 5);

    CHECK(texture.levels[0].width == 16);
    CHECK(texture.levels[0].height == 4);
    CHECK(texture.levels[2].width == 4);
    CHECK(texture.levels[2].height == 1);
    CHECK(texture.levels[3].width == 2);
    CHECK(texture.levels[3].height == 1);
    CHECK(texture.levels[4].width == 1);
    CHECK(texture.levels[4].height == 1);
    for (const Ktx2Level& level : texture.levels) {
        CHECK(level.width >= 1);
        CHECK(level.height >= 1);
    }
}

TEST_CASE("T0: berkas yang tidak didukung ditolak beserta sebabnya") {
    using namespace sim::rhi;
    Ktx2Texture texture;

    // Bukan KTX2 sama sekali.
    const std::vector<uint8_t> garbage(200, 0x42);
    Ktx2Result result = ReadKtx2(garbage, texture);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("identifier") != std::string::npos);

    // Terlalu pendek untuk sebuah header.
    CHECK_FALSE(ReadKtx2(std::vector<uint8_t>(20, 0), texture).ok);

    // **Basis Universal menuntut transcode**, dan mengunggah bloknya ke sebuah
    // `VkImage` ber-BCn menghasilkan gambar yang salah tanpa satu pun galat.
    result = ReadKtx2(MakeKtx2(0, 16, 16, {256}), texture);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("Basis") != std::string::npos);

    // Supercompression belum didukung; ia harus disebut, bukan dibaca separuh.
    result = ReadKtx2(MakeKtx2(kBc7Srgb, 16, 16, {256}, /*supercompression=*/2), texture);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("supercompressed") != std::string::npos);

    // Kubus dan larik punya jalurnya sendiri.
    CHECK_FALSE(ReadKtx2(MakeKtx2(kBc7Srgb, 16, 16, {256}, 0, /*faceCount=*/6), texture).ok);
    CHECK_FALSE(
        ReadKtx2(MakeKtx2(kBc7Srgb, 16, 16, {256}, 0, 1, /*layerCount=*/4), texture).ok);
    CHECK_FALSE(
        ReadKtx2(MakeKtx2(kBc7Srgb, 16, 16, {256}, 0, 1, 0, /*pixelDepth=*/4), texture).ok);

    // levelCount nol berarti "bangkitkan mip sendiri" — mesin ini tidak.
    CHECK_FALSE(ReadKtx2(MakeKtx2(kBc7Srgb, 16, 16, {}), texture).ok);

    // Level yang menunjuk ke luar berkas dipotong, bukan dipercaya.
    std::vector<uint8_t> truncated = MakeKtx2(kBc7Srgb, 16, 16, {256});
    truncated.resize(truncated.size() - 100);
    result = ReadKtx2(truncated, texture);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("past the end") != std::string::npos);
}

TEST_CASE("T0: berkas yang dibuat jahat tidak membuat pembacanya membaca ke luar") {
    using namespace sim::rhi;

    // **Berkas aset datang dari luar** — unduhan, kiriman artis, arsip yang
    // rusak. Pemeriksaan `offset + length > size` melimpah diam-diam pada nilai
    // besar: hasilnya mengecil, pemeriksaannya lolos, dan yang berikutnya
    // terjadi adalah membaca memori di luar buffer.
    std::vector<uint8_t> file = MakeKtx2(kBc7Srgb, 16, 16, {256});
    const std::size_t levelIndex = 80;
    const auto put64 = [&](std::size_t at, uint64_t value) {
        std::memcpy(file.data() + at, &value, sizeof(value));
    };

    Ktx2Texture texture;

    // Offset raksasa dengan panjang satu: `offset + length` membungkus menjadi
    // nol pada aritmetika 64 bit.
    put64(levelIndex, 0xFFFFFFFFFFFFFFFFull);
    put64(levelIndex + 8, 1);
    Ktx2Result result = ReadKtx2(file, texture);
    INFO(result.error);
    CHECK_FALSE(result.ok);

    // Panjang raksasa dengan offset yang sah — bentuk yang sama, sisi lain.
    put64(levelIndex, 80 + 24);
    put64(levelIndex + 8, 0xFFFFFFFFFFFFFF00ull);
    result = ReadKtx2(file, texture);
    INFO(result.error);
    CHECK_FALSE(result.ok);

    // Offset tepat di ujung berkas dengan panjang nol pun tidak lolos: level
    // kosong ditolak lebih dulu, dan yang penting di sini ia tidak membentuk
    // pointer di luar larik.
    put64(levelIndex, file.size());
    put64(levelIndex + 8, 0);
    CHECK_FALSE(ReadKtx2(file, texture).ok);

    // `levelCount` empat miliar adalah permintaan alokasi puluhan gigabyte
    // sebelum satu byte pun diperiksa.
    std::vector<uint8_t> huge = MakeKtx2(kBc7Srgb, 16, 16, {256});
    uint32_t levels = 0xFFFFFFFFu;
    std::memcpy(huge.data() + 40, &levels, sizeof(levels));
    result = ReadKtx2(huge, texture);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("impossible") != std::string::npos);

    // Dan `Ktx2Texture` yang disusun tangan dengan rentang yang melimpah tetap
    // menjawab kosong: ia struct terbuka, dan yang menyusunnya salah tidak boleh
    // berakhir sebagai pointer di luar buffer.
    Ktx2Texture handmade;
    handmade.width = 4;
    handmade.height = 4;
    handmade.bytes.assign(64, 0);
    handmade.levels.push_back(Ktx2Level{0xFFFFFFFFFFFFFFFFull, 1, 1, 4, 4});
    CHECK(handmade.LevelBytes(0).empty());
}

// ---------------------------------------------------------------------------
// T3 — renderer memakai KTX2
// ---------------------------------------------------------------------------

TEST_CASE("T3: jalur tekstur material di renderer tidak mendekode gambar sendiri") {
    // **Aturan struktural, bukan gaya.** Rencananya menyebutnya sendiri: kalau
    // jalur tekstur kedua terlanjur ada, ia tidak akan pernah dihapus — dan
    // renderer yang bisa memuat PNG akan terus memuat PNG, tanpa mip dan tanpa
    // kompresi, untuk setiap tekstur yang kebetulan belum di-bake.
    //
    // `Ibl.cpp` sengaja **tidak** ikut aturan ini. Peta lingkungan masih dibaca
    // sebagai float dari `.exr`, dan yang memindahkannya ke BC6H adalah T4.
    const std::filesystem::path renderer =
        std::filesystem::path(SIM_CODE_DIR) / "Render" / "src" / "VulkanRenderer.cpp";
    std::ifstream file(renderer);
    REQUIRE_MESSAGE(file, "tidak bisa membuka " << renderer.string());
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

    // Komentarnya dibuang lebih dulu. Aturan ini soal kode, bukan soal kata —
    // dan tanpa langkah ini, catatan yang menerangkan aturannya sendiri
    // menggagalkannya.
    std::string source;
    source.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n') {
                ++i;
            }
        }
        if (i < text.size()) {
            source.push_back(text[i]);
        }
    }

    CHECK(source.find("imageio::") == std::string::npos);
    CHECK(source.find("stbi_") == std::string::npos);
    // Dan ia memang memakai pembaca KTX2 — tanpa baris ini, berkas yang tidak
    // memuat tekstur sama sekali juga lulus.
    CHECK(source.find("ReadKtx2") != std::string::npos);
    CHECK(source.find("CreateFromKtx2") != std::string::npos);
}
