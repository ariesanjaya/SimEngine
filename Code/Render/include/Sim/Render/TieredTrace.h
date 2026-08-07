#pragma once

#include "Sim/Render/ScreenTrace.h"
#include "Sim/Render/SdfVolume.h"
#include "Sim/Render/TraceBackend.h"

#include <memory>

namespace sim::render {

struct TieredTraceSettings {
    ScreenTraceSettings screen;
    /// Anggaran langkah sphere tracing SDF.
    uint32_t sdfMaxSteps = 48;
    /// Lapis screen-space bisa dimatikan untuk membandingkan keduanya secara
    /// langsung. Bukan tombol kualitas: ia alat untuk melihat berapa banyak yang
    /// sebenarnya dijawab lapis pertama.
    bool screenEnabled = true;
};

/// Backend berjenjang: depth buffer layar, lalu clipmap SDF, lalu langit.
///
/// **Urutannya bukan soal biaya melainkan soal ketelitian.** Depth buffer punya
/// resolusi geometri sungguhan — satu piksel — sedangkan voxel SDF terhalus
/// sepuluh sentimeter, dan detail kontak yang hilang di SDF justru yang paling
/// terlihat: bayangan kontak dan pantulan di dekat perpotongan permukaan.
/// Screen-space karena itu ditanya lebih dulu, dan SDF menangkap segalanya yang
/// tidak ada di layar.
///
/// **Meleset di lapis pertama bukan jawaban.** Sinar yang keluar layar, atau
/// yang tersembunyi di balik permukaan lain, tidak berarti "tidak ada apa-apa
/// di sana" — ia berarti "layar tidak tahu". Membedakan keduanya adalah seluruh
/// gunanya jenjang ini; menyamakannya menghasilkan lubang gelap tepat di tepi
/// layar, cacat khas GI screen-space.
///
/// `depth` dan `volume` dipinjam, tidak dimiliki: keduanya hidup selama frame
/// dan dibangun ulang tiap frame.
std::unique_ptr<ITraceBackend> CreateTieredTraceBackend(const HiZPyramid& depth,
                                                        const ScreenTraceView& view,
                                                        const SdfVolume& volume,
                                                        const TieredTraceSettings& settings);

}  // namespace sim::render
