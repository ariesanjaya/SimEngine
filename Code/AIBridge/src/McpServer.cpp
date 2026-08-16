#include "Sim/AIBridge/McpServer.h"

#include "JsonRpc.h"

#include "Sim/Core/Log.h"
#include "Sim/Core/MainThreadQueue.h"

#include <httplib.h>

#include <array>
#include <atomic>
#include <fstream>
#include <future>
#include <random>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
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

/// Token acak 32 heksadesimal — 128 bit dari `random_device`.
///
/// Bukan `mt19937` yang di-seed waktu: benih seperti itu bisa ditebak oleh
/// proses lain di mesin yang sama, dan proses lain di mesin yang sama adalah
/// tepat yang hendak dihalangi token ini.
/// Base64 standar (RFC 4648), dengan padding.
///
/// Ditulis di sini alih-alih dipinjam dari httplib: milik httplib tidak
/// diekspor, dan menariknya keluar berarti bergantung pada bagian dalam sebuah
/// pustaka yang kebetulan terlihat.
std::string Base64(const std::vector<uint8_t>& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t triple = (uint32_t(bytes[i]) << 16) | (uint32_t(bytes[i + 1]) << 8) |
                                uint32_t(bytes[i + 2]);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
    }
    if (i < bytes.size()) {
        const bool two = i + 1 < bytes.size();
        const uint32_t triple =
            (uint32_t(bytes[i]) << 16) | (two ? uint32_t(bytes[i + 1]) << 8 : 0u);
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(two ? kAlphabet[(triple >> 6) & 0x3F] : '=');
        out.push_back('=');
    }
    return out;
}

/// PID proses ini, sebagai angka biasa.
int64_t ProcessId() {
#if defined(_WIN32)
    return static_cast<int64_t>(_getpid());
#else
    return static_cast<int64_t>(getpid());
#endif
}

std::string GenerateToken() {
    std::random_device source;
    std::uniform_int_distribution<int> nibble(0, 15);
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string token;
    token.reserve(32);
    for (int i = 0; i < 32; ++i) {
        token.push_back(kDigits[nibble(source)]);
    }
    return token;
}

}  // namespace

