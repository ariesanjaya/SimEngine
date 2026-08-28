#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "Sim/Render/ProbeVolume.h"

// Berkas ini dibangkitkan saat build dari `Shaders/probe_grid_cpu.slang`, yang
// sendirinya hanya membungkus `Shaders/probe_grid.slang` — berkas yang sama yang
// di-`#include` `shadow_common.slang` dan karena itu benar-benar dijalankan GPU.
// Lihat CMakeLists.txt direktori ini.
#include "probe_grid_cpu.cpp"

#include <random>
#include <vector>

// Pengalamatan kisi probe: sisi shader lawan sisi C++ (S2 di
// docs/PLAN-STATIC-GI.md).
//
// **Ini yang tidak bisa dibuktikan S1.** Di S1 seluruh probe bernilai sama,
// sehingga indeks yang meleset pun mengembalikan angka yang sama persis — dan
// gambar yang identik terbaca sebagai "pengalamatannya benar" padahal ia tidak
// diuji sama sekali. Begitu S2 mengisi probe dengan transport, tiap probe
// berbeda, dan indeks yang meleset satu brick menggeser cahaya tak-langsung
// beberapa meter tanpa satu pun galat.
//
// **Yang diadu di sini bukan dua tulisan yang mirip melainkan satu sumber.**
// Sisi shader dijalankan apa adanya lewat `slangc -target cpp`; sisi C++ yang
// menulis artefaknya ada di `ProbeVolume.cpp`. Kalau keduanya berselisih, yang
// gagal uji ini — bukan sebuah adegan, beberapa milestone kemudian.

namespace {

using sim::Vec3;
using sim::render::ProbeVolumeLayout;

/// Menjalankan entry point compute yang dipancarkan slangc, satu query.
ProbeAddressResult_0 RunShaderAddressing(const ProbeVolumeLayout& layout, const Vec3& position,
                                         uint32_t slot) {
    ProbeAddressQuery_0 query{};
    query.position_0 = Vector<float, 3>{position.x, position.y, position.z};
    query.origin_0 = Vector<float, 3>{layout.origin.x, layout.origin.y, layout.origin.z};
    query.spacing_0 = layout.spacing;
    query.counts_0 = Vector<uint32_t, 3>{layout.counts.x, layout.counts.y, layout.counts.z};
    query.slot_0 = slot;

    ProbeAddressResult_0 result{};
    GlobalParams_0 params{};
    params.gQueries_0.data = &query;
    params.gQueries_0.count = 1;
    params.gResults_0.data = &result;
    params.gResults_0.count = 1;

    ComputeThreadVaryingInput in{};
    in.groupID = uint3{0, 0, 0};
    in.groupThreadID = uint3{0, 0, 0};
    addressQueries_Thread(&in, nullptr, &params);
    return result;
}

}  // namespace

