// Jembatan antara C++ yang dipancarkan slangc dan tipe-tipe engine.
//
// **Seluruh nama bermangling tinggal di berkas ini.** slangc memberi akhiran
// angka pada nama yang dipancarkannya, dan akhiran itu bisa bergeser ketika
// slangc diperbarui. Membiarkannya bocor ke pemanggil berarti setiap pemakai
// model shading ikut rusak; dikurung di sini, yang rusak satu berkas dan
// perbaikannya satu tempat.

#include "Sim/Reference/Shading.h"

// Berkas ini dibangkitkan saat build dari `Shaders/openpbr_cpu.slang`, yang
// sendirinya hanya membungkus `Shaders/openpbr.slang` tanpa satu baris
// matematika pun. Lihat CMakeLists.txt modul ini.
#include "openpbr_cpu.cpp"

#include <cmath>

namespace sim::reference {
namespace {

Vector<float, 3> ToSlang(const Vec3& v) {
    Vector<float, 3> out;
    out.x = v.x;
    out.y = v.y;
    out.z = v.z;
    return out;
}

Vec3 FromSlang(const Vector<float, 3>& v) { return Vec3(v.x, v.y, v.z); }

OpenPBRSurface_0 ToSlang(const Surface& s) {
    OpenPBRSurface_0 out{};
    out.baseWeight_0 = s.baseWeight;
    out.baseColor_0 = ToSlang(s.baseColor);
    out.baseMetalness_0 = s.baseMetalness;
    out.baseDiffuseRoughness_0 = s.baseDiffuseRoughness;
    out.specularWeight_0 = s.specularWeight;
    out.specularColor_0 = ToSlang(s.specularColor);
    out.specularRoughness_0 = s.specularRoughness;
    out.specularRoughnessAnisotropy_0 = s.specularRoughnessAnisotropy;
    out.specularIor_0 = s.specularIor;
    out.coatWeight_0 = s.coatWeight;
    out.coatColor_0 = ToSlang(s.coatColor);
    out.coatRoughness_0 = s.coatRoughness;
    out.coatRoughnessAnisotropy_0 = s.coatRoughnessAnisotropy;
    out.coatIor_0 = s.coatIor;
    out.coatDarkening_0 = s.coatDarkening;
    out.fuzzWeight_0 = s.fuzzWeight;
    out.fuzzColor_0 = ToSlang(s.fuzzColor);
    out.fuzzRoughness_0 = s.fuzzRoughness;
    return out;
}

struct Evaluated {
    Vec3 direct;
    Vec3 ambient;
};

Evaluated Run(const Surface& surface, const Frame& frame, const Frame& coatFrame,
              const Vec3& lightDirection, const Vec3& radiance,
              const Environment& environment) {
    OpenPbrQuery_0 q{};
    q.shadingNormal_0 = ToSlang(frame.normal);
    q.shadingTangent_0 = ToSlang(frame.tangent);
    q.shadingBitangent_0 = ToSlang(frame.bitangent);
    q.viewDirection_0 = ToSlang(frame.view);
    q.coatNormal_0 = ToSlang(coatFrame.normal);
    q.coatTangent_0 = ToSlang(coatFrame.tangent);
    q.coatBitangent_0 = ToSlang(coatFrame.bitangent);
    q.light_0 = ToSlang(lightDirection);
    q.radiance_0 = ToSlang(radiance);
    q.irradiance_0 = ToSlang(environment.irradiance);
    q.prefilteredBase_0 = ToSlang(environment.prefilteredBase);
    q.prefilteredCoat_0 = ToSlang(environment.prefilteredCoat);
    q.dfg_0.x = environment.dfgScale;
    q.dfg_0.y = environment.dfgBias;
    q.surface_0 = ToSlang(surface);

    Vector<float, 3> direct{};
    Vector<float, 3> ambient{};

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

    return Evaluated{FromSlang(direct), FromSlang(ambient)};
}

}  // namespace

Frame Frame::FromNormal(const Vec3& normal, const Vec3& view) {
    Frame frame;
    frame.normal = glm::normalize(normal);
    frame.view = view;
    // Sumbu bantu yang tidak sejajar normal. Ambang 0,99 memakai komponen y
    // karena permukaan mendatar — lantai, meja — jauh lebih sering daripada
    // dinding tegak, jadi cabang yang mahalnya sama sebaiknya jarang diambil.
    const Vec3 axis =
        std::abs(frame.normal.y) < 0.99f ? Vec3(0.0f, 1.0f, 0.0f) : Vec3(1.0f, 0.0f, 0.0f);
    frame.tangent = glm::normalize(glm::cross(axis, frame.normal));
    frame.bitangent = glm::cross(frame.normal, frame.tangent);
    return frame;
}

Vec3 EvaluateDirect(const Surface& surface, const Frame& frame, const Frame& coatFrame,
                    const Vec3& lightDirection, const Vec3& radiance) {
    return Run(surface, frame, coatFrame, lightDirection, radiance, Environment{}).direct;
}

Vec3 EvaluateDirect(const Surface& surface, const Frame& frame, const Vec3& lightDirection,
                    const Vec3& radiance) {
    return EvaluateDirect(surface, frame, frame, lightDirection, radiance);
}

Vec3 EvaluateEnvironment(const Surface& surface, const Frame& frame, const Frame& coatFrame,
                         const Environment& environment) {
    return Run(surface, frame, coatFrame, Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f), environment)
        .ambient;
}

Vec3 EvaluateEnvironment(const Surface& surface, const Frame& frame,
                         const Environment& environment) {
    return EvaluateEnvironment(surface, frame, frame, environment);
}

}  // namespace sim::reference
