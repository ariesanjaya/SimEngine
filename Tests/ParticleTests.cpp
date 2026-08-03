#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/Core/Curve.h"
#include "Sim/Particle/ParticleEffect.h"
#include "Sim/Particle/ParticleSystem.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <vector>

using namespace sim;
using namespace sim::particle;

namespace {

/// Emitter dengan lima modul menyala dan tiga kurva yang benar-benar berbentuk —
/// bahan kriteria terima E7.2 nomor 1.
ParticleEmitter RichEmitter() {
    ParticleEmitter effect;
    effect.name = "Percikan";
    effect.seed = 987;
    effect.maxParticles = 500;

    effect.spawn.rate = 60.0f;
    effect.spawn.burstCount = 20;
    effect.spawn.burstTime = 0.25f;
    effect.spawn.duration = 3.0f;
    effect.spawn.looping = false;

    effect.shape.shape = EmitterShape::Cone;
    effect.shape.size = Vec3(0.3f, 0.3f, 0.3f);
    effect.shape.coneAngle = 0.6f;

    effect.initial.velocity = Vec3(0.0f, 4.0f, 0.0f);
    effect.initial.lifetime = 1.5f;

    effect.overLifetime.sizeOverLife = Curve();
    effect.overLifetime.sizeOverLife.AddKey({0.0f, 0.0f, 0.0f, 4.0f, Interpolation::Bezier});
    effect.overLifetime.sizeOverLife.AddKey({0.3f, 1.0f, 0.0f, 0.0f, Interpolation::Bezier});
    effect.overLifetime.sizeOverLife.AddKey({1.0f, 0.0f, -2.0f, 0.0f, Interpolation::Bezier});

    effect.overLifetime.velocityScale = Curve();
    effect.overLifetime.velocityScale.AddKey({0.0f, 1.0f, 0.0f, 0.0f, Interpolation::Linear});
    effect.overLifetime.velocityScale.AddKey({1.0f, 0.2f, 0.0f, 0.0f, Interpolation::Linear});

    effect.overLifetime.rotationRate = Curve();
    effect.overLifetime.rotationRate.AddKey({0.0f, 3.0f, 0.0f, 0.0f, Interpolation::Constant});
    effect.overLifetime.rotationRate.AddKey({1.0f, 0.0f, 0.0f, 0.0f, Interpolation::Constant});

    effect.overLifetime.colorOverLife.AddColorStop(0.0f, Vec3(1.0f, 0.9f, 0.4f));
    effect.overLifetime.colorOverLife.AddColorStop(1.0f, Vec3(0.8f, 0.1f, 0.0f));
    effect.overLifetime.colorOverLife.AddAlphaStop(0.0f, 1.0f);
    effect.overLifetime.colorOverLife.AddAlphaStop(0.8f, 1.0f);
    effect.overLifetime.colorOverLife.AddAlphaStop(1.0f, 0.0f);

    effect.force.gravity = Vec3(0.0f, -6.0f, 0.0f);
    effect.force.drag = 0.5f;

    effect.collision.enabled = true;
    effect.collision.bounce = 0.3f;
    return effect;
}

ParticleEffect RichEffect() {
    ParticleEffect effect;
    effect.name = "Percikan";
    effect.emitters.push_back(RichEmitter());
    return effect;
}

/// Emitter kedua yang berbeda jelas dari yang pertama: lebih lambat, lebih
/// besar, naik pelan — asap di atas percikan.
ParticleEmitter SmokeEmitter() {
    ParticleEmitter emitter;
    emitter.name = "Asap";
    emitter.seed = 4242;
    emitter.maxParticles = 200;
    emitter.spawn.rate = 12.0f;
    emitter.shape.shape = EmitterShape::Sphere;
    emitter.shape.size = Vec3(0.8f, 0.8f, 0.8f);
    emitter.initial.velocity = Vec3(0.0f, 0.7f, 0.0f);
    emitter.initial.size = 0.8f;
    emitter.initial.lifetime = 4.0f;
    emitter.force.gravity = Vec3(0.0f, 0.4f, 0.0f);
    return emitter;
}

}  // namespace

