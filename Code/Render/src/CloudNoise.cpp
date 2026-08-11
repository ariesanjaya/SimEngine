#include "Sim/Render/CloudNoise.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace sim::render {
namespace {

/// Hash bilangan bulat 3D → 32 bit. Harus **hanya** bergantung pada indeks sel
/// yang sudah dibungkus: hash yang ikut membaca indeks sebelum pembungkusan
/// menghasilkan derau yang tampak benar di mana-mana kecuali tepat di batas
/// pengulangan.
uint32_t Hash(int32_t x, int32_t y, int32_t z, uint32_t seed) {
    uint32_t h = seed * 747796405u;
    h ^= static_cast<uint32_t>(x) * 2654435761u;
    h = (h ^ (h >> 15)) * 2246822519u;
    h ^= static_cast<uint32_t>(y) * 2246822519u;
    h = (h ^ (h >> 13)) * 3266489917u;
    h ^= static_cast<uint32_t>(z) * 3266489917u;
    h = (h ^ (h >> 16)) * 668265263u;
    return h ^ (h >> 15);
}

float UnitFloat(uint32_t hash) {
    return static_cast<float>(hash & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
}

/// Indeks sel yang dibungkus ke 0..cells-1, benar juga untuk indeks negatif.
int32_t Wrap(int32_t value, int32_t cells) {
    const int32_t result = value % cells;
    return result < 0 ? result + cells : result;
}

/// Titik fitur di dalam sebuah sel, dalam kubus satuan.
Vec3 FeaturePoint(int32_t x, int32_t y, int32_t z, int32_t cells, uint32_t seed) {
    const uint32_t h = Hash(Wrap(x, cells), Wrap(y, cells), Wrap(z, cells), seed);
    return Vec3(UnitFloat(h), UnitFloat(h * 2654435761u + 1u), UnitFloat(h * 2246822519u + 2u));
}

/// Gradien satuan dari sebuah hash. Dua belas arah rusuk kubus — pilihan Perlin
/// sendiri, dan bukan arah acak: gradien acak menumpuk di beberapa arah dan
/// meninggalkan artefak yang searah di seluruh volume.
Vec3 Gradient(uint32_t hash) {
    static const Vec3 kGradients[12] = {
        {1, 1, 0},  {-1, 1, 0},  {1, -1, 0},  {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1},
        {1, 0, -1}, {-1, 0, -1}, {0, 1, 1},   {0, -1, 1},  {0, 1, -1}, {0, -1, -1}};
    return kGradients[hash % 12u];
}

/// Kurva pelunak Perlin: 6t⁵ − 15t⁴ + 10t³. Turunan pertama **dan kedua**-nya
/// nol di kedua ujung; kurva kubik yang lebih murah hanya menolkan yang pertama,
/// dan turunan kedua yang meloncat terlihat sebagai pita di setiap batas sel.
float Fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

}  // namespace

float CloudNoise::Worley(const Vec3& position, uint32_t cellsPerAxis, uint32_t seed) {
    if (cellsPerAxis == 0) {
        return 0.0f;
    }
    const auto cells = static_cast<int32_t>(cellsPerAxis);
    const Vec3 scaled = position * static_cast<float>(cells);
    const auto baseX = static_cast<int32_t>(std::floor(scaled.x));
    const auto baseY = static_cast<int32_t>(std::floor(scaled.y));
    const auto baseZ = static_cast<int32_t>(std::floor(scaled.z));

    float nearest = 1e9f;
    for (int32_t dz = -1; dz <= 1; ++dz) {
        for (int32_t dy = -1; dy <= 1; ++dy) {
            for (int32_t dx = -1; dx <= 1; ++dx) {
                const int32_t cx = baseX + dx;
                const int32_t cy = baseY + dy;
                const int32_t cz = baseZ + dz;
                // Titik fitur diambil dari sel yang **dibungkus**, tapi
                // posisinya dihitung di sel yang tidak dibungkus. Itulah
                // seluruh triknya: bidang deraunya periodik tanpa satu pun
                // tetangga yang hilang di tepi.
                const Vec3 point =
                    FeaturePoint(cx, cy, cz, cells, seed) + Vec3(static_cast<float>(cx),
                                                                 static_cast<float>(cy),
                                                                 static_cast<float>(cz));
                nearest = std::min(nearest, glm::length(scaled - point));
            }
        }
    }
    // Jarak terjauh yang mungkin ke titik terdekat kira-kira setengah diagonal
    // sel. Menjepit lebih aman daripada menskalakan dengan angka yang pas-pasan:
    // satu texel yang melewati satu adalah satu texel putih menyala di dalam
    // awan.
    return std::clamp(nearest, 0.0f, 1.0f);
}

float CloudNoise::Perlin(const Vec3& position, uint32_t cellsPerAxis, uint32_t seed) {
    if (cellsPerAxis == 0) {
        return 0.5f;
    }
    const auto cells = static_cast<int32_t>(cellsPerAxis);
    const Vec3 scaled = position * static_cast<float>(cells);
    const auto x0 = static_cast<int32_t>(std::floor(scaled.x));
    const auto y0 = static_cast<int32_t>(std::floor(scaled.y));
    const auto z0 = static_cast<int32_t>(std::floor(scaled.z));
    const Vec3 fraction = scaled - Vec3(static_cast<float>(x0), static_cast<float>(y0),
                                        static_cast<float>(z0));

    const float u = Fade(fraction.x);
    const float v = Fade(fraction.y);
    const float w = Fade(fraction.z);

    const auto corner = [&](int32_t dx, int32_t dy, int32_t dz) {
        const Vec3 gradient =
            Gradient(Hash(Wrap(x0 + dx, cells), Wrap(y0 + dy, cells), Wrap(z0 + dz, cells), seed));
        const Vec3 offset = fraction - Vec3(static_cast<float>(dx), static_cast<float>(dy),
                                            static_cast<float>(dz));
        return glm::dot(gradient, offset);
    };

    const float x00 = Lerp(corner(0, 0, 0), corner(1, 0, 0), u);
    const float x10 = Lerp(corner(0, 1, 0), corner(1, 1, 0), u);
    const float x01 = Lerp(corner(0, 0, 1), corner(1, 0, 1), u);
    const float x11 = Lerp(corner(0, 1, 1), corner(1, 1, 1), u);
    const float y0v = Lerp(x00, x10, v);
    const float y1v = Lerp(x01, x11, v);
    const float value = Lerp(y0v, y1v, w);

    // Perlin membentang kira-kira ±1 pada gradien rusuk kubus. Dipetakan ke
    // 0..1 lalu dijepit.
    return std::clamp(value * 0.5f + 0.5f, 0.0f, 1.0f);
}

float CloudNoise::WorleyFbm(const Vec3& position, uint32_t baseCells, uint32_t octaves,
                            uint32_t seed) {
    if (octaves == 0) {
        return 0.0f;
    }
    float sum = 0.0f;
    float amplitude = 0.5f;
    float total = 0.0f;
    uint32_t cells = baseCells;
    for (uint32_t octave = 0; octave < octaves; ++octave) {
        sum += Worley(position, cells, seed + octave * 131u) * amplitude;
        total += amplitude;
        // Frekuensi berlipat dua tiap oktaf, dan **tetap bilangan bulat** —
        // itulah syarat agar tiap oktaf punya periode yang sama dengan volumenya.
        // Faktor pecahan (mis. 2,3) menghasilkan oktaf yang masing-masing rapi
        // tapi jumlahnya tidak menyambung sama sekali.
        cells *= 2u;
        amplitude *= 0.5f;
    }
    // Dibalik: gumpalan Worley berada di jarak **kecil**, dan awan harus padat
    // di sana.
    return std::clamp(1.0f - sum / total, 0.0f, 1.0f);
}

float CloudNoise::PerlinFbm(const Vec3& position, uint32_t baseCells, uint32_t octaves,
                            uint32_t seed) {
    if (octaves == 0) {
        return 0.5f;
    }
    float sum = 0.0f;
    float amplitude = 0.5f;
    float total = 0.0f;
    uint32_t cells = baseCells;
    for (uint32_t octave = 0; octave < octaves; ++octave) {
        sum += Perlin(position, cells, seed + octave * 977u) * amplitude;
        total += amplitude;
        cells *= 2u;
        amplitude *= 0.5f;
    }
    return std::clamp(sum / total, 0.0f, 1.0f);
}

float CloudNoise::PerlinWorley(const Vec3& position, uint32_t baseCells, uint32_t seed) {
    const float perlin = PerlinFbm(position, baseCells, 3, seed);
    const float worley = WorleyFbm(position, baseCells, 3, seed + 7919u);
    // Pemetaan ulang Perlin ke rentang yang **dasarnya digeser Worley**:
    // remap(perlin, worley − 1, 1, 0, 1). Worley menaikkan atau menurunkan
    // lantainya, Perlin mengisi di atasnya.
    //
    // **Bentuk pertama saya membagi dengan Worley**, dan hasilnya terukur:
    // median kanal ini nol — separuh volume terjepit habis di bawah, karena
    // Perlin dan Worley sama-sama bermean sekitar 0,5 sehingga pembilangnya
    // sama seringnya negatif. Awan yang lahir dari kanal semacam itu tidak
    // muncul sama sekali, dan yang terlihat bukan derau yang salah melainkan
    // langit yang cerah — tidak bisa dibedakan dari sakelar yang mati.
    const float low = worley - 1.0f;
    const float remapped = (perlin - low) / std::max(1.0f - low, 1e-4f);
    return std::clamp(remapped, 0.0f, 1.0f);
}

float CloudNoiseVolume::At(uint32_t x, uint32_t y, uint32_t z, uint32_t channel) const {
    // **Dijaga per sumbu, bukan pada indeks datarnya.** Indeks datar dari x yang
    // melewati tepi tetap jatuh di dalam larik — ia hanya jatuh di baris
    // berikutnya. Pemeriksaan yang hanya melihat indeks akhir karena itu
    // meloloskan justru kesalahan yang paling mungkin terjadi, dan yang
    // dikembalikannya adalah texel tetangga yang tampak masuk akal.
    if (size == 0 || channel >= 4 || x >= size || y >= size || z >= size) {
        return 0.0f;
    }
    const std::size_t index =
        ((static_cast<std::size_t>(z) * size + y) * size + x) * 4 + channel;
    if (index >= texels.size()) {
        return 0.0f;
    }
    return static_cast<float>(texels[index]) / 255.0f;
}

namespace {

/// Satu irisan z, atau beberapa. Dipisahkan supaya pembangkitannya bisa dibagi
/// antar-thread tanpa mengubah hasilnya: **tiap texel berdiri sendiri**, jadi
/// pembagian apa pun menghasilkan volume yang sama persis. Determinisme itu
/// bukan kenyamanan — volume yang berubah antar-jalan membuat setiap uji
/// perbandingan gambar menjadi tidak berarti.
void FillSlices(CloudNoiseVolume& volume, uint32_t seed, bool detail, uint32_t zBegin,
                uint32_t zEnd) {
    const uint32_t size = volume.size;
    const float inverse = 1.0f / static_cast<float>(size);
    for (uint32_t z = zBegin; z < zEnd; ++z) {
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                // Tengah texel, bukan pojoknya. Mencuplik di pojok menggeser
                // seluruh volume setengah texel terhadap dirinya sendiri, dan
                // yang menyambung lalu tidak lagi menyambung tepat sebesar itu.
                const Vec3 position((static_cast<float>(x) + 0.5f) * inverse,
                                    (static_cast<float>(y) + 0.5f) * inverse,
                                    (static_cast<float>(z) + 0.5f) * inverse);
                float channels[4];
                if (detail) {
                    channels[0] = CloudNoise::WorleyFbm(position, 4, 3, seed);
                    channels[1] = CloudNoise::WorleyFbm(position, 8, 3, seed + 31u);
                    channels[2] = CloudNoise::WorleyFbm(position, 16, 2, seed + 61u);
                    channels[3] = CloudNoise::WorleyFbm(position, 16, 1, seed + 97u);
                } else {
                    channels[0] = CloudNoise::PerlinWorley(position, 4, seed);
                    channels[1] = CloudNoise::WorleyFbm(position, 4, 3, seed + 11u);
                    channels[2] = CloudNoise::WorleyFbm(position, 8, 3, seed + 23u);
                    channels[3] = CloudNoise::WorleyFbm(position, 16, 2, seed + 41u);
                }
                const std::size_t base =
                    ((static_cast<std::size_t>(z) * size + y) * size + x) * 4;
                for (uint32_t channel = 0; channel < 4; ++channel) {
                    volume.texels[base + channel] = static_cast<uint8_t>(
                        std::clamp(channels[channel], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
        }
    }
}

/// Meregangkan tiap kanal ke seluruh rentang 0..1.
///
/// **Bukan penghalusan.** FBM yang dijumlahkan dari beberapa oktaf memusat di
/// sekitar mean-nya; terukur sebelum langkah ini ada, kanal gabungannya
/// membentang 0,09..0,71 dengan median 0,24. Seluruh parameter di hilir — dan
/// terutama cakupan, yang berupa ambang — lalu bergantung pada statistik yang
/// kebetulan itu: cakupan 0,45 berarti ambang 0,55, dan hanya 0,2% volume yang
/// melewatinya. Yang terlihat bukan derau yang salah melainkan **langit yang
/// cerah**, yang tidak bisa dibedakan dari sakelar yang mati. Acuannya
/// menormalkan deraunya dengan pass tersendiri, dan alasannya sama.
void NormalizeChannels(CloudNoiseVolume& volume) {
    if (volume.texels.empty()) {
        return;
    }
    for (std::size_t channel = 0; channel < 4; ++channel) {
        uint8_t lowest = 255;
        uint8_t highest = 0;
        for (std::size_t i = channel; i < volume.texels.size(); i += 4) {
            lowest = std::min(lowest, volume.texels[i]);
            highest = std::max(highest, volume.texels[i]);
        }
        // Kanal yang datar dibiarkan apa adanya: meregangkannya berarti membagi
        // dengan nol, dan hasilnya bukan kanal yang lebih berguna melainkan NaN.
        if (highest <= lowest) {
            continue;
        }
        const float scale = 255.0f / static_cast<float>(highest - lowest);
        for (std::size_t i = channel; i < volume.texels.size(); i += 4) {
            const float stretched = static_cast<float>(volume.texels[i] - lowest) * scale;
            volume.texels[i] = static_cast<uint8_t>(std::clamp(stretched, 0.0f, 255.0f) + 0.5f);
        }
    }
}

CloudNoiseVolume BuildVolume(uint32_t size, uint32_t seed, bool detail) {
    CloudNoiseVolume volume;
    if (size == 0) {
        return volume;
    }
    volume.size = size;
    volume.texels.assign(static_cast<std::size_t>(size) * size * size * 4, 0);

    // **Dibagi antar-thread, dan itu bukan penghalusan yang bisa ditunda.**
    // Volume bentuk 64³ menghabiskan 1,8 detik satu thread, dan 1,8 detik yang
    // dibayar saat editor dibuka adalah 1,8 detik yang dibayar setiap kali
    // editor dibuka. Pembagian per irisan z aman karena tiap texel berdiri
    // sendiri; hasilnya sama persis berapa pun thread-nya.
    const unsigned int cores = std::max(1u, std::thread::hardware_concurrency());
    const uint32_t workers = std::min<uint32_t>(cores, size);
    if (workers <= 1) {
        FillSlices(volume, seed, detail, 0, size);
        return volume;
    }

    std::vector<std::thread> threads;
    threads.reserve(workers - 1);
    const uint32_t perWorker = (size + workers - 1) / workers;
    for (uint32_t worker = 1; worker < workers; ++worker) {
        const uint32_t begin = std::min(size, worker * perWorker);
        const uint32_t end = std::min(size, begin + perWorker);
        if (begin >= end) {
            break;
        }
        threads.emplace_back([&volume, seed, detail, begin, end] {
            FillSlices(volume, seed, detail, begin, end);
        });
    }
    FillSlices(volume, seed, detail, 0, std::min(size, perWorker));
    for (std::thread& thread : threads) {
        thread.join();
    }
    NormalizeChannels(volume);
    return volume;
}

}  // namespace

CloudNoiseVolume BuildCloudShapeVolume(uint32_t size, uint32_t seed) {
    return BuildVolume(size, seed, /*detail=*/false);
}

CloudNoiseVolume BuildCloudDetailVolume(uint32_t size, uint32_t seed) {
    return BuildVolume(size, seed, /*detail=*/true);
}

float CloudHeightGradient(float heightKm, float bottomKm, float topKm) {
    const float thickness = topKm - bottomKm;
    if (thickness <= 0.0f) {
        return 0.0f;
    }
    const float fraction = (heightKm - bottomKm) / thickness;
    if (fraction <= 0.0f || fraction >= 1.0f) {
        return 0.0f;
    }
    // Naik cepat di alas, memudar panjang di puncak. Kumulus punya dasar yang
    // hampir rata dan puncak yang berjumbai, dan gradien simetris memberi
    // keduanya bentuk yang sama — yang terlihat sebagai awan berbentuk lensa.
    const float bottomFade = std::clamp(fraction / 0.15f, 0.0f, 1.0f);
    const float topFade = std::clamp((1.0f - fraction) / 0.55f, 0.0f, 1.0f);
    return bottomFade * topFade * topFade;
}

}  // namespace sim::render
