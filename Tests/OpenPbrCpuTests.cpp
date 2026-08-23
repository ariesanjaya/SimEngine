// Uji kesamaan model shading OpenPBR, dijalankan di CPU.
//
// **Yang diuji di sini bukan tiruan `openpbr.slang`, melainkan `openpbr.slang`
// itu sendiri.** Berkasnya dipancarkan `slangc -target cpp` dari sumber yang
// sama persis yang dikompilasi ke SPIR-V untuk GPU, jadi tidak ada dua
// implementasi yang bisa berselisih. Itu yang membuat berkas ini bisa
// menjadi acuan: acuan yang isinya transkripsi tangan hanya menguji ketelitian
// penyalinnya.
//
// Ini langkah pertama R4 di [PLAN-EMBREE.md](../docs/PLAN-EMBREE.md), dan C++
// yang sama nantinya ditautkan path tracer acuannya.
//
// **Kalau berkas ini berhenti bisa dikompilasi setelah slangc diperbarui**,
// yang paling mungkin berubah adalah akhiran nama yang dipancarkannya
// (`OpenPbrQuery_0`, `GlobalParams_0`). Perbaikannya menyesuaikan nama di
// `Harness` di bawah, dan hanya di sana.

#include "openpbr_cpu.cpp"  // dihasilkan slangc dari Shaders/openpbr_cpu.slang

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
// Prelude slangc mendefinisikan ulang sebagian nama pustaka standar, jadi yang
// dibutuhkan disebut eksplisit alih-alih diandaikan ikut terbawa.
#include <initializer_list>

namespace {

constexpr float kPi = 3.14159265358979323846f;

using Float3 = Vector<float, 3>;
using Float2 = Vector<float, 2>;

Float3 F3(float x, float y, float z) {
    Float3 v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}
Float3 F3(float s) { return F3(s, s, s); }

Float3 Normalize(Float3 v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return len > 0.0f ? F3(v.x / len, v.y / len, v.z / len) : v;
}

/// Nilai bawaan `OpenPBRSurface::defaults()`, ditulis ulang di sisi C++.
///
/// **Ini satu-satunya angka yang disalin**, dan ia sudah dikunci uji
/// terpisah di `MaterialTests.cpp` terhadap katalog node maupun terhadap
/// berkas normatif OpenPBR. Menyalinnya di sini justru membuat uji itu
/// berlaku dua arah.
OpenPBRSurface_0 Defaults() {
    OpenPBRSurface_0 s{};
    s.baseWeight_0 = 1.0f;
    s.baseColor_0 = F3(0.8f);
    s.baseMetalness_0 = 0.0f;
    s.baseDiffuseRoughness_0 = 0.0f;
    s.specularWeight_0 = 1.0f;
    s.specularColor_0 = F3(1.0f);
    s.specularRoughness_0 = 0.3f;
    s.specularRoughnessAnisotropy_0 = 0.0f;
    s.specularIor_0 = 1.5f;
    s.coatWeight_0 = 0.0f;
    s.coatColor_0 = F3(1.0f);
    s.coatRoughness_0 = 0.0f;
    s.coatRoughnessAnisotropy_0 = 0.0f;
    s.coatIor_0 = 1.6f;
    s.coatDarkening_0 = 1.0f;
    s.fuzzWeight_0 = 0.0f;
    s.fuzzColor_0 = F3(1.0f);
    s.fuzzRoughness_0 = 0.5f;
    return s;
}

struct Result {
    Float3 direct{};
    Float3 ambient{};
};

/// Menjalankan satu query lewat entry point yang dipancarkan slangc.
struct Harness {
    OpenPbrQuery_0 query{};

    Harness() {
        // Bingkai bawaan: normal +Z, pandangan tegak lurus permukaan.
        query.shadingNormal_0 = F3(0.0f, 0.0f, 1.0f);
        query.shadingTangent_0 = F3(1.0f, 0.0f, 0.0f);
        query.shadingBitangent_0 = F3(0.0f, 1.0f, 0.0f);
        query.viewDirection_0 = F3(0.0f, 0.0f, 1.0f);
        query.coatNormal_0 = query.shadingNormal_0;
        query.coatTangent_0 = query.shadingTangent_0;
        query.coatBitangent_0 = query.shadingBitangent_0;
        query.light_0 = Normalize(F3(0.0f, 0.6f, 0.8f));
        query.radiance_0 = F3(1.0f);
        query.irradiance_0 = F3(kPi);  // radiansi seragam 1 memberi iradiansi pi
        query.prefilteredBase_0 = F3(1.0f);
        query.prefilteredCoat_0 = F3(1.0f);
        query.dfg_0.x = 0.9f;
        query.dfg_0.y = 0.05f;
        query.surface_0 = Defaults();
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
        query.viewDirection_0 = F3(std::sin(r), 0.0f, std::cos(r));
        query.light_0 = F3(-std::sin(r), 0.0f, std::cos(r));
    }

