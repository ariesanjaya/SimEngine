#include "Sim/Render/TieredTrace.h"

#include <algorithm>

namespace sim::render {
namespace {

class TieredTraceBackend final : public ITraceBackend {
public:
    TieredTraceBackend(const HiZPyramid& depth, const ScreenTraceView& view,
                       const SdfVolume& volume, const TieredTraceSettings& settings)
        : depth_(depth),
          view_(view),
          settings_(settings),
          sdf_(CreateSdfTraceBackend(volume, settings.sdfMaxSteps)) {}

    // Tetap `Sdf`. Jenjangnya — layar, lalu SDF, lalu langit — memang isi dari
    // apa yang disebut jalur SDF di rencana; lapis screen-space bukan backend
    // tersendiri yang bisa dipilih pengguna, melainkan bagian dari jalur yang
    // harus bekerja di setiap GPU.
    TraceBackendKind Kind() const override { return TraceBackendKind::Sdf; }

    TraceResult Trace(const Vec3& origin, const Vec3& direction, float tMax) const override {
        TraceResult result;

        if (settings_.screenEnabled && depth_.IsValid()) {
            const ScreenTraceResult screen =
                TraceScreenSpace(depth_, view_, origin, direction, tMax, settings_.screen);
            result.steps = screen.steps;
            if (screen.hit) {
                result.hit = true;
                result.layer = TraceLayer::Screen;
                result.distance = screen.distance;
                result.position = screen.position;
                // Normalnya tidak diambil dari depth buffer di sini. Gradien
                // depth di tepi siluet menunjuk ke arah yang sama sekali salah,
                // dan pemakainya — M3 dan seterusnya — membacanya dari G-buffer
                // yang memang menyimpannya.
                return result;
            }
        }

        // Meleset di layar bukan jawaban: sinar yang keluar layar atau
        // tersembunyi di balik permukaan lain berarti "layar tidak tahu".
        TraceResult sdf = sdf_->Trace(origin, direction, tMax);
        // Langkah kedua lapis dijumlahkan. Yang diawasi anggaran adalah biaya
        // satu sinar, dan sinar yang gagal di layar lalu berjalan penuh di SDF
        // adalah sinar yang paling mahal — persis yang harus terlihat di
        // heatmap.
        sdf.steps += result.steps;
        sdf.layer = sdf.hit ? TraceLayer::Sdf : TraceLayer::Sky;
        return sdf;
    }

private:
    const HiZPyramid& depth_;
    ScreenTraceView view_;
    TieredTraceSettings settings_;
    std::unique_ptr<ITraceBackend> sdf_;
};

}  // namespace

std::unique_ptr<ITraceBackend> CreateTieredTraceBackend(const HiZPyramid& depth,
                                                        const ScreenTraceView& view,
                                                        const SdfVolume& volume,
                                                        const TieredTraceSettings& settings) {
    return std::make_unique<TieredTraceBackend>(depth, view, volume, settings);
}

}  // namespace sim::render
