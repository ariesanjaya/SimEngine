#include "Sim/Render/TraceBackend.h"

namespace sim::render {
namespace {

/// Backend yang selalu meleset.
///
/// Ia tidak menembakkan apa pun dan tidak menghitung langkah apa pun, jadi
/// `steps` tetap nol — dan heatmap langkah yang seluruhnya nol adalah jawaban
/// yang benar untuk backend yang memang tidak melangkah.
class NullTraceBackend final : public ITraceBackend {
public:
    TraceBackendKind Kind() const override { return TraceBackendKind::Null; }

    TraceResult Trace(const Vec3&, const Vec3&, float) const override { return {}; }
};

}  // namespace

const char* ToString(TraceBackendKind kind) {
    switch (kind) {
        case TraceBackendKind::Null:
            return "None";
        case TraceBackendKind::Sdf:
            return "SDF";
        case TraceBackendKind::RayQuery:
            return "Ray query";
    }
    return "None";
}

const char* ToString(TraceLayer layer) {
    switch (layer) {
        case TraceLayer::None:
            return "None";
        case TraceLayer::Screen:
            return "Screen";
        case TraceLayer::Sdf:
            return "SDF";
        case TraceLayer::Sky:
            return "Sky";
    }
    return "None";
}

const char* ToString(TraceBackendPreference preference) {
    switch (preference) {
        case TraceBackendPreference::Auto:
            return "Auto";
        case TraceBackendPreference::ForceSdf:
            return "Force SDF";
        case TraceBackendPreference::ForceRayQuery:
            return "Force ray query";
    }
    return "Auto";
}

const char* ToString(GiDebugView view) {
    switch (view) {
        case GiDebugView::Off:
            return "Off";
        case GiDebugView::Albedo:
            return "Albedo";
        case GiDebugView::Normal:
            return "Normal";
        case GiDebugView::Irradiance:
            return "Irradiance";
        case GiDebugView::RayCount:
            return "Ray count";
        case GiDebugView::TraceLayers:
            return "Trace layer";
        case GiDebugView::MarchSteps:
            return "March steps";
    }
    return "Off";
}

TraceBackendSelection SelectTraceBackend(const TraceBackendCaps& caps,
                                         TraceBackendPreference preference) {
    TraceBackendSelection selection;

    // Tanpa compute, tidak satu pun backend bisa jalan. Disebutkan lebih dulu
    // supaya pesannya menyebut sebab yang sebenarnya, bukan "ray query tidak
    // tersedia" pada perangkat yang masalahnya jauh lebih mendasar.
    if (!caps.compute) {
        selection.kind = TraceBackendKind::Null;
        selection.reason = "GI needs compute shaders; this device has none";
        return selection;
    }

    switch (preference) {
        case TraceBackendPreference::ForceSdf:
            // Dipaksa SDF pada perangkat yang punya ray query bukan kesalahan
            // melainkan **cara jalur SDF diuji sama sekali**. Mesin pengembangan
            // punya RT core, jadi pemilihan otomatis tidak akan pernah
            // menjalankan jalur yang justru harus bekerja di setiap GPU.
            selection.kind = TraceBackendKind::Sdf;
            selection.reason = "SDF forced by preference";
            return selection;

        case TraceBackendPreference::ForceRayQuery:
            if (caps.rayQuery) {
                selection.kind = TraceBackendKind::RayQuery;
                selection.reason = "Ray query forced by preference";
                return selection;
            }
            selection.kind = TraceBackendKind::Sdf;
            selection.fellBack = true;
            selection.reason = "Ray query unsupported on this device; using SDF";
            return selection;

        case TraceBackendPreference::Auto:
            break;
    }

    if (caps.rayQuery) {
        selection.kind = TraceBackendKind::RayQuery;
        selection.reason = "Ray query available";
    } else {
        selection.kind = TraceBackendKind::Sdf;
        selection.reason = "No ray query on this device; using SDF";
    }
    return selection;
}

std::unique_ptr<ITraceBackend> CreateNullTraceBackend() {
    return std::make_unique<NullTraceBackend>();
}

}  // namespace sim::render
