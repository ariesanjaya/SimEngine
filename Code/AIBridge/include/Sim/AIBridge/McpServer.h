#pragma once

#include "Sim/AIBridge/ToolRegistry.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace sim::ai {

struct McpServerConfig {
    /// Port yang dicoba lebih dulu.
    uint16_t preferredPort = 7777;
    /// Berapa port berikutnya yang boleh dicoba bila yang diminta sudah dipakai.
    ///
    /// **Naik, bukan menyerah.** Dua editor terbuka sekaligus adalah keadaan
    /// biasa, bukan kesalahan — dan yang kedua tetap harus bisa dikendalikan
    /// agen. Yang membuatnya bisa ditemukan adalah `advertisePath`.
    uint16_t portSearchRange = 16;

    /// Berkas tempat port yang **benar-benar** dipakai dituliskan, supaya agen
    /// tidak perlu menebak. Kosong berarti tidak ditulis.
    std::filesystem::path advertisePath;

    /// Berapa lama thread jaringan menunggu main thread sebelum menyerah.
    ///
    /// **Ada supaya agen tidak menggantung selamanya saat editor sedang modal.**
    /// Dialog "simpan perubahan?" menghentikan `Drain()` tanpa batas waktu, dan
    /// tanpa tenggat ini setiap tool call yang datang saat itu menahan satu
    /// koneksi sampai seseorang mengklik.
    std::chrono::milliseconds mainThreadTimeout{5000};

    std::string serverName = "simengine";
    std::string serverVersion = "0.1.0";
};

/// Server MCP di atas HTTP, terkunci ke localhost.
///
/// **Bind-nya 127.0.0.1 dan tidak ada opsi mengubahnya.** Server ini memberi
/// siapa pun yang menyambung kendali penuh atas project yang sedang dibuka;
/// sebuah opsi `bind_address` adalah opsi untuk membukanya ke jaringan, dan
/// tidak ada alasan yang cukup baik untuk menyediakannya.
///
/// Seluruh permintaan datang di thread jaringan milik httplib. Handler yang
/// ditandai `needsMainThread` diantrikan lewat `MainThreadQueue` dan ditunggu
/// dengan tenggat; yang tidak ditandai berjalan langsung di thread itu.
class McpServer {
public:
    McpServer();
    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    /// `tools` harus hidup lebih lama daripada server. Ia dipegang sebagai
    /// referensi, bukan disalin: tool boleh didaftarkan sesudah server menyala,
    /// dan salinan akan membekukan daftarnya pada saat start.
    bool Start(const ToolRegistry& tools, const McpServerConfig& config);

    /// Menghentikan server dan menunggu thread jaringannya selesai. Aman
    /// dipanggil berkali-kali, dan aman dipanggil saat ada permintaan yang
    /// sedang ditangani — itu justru keadaan yang diuji kriteria terima A0
    /// nomor 4.
    void Stop();

    bool IsRunning() const;
    /// Nol bila belum menyala.
    uint16_t Port() const;
    /// URL lengkap endpoint MCP, untuk ditampilkan panel dan disalin pengguna.
    std::string Url() const;

    /// Berapa permintaan yang sudah dilayani. Untuk panel dan uji.
    uint64_t RequestCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sim::ai
