#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/AIBridge/McpServer.h"
#include "Sim/AIBridge/ToolRegistry.h"
#include "Sim/Core/MainThreadQueue.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

// Diuji lewat HTTP sungguhan, bukan dengan memanggil dispatcher langsung.
//
// **Alasannya sama dengan yang membuat uji tingkat-pass hilang di track GI:**
// rumus yang benar di balik parameter yang salah tetap lulus setiap uji yang
// hanya memanggil rumusnya. Yang dilakukan agen adalah mengirim byte ke sebuah
// soket, jadi itu yang diuji — termasuk kode status, tipe konten, dan jalur
// marshaling ke main thread.

using namespace sim;
using namespace sim::ai;
using nlohmann::json;

namespace {

/// Server yang menyala di port bebas mana pun, beserta klien yang menunjuknya.
struct Harness {
    ToolRegistry tools;
    McpServer server;
    McpServerConfig config;

    explicit Harness(std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
        // Port tinggi supaya tidak bentrok dengan editor yang mungkin sedang
        // jalan di 7777 saat uji ini dijalankan di mesin pengembang.
        config.preferredPort = 47800;
        config.portSearchRange = 64;
        config.mainThreadTimeout = timeout;
    }

    bool Start() { return server.Start(tools, config); }

    httplib::Client Client() const {
        httplib::Client client("127.0.0.1", server.Port());
        client.set_read_timeout(5, 0);
        return client;
    }

    /// Mengirim payload mentah supaya bentuk yang rusak pun bisa diuji.
    httplib::Result Post(const std::string& body) const {
        httplib::Client client = Client();
        return client.Post("/mcp", body, "application/json");
    }

    json Call(const std::string& method, const json& params = json::object(), int id = 1) const {
        json request{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
        if (!params.is_null()) {
            request["params"] = params;
        }
        const httplib::Result result = Post(request.dump());
        REQUIRE(result);
        REQUIRE(result->status == 200);
        return json::parse(result->body);
    }
};

ToolDefinition EchoTool() {
    ToolDefinition tool;
    tool.name = "test.echo";
    tool.description = "Returns its argument.";
    tool.inputSchemaJson = R"({"type":"object","properties":{"value":{"type":"string"}}})";
    tool.needsMainThread = false;
    tool.handler = [](std::string_view argumentsJson) {
        const json arguments = json::parse(argumentsJson);
        ToolResult result;
        result.text = arguments.value("value", std::string{});
        return result;
    };
    return tool;
}

}  // namespace

TEST_CASE("Server menyala di localhost dan mengumumkan port yang benar-benar dipakai") {
    Harness harness;
    REQUIRE(harness.Start());
    CHECK(harness.server.IsRunning());
    CHECK(harness.server.Port() >= harness.config.preferredPort);
    CHECK(harness.server.Url() ==
          "http://127.0.0.1:" + std::to_string(harness.server.Port()) + "/mcp");

    harness.server.Stop();
    CHECK_FALSE(harness.server.IsRunning());
    CHECK(harness.server.Port() == 0);

    // Berhenti dua kali bukan galat: `~McpServer` memanggilnya lagi sesudah
    // pemanggil yang tertib sudah melakukannya.
    harness.server.Stop();
}

TEST_CASE("Port yang sudah dipakai membuat server naik ke berikutnya") {
    Harness first;
    REQUIRE(first.Start());

    Harness second;
    second.config.preferredPort = first.server.Port();
    REQUIRE(second.Start());

    // **Naik, bukan menyerah.** Dua editor terbuka sekaligus adalah keadaan
    // biasa, dan yang kedua tetap harus bisa dikendalikan agen.
    CHECK(second.server.Port() > first.server.Port());
}

TEST_CASE("initialize menjawab dengan versi protokol yang dikenal") {
    Harness harness;
    REQUIRE(harness.Start());

    SUBCASE("versi yang dikenal digemakan") {
        const json response =
            harness.Call("initialize", json{{"protocolVersion", "2024-11-05"}});
        CHECK(response.at("result").at("protocolVersion") == "2024-11-05");
        CHECK(response.at("result").at("serverInfo").at("name") == "simengine");
        CHECK(response.at("result").at("capabilities").contains("tools"));
    }

    SUBCASE("versi yang tidak dikenal dijawab dengan yang terbaru dikenal") {
        // Menggemakan apa pun berarti mengaku mendukung versi yang belum ada
        // saat kode ini ditulis.
        const json response = harness.Call("initialize", json{{"protocolVersion", "1999-01-01"}});
        CHECK(response.at("result").at("protocolVersion") == "2025-06-18");
    }
}

TEST_CASE("tools/list membangkitkan daftarnya dari registry") {
    Harness harness;
    harness.tools.Register(EchoTool());
    REQUIRE(harness.Start());

    const json response = harness.Call("tools/list");
    const json& tools = response.at("result").at("tools");
    REQUIRE(tools.size() == 1);
    CHECK(tools[0].at("name") == "test.echo");
    CHECK(tools[0].at("inputSchema").at("type") == "object");
    CHECK(tools[0].at("_meta").at("simengine/permission") == "read");
}

TEST_CASE("tools/call menjalankan handler dan membungkus hasilnya") {
    Harness harness;
    harness.tools.Register(EchoTool());
    REQUIRE(harness.Start());

    SUBCASE("panggilan yang sah") {
        const json response = harness.Call(
            "tools/call", json{{"name", "test.echo"}, {"arguments", json{{"value", "halo"}}}});
        const json& result = response.at("result");
        CHECK(result.at("isError") == false);
        CHECK(result.at("content")[0].at("type") == "text");
        CHECK(result.at("content")[0].at("text") == "halo");
    }

    SUBCASE("tanpa arguments sama sekali") {
        // Tool tanpa parameter dipanggil tanpa `arguments`; memaksanya ada
        // berarti menolak panggilan yang sah.
        const json response = harness.Call("tools/call", json{{"name", "test.echo"}});
        CHECK(response.at("result").at("content")[0].at("text") == "");
    }

    SUBCASE("tool yang tidak dikenal adalah MethodNotFound, bukan hasil kosong") {
        const json response = harness.Call("tools/call", json{{"name", "test.tidak_ada"}});
        CHECK(response.at("error").at("code") == -32601);
    }

    SUBCASE("arguments yang bukan objek adalah InvalidParams") {
        const json response =
            harness.Call("tools/call", json{{"name", "test.echo"}, {"arguments", 42}});
        CHECK(response.at("error").at("code") == -32602);
    }
}

TEST_CASE("Galat JSON-RPC memakai kode yang ditetapkan spesifikasi") {
    Harness harness;
    REQUIRE(harness.Start());

    SUBCASE("JSON rusak") {
        const httplib::Result result = harness.Post("{ini bukan json");
        REQUIRE(result);
        CHECK(json::parse(result->body).at("error").at("code") == -32700);
    }

    SUBCASE("bukan objek") {
        const httplib::Result result = harness.Post("42");
        REQUIRE(result);
        CHECK(json::parse(result->body).at("error").at("code") == -32600);
    }

    SUBCASE("versi jsonrpc salah") {
        const httplib::Result result =
            harness.Post(R"({"jsonrpc":"1.0","id":1,"method":"ping"})");
        REQUIRE(result);
        CHECK(json::parse(result->body).at("error").at("code") == -32600);
    }

    SUBCASE("metode tidak dikenal") {
        const json response = harness.Call("tidak/ada");
        CHECK(response.at("error").at("code") == -32601);
    }

    SUBCASE("batch kosong") {
        const httplib::Result result = harness.Post("[]");
        REQUIRE(result);
        CHECK(json::parse(result->body).at("error").at("code") == -32600);
    }
}

TEST_CASE("Notifikasi tidak dibalas") {
    Harness harness;
    REQUIRE(harness.Start());

    // Klien MCP mengirim `notifications/initialized` sebagai notifikasi, dan
    // balasan atasnya adalah pelanggaran protokol yang sebagian klien
    // perlakukan sebagai galat koneksi.
    const httplib::Result result =
        harness.Post(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    REQUIRE(result);
    CHECK(result->status == 202);
    CHECK(result->body.empty());
}

TEST_CASE("Batch menjawab hanya yang punya id") {
    Harness harness;
    harness.tools.Register(EchoTool());
    REQUIRE(harness.Start());

    const json batch = json::array(
        {json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "ping"}},
         json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}},
         json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}});

    const httplib::Result result = harness.Post(batch.dump());
    REQUIRE(result);
    const json responses = json::parse(result->body);
    REQUIRE(responses.is_array());
    CHECK(responses.size() == 2);
    CHECK(responses[0].at("id") == 1);
    CHECK(responses[1].at("id") == 2);
}

