#pragma once

// Header internal: ia menyebut `nlohmann::json` di tanda tangannya, dan header
// publik AIBridge tidak boleh membocorkannya. Yang keluar dari modul ini adalah
// teks.

#include <nlohmann/json.hpp>

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace sim::ai {

/// Kode galat JSON-RPC 2.0. Angkanya ditetapkan spesifikasi, bukan dipilih.
enum class RpcError : int {
    Parse = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    Internal = -32603,
};

/// Cara sebuah handler menyatakan galat yang **bukan** kesalahan tool.
///
/// Argumen yang tidak lolos validasi adalah `InvalidParams` dan dilempar dari
/// sini; tool yang berjalan lalu gagal mengembalikan `ToolResult{isError}` dan
/// bukan lemparan. Bedanya penting bagi agen: yang satu berarti "panggilanmu
/// salah bentuk", yang lain berarti "panggilanmu benar, hasilnya gagal".
class RpcException : public std::runtime_error {
public:
    RpcException(RpcError code, std::string message, nlohmann::json data = nullptr)
        : std::runtime_error(std::move(message)), code_(code), data_(std::move(data)) {}

    RpcError Code() const noexcept { return code_; }
    const nlohmann::json& Data() const noexcept { return data_; }

private:
    RpcError code_;
    nlohmann::json data_;
};

/// Satu permintaan yang sudah lolos validasi bentuk.
struct RpcRequest {
    std::string method;
    /// Objek atau array; `null` bila permintaannya tidak membawa params.
    nlohmann::json params;
    /// Identitas yang harus dikembalikan di balasan. Tidak berarti apa-apa bila
    /// `isNotification`.
    nlohmann::json id;
    /// Permintaan tanpa `id`. **Tidak dibalas sama sekali** — bukan dibalas
    /// dengan hasil kosong. Klien MCP mengirim `notifications/initialized`
    /// sebagai notifikasi, dan balasan atasnya adalah pelanggaran protokol yang
    /// sebagian klien perlakukan sebagai galat koneksi.
    bool isNotification = false;
};

/// Yang menjalankan sebuah metode. Melempar `RpcException` untuk galat
/// protokol; lemparan lain menjadi `Internal`.
using RpcMethod = std::function<nlohmann::json(const RpcRequest&)>;

/// Menjalankan satu payload JSON-RPC — objek tunggal atau batch — dan
/// mengembalikan teks balasannya.
///
/// String kosong berarti **tidak ada yang perlu dikirim**: seluruh isinya
/// notifikasi. Pemanggil membalasnya dengan 202 tanpa body, bukan dengan objek
/// kosong.
std::string DispatchJsonRpc(std::string_view payload, const RpcMethod& method);

/// Objek galat JSON-RPC utuh, untuk pemanggil yang perlu membentuknya sendiri
/// (mis. menolak permintaan sebelum sempat di-parse).
std::string MakeRpcErrorResponse(RpcError code, std::string_view message);

}  // namespace sim::ai
