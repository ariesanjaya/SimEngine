#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <memory>
#include <string>

namespace sim::render {

/// Backend penelusuran sinar untuk global illumination.
///
/// **Satu keputusan yang menentukan seluruh sistem GI: backend tracing adalah
/// plugin, dan sisa sistem tidak boleh tahu ray-nya ditembak ke apa.** Screen
/// probe, hash grid, denoiser, dan integrasi shading harus identik di kedua
/// tier. Satu saja pass yang bercabang menurut tier di luar batas ini dan
/// desainnya bocor — biaya perawatannya lalu berlipat, karena setiap perubahan
/// harus dipikirkan dua kali dan diuji dua kali.
enum class TraceBackendKind : uint8_t {
    /// Selalu meleset. Bukan penambal sementara: ia yang membuat seluruh sistem
    /// di atasnya bisa dibangun dan diuji sebelum ada satu pun ray yang benar-
    /// benar ditembakkan, dan ia yang menjadi jawaban benar saat GI dimatikan.
    Null,
    /// Screen-space HiZ + SDF clipmap global. Jalur yang harus bekerja di setiap
    /// GPU.
    Sdf,
    /// `VK_KHR_ray_query`. Hanya perangkat yang mendukungnya.
    RayQuery,
};

const char* ToString(TraceBackendKind kind);

/// Hasil satu penelusuran.
struct TraceResult {
    bool hit = false;
    /// Jarak sepanjang sinar. Tidak berarti bila `hit` false.
    float distance = 0.0f;
    Vec3 position{0.0f};
    Vec3 normal{0.0f, 1.0f, 0.0f};
    /// Banyaknya langkah march yang dipakai.
    ///
    /// **Diagnostik, bukan hasil.** Ia yang menjadi heatmap jumlah langkah per
    /// piksel — alat yang paling sering dipakai selama pengembangan SDF, karena
    /// ia satu-satunya yang menunjukkan di mana sphere tracing kehabisan
    /// anggaran sebelum sampai ke permukaan.
    uint32_t steps = 0;
};

/// Kemampuan perangkat yang menentukan backend mana yang bisa dipakai.
struct TraceBackendCaps {
    bool rayQuery = false;
    bool compute = true;
};

/// Apa yang diminta pengguna, bukan apa yang akhirnya dipakai.
enum class TraceBackendPreference : uint8_t {
    /// Pilih sendiri yang terbaik yang tersedia.
    Auto,
    ForceSdf,
    ForceRayQuery,
};

const char* ToString(TraceBackendPreference preference);

/// Hasil pemilihan backend, beserta alasannya.
///
/// **Alasannya ikut, dan itu bukan kemewahan.** Backend yang dipilih diam-diam
/// adalah backend yang tidak ada yang tahu sedang berjalan — dan pada mesin yang
/// punya RT core, jalur SDF berhenti dijalankan siapa pun tanpa ada yang
/// menyadarinya. Jalur itu justru yang harus bekerja di setiap GPU.
struct TraceBackendSelection {
    TraceBackendKind kind = TraceBackendKind::Null;
    /// True bila permintaan pengguna tidak bisa dipenuhi dan diturunkan.
    bool fellBack = false;
    /// Kalimat siap tampil, dalam bahasa Inggris seperti sisa UI.
    std::string reason;
};

/// Memilih backend dari kemampuan perangkat dan permintaan pengguna.
///
/// Memaksa ray query pada perangkat yang tidak mendukungnya **diturunkan, bukan
/// ditolak** — editor yang menolak jalan tidak bisa dipakai menyunting data —
/// tapi penurunannya disebutkan, tidak didiamkan.
TraceBackendSelection SelectTraceBackend(const TraceBackendCaps& caps,
                                         TraceBackendPreference preference);

/// Antarmuka penelusuran, sisi CPU.
///
/// **Ini acuan kebenaran, bukan jalur yang dipakai saat menggambar.**
/// Penelusuran sungguhnya terjadi di shader; yang di sini adalah implementasi
/// yang sama persis rumusnya, cukup lambat untuk dijalankan di test dan cukup
/// sederhana untuk dibaca. Kriteria selesai M2 menyebutnya langsung: backend
/// SDF dinyatakan lengkap kalau ia lulus uji ray tunggal terhadap referensi CPU.
class ITraceBackend {
public:
    virtual ~ITraceBackend() = default;
    virtual TraceBackendKind Kind() const = 0;
    const char* Name() const { return ToString(Kind()); }

    /// `direction` tidak harus ternormalisasi; implementasi yang
    /// menormalkannya sendiri membuat pemanggil tidak perlu mengingat aturan
    /// yang hanya berlaku di satu tempat.
    virtual TraceResult Trace(const Vec3& origin, const Vec3& direction, float tMax) const = 0;
};

/// Backend yang selalu meleset.
std::unique_ptr<ITraceBackend> CreateNullTraceBackend();

// --- Debug view --------------------------------------------------------------

/// Tampilan diagnostik GI.
///
/// Didaftarkan sejak M0 walaupun sebagian besarnya belum ada isinya. Alasannya
/// sama dengan alasan pemilih backend harus terlihat sejak awal: alat diagnostik
/// yang ditambahkan belakangan adalah alat yang tidak dipakai saat ia paling
/// dibutuhkan, yaitu ketika sistemnya masih salah dan belum jelas salahnya di
/// mana.
enum class GiDebugView : uint8_t {
    Off,
    Albedo,
    Normal,
    /// Iradiansi mentah, sebelum denoise.
    Irradiance,
    /// Banyaknya ray per piksel.
    RayCount,
    /// Heatmap jumlah langkah march. Yang paling sering dipakai.
    MarchSteps,
};

const char* ToString(GiDebugView view);

/// Pengaturan GI yang terlihat di UI.
struct GiSettings {
    bool enabled = false;
    TraceBackendPreference backend = TraceBackendPreference::Auto;
    GiDebugView debugView = GiDebugView::Off;
};

}  // namespace sim::render