TEST_CASE("Kurva menahan nilai di luar rentang kuncinya, tidak mengekstrapolasi") {
    Curve curve;
    curve.AddKey({0.0f, 1.0f, 0.0f, 0.0f, Interpolation::Bezier});
    curve.AddKey({1.0f, 3.0f, 0.0f, 0.0f, Interpolation::Bezier});

    // Ekstrapolasi bezier menghasilkan angka yang meledak jauh di luar rentang
    // yang terlihat di editor, dan tidak ada modul yang menginginkannya.
    CHECK(curve.Evaluate(-5.0f) == doctest::Approx(1.0f));
    CHECK(curve.Evaluate(9.0f) == doctest::Approx(3.0f));
    CHECK(curve.Evaluate(0.0f) == doctest::Approx(1.0f));
    CHECK(curve.Evaluate(1.0f) == doctest::Approx(3.0f));
    // Tangen nol pada keduanya: ease-in-out, jadi titik tengahnya tepat di
    // tengah nilai.
    CHECK(curve.Evaluate(0.5f) == doctest::Approx(2.0f));
}

TEST_CASE("Kunci constant menahan sampai kunci berikutnya") {
    Curve curve;
    curve.AddKey({0.0f, 5.0f, 0.0f, 0.0f, Interpolation::Constant});
    curve.AddKey({1.0f, 9.0f, 0.0f, 0.0f, Interpolation::Constant});
    CHECK(curve.Evaluate(0.001f) == doctest::Approx(5.0f));
    CHECK(curve.Evaluate(0.999f) == doctest::Approx(5.0f));
    CHECK(curve.Evaluate(1.0f) == doctest::Approx(9.0f));
}

TEST_CASE("Kunci pada waktu yang sama diganti, bukan ditumpuk") {
    Curve curve;
    curve.AddKey({0.5f, 1.0f, 0.0f, 0.0f, Interpolation::Linear});
    curve.AddKey({0.5f, 7.0f, 0.0f, 0.0f, Interpolation::Linear});
    // Dua kunci pada satu waktu membuat evaluasinya bergantung urutan
    // penyimpanan — perilaku yang tidak terlihat di editor sama sekali.
    REQUIRE(curve.Keys().size() == 1);
    CHECK(curve.Keys().front().value == doctest::Approx(7.0f));
}

TEST_CASE("Memindahkan kunci menjaga urutan waktu") {
    Curve curve;
    curve.AddKey({0.0f, 0.0f, 0.0f, 0.0f, Interpolation::Linear});
    curve.AddKey({0.5f, 1.0f, 0.0f, 0.0f, Interpolation::Linear});
    curve.AddKey({1.0f, 2.0f, 0.0f, 0.0f, Interpolation::Linear});

    // Kunci tengah diseret melewati yang terakhir.
    const std::size_t moved = curve.MoveKey(1, 1.5f, 9.0f);
    CHECK(moved == 2);
    CHECK(curve.Keys()[0].time == doctest::Approx(0.0f));
    CHECK(curve.Keys()[1].time == doctest::Approx(1.0f));
    CHECK(curve.Keys()[2].time == doctest::Approx(1.5f));
}

TEST_CASE("Gradient memisahkan perhentian warna dan alpha") {
    Gradient gradient;
    gradient.AddColorStop(0.0f, Vec3(1.0f, 0.0f, 0.0f));
    gradient.AddColorStop(1.0f, Vec3(0.0f, 0.0f, 1.0f));
    // Alpha memudar hanya di ujung — tanpa perhentian warna tambahan di 0.5,
    // yang warnanya harus ditebak kalau keduanya disatukan.
    gradient.AddAlphaStop(0.0f, 1.0f);
    gradient.AddAlphaStop(0.5f, 1.0f);
    gradient.AddAlphaStop(1.0f, 0.0f);

    const Vec4 middle = gradient.Evaluate(0.5f);
    CHECK(middle.x == doctest::Approx(0.5f));
    CHECK(middle.z == doctest::Approx(0.5f));
    CHECK(middle.w == doctest::Approx(1.0f));
    CHECK(gradient.Evaluate(1.0f).w == doctest::Approx(0.0f));
}

