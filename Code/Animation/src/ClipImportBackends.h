#pragma once

#include "Sim/Animation/Clip.h"
#include "Sim/Core/Math.h"

#include <filesystem>
#include <string>
#include <vector>

/// Bagian dalam importir klip: satu pembaca per format, di atas aturan yang
/// sama.
///
/// **Aturannya di sini, bukan di tiap pembaca.** Membuang kanal yang tidak
/// bergerak, menyamakan belahan kuaternion, dan memilih interpolasi adalah
/// keputusan tentang bentuk `Clip` — bukan tentang FBX, glTF, maupun USD. Yang
/// menuliskannya ulang di tiap pembaca akan berbeda diam-diam di salah satunya,
/// dan yang terlihat adalah satu format yang klipnya "agak lain" tanpa ada yang
/// tahu kenapa.
namespace sim::animation::detail {

/// Selisih di bawah ini dianggap "tidak bergerak". Lihat catatan panjangnya di
/// `ClipImport.cpp`.
inline constexpr float kConstantEpsilon = 1e-6f;

/// Satu frame hasil cuplikan sebuah sendi.
struct SampledFrame {
    float time = 0.0f;
    Vec3 translation{0.0f};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    Vec3 scale{1.0f};
};

/// Menyamakan belahan tiap kuaternion dengan frame sebelumnya.
///
/// `q` dan `−q` adalah rotasi yang sama, dan tiap frame diuraikan sendiri — jadi
/// tandanya boleh berbalik di tengah klip tanpa ada yang berubah pada rotasinya.
/// Yang membacanya meng-slerp dua kunci berurutan, dan dua kunci yang
/// berseberangan belahan diputar lewat jalan memutar: satu frame yang berputar
/// hampir penuh lingkaran.
void AlignHemisphere(std::vector<SampledFrame>& frames);

/// Menambahkan kanal translasi/skala yang benar-benar bergerak, plus track
/// rotasinya.
void AddBoneChannels(Clip& clip, const std::string& bone,
                     const std::vector<SampledFrame>& frames);

/// Menambahkan satu kanal skalar dari deret nilai, kecuali bila nilainya tetap.
void AddScalarChannel(Clip& clip, const std::string& bone, Channel channel,
                      const std::vector<float>& times, const std::vector<float>& values);

/// Ruas terakhir sebuah jalur sendi. USD menamai sendinya `Root/Hip/Knee`,
/// sedangkan rangka menyimpan `Knee` — dan track diikat lewat nama itu.
std::string JointLeafName(const std::string& path);

std::vector<Clip> ImportClipsFromFbxFile(const std::filesystem::path& path, std::string& error);
std::vector<Clip> ImportClipsFromGltfFile(const std::filesystem::path& path, std::string& error);
std::vector<Clip> ImportClipsFromUsdFile(const std::filesystem::path& path, std::string& error);

}  // namespace sim::animation::detail
