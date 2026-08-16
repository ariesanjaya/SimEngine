#pragma once

#include "Sim/AIBridge/ResourceRegistry.h"
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
    ///
    /// Isinya menyebut `pid` pemiliknya. Editor yang mati paksa melewati
    /// `Stop()` dan meninggalkan berkas ini menunjuk port yang sudah tidak ada;
    /// yang membedakan berkas hidup dari berkas basi tidak bisa ditulis oleh
    /// yang sudah mati, jadi yang ditulis adalah siapa pemiliknya — dan pembaca
    /// memeriksa apakah proses itu masih ada sebelum mempercayai portnya.
    ///
    /// Yang menghapusnya saat berhenti hanyalah pemilik yang tercatat. Dua
    /// editor terbuka berarti yang kedua menimpa berkas ini; menghapusnya tanpa
    /// memeriksa akan mencabut pengumuman milik editor pertama yang masih
    /// berjalan.
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

    /// Token yang harus dibawa setiap permintaan di header `Authorization`.
    /// Kosong berarti tanpa autentikasi.
    ///
    /// **Yang dilindunginya bukan jaringan melainkan mesin ini sendiri.** Server
    /// sudah terkunci ke 127.0.0.1, jadi tidak ada yang bisa menyambung dari
    /// luar. Yang tersisa adalah proses lain milik pengguna yang sama — sebuah
    /// tab peramban yang menjalankan skrip pihak ketiga bisa memanggil localhost
    /// — dan itulah yang ditutup token ini.
    std::string bearerToken;

    /// Membangkitkan `bearerToken` acak saat start bila ia masih kosong.
    ///
    /// Bawaannya mati: token membuat `claude mcp add` menuntut `--header`, dan
    /// menyalakannya diam-diam berarti setiap orang yang mengikuti PLAN-AI.md
    /// apa adanya mendapat 401 tanpa tahu sebabnya. Yang menyalakannya adalah
    /// pengguna, lewat panel.
    bool generateBearerToken = false;
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

    /// Bentuk lengkapnya. `resources` juga dipegang sebagai referensi dan harus
    /// hidup lebih lama daripada server.
    bool Start(const ToolRegistry& tools, const ResourceRegistry& resources,
               const McpServerConfig& config);

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
    /// Berapa yang ditolak karena token. Nol yang terus naik adalah tanda ada
    /// sesuatu di mesin ini yang mencoba menyambung tanpa diundang.
    uint64_t RejectedCount() const;

    /// Token yang sedang berlaku, kosong bila tanpa autentikasi. Panel
    /// menampilkannya supaya pengguna bisa menyalinnya.
    std::string BearerToken() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sim::ai
