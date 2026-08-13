#include "ClipImportBackends.h"

#include "Sim/Core/Log.h"

#if SIM_WITH_USD

#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3h.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdSkel/animation.h>

#include <algorithm>
#include <set>

namespace sim::animation::detail {
namespace {

namespace usd = pxr;

/// Konversi ruang panggung → ruang mesin, **sama persis dengan `UsdImport.cpp`**.
///
/// Klip membawa transform LOKAL dan rangkanya membawa bind pose; keduanya
/// dirangkai induk-ke-anak oleh `Pose::ComputeGlobal`. Satu saja yang memakai
/// konvensi berbeda membuat bone yang tidak dianimasikan klip terlempar dari
/// tempatnya — dan itu tidak menghasilkan galat, hanya rangka yang meledak.
///
/// Termasuk kejanggalannya: translasi diputar sumbunya, rotasi **tidak**. Itu
/// yang dilakukan `ReadSkeleton` untuk rest transform, jadi itu pula yang harus
/// dilakukan di sini. Yang memperbaiki salah satunya harus memperbaiki keduanya.
struct StageSpace {
    double metersPerUnit = 1.0;
    bool zUp = false;

    Vec3 Point(const usd::GfVec3f& value) const {
        const auto x = static_cast<float>(value[0] * metersPerUnit);
        const auto y = static_cast<float>(value[1] * metersPerUnit);
        const auto z = static_cast<float>(value[2] * metersPerUnit);
        return zUp ? Vec3(x, z, -y) : Vec3(x, y, z);
    }
};

/// Seluruh waktu cuplikan yang disebut ketiga atributnya, tergabung dan terurut.
///
/// **Digabung, bukan diambil dari salah satunya.** Sebuah SkelAnimation boleh
/// menganimasikan rotasi pada dua puluh empat frame sementara translasinya hanya
/// pada dua; mengambil waktu dari rotasi saja membuang kunci translasi yang
/// tidak berdampingan dengannya.
std::vector<double> SampleTimes(const usd::UsdSkelAnimation& animation) {
    std::set<double> unique;
    for (const usd::UsdAttribute& attribute :
         {animation.GetRotationsAttr(), animation.GetTranslationsAttr(),
          animation.GetScalesAttr()}) {
        if (!attribute) {
            continue;
        }
        std::vector<double> times;
        attribute.GetTimeSamples(&times);
        unique.insert(times.begin(), times.end());
    }
    return {unique.begin(), unique.end()};
}

}  // namespace

std::vector<Clip> ImportClipsFromUsdFile(const std::filesystem::path& path, std::string& error) {
    std::vector<Clip> clips;
    error.clear();

    const usd::UsdStageRefPtr stage = usd::UsdStage::Open(path.string());
    if (!stage) {
        error = "cannot open the USD stage";
        return clips;
    }

    StageSpace space;
    space.metersPerUnit = usd::UsdGeomGetStageMetersPerUnit(stage);
    space.zUp = usd::UsdGeomGetStageUpAxis(stage) == usd::UsdGeomTokens->z;

    const double perSecond = stage->GetTimeCodesPerSecond();
    const float frameRate = perSecond > 1.0 ? static_cast<float>(perSecond) : 24.0f;

    for (const usd::UsdPrim& prim : stage->Traverse()) {
        if (!prim.IsA<usd::UsdSkelAnimation>()) {
            continue;
        }
        const usd::UsdSkelAnimation animation(prim);

        usd::VtTokenArray joints;
        if (!animation.GetJointsAttr().Get(&joints) || joints.empty()) {
            continue;
        }
        const std::vector<double> times = SampleTimes(animation);
        if (times.size() < 2) {
            // Satu cuplikan bukan animasi melainkan sebuah pose. Mengimpornya
            // menghasilkan klip berdurasi nol yang tidak bisa diputar.
            continue;
        }

        Clip clip;
        clip.name = prim.GetName().GetString();
        clip.frameRate = frameRate;
        clip.duration = std::max(
            static_cast<float>((times.back() - times.front()) / std::max(perSecond, 1.0)), 1e-6f);
        clip.looping = true;

        // Dicuplik per waktu lebih dulu, baru dibalik menjadi per sendi: tiap
        // atribut USD menyimpan seluruh sendi dalam satu larik per waktu, jadi
        // membacanya per sendi berarti membaca ulang seluruh larik itu sekali
        // untuk tiap sendi.
        std::vector<std::vector<SampledFrame>> perJoint(joints.size());
        for (std::vector<SampledFrame>& frames : perJoint) {
            frames.resize(times.size());
        }

        for (std::size_t t = 0; t < times.size(); ++t) {
            const auto when = static_cast<usd::UsdTimeCode>(times[t]);
            const auto seconds =
                static_cast<float>((times[t] - times.front()) / std::max(perSecond, 1.0));

            usd::VtVec3fArray translations;
            usd::VtQuatfArray rotations;
            usd::VtVec3hArray scales;
            animation.GetTranslationsAttr().Get(&translations, when);
            animation.GetRotationsAttr().Get(&rotations, when);
            animation.GetScalesAttr().Get(&scales, when);

            for (std::size_t j = 0; j < joints.size(); ++j) {
                SampledFrame& frame = perJoint[j][t];
                frame.time = seconds;
                if (j < translations.size()) {
                    frame.translation = space.Point(translations[j]);
                }
                if (j < rotations.size()) {
                    // USD menyimpan bagian nyatanya terpisah; `GetImaginary`
                    // yang memberi x, y, z.
                    const usd::GfQuatf& q = rotations[j];
                    frame.rotation = glm::normalize(Quat(q.GetReal(), q.GetImaginary()[0],
                                                         q.GetImaginary()[1], q.GetImaginary()[2]));
                }
                if (j < scales.size()) {
                    frame.scale = Vec3(static_cast<float>(scales[j][0]),
                                       static_cast<float>(scales[j][1]),
                                       static_cast<float>(scales[j][2]));
                }
            }
        }

        for (std::size_t j = 0; j < joints.size(); ++j) {
            AlignHemisphere(perJoint[j]);
            AddBoneChannels(clip, JointLeafName(joints[j].GetString()), perJoint[j]);
        }

        if (clip.TrackCount() == 0 && clip.RotationTrackCount() == 0) {
            continue;
        }
        clips.push_back(std::move(clip));
    }

    if (clips.size() == 1) {
        clips.front().name = path.stem().string();
    }
    if (clips.empty()) {
        error = "no UsdSkelAnimation in this file animates a joint";
    }
    return clips;
}

}  // namespace sim::animation::detail

#else

namespace sim::animation::detail {

std::vector<Clip> ImportClipsFromUsdFile(const std::filesystem::path& /*path*/,
                                         std::string& error) {
    error = "this build of SimEngine has no USD support";
    return {};
}

}  // namespace sim::animation::detail

#endif
