#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <vector>

namespace sim::render {

/// Kunci sebuah entri cache: posisi terkuantisasi, arah mayor, dan tingkatnya.
///
/// **Arah ikut ke dalam kunci, bukan hanya posisi.** Sebuah titik di lantai
/// memancarkan radiansi yang sangat berbeda ke atas dan ke samping; menyimpan
/// satu angka per posisi berarti merata-ratakan keduanya, dan hasilnya cahaya
/// yang bocor menembus permukaan tipis — sisi gelap sebuah dinding menerima
/// rata-rata sisi terangnya.
///
/// Enam arah, bukan bola penuh: yang dicatat radiansi permukaan, dan permukaan
/// punya normal. Enam sudah membedakan lantai dari langit-langit dan keempat
/// dinding, dan itulah pembedaan yang menentukan.
struct RadianceCacheKey {
    glm::ivec3 cell{0};
    /// 0..5 untuk +X, −X, +Y, −Y, +Z, −Z.
    uint8_t face = 0;
    /// Tingkat kuantisasi. Ikut ke kunci supaya sel halus dan sel kasar yang
    /// kebetulan bertumpuk tidak pernah berbagi entri.
    uint8_t level = 0;

    bool operator==(const RadianceCacheKey& other) const {
        return cell == other.cell && face == other.face && level == other.level;
    }
};

struct RadianceCacheSettings {
    /// Banyaknya entri, pangkat dua. Rencana GI menyebut 2²⁰.
    ///
    /// **Tetap, bukan tumbuh.** Cache yang tumbuh mengikuti adegan adalah cache
    /// yang biayanya tidak bisa disebut di muka — dan yang harus dialokasi ulang
    /// tepat saat kamera memasuki ruangan baru, yaitu frame yang paling tidak
    /// punya waktu luang. Yang tetap kadang membuang entri; membuang entri hanya
    /// berarti satu query menjawab "tidak tahu", dan pemanggilnya sudah harus
    /// menangani jawaban itu.
    uint32_t capacity = 1u << 20;
    /// Ukuran sel pada tingkat terhalus, meter.
    float cellSize = 0.25f;
    /// Jarak tempat sel mulai membesar. Di luar itu ukurannya berlipat setiap
    /// kali jaraknya berlipat.
    ///
    /// **Sel membesar mengikuti jarak, sama seperti kaskade SDF.** Detail
    /// radiansi pada jarak lima puluh meter tidak terlihat sama sekali, dan
    /// menyimpannya sehalus yang di depan mata menghabiskan seluruh cache untuk
    /// hal yang tidak ada yang melihat.
    float lodDistance = 8.0f;
    /// Langkah probing linear maksimum sebelum menyerah.
    ///
    /// Menyerah berarti satu sampel hilang atau satu query menjawab "tidak
    /// tahu". Probing tanpa batas menukar itu dengan lingkaran yang panjangnya
    /// bergantung isi cache — dan itu biaya yang tidak bisa disebut.
    uint32_t maxProbe = 4;
    /// Banyaknya frame yang diakumulasi tiap entri.
    uint32_t accumulationFrames = 32;
    /// Berapa frame sebuah entri boleh tidak tersentuh sebelum slotnya boleh
    /// direbut kunci lain.
    ///
    /// **Tanpa ini cache terisi permanen.** Kamera yang berkeliling meninggalkan
    /// entri untuk setiap tempat yang pernah dilihatnya, dan entri itu tidak
    /// pernah dibaca lagi tapi tetap memakai slotnya — sampai seluruh cache
    /// penuh oleh masa lalu dan setiap penyisipan baru terbuang. Gejalanya GI
    /// yang bekerja saat editor baru dibuka lalu diam-diam berhenti bekerja
    /// setelah beberapa menit menjelajah.
    uint32_t staleFrames = 120;
};

/// Cache radiansi berbasis hash spasial.
///
/// **Ini acuan kebenaran, bukan yang dipakai menggambar.** Yang menggambar
/// memakai storage buffer di GPU; yang di sini menyimpan angka yang sama dengan
/// rumus yang sama, cukup lambat untuk test dan cukup sederhana untuk dibaca.
class RadianceCache {
public:
    void Configure(const RadianceCacheSettings& settings);
    void Clear();

    const RadianceCacheSettings& Settings() const { return settings_; }

    /// Kunci sebuah titik permukaan, dilihat dari sebuah kamera.
    RadianceCacheKey KeyFor(const Vec3& position, const Vec3& normal,
                            const Vec3& cameraPosition) const;

    /// Menambahkan satu sampel radiansi.
    ///
    /// Mengembalikan false bila slotnya penuh oleh kunci lain sampai batas
    /// probing — sampelnya hilang, dan itu memang perilaku cache yang tetap
    /// ukurannya.
    bool Insert(const RadianceCacheKey& key, const Vec3& radiance, uint32_t frame);

    /// Radiansi sebuah kunci.
    ///
    /// **Mengembalikan false untuk entri yang belum pernah diisi, bukan hitam.**
    /// Pelajaran yang sama dengan sinar SDF di M3: nilai yang tidak diketahui
    /// dan nilai nol adalah dua hal berbeda, dan menyamakannya membuat pemakainya
    /// menggelapkan gambar sambil menambah derau.
    bool Query(const RadianceCacheKey& key, Vec3& radiance) const;

    /// Entri yang slotnya direbut karena tidak tersentuh selama `staleFrames`.
    uint64_t EvictedEntries() const { return evicted_; }

    /// Banyaknya entri yang pernah diisi.
    uint32_t LiveEntries() const;

    /// Banyaknya sampel yang hilang karena slotnya penuh, sejak `Clear`.
    /// Diagnostik: cache yang terlalu kecil terlihat di sini, bukan pada gambar.
    uint64_t DroppedSamples() const { return dropped_; }

private:
    struct Entry {
        RadianceCacheKey key;
        Vec3 radiance{0.0f};
        uint32_t samples = 0;
        uint32_t lastFrame = 0;
        bool used = false;
    };

    /// Slot awal sebuah kunci.
    uint32_t SlotOf(const RadianceCacheKey& key) const;

    RadianceCacheSettings settings_;
    std::vector<Entry> entries_;
    uint64_t dropped_ = 0;
    uint64_t evicted_ = 0;
};

/// Sumbu mayor sebuah normal, 0..5 untuk +X, −X, +Y, −Y, +Z, −Z.
uint8_t MajorAxis(const Vec3& normal);

}  // namespace sim::render