TEST_CASE("S2: pengalamatan kisi probe sama di shader dan di C++") {
    using namespace sim;
    using namespace sim::render;

    // Kisi yang jumlahnya **bukan** kelipatan ukuran brick di ketiga sumbu.
    // Kelipatan yang pas menyembunyikan justru kesalahan yang paling mungkin:
    // baris terakhir yang tidak penuh, dan brick tepi yang sebagian probenya di
    // luar kisi.
    const std::vector<ProbeVolumeLayout> layouts{
        MakeProbeLayout(Vec3(-3.0f, 0.0f, -5.0f), Vec3(4.0f, 2.0f, 6.0f), 1.0f),
        MakeProbeLayout(Vec3(0.0f), Vec3(9.0f, 9.0f, 9.0f), 2.0f),
        MakeProbeLayout(Vec3(-1.5f), Vec3(1.5f), 0.5f),
    };

    std::mt19937 rng(20260828u);
    std::size_t compared = 0;

    for (const ProbeVolumeLayout& layout : layouts) {
        REQUIRE(layout.IsValid());
        INFO("kisi ", layout.counts.x, "x", layout.counts.y, "x", layout.counts.z, " jarak ",
             layout.spacing);

        // Titik di dalam kisi, di tepinya, dan di luarnya. Yang di luar penting:
        // kedua sisi harus menjepitnya dengan cara yang sama, dan jepitan yang
        // berbeda cuma terlihat pada permukaan yang menyentuh batas adegan.
        std::vector<Vec3> positions{
            layout.origin,
            layout.ProbePosition(layout.counts - glm::uvec3(1u)),
            layout.origin - Vec3(50.0f),
            layout.origin + Vec3(50.0f),
        };
        std::uniform_real_distribution<float> pick(-2.0f, 2.0f);
        for (int i = 0; i < 64; ++i) {
            const Vec3 span = Vec3(layout.counts - glm::uvec3(1u)) * layout.spacing;
            positions.push_back(layout.origin + span * Vec3(pick(rng), pick(rng), pick(rng)) *
                                                    0.5f);
        }

        for (const Vec3& position : positions) {
            const uint32_t slot = rng() % 97u;
            const ProbeAddressResult_0 shader = RunShaderAddressing(layout, position, slot);

            glm::uvec3 base(0u);
            Vec3 fraction(0.0f);
            ProbeCell(layout, position, base, fraction);

            INFO("posisi (", position.x, ",", position.y, ",", position.z, ")");
            CHECK(shader.base_0.x == base.x);
            CHECK(shader.base_0.y == base.y);
            CHECK(shader.base_0.z == base.z);
            CHECK(shader.fraction_0.x == doctest::Approx(fraction.x).epsilon(1e-6));
            CHECK(shader.fraction_0.y == doctest::Approx(fraction.y).epsilon(1e-6));
            CHECK(shader.fraction_0.z == doctest::Approx(fraction.z).epsilon(1e-6));

            float totalWeight = 0.0f;
            for (uint32_t corner = 0; corner < 8; ++corner) {
                const glm::uvec3 probe = ProbeCorner(layout, base, corner);
                INFO("sudut ", corner);
                CHECK(shader.brickIndex_0[corner] == ProbeBrickIndex(layout, probe));
                CHECK(shader.coefficientBase_0[corner] == ProbeSlotOffset(slot, probe) * 9u);
                CHECK(shader.weight_0[corner] ==
                      doctest::Approx(ProbeCornerWeight(corner, fraction)).epsilon(1e-6));
                totalWeight += shader.weight_0[corner];
            }
            // Bobot yang tidak berjumlah satu bukan kesalahan indeks melainkan
            // kesalahan energi: ia menggelapkan atau menerangkan seluruh
            // permukaan sekaligus, dan itu paling mudah dikira masalah eksposur.
            CHECK(totalWeight == doctest::Approx(1.0f).epsilon(1e-5));
            ++compared;
        }
    }

    MESSAGE(compared, " posisi diadu, masing-masing delapan sudut");
}

TEST_CASE("S2: tiap probe punya offset koefisien sendiri, dan tidak ada yang bertabrakan") {
    // **Yang dijaga di sini surjektif dan injektif sekaligus.** Sebuah brick
    // memuat 64 probe; kalau dua di antaranya memetakan ke offset yang sama,
    // salah satunya tidak pernah terbaca dan yang lain terbaca dua kali — dan
    // keduanya tetap menghasilkan gambar yang mulus.
    using namespace sim;
    using namespace sim::render;

    const ProbeVolumeLayout layout =
        MakeProbeLayout(Vec3(0.0f), Vec3(6.0f, 6.0f, 6.0f), 1.0f);
    REQUIRE(layout.IsValid());

    std::vector<int> seen(layout.FullProbeCount(), 0);
    for (uint32_t z = 0; z < layout.counts.z; ++z) {
        for (uint32_t y = 0; y < layout.counts.y; ++y) {
            for (uint32_t x = 0; x < layout.counts.x; ++x) {
                const glm::uvec3 probe(x, y, z);
                const uint32_t brick = ProbeBrickIndex(layout, probe);
                REQUIRE(brick < layout.BrickCount());
                const uint32_t offset = ProbeSlotOffset(brick, probe);
                REQUIRE(offset < seen.size());
                CHECK(seen[offset] == 0);
                seen[offset] = 1;

                // Dan sisi shader menyetujui keduanya.
                const ProbeAddressResult_0 shader =
                    RunShaderAddressing(layout, layout.ProbePosition(probe), brick);
                CHECK(shader.brickIndex_0[0] == brick);
                CHECK(shader.coefficientBase_0[0] == offset * 9u);
            }
        }
    }
}
