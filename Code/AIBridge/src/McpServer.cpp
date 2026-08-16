#include "Sim/AIBridge/McpServer.h"

#include "JsonRpc.h"

#include "Sim/Core/Log.h"
#include "Sim/Core/MainThreadQueue.h"

#include <httplib.h>

#include <array>
#include <atomic>
#include <fstream>
#include <future>
#include <string_view>
#include <thread>

namespace sim::ai {
namespace {

using nlohmann::json;

/// Versi protokol MCP yang benar-benar dipahami server ini, terbaru lebih dulu.
///
/// **Dicocokkan, bukan digemakan.** Menggemakan apa pun yang dikirim klien
/// berarti mengaku mendukung versi yang belum ada saat kode ini ditulis, dan
/// kegagalannya lalu muncul sebagai perilaku aneh di tengah sesi alih-alih
/// sebagai penolakan di awalnya. Yang tidak dikenal dijawab dengan versi terbaru
/// yang dikenal — klien lalu memutuskan sendiri apakah masih mau melanjutkan.
constexpr std::array<std::string_view, 3> kProtocolVersions{"2025-06-18", "2025-03-26",
                                                            "2024-11-05"};

std::string NegotiateProtocol(const json& params) {
    if (params.is_object() && params.contains("protocolVersion") &&
        params.at("protocolVersion").is_string()) {
        const std::string requested = params.at("protocolVersion").get<std::string>();
        for (const std::string_view known : kProtocolVersions) {
            if (requested == known) {
                return requested;
            }
        }
        SIM_INFO("AIBridge", "client asked for MCP {} which is unknown here; offering {}",
                 requested, kProtocolVersions.front());
    }
    return std::string(kProtocolVersions.front());
}

/// Skema sebagai JSON, atau objek kosong bila teksnya tidak sah.
///
/// Skema rusak tidak boleh menjatuhkan seluruh `tools/list`: yang hilang lalu
/// bukan satu tool melainkan semuanya, dan agen tidak punya cara menebak mana
/// yang salah.
json ParseSchema(const ToolDefinition& tool) {
    json schema = json::parse(tool.inputSchemaJson, nullptr, /*allow_exceptions=*/false);
    if (schema.is_discarded() || !schema.is_object()) {
        SIM_WARN("AIBridge", "tool {} has an invalid input schema; offering an empty one",
                 tool.name);
        return json{{"type", "object"}, {"properties", json::object()}};
    }
    return schema;
}

json DescribeTool(const ToolDefinition& tool) {
    return json{{"name", tool.name},
                {"description", tool.description},
                {"inputSchema", ParseSchema(tool)},
                // Bukan bagian spesifikasi, tapi terbaca agen dan terbaca
                // manusia yang mengaudit log: tingkat izin sebuah tool adalah
                // hal pertama yang ditanyakan saat sesuatu berubah tanpa
                // diminta.
                {"_meta", json{{"simengine/permission", ToString(tool.permission)}}}};
}

}  // namespace

struct McpServer::Impl {
    httplib::Server server;
    std::thread thread;
    const ToolRegistry* tools = nullptr;
    McpServerConfig config;
    std::atomic<uint16_t> port{0};
    std::atomic<uint64_t> requests{0};
    std::atomic<bool> running{false};

    /// Menjalankan handler sebuah tool, di thread yang benar.
    ToolResult Invoke(const ToolDefinition& tool, const std::string& argumentsJson) {
        if (!tool.needsMainThread) {
            return tool.handler(argumentsJson);
        }

        // Yang diantrikan menyalin argumennya. Thread jaringan bisa saja sudah
        // menyerah karena tenggat dan membuang buffer-nya sebelum main thread
        // sempat menjalankan ini.
        std::future<ToolResult> pending = MainThreadQueue::Get().Submit(
            [handler = tool.handler, argumentsJson]() { return handler(argumentsJson); });

        if (pending.wait_for(config.mainThreadTimeout) != std::future_status::ready) {
            throw RpcException(
                RpcError::Internal,
                "the editor did not answer within " +
                    std::to_string(config.mainThreadTimeout.count()) +
                    " ms; it is probably showing a modal dialog");
        }
        return pending.get();
    }