TEST_CASE("Efek dengan lima modul dan tiga kurva disimpan lalu dimuat identik") {
    const ParticleEffect original = RichEffect();
    const std::string text = SaveEffectToString(original);

    ParticleEffect loaded;
    const EffectIoResult result = LoadEffectFromString(loaded, text);
    REQUIRE(result.ok);

    // Byte-per-byte sama, seperti berkas level dan `.simmat`: itu yang membuat
    // menyimpan efek yang tidak disunting tidak menghasilkan diff palsu.
    CHECK(SaveEffectToString(loaded) == text);

    CHECK(loaded.name == "Percikan");
    REQUIRE(loaded.emitters.size() == 1);
    const ParticleEmitter& emitter = loaded.emitters.front();
    CHECK(emitter.name == "Percikan");
    CHECK(emitter.seed == 987);
    CHECK(emitter.spawn.burstCount == 20);
    CHECK(emitter.shape.shape == EmitterShape::Cone);
    CHECK(emitter.force.gravity.y == doctest::Approx(-6.0f));

    REQUIRE(emitter.overLifetime.sizeOverLife.Keys().size() == 3);
    CHECK(emitter.overLifetime.sizeOverLife.Keys()[0].outTangent == doctest::Approx(4.0f));
    CHECK(emitter.overLifetime.rotationRate.Keys()[0].interpolation == Interpolation::Constant);
    REQUIRE(emitter.overLifetime.colorOverLife.AlphaStops().size() == 3);
    CHECK(emitter.overLifetime.colorOverLife.Evaluate(1.0f).w == doctest::Approx(0.0f));
}

TEST_CASE("Menonaktifkan modul tidak menghapus datanya") {
    ParticleEffect effect = RichEffect();
    effect.emitters.front().SetEnabled(ModuleKind::Force, false);
    effect.emitters.front().SetEnabled(ModuleKind::OverLifetime, false);

    ParticleEffect loaded;
    REQUIRE(LoadEffectFromString(loaded, SaveEffectToString(effect)).ok);

    // Kriteria terima E7.2 nomor 3. Modul yang dimatikan tetap tertulis lengkap,
    // sehingga menyalakannya kembali mengembalikan efek yang sama persis — bukan
    // efek dengan nilai bawaan.
    ParticleEmitter& emitter = loaded.emitters.front();
    CHECK_FALSE(emitter.IsEnabled(ModuleKind::Force));
    CHECK(emitter.force.gravity.y == doctest::Approx(-6.0f));
    CHECK(emitter.force.drag == doctest::Approx(0.5f));
    CHECK_FALSE(emitter.IsEnabled(ModuleKind::OverLifetime));
    CHECK(emitter.overLifetime.sizeOverLife.Keys().size() == 3);

    emitter.SetEnabled(ModuleKind::Force, true);
    emitter.SetEnabled(ModuleKind::OverLifetime, true);
    CHECK(SaveEffectToString(loaded) == SaveEffectToString(RichEffect()));
}

TEST_CASE("Emitter yang dimatikan tidak melahirkan apa pun, tapi datanya utuh") {
    ParticleEffect effect;
    effect.emitters.push_back(RichEmitter());
    effect.emitters.push_back(SmokeEmitter());
    effect.emitters[1].enabled = false;

    ParticleSystem system;
    system.SetEffect(effect);
    system.SimulateTo(1.0f);
    CHECK(system.StatsFor(1).alive == 0);
    CHECK(system.StatsFor(0).alive > 0);

    // Aturan yang sama dengan modul: mematikan menyembunyikan, tidak menghapus.
    ParticleEffect loaded;
    REQUIRE(LoadEffectFromString(loaded, SaveEffectToString(effect)).ok);
    REQUIRE(loaded.emitters.size() == 2);
    CHECK_FALSE(loaded.emitters[1].enabled);
    CHECK(loaded.emitters[1].initial.lifetime == doctest::Approx(4.0f));
    CHECK(loaded.emitters[1].name == "Asap");
}

TEST_CASE("Scrub timeline deterministik: waktu yang sama, keadaan yang sama") {
    const ParticleEffect effect = RichEffect();

    // Jalan A: maju langsung ke 2 detik.
    ParticleSystem direct;
    direct.SetEffect(effect);
    direct.SimulateTo(2.0f);

    // Jalan B: maju melewatinya, mundur, lalu maju lagi — persis yang dilakukan
    // pengguna saat menyeret penanda timeline bolak-balik.
    ParticleSystem scrubbed;
    scrubbed.SetEffect(effect);
    scrubbed.SimulateTo(3.5f);
    scrubbed.SimulateTo(0.4f);
    scrubbed.SimulateTo(2.7f);
    scrubbed.SimulateTo(2.0f);

    REQUIRE(direct.Particles().size() == scrubbed.Particles().size());
    REQUIRE_FALSE(direct.Particles().empty());
    for (std::size_t i = 0; i < direct.Particles().size(); ++i) {
        const Particle& a = direct.Particles()[i];
        const Particle& b = scrubbed.Particles()[i];
        INFO("partikel ke-", i);
        CHECK(a.index == b.index);
        CHECK(a.position.x == doctest::Approx(b.position.x));
        CHECK(a.position.y == doctest::Approx(b.position.y));
        CHECK(a.position.z == doctest::Approx(b.position.z));
        CHECK(a.age == doctest::Approx(b.age));
        CHECK(a.size == doctest::Approx(b.size));
    }
}