TEST_CASE("GET /mcp adalah 405, bukan 404") {
    Harness harness;
    REQUIRE(harness.Start());

    // 404 berarti "endpoint-nya tidak ada" dan membuat klien menyerah;
    // 405 berarti "tidak ada aliran SSE di sini" dan ia melanjutkan tanpanya.
    httplib::Client client = harness.Client();
    const httplib::Result result = client.Get("/mcp");
    REQUIRE(result);
    CHECK(result->status == 405);
}

TEST_CASE("Tool yang menyentuh editor dijalankan di main thread") {
    MainThreadQueue::Get().BindMainThread();

    Harness harness;
    std::atomic<bool> ranOnMainThread{false};

    ToolDefinition tool;
    tool.name = "test.main_thread";
    tool.needsMainThread = true;
    tool.handler = [&ranOnMainThread](std::string_view) {
        ranOnMainThread.store(MainThreadQueue::Get().IsMainThread());
        ToolResult result;
        result.text = "selesai";
        return result;
    };
    harness.tools.Register(std::move(tool));
    REQUIRE(harness.Start());

    // Permintaannya dikirim dari thread lain supaya thread uji ini bebas
    // menjadi main thread yang men-drain antriannya — persis pembagian peran
    // di editor.
    json response;
    std::thread caller([&]() {
        response = harness.Call("tools/call", json{{"name", "test.main_thread"}});
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        MainThreadQueue::Get().Drain();
        if (!response.is_null()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    caller.join();
    MainThreadQueue::Get().Drain();

    CHECK(ranOnMainThread.load());
    REQUIRE(response.contains("result"));
    CHECK(response.at("result").at("content")[0].at("text") == "selesai");
}

TEST_CASE("Main thread yang tidak pernah men-drain menghasilkan tenggat, bukan gantung") {
    MainThreadQueue::Get().BindMainThread();

    Harness harness{std::chrono::milliseconds{150}};
    ToolDefinition tool;
    tool.name = "test.never_drained";
    tool.needsMainThread = true;
    tool.handler = [](std::string_view) { return ToolResult{}; };
    harness.tools.Register(std::move(tool));
    REQUIRE(harness.Start());

    // Tidak ada Drain() sama sekali di sini — inilah editor yang sedang
    // menampilkan dialog modal.
    const json response = harness.Call("tools/call", json{{"name", "test.never_drained"}});
    CHECK(response.at("error").at("code") == -32603);
    CHECK(std::string(response.at("error").at("message")).find("modal") != std::string::npos);

    // Pekerjaan yang terlanjur diantrikan dibuang supaya tidak bocor ke uji
    // berikutnya.
    MainThreadQueue::Get().Drain();
}
