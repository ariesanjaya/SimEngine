#pragma once

#include "Sim/Particle/ParticleEffect.h"

#include <cstdint>
#include <vector>

namespace sim::particle {

/// Satu partikel hidup, sudah dalam ruang dunia.
struct Particle {
    Vec3 position{0.0f};
    Vec3 velocity{0.0f};
    Vec4 color{1.0f};
    float size = 0.0f;
    float rotation = 0.0f;
    float age = 0.0f;
    float lifetime = 1.0f;
    /// Nomor urut kelahiran. Inilah yang menentukan seluruh angka acak partikel
    /// ini — lihat catatan determinisme di ParticleSystem.
    uint32_t index = 0;

    float NormalizedAge() const { return lifetime > 0.0f ? age / lifetime : 1.0f; }
};

/// Simulasi partikel di CPU, deterministik terhadap waktu.
///
/// **Keadaan pada waktu T selalu sama, apa pun jalan menuju ke sana.** Itu
/// kriteria terima E7.2 nomor 2, dan dua keputusan yang menjaminnya:
///
/// 1. **Angka acak sebuah partikel diturunkan dari nomor urutnya**, bukan
///    diambil dari aliran RNG yang berjalan. Aliran yang berjalan membuat
///    partikel ke-100 bergantung pada berapa kali RNG dipanggil sebelumnya —
///    sehingga menggeser timeline mundur lalu maju lagi menghasilkan efek yang
///    berbeda, dan penulis efek kehilangan kepercayaan pada apa yang dilihatnya.
///
/// 2. **Langkah waktunya tetap**, dihitung dari nol. Melangkah 0→1 detik
///    sekaligus tidak sama dengan enam puluh langkah 1/60 detik, jadi jumlah
///    langkah harus ditentukan waktunya — bukan oleh laju frame editor.
///
/// Menggeser MAJU melanjutkan dari keadaan sekarang; menggeser MUNDUR memulai
/// ulang dari nol. Keduanya mendarat di keadaan yang sama karena keduanya
/// melewati batas langkah yang sama.
class ParticleSystem {
public:
    /// Langkah simulasi tetap. Dipilih 1/60 detik: cukup halus untuk gerak yang
    /// terlihat, dan angka yang sama dengan laju frame yang paling lazim
    /// sehingga preview terasa sama dengan hasil akhirnya.
    static constexpr float kFixedStep = 1.0f / 60.0f;

    void SetEffect(const ParticleEffect& effect);
    const ParticleEffect& Effect() const { return effect_; }

    /// Memajukan atau memundurkan simulasi ke sebuah waktu absolut.
    void SimulateTo(float time);
    void Reset();

    const std::vector<Particle>& Particles() const { return particles_; }
    float Time() const { return static_cast<float>(step_) * kFixedStep; }
    /// Partikel yang lahir sejak awal, termasuk yang sudah mati. Dipakai
    /// statistik panel.
    uint32_t SpawnedTotal() const { return spawned_; }
    /// True bila jumlah partikel tertahan `maxParticles`. Panel memakainya
    /// untuk memperingatkan anggaran, bukan diam-diam melahirkan lebih sedikit.
    bool AtBudget() const { return atBudget_; }

private:
    void Step();
    void Spawn(uint32_t index);

    ParticleEffect effect_;
    std::vector<Particle> particles_;
    int64_t step_ = 0;
    uint32_t spawned_ = 0;
    /// Sisa pecahan partikel yang belum sempat lahir, supaya laju yang bukan
    /// kelipatan langkah tetap tepat rata-ratanya.
    float spawnAccumulator_ = 0.0f;
    bool burstDone_ = false;
    bool atBudget_ = false;
};

}  // namespace sim::particle
