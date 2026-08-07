#pragma once

#include "Sim/Render/TraceBackend.h"
#include "Sim/Render/Types.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace sim::render {

/// Waktu GPU satu pass, milidetik.
///
/// **Angkanya tertinggal beberapa frame dari yang terlihat**, dan itu disengaja:
/// membacanya tepat waktu menuntut CPU menunggu GPU, yaitu persis yang tidak
/// boleh dilakukan alat ukur. Untuk angka yang dibaca manusia, keterlambatan itu
/// tidak berarti apa-apa.
///
/// `name` menunjuk penyimpanan milik renderer dan hanya sah sampai `Render()`
/// berikutnya — sama disiplinnya dengan span di `ViewportScene`.
struct PassTiming {
    std::string_view name;
    float milliseconds = 0.0f;
};

/// Batas antara editor dan rendering (seam #1 di docs/ARCHITECTURE.md).
///
/// Panel Viewport hanya melihat antarmuka ini. Sampai E8 implementasinya adalah
/// StubRenderer (clear + grid prosedural); setelah E8 diganti VulkanRenderer
/// tanpa satu baris pun panel berubah. Header ini sengaja tidak meng-include
/// apa pun dari Vulkan — kalau suatu saat ia perlu, berarti ada seam yang bocor.
class IViewportRenderer {
public:
    virtual ~IViewportRenderer() = default;

    /// Menyesuaikan ukuran target render. Aman dipanggil tiap frame dengan
    /// ukuran yang sama — implementasi yang membangun ulang setiap panggilan
    /// akan membuat panel berkedip saat diseret.
    virtual void Resize(uint32_t width, uint32_t height) = 0;

    virtual void Render(const ViewportDesc& desc, const ViewportScene& scene) = 0;

    /// Handle tekstur hasil render terakhir, siap dilempar ke ImGui::Image().
    /// Bisa berubah setelah Resize().
    virtual TextureHandle ColorTarget() const = 0;

    /// Koordinat tekstur pojok kanan-bawah dari bagian yang benar-benar
    /// digambar.
    ///
    /// Tidak selalu (1,1): implementasi boleh mengalokasikan gambar lebih besar
    /// daripada yang diminta agar tidak perlu mengalokasi ulang setiap panel
    /// digeser sedikit. UI wajib memakai nilai ini saat menggambar, kalau tidak
    /// bagian gambar yang belum diisi ikut terlihat.
    virtual Vec2 ColorTargetUvMax() const = 0;

    virtual uint32_t Width() const = 0;
    virtual uint32_t Height() const = 0;

    /// Name implementasi, ditampilkan di pojok viewport supaya jelas bahwa yang
    /// terlihat masih preview stub, bukan hasil rendering sungguhan.
    virtual const char* Name() const = 0;

    /// Waktu GPU per pass frame yang terakhir selesai. Kosong bila perangkatnya
    /// tidak mendukung timestamp, atau selama beberapa frame pertama.
    virtual std::span<const PassTiming> PassTimings() const { return {}; }

    /// Biaya pembaruan clipmap SDF di CPU, milidetik, beserta jumlah voxel
    /// yang ditulis frame terakhir.
    ///
    /// **CPU, bukan GPU, dan karena itu tidak muncul di `PassTimings`.**
    /// Komposit clipmap masih berjalan di CPU sampai ia pindah ke compute, dan
    /// biaya yang tidak muncul di tabel mana pun adalah biaya yang tidak ada
    /// yang mengawasinya — sementara justru angka inilah yang dibatasi anggaran
    /// 0,4 ms rencana GI.
    virtual float SdfUpdateMilliseconds() const { return 0.0f; }
    virtual uint64_t SdfVoxelsWritten() const { return 0; }

    /// Backend trace yang **benar-benar dipakai**, beserta alasannya. Berbeda
    /// dari yang diminta lewat `ViewportDesc::gi` bila permintaannya tidak bisa
    /// dipenuhi perangkat ini.
    virtual TraceBackendSelection GiBackend() const { return {}; }
};

}  // namespace sim::render