TEST_CASE("Benih yang sama menghasilkan sebaran yang sama, benih lain tidak") {
    ParticleEffect effect = RichEffect();

    ParticleSystem a;
    a.SetEffect(effect);
    a.SimulateTo(1.0f);

    ParticleSystem b;
    b.SetEffect(effect);
    b.SimulateTo(1.0f);
    REQUIRE(a.Particles().size() == b.Particles().size());
    CHECK(a.Particles().front().position.x == doctest::Approx(b.Particles().front().position.x));

    effect.emitters.front().seed = 555;
    ParticleSystem other;
    other.SetEffect(effect);
    other.SimulateTo(1.0f);
    REQUIRE_FALSE(other.Particles().empty());
    CHECK(other.Particles().front().position.x !=
          doctest::Approx(a.Particles().front().position.x));
}

TEST_CASE("Anggaran partikel ditahan, dan penahanannya dilaporkan") {
    ParticleEffect effect;
    ParticleEmitter& emitter = effect.emitters.emplace_back();
    emitter.spawn.rate = 100000.0f;
    emitter.initial.lifetime = 100.0f;
    emitter.initial.lifetimeJitter = 0.0f;
    emitter.maxParticles = 250;

    ParticleSystem system;
    system.SetEffect(effect);
    system.SimulateTo(1.0f);

    CHECK(system.Particles().size() == 250);
    // Dilaporkan, bukan diam-diam melahirkan lebih sedikit: penulis efek harus
    // tahu bahwa yang dilihatnya sudah dipotong anggaran.
    CHECK(system.AtBudget());
}

TEST_CASE("Seratus ribu partikel disimulasikan dalam anggaran satu frame") {
    ParticleEffect effect;
    ParticleEmitter& emitter = effect.emitters.emplace_back();
    emitter.spawn.rate = 2000000.0f;
    emitter.initial.lifetime = 100.0f;
    emitter.initial.lifetimeJitter = 0.0f;
    emitter.maxParticles = 100000;
    emitter.force.enabled = true;
    emitter.overLifetime.enabled = true;

    ParticleSystem system;
    system.SetEffect(effect);
    system.SimulateTo(0.2f);
    REQUIRE(system.Particles().size() == 100000);

    // Satu langkah pada populasi penuh.
    //
    // Anggarannya berbeda per build, dan itu disengaja. Yang dijanjikan kriteria
    // terima adalah pengalaman yang dikirim, yaitu Release — di situ batasnya
    // ketat: 8 ms, separuh frame 60 Hz, menyisakan ruang untuk menggambarnya.
    // Debug berjalan sekitar 14x lebih lambat karena glm tanpa optimisasi, dan
    // memakai angka yang sama di sana hanya akan membuat test gagal untuk sebab
    // yang tidak ada hubungannya. Batas Debug tetap ada supaya kemunduran
    // algoritmik — misalnya sesuatu yang berubah jadi kuadratik — tetap
    // tertangkap di build mana pun.
#if SIM_DEBUG
    constexpr double kBudgetMs = 120.0;
#else
    constexpr double kBudgetMs = 8.0;
#endif
    const auto begin = std::chrono::steady_clock::now();
    system.SimulateTo(system.Time() + ParticleSystem::kFixedStep * 1.5f);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
            .count();
    INFO("satu langkah pada 100k partikel: ", ms, " ms (anggaran ", kBudgetMs, " ms)");
    CHECK(ms < kBudgetMs);
}

