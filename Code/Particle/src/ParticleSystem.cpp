#include "Sim/Particle/ParticleSystem.h"

#include <algorithm>
#include <cmath>

namespace sim::particle {
namespace {

/// Hash bilangan bulat → bilangan bulat (Wang/Jenkins, varian 32-bit).
///
/// Dipakai sebagai pengganti aliran RNG. Yang dibutuhkan bukan mutu statistik
/// tertinggi melainkan sifat lain: nilai yang sama untuk masukan yang sama, di
/// mesin mana pun, tanpa keadaan yang harus dibawa-bawa. Aliran RNG punya
/// keadaan, dan keadaan itulah yang membuat timeline tidak bisa digeser mundur.
uint32_t Hash(uint32_t value) {
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return value;
}

/// Angka acak [0,1) untuk sebuah partikel pada "saluran" tertentu.
///
/// Saluran memisahkan kegunaan: mengambil dua angka dari saluran yang sama akan
/// memberi nilai identik, dan mengambil kecepatan lalu ukuran dari saluran yang
/// bergeser satu membuat keduanya berkorelasi terlihat.
float Random01(uint32_t seed, uint32_t index, uint32_t channel) {
    const uint32_t bits = Hash(seed ^ Hash(index * 0x9e3779b9U + channel));
    return static_cast<float>(bits >> 8) * (1.0f / 16777216.0f);
}

float RandomSigned(uint32_t seed, uint32_t index, uint32_t channel) {
    return Random01(seed, index, channel) * 2.0f - 1.0f;
}

/// Noise nilai sederhana, deterministik terhadap posisi dan waktu.
float ValueNoise(const Vec3& p, float frequency, uint32_t channel) {
    const auto quantize = [frequency](float v) {
        return static_cast<uint32_t>(static_cast<int32_t>(std::floor(v * frequency)) + 4096);
    };
    const uint32_t bits =
        Hash(quantize(p.x) ^ Hash(quantize(p.y) ^ Hash(quantize(p.z) + channel)));
    return static_cast<float>(bits >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

}  // namespace

void ParticleSystem::SetEffect(const ParticleEffect& effect) {
    effect_ = effect;
    Reset();
}

void ParticleSystem::Reset() {
    particles_.clear();
    step_ = 0;
    spawned_ = 0;
    spawnAccumulator_ = 0.0f;
    burstDone_ = false;
    atBudget_ = false;
}

void ParticleSystem::SimulateTo(float time) {
    const int64_t target =
        time <= 0.0f ? 0 : static_cast<int64_t>(std::floor(time / kFixedStep));
    // Mundur berarti mulai ulang. Menyimpan riwayat akan jauh lebih cepat, tapi
    // juga berarti keadaan pada waktu T bergantung pada apa yang tersimpan —
    // yaitu persis sifat yang sedang dihindari.
    if (target < step_) {
        Reset();
    }
    // Batas atas yang murah: menggeser ke menit ke-10 pada efek yang looping
    // tidak boleh menggantungkan editor selama puluhan detik.
    constexpr int64_t kMaxCatchUp = 60 * 60 * 5;  // 5 menit pada 60 Hz
    if (target - step_ > kMaxCatchUp) {
        step_ = target - kMaxCatchUp;
    }
    while (step_ < target) {
        Step();
    }
}

void ParticleSystem::Spawn(uint32_t index) {
    const uint32_t seed = effect_.seed;
    Particle particle;
    particle.index = index;

    // --- bentuk emitter ---
    Vec3 offset(0.0f);
    Vec3 direction(0.0f, 1.0f, 0.0f);
    const ShapeModule& shape = effect_.shape;
    if (shape.enabled) {
        const float u = Random01(seed, index, 1);
        const float v = Random01(seed, index, 2);
        const float w = Random01(seed, index, 3);
        switch (shape.shape) {
            case EmitterShape::Point: break;
            case EmitterShape::Sphere: {
                // Arah seragam di bola lewat metode inversi — bukan tiga angka
                // acak yang dinormalkan, yang menumpuk di sudut kubus.
                const float z = v * 2.0f - 1.0f;
                const float theta = u * kTwoPi;
                const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
                direction = Vec3(r * std::cos(theta), z, r * std::sin(theta));
                const float radius = shape.emitFromShell ? 1.0f : std::cbrt(w);
                offset = direction * radius * shape.size.x;
                break;
            }
            case EmitterShape::Box:
                offset = Vec3((u * 2.0f - 1.0f) * shape.size.x,
                              (v * 2.0f - 1.0f) * shape.size.y,
                              (w * 2.0f - 1.0f) * shape.size.z);
                break;
            case EmitterShape::Cone: {
                const float theta = u * kTwoPi;
                const float radius = std::sqrt(v) * shape.size.x;
                offset = Vec3(std::cos(theta) * radius, 0.0f, std::sin(theta) * radius);
                const float spread = std::tan(shape.coneAngle);
                direction = glm::normalize(
                    Vec3(std::cos(theta) * spread, 1.0f, std::sin(theta) * spread));
                break;
            }
        }
    }
    particle.position = offset;

    // --- nilai awal ---
    const InitialModule& initial = effect_.initial;
    if (initial.enabled) {
        particle.velocity =
            initial.velocity + Vec3(RandomSigned(seed, index, 10) * initial.velocityJitter.x,
                                    RandomSigned(seed, index, 11) * initial.velocityJitter.y,
                                    RandomSigned(seed, index, 12) * initial.velocityJitter.z);
        // Bentuk emitter membelokkan arah kecepatan awalnya, bukan
        // menggantikannya: kerucut yang menyemburkan ke atas tetap menyembur ke
        // atas walau sudutnya nol.
        if (effect_.shape.enabled && effect_.shape.shape != EmitterShape::Point) {
            particle.velocity = direction * glm::length(particle.velocity);
        }
        particle.size = std::max(0.0f, initial.size + RandomSigned(seed, index, 13) *
                                                          initial.sizeJitter);
        particle.rotation =
            initial.rotation + RandomSigned(seed, index, 14) * initial.rotationJitter;
        particle.lifetime = std::max(
            0.01f, initial.lifetime + RandomSigned(seed, index, 15) * initial.lifetimeJitter);
        particle.color = Vec4(initial.color, 1.0f);
    } else {
        particle.size = 0.2f;
        particle.lifetime = 2.0f;
    }
    particles_.push_back(particle);
}

void ParticleSystem::Step() {
    const float dt = kFixedStep;
    const float now = static_cast<float>(step_) * dt;

    // --- kelahiran ---
    const SpawnModule& spawn = effect_.spawn;
    const bool withinDuration =
        !spawn.enabled ? false
                       : (spawn.looping || spawn.duration <= 0.0f || now < spawn.duration);
    if (spawn.enabled && withinDuration) {
        spawnAccumulator_ += spawn.rate * dt;
    }
    if (spawn.enabled && !burstDone_ && spawn.burstCount > 0 && now >= spawn.burstTime) {
        spawnAccumulator_ += static_cast<float>(spawn.burstCount);
        burstDone_ = true;
    }
    atBudget_ = false;
    while (spawnAccumulator_ >= 1.0f) {
        spawnAccumulator_ -= 1.0f;
        if (static_cast<int>(particles_.size()) >= effect_.maxParticles) {
            atBudget_ = true;
            // Sisa pecahan dibuang, bukan ditumpuk: menumpuknya membuat efek
            // meledak begitu ada partikel yang mati, jauh setelah anggarannya
            // tercapai.
            spawnAccumulator_ = 0.0f;
            break;
        }
        Spawn(spawned_++);
    }

    // --- gerak ---
    const ForceModule& force = effect_.force;
    const OverLifetimeModule& life = effect_.overLifetime;
    const CollisionModule& collision = effect_.collision;

    for (Particle& particle : particles_) {
        particle.age += dt;
        const float t = particle.NormalizedAge();

        if (force.enabled) {
            particle.velocity += force.gravity * dt;
            if (force.drag > 0.0f) {
                particle.velocity -= particle.velocity * std::min(1.0f, force.drag * dt);
            }
            if (force.vortexStrength != 0.0f) {
                const Vec3 axis = glm::normalize(force.vortexAxis);
                const Vec3 toAxis = particle.position - axis * glm::dot(particle.position, axis);
                if (glm::length(toAxis) > 1e-4f) {
                    particle.velocity += glm::cross(axis, toAxis) * force.vortexStrength * dt;
                }
            }
            if (force.attractorStrength != 0.0f) {
                const Vec3 delta = force.attractorPosition - particle.position;
                const float distance = glm::length(delta);
                if (distance > 1e-3f) {
                    particle.velocity += (delta / distance) * force.attractorStrength * dt;
                }
            }
            if (force.noiseStrength != 0.0f) {
                particle.velocity +=
                    Vec3(ValueNoise(particle.position, force.noiseFrequency, 1),
                         ValueNoise(particle.position, force.noiseFrequency, 2),
                         ValueNoise(particle.position, force.noiseFrequency, 3)) *
                    force.noiseStrength * dt;
            }
        }

        Vec3 velocity = particle.velocity;
        if (life.enabled) {
            velocity *= life.velocityScale.Evaluate(t);
            particle.rotation += life.rotationRate.Evaluate(t) * dt;
            particle.color = life.colorOverLife.Evaluate(t);
        }
        particle.position += velocity * dt;

        if (collision.enabled && particle.position.y < collision.planeHeight) {
            particle.position.y = collision.planeHeight;
            if (collision.killOnContact) {
                particle.age = particle.lifetime;
            } else {
                particle.velocity.y = -particle.velocity.y * collision.bounce;
                particle.velocity.x *= 1.0f - collision.friction;
                particle.velocity.z *= 1.0f - collision.friction;
            }
        }
    }

    // --- kematian ---
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                    [](const Particle& particle) {
                                        return particle.age >= particle.lifetime;
                                    }),
                     particles_.end());

    ++step_;
}

}  // namespace sim::particle
