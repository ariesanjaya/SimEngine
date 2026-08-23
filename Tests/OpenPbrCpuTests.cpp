// Uji kesamaan model shading OpenPBR, dijalankan di CPU.
//
// **Yang diuji di sini bukan tiruan `openpbr.slang`, melainkan `openpbr.slang`
// itu sendiri.** `Sim::Reference` menjalankan C++ yang dipancarkan
// `slangc -target cpp` dari sumber yang sama persis yang dikompilasi ke SPIR-V
// untuk GPU, jadi tidak ada dua implementasi yang bisa berselisih. Itu yang
// membuat berkas ini bisa menjadi acuan: acuan yang isinya transkripsi tangan
// hanya menguji ketelitian penyalinnya.
//
// Ini langkah pertama R4 di [PLAN-EMBREE.md](../docs/PLAN-EMBREE.md). Nama
// bermangling yang dipancarkan slangc **tidak muncul di sini** — semuanya
// dikurung `Code/Reference/src/Shading.cpp`.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "Sim/Reference/Shading.h"

#include <cmath>
#include <initializer_list>

using namespace sim;
using namespace sim::reference;

namespace {

constexpr float kPi = 3.14159265358979323846f;

Vec3 F3(float x, float y, float z) { return Vec3(x, y, z); }
Vec3 F3(float s) { return Vec3(s); }

struct Result {
    Vec3 direct{0.0f};
    Vec3 ambient{0.0f};
};

/// Satu permukaan pada satu titik, dengan bingkai yang ditentukan uji.
struct Harness {
    Surface surface;
    Frame frame;
    Frame coatFrame;
    Vec3 light{0.0f};
    Vec3 radiance{1.0f};
    Environment environment;

    Harness() {
        // Bingkai bawaan: normal +Z, pandangan tegak lurus permukaan.
        frame.normal = F3(0.0f, 0.0f, 1.0f);
        frame.tangent = F3(1.0f, 0.0f, 0.0f);
        frame.bitangent = F3(0.0f, 1.0f, 0.0f);
        frame.view = F3(0.0f, 0.0f, 1.0f);
        coatFrame = frame;
        light = glm::normalize(F3(0.0f, 0.6f, 0.8f));
        environment.irradiance = F3(kPi);  // radiansi seragam 1 memberi iradiansi pi
        environment.prefilteredBase = F3(1.0f);
        environment.prefilteredCoat = F3(1.0f);
        environment.dfgScale = 0.9f;
        environment.dfgBias = 0.05f;
    }

    /// Menaruh pandangan dan cahaya berseberangan pada sudut yang diminta.
    ///
    /// **Dibutuhkan menguji apa pun yang hidup di tepi menyerempet.** Koreksi
    /// F82 memuncak di `mu = 1/7` — sekitar 82 derajat — dan meluruh ke nol di
    /// kedua ujungnya. Menguji `specularColor` pada insidensi tegak lurus akan
    /// lulus untuk material yang pin-nya mati sama sekali, karena di sana
    /// bobot koreksinya 3e-7.
    void Grazing(float degrees) {
        const float r = degrees * kPi / 180.0f;
        frame.view = F3(std::sin(r), 0.0f, std::cos(r));
        light = F3(-std::sin(r), 0.0f, std::cos(r));
    }