TEST_CASE("Menggeser jauh ke depan tidak menggantungkan editor") {
    ParticleEffect effect = RichEffect();
    effect.emitters.front().spawn.looping = true;
    effect.emitters.front().spawn.duration = 0.0f;

    ParticleSystem system;
    system.SetEffect(effect);

    const auto begin = std::chrono::steady_clock::now();
    system.SimulateTo(3600.0f);  // satu jam
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin)
            .count();
    INFO("menggeser ke satu jam: ", ms, " ms");
    // Dibatasi lima menit simulasi susulan; tanpa batas itu, satu jam berarti
    // 216 ribu langkah dan editor berhenti menanggapi.
    CHECK(ms < 2000.0);
}

TEST_CASE("Beberapa emitter disimpan berurut dan dimuat utuh") {
    ParticleEffect effect;
    effect.name = "Ledakan";
    effect.emitters.push_back(RichEmitter());
    effect.emitters.push_back(SmokeEmitter());

    const std::string text = SaveEffectToString(effect);
    ParticleEffect loaded;
    REQUIRE(LoadEffectFromString(loaded, text).ok);
    CHECK(SaveEffectToString(loaded) == text);

    REQUIRE(loaded.emitters.size() == 2);
    // Urutannya bagian dari efeknya, bukan detail penyimpanan: penulis efek
    // menyusun daftarnya, dan daftar yang berubah urutan sendiri setiap kali
    // dibuka tidak bisa dipakai untuk apa pun.
    CHECK(loaded.emitters[0].name == "Percikan");
    CHECK(loaded.emitters[1].name == "Asap");
    CHECK(loaded.emitters[1].seed == 4242);
    CHECK(loaded.emitters[1].initial.size == doctest::Approx(0.8f));
    CHECK(loaded.emitters[0].shape.shape == EmitterShape::Cone);
}

TEST_CASE("Berkas v1 dibaca sebagai efek dengan satu emitter") {
    // Bentuk v1: modul langsung di akar, tanpa daftar `emitters`. Berkas yang
    // sudah ada di cakram tidak boleh hilang hanya karena skemanya berkembang.
    const std::string v1 = R"({
  "version": 1,
  "name": "Lama",
  "seed": 777,
  "maxParticles": 321,
  "spawn": { "enabled": true, "rate": 33.0, "burstCount": 5 },
  "shape": { "enabled": true, "shape": "box" },
  "initial": { "enabled": true, "lifetime": 3.5 }
})";

    ParticleEffect loaded;
    const EffectIoResult result = LoadEffectFromString(loaded, v1);
    REQUIRE(result.ok);
    CHECK(result.sourceVersion == 1);
    CHECK(loaded.name == "Lama");

    REQUIRE(loaded.emitters.size() == 1);
    const ParticleEmitter& emitter = loaded.emitters.front();
    CHECK(emitter.seed == 777);
    CHECK(emitter.maxParticles == 321);
    CHECK(emitter.spawn.rate == doctest::Approx(33.0f));
    CHECK(emitter.spawn.burstCount == 5);
    CHECK(emitter.shape.shape == EmitterShape::Box);
    CHECK(emitter.initial.lifetime == doctest::Approx(3.5f));
}

TEST_CASE("Emitter tidak saling mengganggu: yang berdua sama dengan yang sendiri") {
    ParticleEffect solo;
    solo.emitters.push_back(RichEmitter());

    ParticleEffect duo;
    duo.emitters.push_back(RichEmitter());
    duo.emitters.push_back(SmokeEmitter());

    ParticleSystem alone;
    alone.SetEffect(solo);
    alone.SimulateTo(1.5f);

    ParticleSystem together;
    together.SetEffect(duo);
    together.SimulateTo(1.5f);

    // Inilah janji yang membuat menggabungkan emitter aman: menambah emitter
    // kedua tidak boleh menggeser satu partikel pun milik yang pertama. Kalau
    // bisa, menyusun efek berlapis berubah jadi menebak — setiap penambahan
    // merusak apa yang sudah disetel sebelumnya.
    std::vector<Particle> first;
    for (const Particle& particle : together.Particles()) {
        if (particle.emitter == 0) {
            first.push_back(particle);
        }
    }
    REQUIRE(first.size() == alone.Particles().size());
    REQUIRE_FALSE(first.empty());
    for (std::size_t i = 0; i < first.size(); ++i) {
        INFO("partikel ke-", i);
        CHECK(first[i].index == alone.Particles()[i].index);
        CHECK(first[i].position.x == doctest::Approx(alone.Particles()[i].position.x));
        CHECK(first[i].position.y == doctest::Approx(alone.Particles()[i].position.y));
        CHECK(first[i].size == doctest::Approx(alone.Particles()[i].size));
    }
}

