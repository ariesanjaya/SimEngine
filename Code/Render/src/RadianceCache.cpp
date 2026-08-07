#include "Sim/Render/RadianceCache.h"

#include <algorithm>
#include <cmath>

namespace sim::render {
namespace {

/// Pencampur bilangan bulat 32-bit.
///
/// Sifat yang dibutuhkan bukan keacakan kriptografis melainkan **penyebaran
/// bit**: dua sel bertetangga berbeda satu pada satu sumbu, dan tanpa
/// pencampuran keduanya mendarat di slot bertetangga — lalu setiap probing
/// linear langsung menabrak tetangganya sendiri. Konstantanya dari murmur3.
uint32_t Mix(uint32_t value) {
    value ^= value >> 16;
    value *= 0x85EBCA6Bu;
    value ^= value >> 13;
    value *= 0xC2B2AE35u;
    value ^= value >> 16;
    return value;
}

}  // namespace

uint8_t MajorAxis(const Vec3& normal) {
    const Vec3 magnitude = glm::abs(normal);
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z) {
        return normal.x >= 0.0f ? 0 : 1;
    }
    if (magnitude.y >= magnitude.z) {
        return normal.y >= 0.0f ? 2 : 3;
    }
    return normal.z >= 0.0f ? 4 : 5;
}

void RadianceCache::Configure(const RadianceCacheSettings& settings) {
    settings_ = settings;
    settings_.cellSize = std::max(settings.cellSize, 1e-3f);
    settings_.lodDistance = std::max(settings.lodDistance, 1e-3f);
    settings_.maxProbe = std::max(settings.maxProbe, 1u);
    settings_.accumulationFrames = std::max(settings.accumulationFrames, 1u);
    settings_.staleFrames = std::max(settings.staleFrames, 1u);
    // Dibulatkan ke pangkat dua terdekat ke bawah: slot dipilih dengan masker
    // bit, bukan dengan modulo, dan masker hanya benar pada pangkat dua.
    uint32_t capacity = 1;
    while (capacity * 2 <= std::max(settings.capacity, 2u)) {
        capacity *= 2;
    }
    settings_.capacity = capacity;
    Clear();
}

void RadianceCache::Clear() {
    entries_.assign(settings_.capacity, Entry{});
    dropped_ = 0;
    evicted_ = 0;
}

RadianceCacheKey RadianceCache::KeyFor(const Vec3& position, const Vec3& normal,
                                       const Vec3& cameraPosition) const {
    RadianceCacheKey key;
    key.face = MajorAxis(normal);

    const float distance = glm::length(position - cameraPosition);
    // Tingkat naik satu setiap kali jaraknya berlipat di luar `lodDistance`.
    uint32_t level = 0;
    float span = settings_.lodDistance;
    while (distance > span && level < 15) {
        span *= 2.0f;
        ++level;
    }
    key.level = static_cast<uint8_t>(level);

    const float size = settings_.cellSize * static_cast<float>(1u << level);
    // `std::floor`, bukan pemotongan ke arah nol: pemotongan membuat kedua sisi
    // titik nol berbagi satu sel, dan kesalahannya baru muncul setelah adegannya
    // melewati origin.
    key.cell = {static_cast<int32_t>(std::floor(position.x / size)),
                static_cast<int32_t>(std::floor(position.y / size)),
                static_cast<int32_t>(std::floor(position.z / size))};
    return key;
}

uint32_t RadianceCache::SlotOf(const RadianceCacheKey& key) const {
    uint32_t hash = Mix(static_cast<uint32_t>(key.cell.x) * 0x9E3779B9u);
    hash ^= Mix(static_cast<uint32_t>(key.cell.y) * 0x85EBCA6Bu);
    hash ^= Mix(static_cast<uint32_t>(key.cell.z) * 0xC2B2AE35u);
    hash ^= Mix((static_cast<uint32_t>(key.face) << 8) | key.level);
    return Mix(hash) & (settings_.capacity - 1);
}

bool RadianceCache::Insert(const RadianceCacheKey& key, const Vec3& radiance, uint32_t frame) {
    if (entries_.empty()) {
        return false;
    }
    const uint32_t start = SlotOf(key);
    // Dua lintasan: yang pertama mencari slot kosong atau kunci yang sama, yang
    // kedua baru merebut yang basi. Merebut di lintasan pertama akan membuang
    // entri hidup yang kebetulan ditemui lebih dulu daripada slot kosong di
    // belakangnya.
    for (uint32_t step = 0; step < settings_.maxProbe; ++step) {
        const uint32_t slot = (start + step) & (settings_.capacity - 1);
        Entry& entry = entries_[slot];
        if (!entry.used) {
            entry.used = true;
            entry.key = key;
            entry.radiance = radiance;
            entry.samples = 1;
            entry.lastFrame = frame;
            return true;
        }
        if (entry.key == key) {
            // Rata-rata berjalan dengan jumlah sampel dibatasi — bentuk yang
            // sama dengan akumulasi probe, dan alasannya sama: rata-rata sejati
            // atas seluruh riwayat berhenti merespons perubahan.
            const uint32_t weight =
                std::min(entry.samples + 1, settings_.accumulationFrames);
            entry.radiance += (radiance - entry.radiance) / static_cast<float>(weight);
            entry.samples = std::min(entry.samples + 1, settings_.accumulationFrames);
            entry.lastFrame = frame;
            return true;
        }
    }

    for (uint32_t step = 0; step < settings_.maxProbe; ++step) {
        const uint32_t slot = (start + step) & (settings_.capacity - 1);
        Entry& entry = entries_[slot];
        if (frame - entry.lastFrame >= settings_.staleFrames) {
            entry.key = key;
            entry.radiance = radiance;
            entry.samples = 1;
            entry.lastFrame = frame;
            ++evicted_;
            return true;
        }
    }
    ++dropped_;
    return false;
}

bool RadianceCache::Query(const RadianceCacheKey& key, Vec3& radiance) const {
    if (entries_.empty()) {
        return false;
    }
    const uint32_t start = SlotOf(key);
    for (uint32_t step = 0; step < settings_.maxProbe; ++step) {
        const Entry& entry = entries_[(start + step) & (settings_.capacity - 1)];
        if (!entry.used) {
            // Slot kosong berarti kuncinya tidak pernah dimasukkan: penyisipan
            // selalu mengisi slot kosong pertama yang ditemuinya, jadi tidak ada
            // yang bisa bersembunyi di belakang lubang.
            return false;
        }
        if (entry.key == key) {
            radiance = entry.radiance;
            return true;
        }
    }
    return false;
}

uint32_t RadianceCache::LiveEntries() const {
    uint32_t live = 0;
    for (const Entry& entry : entries_) {
        if (entry.used) {
            ++live;
        }
    }
    return live;
}

}  // namespace sim::render