    json HandleMethod(const RpcRequest& request) {
        if (request.method == "initialize") {
            return json{
                {"protocolVersion", NegotiateProtocol(request.params)},
                {"capabilities", json{{"tools", json{{"listChanged", false}}}}},
                {"serverInfo",
                 json{{"name", config.serverName}, {"version", config.serverVersion}}}};
        }

        // Notifikasi siklus hidup. Balasannya tidak pernah dikirim — dispatcher
        // membuangnya — tapi metodenya tetap harus dikenali, kalau tidak ia
        // tercatat sebagai MethodNotFound di log dan terbaca sebagai masalah.
        if (request.method == "notifications/initialized" || request.method == "ping") {
            return json::object();
        }

        if (request.method == "tools/list") {
            json list = json::array();
            for (const ToolDefinition& tool : tools->All()) {
                list.push_back(DescribeTool(tool));
            }
            return json{{"tools", std::move(list)}};
        }

        if (request.method == "tools/call") {
            if (!request.params.is_object() || !request.params.contains("name") ||
                !request.params.at("name").is_string()) {
                throw RpcException(RpcError::InvalidParams, "\"name\" must be a string");
            }
            const std::string name = request.params.at("name").get<std::string>();
            const ToolDefinition* tool = tools->Find(name);
            if (tool == nullptr) {
                throw RpcException(RpcError::MethodNotFound, "no tool named \"" + name + "\"");
            }

            // Argumen boleh tidak ada: tool tanpa parameter dipanggil tanpa
            // `arguments`, dan memaksanya ada berarti menolak panggilan yang sah.
            std::string argumentsJson = "{}";
            if (request.params.contains("arguments")) {
                const json& arguments = request.params.at("arguments");
                if (!arguments.is_object()) {
                    throw RpcException(RpcError::InvalidParams, "\"arguments\" must be an object");
                }
                argumentsJson = arguments.dump();
            }

            const ToolResult result = Invoke(*tool, argumentsJson);
            return json{
                {"content", json::array({json{{"type", "text"}, {"text", result.text}}})},
                {"isError", result.isError}};
        }

        throw RpcException(RpcError::MethodNotFound, "no method named \"" + request.method + "\"");
    }

    void WriteAdvertiseFile() {
        if (config.advertisePath.empty()) {
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(config.advertisePath.parent_path(), ec);
        std::ofstream file(config.advertisePath, std::ios::trunc);
        if (!file) {
            SIM_WARN("AIBridge", "cannot write {}", config.advertisePath.string());
            return;
        }
        const json advertised{{"name", config.serverName},
                              {"port", port.load()},
                              {"url", "http://127.0.0.1:" + std::to_string(port.load()) + "/mcp"},
                              {"transport", "http"}};
        file << advertised.dump(2) << '\n';
    }

    void RemoveAdvertiseFile() {
        if (config.advertisePath.empty()) {
            return;
        }
        // Berkas basi yang menunjuk port mati lebih buruk daripada tidak ada
        // berkas: agen menyambung, gagal, dan menyalahkan servernya.
        std::error_code ec;
        std::filesystem::remove(config.advertisePath, ec);
    }
};

McpServer::McpServer() : impl_(std::make_unique<Impl>()) {}

McpServer::~McpServer() { Stop(); }

bool McpServer::Start(const ToolRegistry& tools, const McpServerConfig& config) {
    if (impl_->running.load()) {
        return true;
    }

    impl_->tools = &tools;
    impl_->config = config;

    // **SO_REUSEADDR saja, tanpa SO_REUSEPORT.** Bawaan httplib memasang
    // keduanya, dan SO_REUSEPORT membuat `bind_to_port` **berhasil** pada port
    // yang sudah dipegang proses lain — kernel lalu membagi koneksi masuk di
    // antara keduanya. Akibatnya bukan galat melainkan sesuatu yang jauh lebih
    // sulit dipercaya: dua editor terbuka, agen menyambung ke 7777, dan tiap
    // permintaan mendarat di salah satu editor secara acak. Pencarian port di
    // bawah ini juga jadi tidak pernah berjalan, karena percobaan pertama tidak
    // pernah gagal.
    //
    // SO_REUSEADDR tetap dipasang: tanpanya, editor yang baru ditutup
    // meninggalkan soket dalam TIME_WAIT dan yang dijalankan ulang beberapa
    // detik kemudian naik ke port berikutnya tanpa alasan yang terlihat.
    impl_->server.set_socket_options([](socket_t sock) {
        const int enable = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enable),
                   static_cast<socklen_t>(sizeof(enable)));
    });