TEST_CASE("Anggaran dihitung per emitter, bukan dibagi rata") {
    ParticleEmitter hungry;
    hungry.name = "Rakus";
    hungry.spawn.rate = 100000.0f;
    hungry.initial.lifetime = 100.0f;
    hungry.initial.lifetimeJitter = 0.0f;
    hungry.maxParticles = 50;

    ParticleEmitter modest;
    modest.name = "Tenang";
    modest.seed = 31337;
    modest.spawn.rate = 60.0f;
    modest.initial.lifetime = 100.0f;
    modest.initial.lifetimeJitter = 0.0f;
    modest.maxParticles = 1000;

    ParticleEffect effect;
    effect.emitters.push_back(hungry);
    effect.emitters.push_back(modest);

    ParticleSystem system;
    system.SetEffect(effect);
    system.SimulateTo(1.0f);

    // Pembandingnya emitter yang sama dijalankan sendirian, bukan angka yang
    // ditulis tangan: yang sedang diuji adalah "tetangganya tidak berpengaruh",
    // dan angka tetap akan ikut goyah setiap kali batas langkah bergeser satu.
    ParticleEffect aloneEffect;
    aloneEffect.emitters.push_back(modest);
    ParticleSystem alone;
    alone.SetEffect(aloneEffect);
    alone.SimulateTo(1.0f);

    // Emitter pertama mentok, yang kedua sama sekali tidak terpengaruh. Anggaran
    // bersama akan membuat emitter di bawah diam tanpa alasan yang terlihat di
    // panel — dan pengaturannya sendiri terlihat benar.
    CHECK(system.StatsFor(0).alive == 50);
    CHECK(system.StatsFor(0).atBudget);
    CHECK(system.StatsFor(1).alive == alone.Particles().size());
    CHECK(system.StatsFor(1).alive > 50);
    CHECK_FALSE(system.StatsFor(1).atBudget);
    CHECK(system.AtBudget());
}

TEST_CASE("Menyusun ulang emitter tidak mengubah partikelnya") {
    ParticleEffect ordered;
    ordered.emitters.push_back(RichEmitter());
    ordered.emitters.push_back(SmokeEmitter());

    ParticleEffect swapped;
    swapped.emitters.push_back(SmokeEmitter());
    swapped.emitters.push_back(RichEmitter());

    ParticleSystem a;
    a.SetEffect(ordered);
    a.SimulateTo(1.5f);

    ParticleSystem b;
    b.SetEffect(swapped);
    b.SimulateTo(1.5f);

    // Benihnya milik emitter, bukan turunan dari posisinya. Kalau turunan,
    // menggeser satu emitter ke atas akan mengubah efek yang sudah jadi.
    const auto smokeFrom = [](const ParticleSystem& system, uint32_t which) {
        std::vector<Particle> out;
        for (const Particle& particle : system.Particles()) {
            if (particle.emitter == which) {
                out.push_back(particle);
            }
        }
        return out;
    };
    const std::vector<Particle> before = smokeFrom(a, 1);
    const std::vector<Particle> after = smokeFrom(b, 0);
    REQUIRE(before.size() == after.size());
    REQUIRE_FALSE(before.empty());
    for (std::size_t i = 0; i < before.size(); ++i) {
        INFO("partikel asap ke-", i);
        CHECK(before[i].position.x == doctest::Approx(after[i].position.x));
        CHECK(before[i].position.y == doctest::Approx(after[i].position.y));
    }
}

TEST_CASE("Benih emitter berikutnya selalu berbeda dari yang sudah dipakai") {
    ParticleEffect effect;
    for (int i = 0; i < 4; ++i) {
        ParticleEmitter& added = effect.emitters.emplace_back();
        added.seed = effect.NextSeed();
        // Dipanggil sesudah emitternya masuk daftar, jadi benihnya masih
        // terlihat sebagai "sudah terpakai" pada panggilan berikutnya.
        added.seed = effect.NextSeed();
    }
    for (std::size_t i = 0; i < effect.emitters.size(); ++i) {
        for (std::size_t j = i + 1; j < effect.emitters.size(); ++j) {
            INFO("emitter ", i, " vs ", j);
            CHECK(effect.emitters[i].seed != effect.emitters[j].seed);
        }
    }
}