struct McpServer::Impl {
    httplib::Server server;
    std::thread thread;
    const ToolRegistry* tools = nullptr;
    const ResourceRegistry* resources = nullptr;
    McpServerConfig config;
    std::atomic<uint16_t> port{0};
    std::atomic<uint64_t> requests{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<uint64_t> refusedByPolicy{0};
    std::atomic<bool> running{false};
    /// Dibaca dari thread jaringan, ditulis dari thread UI.
    std::atomic<PermissionMode> mode{PermissionMode::Auto};
    /// Dipasang sebelum `Start`. Tidak atomik dengan sengaja: mengganti
    /// penyetuju saat server melayani permintaan adalah perlombaan yang tidak
    /// punya arti yang benar, jadi ia tidak dijanjikan aman.
    ToolApprover approver;

    /// Apakah sebuah tool boleh dijalankan sekarang. Mengisi `refusal` bila tidak.
    ///
    /// Aturannya satu tabel, di satu tempat:
    ///
    ///   mode        Read   Write        Dangerous
    ///   read-only   ya     tidak        tidak
    ///   ask         ya     penyetuju    penyetuju
    ///   auto        ya     ya           penyetuju bila ada, selain itu ya
    ///
    /// `Read` tidak pernah ditanyakan: tool yang hanya membaca tidak mengubah
    /// apa pun, dan meminta persetujuan untuk melihat akan melatih pengguna
    /// mengklik "ya" tanpa membacanya.
    bool Allows(const ToolDefinition& tool, std::string_view arguments, std::string& refusal) {
        if (tool.permission == ToolPermission::Read) {
            return true;
        }
        const PermissionMode current = mode.load();
        if (current == PermissionMode::ReadOnly) {
            refusal = std::string("Refused: the editor is in read-only mode, and \"") +
                      tool.name + "\" is a " + ToString(tool.permission) +
                      " tool. Ask the person at the editor to change the mode in the AI "
                      "Bridge panel.";
            return false;
        }
        if (current == PermissionMode::Auto && tool.permission == ToolPermission::Write) {
            return true;
        }
        if (!approver) {
            if (current == PermissionMode::Auto) {
                // Dangerous di mode auto tanpa penyetuju: tidak ada yang
                // menunggu jawaban, dan menolak akan membuat mode ini tidak
                // berguna justru pada mesin yang memang tidak punya UI.
                return true;
            }
            refusal = std::string("Refused: the editor is in ask mode but has nobody to ask. "
                                  "\"") +
                      tool.name + "\" was not run.";
            return false;
        }
        if (approver(tool, arguments)) {
            return true;
        }
        refusal = std::string("Refused: the person at the editor did not approve \"") +
                  tool.name + "\".";
        return false;
    }

    /// Menjalankan sesuatu di thread yang benar, dengan tenggat.
    ///
    /// Satu bentuk untuk tool dan resource: keduanya punya alasan yang sama
    /// untuk diantrikan, dan dua salinan aturan tenggat adalah dua aturan yang
    /// akan berselisih.
    template <class Work>
    auto OnCorrectThread(bool needsMainThread, Work&& work) -> decltype(work()) {
        if (!needsMainThread) {
            return work();
        }

        // Yang diantrikan menyalin apa yang dibutuhkannya. Thread jaringan bisa
        // saja sudah menyerah karena tenggat dan membuang buffer-nya sebelum
        // main thread sempat menjalankan ini.
        auto pending = MainThreadQueue::Get().Submit(std::forward<Work>(work));

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
            // **`listChanged` false, dan itu jujur.** Mengumumkannya true berarti
            // berjanji mengirim `notifications/tools/list_changed` saat daftarnya
            // berubah — dan server ini tidak punya aliran SSE untuk mengirim apa
            // pun atas inisiatifnya sendiri. Klien yang percaya janji itu akan
            // memakai daftar basi selamanya alih-alih menanyakannya lagi.
            json capabilities{{"tools", json{{"listChanged", false}}}};
            // Kapabilitas resource hanya diumumkan kalau memang ada isinya.
            // Klien memakai pengumuman ini untuk memutuskan apakah akan memanggil
            // `resources/list` sama sekali.
            if (resources != nullptr && resources->Count() > 0) {
                capabilities["resources"] = json{{"listChanged", false}, {"subscribe", false}};
            }
            // `prompts` sengaja tidak diumumkan: metodenya ada supaya klien yang
            // memanggilnya tetap mendapat jawaban yang sah, tapi belum ada satu
            // pun prompt. Yang direncanakan PLAN-AI — sim:build-level dan
            // seterusnya — menggambarkan alur di atas tool yang belum ada.
            return json{
                {"protocolVersion", NegotiateProtocol(request.params)},
                {"capabilities", std::move(capabilities)},
                {"serverInfo",
                 json{{"name", config.serverName}, {"version", config.serverVersion}}}};
        }

        if (request.method == "resources/list") {
            json list = json::array();
            if (resources != nullptr) {
                for (const ResourceDefinition& resource : resources->All()) {
                    list.push_back(json{{"uri", resource.uri},
                                        {"name", resource.name},
                                        {"description", resource.description},
                                        {"mimeType", resource.mimeType}});
                }
            }
            return json{{"resources", std::move(list)}};
        }

        if (request.method == "resources/read") {
            if (!request.params.is_object() || !request.params.contains("uri") ||
                !request.params.at("uri").is_string()) {
                throw RpcException(RpcError::InvalidParams, "\"uri\" must be a string");
            }
            const std::string uri = request.params.at("uri").get<std::string>();
            const ResourceDefinition* resource =
                resources == nullptr ? nullptr : resources->Find(uri);
            if (resource == nullptr) {
                throw RpcException(RpcError::InvalidParams, "no resource at \"" + uri + "\"");
            }
            std::string text = OnCorrectThread(resource->needsMainThread,
                                               [read = resource->read]() { return read(); });
            return json{{"contents", json::array({json{{"uri", resource->uri},
                                                       {"mimeType", resource->mimeType},
                                                       {"text", std::move(text)}}})}};
        }

        if (request.method == "prompts/list") {
            return json{{"prompts", json::array()}};
        }

        // Notifikasi siklus hidup. Balasannya tidak pernah dikirim — dispatcher
        // membuangnya — tapi metodenya tetap harus dikenali, kalau tidak ia
        // tercatat sebagai MethodNotFound di log dan terbaca sebagai masalah.
        if (request.method == "notifications/initialized" || request.method == "ping") {
            return json::object();
        }

        if (request.method == "tools/list") {
            // Mode read-only menyembunyikan tool yang tidak akan dijalankannya.
            // Agen yang tidak melihatnya tidak membuang panggilan mencobanya —
            // dan daftar yang menawarkan sesuatu yang selalu ditolak adalah
            // daftar yang berbohong.
            const bool readOnly = mode.load() == PermissionMode::ReadOnly;
            json list = json::array();
            for (const ToolDefinition& tool : tools->All()) {
                if (readOnly && tool.permission != ToolPermission::Read) {
                    continue;
                }
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

            // **Diperiksa lagi di sini, bukan hanya disaring dari daftar.**
            // Klien yang memegang daftar lama, atau yang menebak nama tool,
            // tidak boleh lolos hanya karena ia tidak membaca daftar terbaru.
            std::string refusal;
            if (!Allows(*tool, argumentsJson, refusal)) {
                refusedByPolicy.fetch_add(1);
                SIM_WARN("AIBridge", "refused {} in {} mode", tool->name,
                         ToString(mode.load()));
                return json{{"content", json::array({json{{"type", "text"}, {"text", refusal}}})},
                            {"isError", true}};
            }

            const ToolResult result = OnCorrectThread(
                tool->needsMainThread,
                [handler = tool->handler, argumentsJson]() { return handler(argumentsJson); });

            json content = json::array();
            // Teks lebih dulu bila ada: ia yang menjelaskan gambarnya, dan
            // sebagian klien hanya menampilkan blok pertama.
            if (!result.text.empty() || result.imageBytes.empty()) {
                content.push_back(json{{"type", "text"}, {"text", result.text}});
            }
            if (!result.imageBytes.empty()) {
                content.push_back(json{{"type", "image"},
                                       {"data", Base64(result.imageBytes)},
                                       {"mimeType", result.imageMimeType}});
            }
            return json{{"content", std::move(content)}, {"isError", result.isError}};
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
        // **Token sengaja tidak ikut.** Berkas ini ada supaya agen menemukan
        // port-nya, dan ia bisa dibaca proses lain milik pengguna yang sama —
        // yaitu tepat yang hendak dihalangi token. Menuliskannya di sini
        // membatalkan seluruh gunanya.
        //
        // **`pid` ikut, dan itu yang menutup berkas basi.** Editor yang mati
        // paksa melewati `Stop()` dan meninggalkan berkas ini menunjuk port yang
        // sudah tidak ada — agen lalu menyambung, gagal, dan menyalahkan
        // servernya. Yang membedakan berkas hidup dari berkas basi tidak bisa
        // ditulis oleh yang sudah mati; yang bisa hanyalah menyebutkan siapa
        // pemiliknya, dan membiarkan pembaca memeriksa apakah ia masih ada.
        const json advertised{{"name", config.serverName},
                              {"auth", config.bearerToken.empty() ? "none" : "bearer"},
                              {"pid", ProcessId()},
                              {"port", port.load()},
                              {"url", "http://127.0.0.1:" + std::to_string(port.load()) + "/mcp"},
                              {"transport", "http"}};
        file << advertised.dump(2) << '\n';
    }

    /// Membuang berkas pengumuman, **tapi hanya bila ia masih menyebut kita**.
    ///
    /// Dua editor terbuka berarti yang kedua menimpa berkas ini dengan portnya
    /// sendiri. Menghapusnya tanpa memeriksa membuat editor kedua yang ditutup
    /// mencabut pengumuman milik editor pertama — yang masih berjalan, dan
    /// sejak itu tidak bisa ditemukan agen mana pun. Yang menghapus hanyalah
    /// pemilik yang tercatat.
    void RemoveAdvertiseFile() {
        if (config.advertisePath.empty()) {
            return;
        }
        std::ifstream file(config.advertisePath);
        if (file) {
            const json existing = json::parse(file, nullptr, /*allow_exceptions=*/false);
            if (existing.is_object() && existing.value("pid", int64_t{0}) != ProcessId()) {
                return;
            }
        }
        std::error_code ec;
        std::filesystem::remove(config.advertisePath, ec);
    }
};

McpServer::McpServer() : impl_(std::make_unique<Impl>()) {}

McpServer::~McpServer() { Stop(); }

bool McpServer::Start(const ToolRegistry& tools, const McpServerConfig& config) {
    // Registry kosong yang hidup selama program: server tanpa resource tetap
    // menjawab `resources/list` dengan daftar kosong, bukan dengan galat.
    static const ResourceRegistry kNoResources;
    return Start(tools, kNoResources, config);
}

bool McpServer::Start(const ToolRegistry& tools, const ResourceRegistry& resources,
                      const McpServerConfig& config) {
    if (impl_->running.load()) {
        return true;
    }

    impl_->tools = &tools;
    impl_->resources = &resources;
    impl_->config = config;
    impl_->mode.store(config.permissionMode);
    if (impl_->config.bearerToken.empty() && impl_->config.generateBearerToken) {
        impl_->config.bearerToken = GenerateToken();
    }

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
        // **401, bukan galat JSON-RPC.** Yang gagal adalah transport, bukan
        // metodenya — dan klien MCP tahu cara menangani 401 (meminta kredensial)
        // sedangkan galat JSON-RPC di dalam body 200 hanya diteruskan ke model
        // sebagai teks yang membingungkan.
        if (!impl_->config.bearerToken.empty()) {
            const std::string expected = "Bearer " + impl_->config.bearerToken;
            if (request.get_header_value("Authorization") != expected) {
                impl_->rejected.fetch_add(1);
                response.status = 401;
                response.set_header("WWW-Authenticate", "Bearer");
                return;
            }
        }
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

uint64_t McpServer::RejectedCount() const { return impl_->rejected.load(); }

std::string McpServer::BearerToken() const { return impl_->config.bearerToken; }

PermissionMode McpServer::Mode() const { return impl_->mode.load(); }

void McpServer::SetMode(PermissionMode mode) { impl_->mode.store(mode); }

void McpServer::SetApprover(ToolApprover approver) { impl_->approver = std::move(approver); }

uint64_t McpServer::RefusedByPolicyCount() const { return impl_->refusedByPolicy.load(); }

}  // namespace sim::ai