    uint16_t bound = 0;
    const int last = static_cast<int>(config.preferredPort) +
                     static_cast<int>(config.portSearchRange);
    for (int candidate = config.preferredPort; candidate < last && candidate <= 65535;
         ++candidate) {
        if (impl_->server.bind_to_port("127.0.0.1", candidate)) {
            bound = static_cast<uint16_t>(candidate);
            break;
        }
        // **`stop()` di sini bukan salah tulis.** `bind_to_port` yang gagal
        // menandai objek `Server` sebagai *decommissioned*, dan sejak itu setiap
        // bind berikutnya mengembalikan -1 tanpa menyentuh soket sama sekali —
        // jadi pencarian port yang memakai satu objek untuk semua percobaan
        // selalu berhenti di percobaan pertama, apa pun isi rentangnya. Yang
        // mengembalikan tanda itu ke semula hanyalah `stop()`, yang di sini
        // tidak menghentikan apa pun karena memang belum ada yang berjalan.
        impl_->server.stop();
    }
    if (bound == 0) {
        SIM_ERROR("AIBridge", "no free port in {}..{}", config.preferredPort, last - 1);
        return false;
    }
    impl_->port.store(bound);

    impl_->server.Post("/mcp", [this](const httplib::Request& request,
                                      httplib::Response& response) {
        impl_->requests.fetch_add(1);
        const std::string reply = DispatchJsonRpc(
            request.body, [this](const RpcRequest& rpc) { return impl_->HandleMethod(rpc); });
        if (reply.empty()) {
            // Seluruh isinya notifikasi. 202 tanpa body adalah yang diminta
            // streamable HTTP; 200 dengan body kosong bukan JSON yang sah dan
            // sebagian klien memperlakukannya sebagai galat parse.
            response.status = 202;
            return;
        }
        response.set_content(reply, "application/json");
    });

    // Server ini tidak pernah mengirim apa pun atas inisiatifnya sendiri, jadi
    // tidak ada aliran SSE untuk dibuka. Spesifikasi menuntut 405 untuk itu —
    // bukan 404, yang berarti "endpoint-nya tidak ada" dan membuat klien
    // menyerah alih-alih melanjutkan tanpa aliran.
    impl_->server.Get("/mcp", [](const httplib::Request&, httplib::Response& response) {
        response.status = 405;
    });

    impl_->server.set_exception_handler(
        [](const httplib::Request&, httplib::Response& response, std::exception_ptr) {
            response.status = 500;
            response.set_content(MakeRpcErrorResponse(RpcError::Internal, "unhandled exception"),
                                 "application/json");
        });

    impl_->running.store(true);
    impl_->thread = std::thread([this]() { impl_->server.listen_after_bind(); });
    impl_->server.wait_until_ready();

    impl_->WriteAdvertiseFile();
    SIM_INFO("AIBridge", "MCP server listening on {} ({} tools)", Url(), tools.Count());
    return true;
}

void McpServer::Stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }
    impl_->server.stop();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    impl_->RemoveAdvertiseFile();
    impl_->port.store(0);
    SIM_INFO("AIBridge", "MCP server stopped");
}

bool McpServer::IsRunning() const { return impl_->running.load(); }

uint16_t McpServer::Port() const { return impl_->port.load(); }

std::string McpServer::Url() const {
    const uint16_t port = impl_->port.load();
    return port == 0 ? std::string() : "http://127.0.0.1:" + std::to_string(port) + "/mcp";
}

uint64_t McpServer::RequestCount() const { return impl_->requests.load(); }

}  // namespace sim::ai