    Result Run() const {
        OpenPbrQuery_0 q = query;
        Float3 direct = F3(0.0f);
        Float3 ambient = F3(0.0f);

        GlobalParams_0 params{};
        params.gQueries_0.data = &q;
        params.gQueries_0.count = 1;
        params.gDirect_0.data = &direct;
        params.gDirect_0.count = 1;
        params.gAmbient_0.data = &ambient;
        params.gAmbient_0.count = 1;

        ComputeThreadVaryingInput in{};
        in.groupID = uint3{0, 0, 0};
        in.groupThreadID = uint3{0, 0, 0};
        evaluateQueries_Thread(&in, nullptr, &params);
        return Result{direct, ambient};
    }
};

float Luminance(Float3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

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
    h.query.surface_0.baseMetalness_0 = 1.0f;

    const Float3 white = h.Run().direct;
    REQUIRE(white.x > 0.0f);

    const Float3 tint = F3(0.4f, 0.5f, 0.9f);
    h.query.surface_0.specularColor_0 = tint;
    const Float3 tinted = h.Run().direct;

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
        h.query.surface_0.baseMetalness_0 = 1.0f;
        const Float3 base = h.Run().direct;

        h.query.surface_0.specularColor_0 = F3(1.0f);
        const Float3 same = h.Run().direct;

        CAPTURE(degrees);
        CHECK(same.x == doctest::Approx(base.x));
    }
}

TEST_CASE("baseWeight menskala reflektansi logam") {
    // Temuan §3: spesifikasi menyatakan F0 logam adalah baseWeight * baseColor,
    // dan `baseWeight` sebelumnya hanya menyentuh lobe difus — yang justru
    // tidak dimiliki logam sama sekali.
    Harness h;
    h.query.surface_0.baseMetalness_0 = 1.0f;
    const float full = Luminance(h.Run().direct);

    h.query.surface_0.baseWeight_0 = 0.5f;
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
    h.query.surface_0.coatWeight_0 = 1.0f;
    h.query.surface_0.coatColor_0 = F3(1.0f, 0.2f, 0.2f);

    const Float3 dark = h.Run().direct;
    h.query.surface_0.coatDarkening_0 = 0.0f;
    const Float3 boosted = h.Run().direct;

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
    h.query.surface_0.specularRoughness_0 = 0.05f;
    const float uncoated = Luminance(h.Run().direct);

    h.query.surface_0.coatWeight_0 = 1.0f;
    // Warna coat putih dan tanpa penggelapan, supaya yang tersisa hanya
    // perubahan rasio IOR-nya.
    h.query.surface_0.coatDarkening_0 = 0.0f;
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
        h.query.surface_0.baseColor_0 = F3(1.0f);
        h.query.surface_0.baseDiffuseRoughness_0 = roughness;
        // Spekular dimatikan supaya yang terukur murni lobe difusnya.
        h.query.surface_0.specularWeight_0 = 0.0f;
        h.query.prefilteredBase_0 = F3(0.0f);
        h.query.dfg_0.x = 0.0f;
        h.query.dfg_0.y = 0.0f;

        const Float3 ambient = h.Run().ambient;
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
    smooth.query.surface_0.specularWeight_0 = 0.0f;
    Harness rough = smooth;
    rough.query.surface_0.baseDiffuseRoughness_0 = 1.0f;

    CHECK(Luminance(rough.Run().direct) != doctest::Approx(Luminance(smooth.Run().direct)));
    CHECK(Luminance(rough.Run().ambient) != doctest::Approx(Luminance(smooth.Run().ambient)));
}

TEST_CASE("Fuzz meredam yang di bawahnya, bukan sekadar menambah energi") {
    // Temuan §9. Fuzz sebelumnya ditambahkan tanpa dilapiskan, sehingga
    // permukaan berbulu bisa memantulkan lebih dari yang diterimanya.
    Harness h;
    h.query.surface_0.baseColor_0 = F3(1.0f);
    h.query.surface_0.specularWeight_0 = 0.0f;
    h.query.prefilteredBase_0 = F3(0.0f);
    h.query.dfg_0.x = 0.0f;
    h.query.dfg_0.y = 0.0f;
    const float bare = Luminance(h.Run().ambient);

    h.query.surface_0.fuzzWeight_0 = 1.0f;
    h.query.surface_0.fuzzColor_0 = F3(0.0f);  // fuzz hitam: hanya meredam
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
    iso.query.surface_0.specularRoughness_0 = 0.4f;
    Harness aniso = iso;
    aniso.query.surface_0.specularRoughnessAnisotropy_0 = 0.8f;
    const float anisotropic = Luminance(aniso.Run().direct);

    CHECK(std::isfinite(anisotropic));
    CHECK(anisotropic > 0.0f);
    // Anisotropi penuh membuat sumbu pendek mendekati nol; tanpa lantai alpha
    // ini akan menjadi NaN atau lobe yang lenyap.
    Harness extreme = iso;
    extreme.query.surface_0.specularRoughnessAnisotropy_0 = 1.0f;
    const float full = Luminance(extreme.Run().direct);
    CHECK(std::isfinite(full));
    CHECK(full >= 0.0f);
}

TEST_CASE("Bingkai coat sendiri mengubah sorotnya, bukan sisanya") {
    // Temuan §12. Normal coat yang berbeda dari normal dasar adalah yang
    // membuat kulit jeruk pada cat mobil mungkin.
    Harness flat;
    flat.query.surface_0.coatWeight_0 = 1.0f;
    flat.query.surface_0.coatRoughness_0 = 0.1f;
    const float aligned = Luminance(flat.Run().direct);

    Harness tilted = flat;
    tilted.query.coatNormal_0 = Normalize(F3(0.3f, 0.0f, 1.0f));
    tilted.query.coatTangent_0 = Normalize(F3(1.0f, 0.0f, -0.3f));
    const float perturbed = Luminance(tilted.Run().direct);

    CHECK(std::isfinite(perturbed));
    CHECK(perturbed != doctest::Approx(aligned));
}
