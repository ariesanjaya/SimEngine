#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/AIBridge/McpServer.h"
#include "Sim/AIBridge/ResourceRegistry.h"
#include "Sim/AIBridge/ToolRegistry.h"
#include "Sim/Core/MainThreadQueue.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <unistd.h>
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
    ResourceRegistry resources;
    McpServer server;
    McpServerConfig config;

    explicit Harness(std::chrono::milliseconds timeout = std::chrono::milliseconds{2000}) {
        // Port tinggi supaya tidak bentrok dengan editor yang mungkin sedang
        // jalan di 7777 saat uji ini dijalankan di mesin pengembang.
        config.preferredPort = 47800;
        config.portSearchRange = 64;
        config.mainThreadTimeout = timeout;
    }

    bool Start() { return server.Start(tools, resources, config); }

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
    // **Yang diperiksa loop tunggu adalah flag, bukan `response` itu sendiri.**
    // Membaca `response` di sini sementara thread pemanggil menulisinya adalah
    // data race — ThreadSanitizer menemukannya persis di bentuk sebelumnya, dan
    // ia benar: `json::is_null()` membaca tag tipe yang sedang ditukar
    // `operator=` di thread lain. Yang membuat versi ini benar bukan atomiknya
    // saja melainkan bahwa `response` tidak disentuh sama sekali sampai
    // `join()`.
    std::atomic<bool> finished{false};
    std::thread caller([&]() {
        response = harness.Call("tools/call", json{{"name", "test.main_thread"}});
        finished.store(true, std::memory_order_release);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!finished.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        MainThreadQueue::Get().Drain();
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

// --- resources & prompts -----------------------------------------------------

namespace {

ResourceDefinition NoteResource() {
    ResourceDefinition resource;
    resource.uri = "simengine://test/note";
    resource.name = "Test note";
    resource.description = "A fixed string, so the test asserts transport, not content.";
    resource.mimeType = "text/plain";
    resource.needsMainThread = false;
    resource.read = []() { return std::string("catatan"); };
    return resource;
}

}  // namespace

TEST_CASE("Kapabilitas resource hanya diumumkan kalau ada isinya") {
    SUBCASE("tanpa resource") {
        Harness harness;
        REQUIRE(harness.Start());
        const json response = harness.Call("initialize");
        // Mengumumkan kapabilitas yang kosong membuat klien memanggil
        // `resources/list` untuk mendapati tidak ada apa-apa — satu perjalanan
        // pulang-pergi untuk sebuah jawaban yang sudah diketahui.
        CHECK_FALSE(response.at("result").at("capabilities").contains("resources"));
    }

    SUBCASE("dengan resource") {
        Harness harness;
        harness.resources.Register(NoteResource());
        REQUIRE(harness.Start());
        const json response = harness.Call("initialize");
        CHECK(response.at("result").at("capabilities").contains("resources"));
        // Tidak pernah true: tidak ada aliran SSE untuk mengirim notifikasi.
        CHECK(response.at("result").at("capabilities").at("resources").at("listChanged") == false);
    }
}

TEST_CASE("resources/list dan resources/read") {
    Harness harness;
    harness.resources.Register(NoteResource());
    REQUIRE(harness.Start());

    SUBCASE("daftar") {
        const json response = harness.Call("resources/list");
        const json& list = response.at("result").at("resources");
        REQUIRE(list.size() == 1);
        CHECK(list[0].at("uri") == "simengine://test/note");
        CHECK(list[0].at("mimeType") == "text/plain");
    }

    SUBCASE("baca") {
        const json response =
            harness.Call("resources/read", json{{"uri", "simengine://test/note"}});
        const json& contents = response.at("result").at("contents");
        REQUIRE(contents.size() == 1);
        CHECK(contents[0].at("uri") == "simengine://test/note");
        CHECK(contents[0].at("text") == "catatan");
    }

    SUBCASE("uri yang tidak ada") {
        const json response =
            harness.Call("resources/read", json{{"uri", "simengine://test/hantu"}});
        CHECK(response.at("error").at("code") == -32602);
    }

    SUBCASE("uri yang bukan string") {
        const json response = harness.Call("resources/read", json{{"uri", 7}});
        CHECK(response.at("error").at("code") == -32602);
    }
}

TEST_CASE("prompts/list menjawab daftar kosong, bukan MethodNotFound") {
    Harness harness;
    REQUIRE(harness.Start());
    // Metodenya ada supaya klien yang memanggilnya tetap mendapat jawaban yang
    // sah; kapabilitasnya sengaja tidak diumumkan karena belum ada isinya.
    const json response = harness.Call("prompts/list");
    CHECK(response.at("result").at("prompts").is_array());
    CHECK(response.at("result").at("prompts").empty());
    CHECK_FALSE(harness.Call("initialize").at("result").at("capabilities").contains("prompts"));
}

// --- token bearer ------------------------------------------------------------

TEST_CASE("Token bearer menolak dengan 401, bukan dengan galat JSON-RPC") {
    Harness harness;
    harness.config.bearerToken = "rahasia";
    harness.tools.Register(EchoTool());
    REQUIRE(harness.Start());

    const std::string body = R"({"jsonrpc":"2.0","id":1,"method":"ping"})";
    httplib::Client client = harness.Client();

    // Penghitung diperiksa di dalam tiap subcase, bukan sesudahnya: doctest
    // menjalankan ulang badan test untuk setiap subcase, jadi apa pun yang
    // ditulis di bawah sini melihat server yang baru saja dibuat.
    SUBCASE("tanpa header sama sekali") {
        const httplib::Result result = client.Post("/mcp", body, "application/json");
        REQUIRE(result);
        // Yang gagal adalah transport, bukan metodenya — dan klien MCP tahu cara
        // menangani 401 sedangkan galat JSON-RPC di dalam 200 hanya diteruskan
        // ke model sebagai teks yang membingungkan.
        CHECK(result->status == 401);
        CHECK(result->get_header_value("WWW-Authenticate") == "Bearer");
        // Yang ditolak dihitung terpisah dari yang dilayani: angka yang terus
        // naik adalah tanda ada sesuatu di mesin ini yang mencoba menyambung
        // tanpa diundang.
        CHECK(harness.server.RejectedCount() == 1);
        CHECK(harness.server.RequestCount() == 0);
    }

    SUBCASE("token salah") {
        const httplib::Result result = client.Post(
            "/mcp", {{"Authorization", "Bearer salah"}}, body, "application/json");
        REQUIRE(result);
        CHECK(result->status == 401);
        CHECK(harness.server.RejectedCount() == 1);
    }

    SUBCASE("token benar") {
        const httplib::Result result = client.Post(
            "/mcp", {{"Authorization", "Bearer rahasia"}}, body, "application/json");
        REQUIRE(result);
        CHECK(result->status == 200);
        CHECK(json::parse(result->body).contains("result"));
        CHECK(harness.server.RejectedCount() == 0);
        CHECK(harness.server.RequestCount() == 1);
    }
}

TEST_CASE("Token dibangkitkan saat diminta, dan tidak ada saat tidak diminta") {
    SUBCASE("bawaan tanpa token") {
        Harness harness;
        REQUIRE(harness.Start());
        CHECK(harness.server.BearerToken().empty());
    }

    SUBCASE("dibangkitkan") {
        Harness harness;
        harness.config.generateBearerToken = true;
        REQUIRE(harness.Start());
        // 128 bit sebagai heksadesimal.
        CHECK(harness.server.BearerToken().size() == 32);

        Harness other;
        other.config.generateBearerToken = true;
        other.config.preferredPort = 47900;
        REQUIRE(other.Start());
        CHECK(other.server.BearerToken() != harness.server.BearerToken());
    }
}

// --- kriteria terima A0 -------------------------------------------------------

TEST_CASE("Dua ratus panggilan beruntun lewat main thread, semuanya benar") {
    // Kriteria terima A0 nomor 3. Yang diawasi bukan kecepatan melainkan
    // ketiadaan lomba: setiap panggilan menyeberang dari thread jaringan ke main
    // thread dan kembali, dan dua ratus penyeberangan berturut-turut adalah
    // bentuk paling sederhana yang membuat TSan punya sesuatu untuk dilihat.
    MainThreadQueue::Get().BindMainThread();

    Harness harness;
    std::atomic<int> ranOnMainThread{0};
    std::atomic<int> ranElsewhere{0};

    ToolDefinition tool;
    tool.name = "test.counter";
    tool.needsMainThread = true;
    tool.handler = [&](std::string_view) {
        if (MainThreadQueue::Get().IsMainThread()) {
            ranOnMainThread.fetch_add(1);
        } else {
            ranElsewhere.fetch_add(1);
        }
        ToolResult result;
        result.text = "ok";
        return result;
    };
    harness.tools.Register(std::move(tool));
    REQUIRE(harness.Start());

    constexpr int kCalls = 200;
    std::atomic<int> succeeded{0};
    std::atomic<bool> done{false};
    std::thread caller([&]() {
        // **Satu klien untuk seluruh dua ratus panggilan, dengan keep-alive.**
        // Bukan penghematan: klien baru per panggilan berarti dua ratus kali
        // `getaddrinfo`, dan glibc melayaninya lewat thread bantu yang ia buat
        // sendiri. ThreadSanitizer tidak mengenal thread itu — cache alokator
        // per-thread miliknya belum ada di sana — dan yang terjadi adalah
        // segfault di dalam sanitizer, bukan laporan race. Agen sungguhan juga
        // memakai satu koneksi, jadi bentuk ini sekaligus lebih jujur.
        httplib::Client client("127.0.0.1", harness.server.Port());
        client.set_keep_alive(true);
        client.set_read_timeout(10, 0);
        for (int i = 0; i < kCalls; ++i) {
            const json request{{"jsonrpc", "2.0"},
                               {"id", i + 1},
                               {"method", "tools/call"},
                               {"params", json{{"name", "test.counter"}}}};
            const httplib::Result result =
                client.Post("/mcp", request.dump(), "application/json");
            if (!result || result->status != 200) {
                continue;
            }
            const json response = json::parse(result->body, nullptr, false);
            if (!response.is_discarded() && response.contains("result") &&
                response.at("result").at("content")[0].at("text") == "ok") {
                succeeded.fetch_add(1);
            }
        }
        done.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
        MainThreadQueue::Get().Drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    caller.join();
    MainThreadQueue::Get().Drain();

    CHECK(succeeded.load() == kCalls);
    CHECK(ranOnMainThread.load() == kCalls);
    // Satu saja yang berjalan di thread jaringan berarti marshaling-nya bocor,
    // dan kebocoran seperti itu muncul sebagai crash acak jauh dari sebabnya.
    CHECK(ranElsewhere.load() == 0);
    CHECK(harness.server.RequestCount() == kCalls);
}

TEST_CASE("Menghentikan server saat ada permintaan menggantung tidak merusak apa pun") {
    // Kriteria terima A0 nomor 4, dalam bentuk yang deterministik: main thread
    // yang tidak pernah men-drain adalah editor yang sedang menampilkan dialog
    // modal, dan `Stop()` di tengahnya adalah pengguna yang menutup editor.
    MainThreadQueue::Get().BindMainThread();

    Harness harness{std::chrono::milliseconds{300}};
    ToolDefinition tool;
    tool.name = "test.hangs";
    tool.needsMainThread = true;
    tool.handler = [](std::string_view) { return ToolResult{}; };
    harness.tools.Register(std::move(tool));
    REQUIRE(harness.Start());

    std::atomic<bool> answered{false};
    std::thread caller([&]() {
        const httplib::Result result = harness.Post(
            R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"test.hangs"}})");
        // Jawabannya boleh apa saja — galat tenggat, atau koneksi yang terputus
        // karena servernya berhenti. Yang tidak boleh adalah crash.
        (void)result;
        answered.store(true);
    });

    // Cukup lama untuk memastikan permintaannya sudah masuk dan sedang menunggu
    // main thread, tapi lebih pendek dari tenggatnya.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    harness.server.Stop();
    caller.join();

    CHECK(answered.load());
    CHECK_FALSE(harness.server.IsRunning());
    // Pekerjaan yang terlanjur diantrikan dibuang supaya tidak bocor ke uji
    // berikutnya.
    MainThreadQueue::Get().Drain();
}

// --- berkas pengumuman --------------------------------------------------------

namespace {

/// Folder sementara yang membersihkan dirinya sendiri.
struct TempDir {
    std::filesystem::path path;

    TempDir() {
        path = std::filesystem::temp_directory_path() /
               ("sim-mcp-" + std::to_string(::getpid()) + "-" +
                std::to_string(reinterpret_cast<uintptr_t>(this)));
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

}  // namespace

TEST_CASE("Berkas pengumuman menyebut pemiliknya, dan hanya pemilik yang menghapusnya") {
    TempDir temp;
    const std::filesystem::path advert = temp.path / "mcp.json";

    SUBCASE("ditulis saat menyala, dibuang saat berhenti") {
        Harness harness;
        harness.config.advertisePath = advert;
        REQUIRE(harness.Start());

        REQUIRE(std::filesystem::exists(advert));
        std::ifstream file(advert);
        const json written = json::parse(file);
        CHECK(written.at("port") == harness.server.Port());
        CHECK(written.at("transport") == "http");
        CHECK(written.at("auth") == "none");
        // Inilah yang membuat berkas basi bisa dikenali: pembaca memeriksa
        // apakah proses ini masih ada sebelum mempercayai portnya.
        CHECK(written.at("pid") == static_cast<int64_t>(::getpid()));
        // Token tidak pernah ikut — berkas ini bisa dibaca proses lain milik
        // pengguna yang sama, yaitu tepat yang hendak dihalangi token.
        CHECK_FALSE(written.contains("token"));
        file.close();

        harness.server.Stop();
        CHECK_FALSE(std::filesystem::exists(advert));
    }

    SUBCASE("berkas milik proses lain tidak disentuh") {
        // **Dua editor terbuka adalah keadaan biasa.** Yang kedua menimpa berkas
        // ini dengan portnya sendiri; menghapusnya tanpa memeriksa akan mencabut
        // pengumuman milik editor pertama yang masih berjalan, dan sejak itu ia
        // tidak bisa ditemukan agen mana pun.
        {
            std::ofstream file(advert);
            file << json{{"name", "simengine"}, {"pid", 999999999}, {"port", 7777}}.dump();
        }

        Harness harness;
        harness.config.advertisePath = advert;
        REQUIRE(harness.Start());
        // Menyala tetap menimpanya: yang paling baru menyala adalah yang paling
        // masuk akal untuk ditemukan.
        {
            std::ifstream file(advert);
            CHECK(json::parse(file).at("pid") == static_cast<int64_t>(::getpid()));
        }

        // Sekarang tulis ulang seolah-olah proses lain merebutnya, lalu berhenti.
        {
            std::ofstream file(advert);
            file << json{{"name", "simengine"}, {"pid", 999999999}, {"port", 7778}}.dump();
        }
        harness.server.Stop();
        CHECK(std::filesystem::exists(advert));
        std::ifstream file(advert);
        CHECK(json::parse(file).at("port") == 7778);
    }
}