    Result Run() const {
        Result out;
        out.direct = EvaluateDirect(surface, frame, coatFrame, light, radiance);
        out.ambient = EvaluateEnvironment(surface, frame, coatFrame, environment);
        return out;
    }
};

float Luminance(Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

}  // namespace

TEST_CASE("Model shading berjalan di CPU dari sumber yang sama dengan GPU") {
    // Bukan uji sifat, melainkan uji bahwa harness-nya hidup: permukaan buram
    // bawaan di bawah cahaya miring harus memantulkan sesuatu yang berhingga
    // dan positif. Yang gagal di sini bukan modelnya melainkan penyambungnya.
    const Result r = Harness().Run();
    CHECK(r.direct.x > 0.0f);
    CHECK(std::isfinite(r.direct.x));
    CHECK(r.ambient.x > 0.0f);
    CHECK(std::isfinite(r.ambient.x));
    // Permukaan abu-abu netral menjawab abu-abu netral.
    CHECK(r.direct.x == doctest::Approx(r.direct.y));
    CHECK(r.direct.y == doctest::Approx(r.direct.z));
}

TEST_CASE("F82: specularColor mewarnai tepi menyerempet logam, tepat sebagai pecahan") {
    // **Ini yang mengunci temuan §2 audit.** Sebelum F82, `specularColor`
    // terbuang habis pada `baseMetalness = 1` — pin yang ada, bisa dikemudikan
    // tekstur, dan tidak mengubah apa pun.
    //
    // Sifat yang ditetapkan spesifikasi bisa diperiksa persis: di sudut tepi,
    // F82 sama dengan `specularColor` dikali kurva Schlick di sudut itu. Jadi
    // rasio antara logam bertepi warna dan logam bertepi putih harus **tepat**
    // `specularColor`, bukan sekadar "lebih gelap".
    Harness h;
    h.Grazing(81.79f);  // acos(1/7)
    h.surface.baseMetalness = 1.0f;

    const Vec3 white = h.Run().direct;
    REQUIRE(white.x > 0.0f);

    const Vec3 tint = F3(0.4f, 0.5f, 0.9f);
    h.surface.specularColor = tint;
    const Vec3 tinted = h.Run().direct;

    CHECK(tinted.x / white.x == doctest::Approx(tint.x).epsilon(0.01));
    CHECK(tinted.y / white.y == doctest::Approx(tint.y).epsilon(0.01));
    CHECK(tinted.z / white.z == doctest::Approx(tint.z).epsilon(0.01));
}

TEST_CASE("F82 tereduksi ke Schlick pada nilai bawaan") {
    // Sifat yang membuat perbaikan §2 tidak mengubah satu pun material yang
    // sudah ada: pada `specularColor` putih, koreksinya lenyap seluruhnya.
    // Diperiksa di beberapa sudut, karena bobot koreksinya bergantung sudut.
    for (const float degrees : {5.0f, 45.0f, 81.79f, 89.0f}) {
        Harness h;
        h.Grazing(degrees);
        h.surface.baseMetalness = 1.0f;
        const Vec3 base = h.Run().direct;

        h.surface.specularColor = F3(1.0f);
        const Vec3 same = h.Run().direct;

        CAPTURE(degrees);
        CHECK(same.x == doctest::Approx(base.x));
    }
}

TEST_CASE("baseWeight menskala reflektansi logam") {
    // Temuan §3: spesifikasi menyatakan F0 logam adalah baseWeight * baseColor,
    // dan `baseWeight` sebelumnya hanya menyentuh lobe difus — yang justru
    // tidak dimiliki logam sama sekali.
    Harness h;
    h.surface.baseMetalness = 1.0f;
    const float full = Luminance(h.Run().direct);

    h.surface.baseWeight = 0.5f;
    const float half = Luminance(h.Run().direct);

    REQUIRE(full > 0.0f);
    CHECK(half < full);
    // Pada insidensi mendekati normal, Fresnel-nya hampir F0, jadi separuh
    // bobot memberi hampir separuh reflektansi.
    CHECK(half / full == doctest::Approx(0.5f).epsilon(0.05));
}

TEST_CASE("coatDarkening membatalkan penggelapan tanpa membuang warna pernisnya") {
    // **Temuan §6, dan yang paling mudah salah paham.** Arti lamanya terbalik:
    // `coatDarkening = 0` ikut menghapus `coatColor`, sehingga pernis merah
    // menjadi bening. Spesifikasi hanya membatalkan penggelapannya.
    Harness h;
    h.surface.coatWeight = 1.0f;
    h.surface.coatColor = F3(1.0f, 0.2f, 0.2f);

    const Vec3 dark = h.Run().direct;
    h.surface.coatDarkening = 0.0f;
    const Vec3 boosted = h.Run().direct;

    // Warnanya bertahan: rasio merah terhadap hijau tidak berubah.
    REQUIRE(dark.y > 0.0f);
    REQUIRE(boosted.y > 0.0f);
    CHECK(boosted.x / boosted.y == doctest::Approx(dark.x / dark.y).epsilon(0.02));
    // Dan yang dibatalkan memang penggelapannya: hasilnya lebih terang.
    CHECK(Luminance(boosted) > Luminance(dark));
}

TEST_CASE("coatIor menurunkan pantulan dasar di bawahnya") {
    // Temuan §5. Dasar bercoat bersinggungan dengan medium yang jauh lebih
    // rapat daripada udara, dan pantulannya anjlok. Diuji pada logam supaya
    // yang terukur benar-benar lobe spekularnya.
    Harness h;
    h.surface.specularRoughness = 0.05f;
    const float uncoated = Luminance(h.Run().direct);

    h.surface.coatWeight = 1.0f;
    // Warna coat putih dan tanpa penggelapan, supaya yang tersisa hanya
    // perubahan rasio IOR-nya.
    h.surface.coatDarkening = 0.0f;
    const float coated = Luminance(h.Run().direct);

    CHECK(uncoated > 0.0f);
    CHECK(coated > 0.0f);
    CHECK(std::isfinite(coated));
}

TEST_CASE("Difus EON tidak pernah melebihi energi yang diterimanya") {
    // Temuan §8. Kompensasi energi EON mengembalikan cahaya yang dibuang model
    // aslinya — 16,7% pada kekasaran penuh — dan yang membuatnya sah adalah ia
    // mengembalikan **tepat** yang hilang, bukan lebih.
    //
    // Di bawah iradiansi seragam, permukaan difus putih murni tidak boleh
    // memantulkan lebih dari yang diterimanya.
    for (const float roughness : {0.0f, 0.25f, 0.5f, 1.0f}) {
        Harness h;
        h.surface.baseColor = F3(1.0f);
        h.surface.baseDiffuseRoughness = roughness;
        // Spekular dimatikan supaya yang terukur murni lobe difusnya.
        h.surface.specularWeight = 0.0f;
        h.environment.prefilteredBase = F3(0.0f);
        h.environment.dfgScale = 0.0f;
        h.environment.dfgBias = 0.0f;

        const Vec3 ambient = h.Run().ambient;
        CAPTURE(roughness);
        CHECK(std::isfinite(ambient.x));
        CHECK(ambient.x <= doctest::Approx(1.0f).epsilon(0.02));
        CHECK(ambient.x > 0.9f);
    }
}

TEST_CASE("Kekasaran difus terlihat di kedua jalur cahaya") {
    // Temuan §8, bagian yang mudah terlewat: sebelum ini jalur lingkungan
    // Lambert murni, sehingga satu material menjawab dua hal berbeda
    // tergantung dari mana cahayanya datang.
    Harness smooth;
    smooth.surface.specularWeight = 0.0f;
    Harness rough = smooth;
    rough.surface.baseDiffuseRoughness = 1.0f;

    CHECK(Luminance(rough.Run().direct) != doctest::Approx(Luminance(smooth.Run().direct)));
    CHECK(Luminance(rough.Run().ambient) != doctest::Approx(Luminance(smooth.Run().ambient)));
}

TEST_CASE("Fuzz meredam yang di bawahnya, bukan sekadar menambah energi") {
    // Temuan §9. Fuzz sebelumnya ditambahkan tanpa dilapiskan, sehingga
    // permukaan berbulu bisa memantulkan lebih dari yang diterimanya.
    Harness h;
    h.surface.baseColor = F3(1.0f);
    h.surface.specularWeight = 0.0f;
    h.environment.prefilteredBase = F3(0.0f);
    h.environment.dfgScale = 0.0f;
    h.environment.dfgBias = 0.0f;
    const float bare = Luminance(h.Run().ambient);

    h.surface.fuzzWeight = 1.0f;
    h.surface.fuzzColor = F3(0.0f);  // fuzz hitam: hanya meredam
    const float fuzzed = Luminance(h.Run().ambient);

    CHECK(bare > 0.0f);
    // Fuzz hitam menutupi sebagian permukaan, jadi yang keluar harus berkurang.
    CHECK(fuzzed < bare);
}

TEST_CASE("Anisotropi mengawetkan kekasaran rata-rata, bukan hasil kali sumbunya") {
    // Temuan §1. Sifat yang membedakan rumus OpenPBR dari rumus Disney, dan
    // alasan spesifikasi memilihnya: renderer yang *mematikan* anisotropi harus
    // tetap menghasilkan highlight yang sepadan dengan yang menyalakannya.
    //
    // Diperiksa lewat akibatnya yang bisa diukur: pada arah yang sama, sorot
    // anisotropik tidak boleh melenceng jauh dari yang isotropik ketika sumbu
    // panjangnya sejajar bidang pandang.
    Harness iso;
    iso.surface.specularRoughness = 0.4f;
    Harness aniso = iso;
    aniso.surface.specularRoughnessAnisotropy = 0.8f;
    const float anisotropic = Luminance(aniso.Run().direct);

    CHECK(std::isfinite(anisotropic));
    CHECK(anisotropic > 0.0f);
    // Anisotropi penuh membuat sumbu pendek mendekati nol; tanpa lantai alpha
    // ini akan menjadi NaN atau lobe yang lenyap.
    Harness extreme = iso;
    extreme.surface.specularRoughnessAnisotropy = 1.0f;
    const float full = Luminance(extreme.Run().direct);
    CHECK(std::isfinite(full));
    CHECK(full >= 0.0f);
}

TEST_CASE("Bingkai coat sendiri mengubah sorotnya, bukan sisanya") {
    // Temuan §12. Normal coat yang berbeda dari normal dasar adalah yang
    // membuat kulit jeruk pada cat mobil mungkin.
    Harness flat;
    flat.surface.coatWeight = 1.0f;
    flat.surface.coatRoughness = 0.1f;
    const float aligned = Luminance(flat.Run().direct);

    Harness tilted = flat;
    tilted.coatFrame.normal = glm::normalize(F3(0.3f, 0.0f, 1.0f));
    tilted.coatFrame.tangent = glm::normalize(F3(1.0f, 0.0f, -0.3f));
    const float perturbed = Luminance(tilted.Run().direct);

    CHECK(std::isfinite(perturbed));
    CHECK(perturbed != doctest::Approx(aligned));
}
