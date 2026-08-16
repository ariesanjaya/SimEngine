#include "Sim/AIBridge/ToolRegistry.h"
#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/MainThreadQueue.h"
#include "Sim/Editor/AiTools.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialNodeCatalog.h"
#include "Sim/Material/MaterialValidation.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelManager.h"
#include "Sim/Scene/Project.h"

#if SIM_WITH_LUA
#include "Sim/Script/ScriptRuntime.h"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace sim::editor {
namespace {

using nlohmann::json;

json ParseArguments(std::string_view argumentsJson) {
    json arguments = json::parse(argumentsJson, nullptr, /*allow_exceptions=*/false);
    return arguments.is_object() ? arguments : json::object();
}

ai::ToolResult Text(std::string value, bool isError = false) {
    ai::ToolResult result;
    result.isError = isError;
    result.text = std::move(value);
    return result;
}

ai::ToolResult Structured(const json& value, bool isError = false) {
    ai::ToolResult result;
    result.isError = isError;
    result.text = value.dump(2);
    return result;
}

/// Memindai ulang indeks aset sekarang juga, alasan yang sama dengan tool aset:
/// agen yang menulis lalu langsung bertanya harus mendapat jawaban yang sudah
/// memperhitungkan tulisannya sendiri.
void RefreshAssets(EditorApp& app) {
    if (assets::AssetDatabase* database = app.Context().assets) {
        database->ScanNow();
    }
}

/// Aset yang ditunjuk agen, dicari lewat GUID **atau** jalur relatif.
///
/// Keduanya diterima karena `asset.search` mengembalikan keduanya, dan memaksa
/// agen memilih satu berarti memaksanya mengingat mana yang diterima tool mana.
const assets::AssetRecord* FindAsset(EditorApp& app, const json& arguments) {
    const assets::AssetDatabase* database = app.Context().assets;
    if (database == nullptr || !arguments.contains("asset") ||
        !arguments.at("asset").is_string()) {
        return nullptr;
    }
    const std::string key = arguments.at("asset").get<std::string>();
    if (const assets::AssetRecord* byGuid = database->Find(Uuid::Parse(key))) {
        return byGuid;
    }
    return database->FindByRelativePath(key);
}

#if SIM_WITH_LUA

// --- lua.eval ---------------------------------------------------------------------

/// Satu nilai hasil evaluasi beserta anaknya.
///
/// Bentuk pohonnya dipertahankan apa adanya, tidak diratakan jadi teks: isi
/// sebuah tabel ada di `children`, dan meratakannya berarti agen harus mengurai
/// teks untuk mendapatkan kembali apa yang tadinya sudah terstruktur.
///
/// **Alamatnya dibuang.** Lua mencetak tabel dan fungsi sebagai
/// `table: 0x55f3c2a1b040`, dan alamat itu berbeda di setiap kali dijalankan
/// meski keadaannya sama persis. Bagi Lua Console alamat itu tidak mengganggu —
/// yang membacanya orang, sekali, di sesi yang sedang berjalan. Bagi agen yang
/// membandingkan jawaban sebelum dan sesudah sebuah perubahan, ia adalah selisih
/// palsu di setiap baris.
json DescribeValue(const script::EvalNode& node) {
    std::string value = node.value;
    if (const std::size_t at = value.find(": 0x"); at != std::string::npos) {
        value.erase(at);
        if (!node.children.empty()) {
            value += " (" + std::to_string(node.children.size()) + " fields)";
        }
    }

    json described{{"label", node.label}, {"value", std::move(value)}};
    if (!node.children.empty()) {
        json children = json::array();
        for (const script::EvalNode& child : node.children) {
            children.push_back(DescribeValue(child));
        }
        described["children"] = std::move(children);
    }
    return described;
}

ai::ToolDefinition LuaEval(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "lua.eval";
    tool.description =
        "Run a snippet of Lua in the editor, in the same state the Lua Console uses. The "
        "snippet is tried as an expression first, so \"sim.editor\" returns a value rather "
        "than nothing. This is the escape hatch for anything that has no tool of its own; "
        "prefer a dedicated tool when one exists, because this one cannot be undone.";
    // **Izin tertinggi, dan bukan karena ia bisa gagal.** Lua di editor bisa
    // menyentuh apa saja yang di-bind ke `sim.*`, termasuk yang tidak melewati
    // CommandHistory. Yang tidak bisa dibatalkan tidak boleh berjalan tanpa
    // persetujuan.
    tool.permission = ai::ToolPermission::Dangerous;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        script::ScriptRuntime* scripts = app.Context().scripts;
        if (scripts == nullptr) {
            return Text("This editor has no Lua runtime.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        if (!arguments.contains("code") || !arguments.at("code").is_string()) {
            return Text("\"code\" is required and must be a string.", /*isError=*/true);
        }

        // `Evaluate` sudah membungkus pemanggilannya: kesalahan Lua kembali
        // sebagai teks beserta traceback, bukan sebagai exception yang menembus
        // ke sini dan bukan sebagai longjmp yang menjatuhkan editor. Kriteria
        // terima A3 nomor 3 persis menuntut itu.
        const script::EvalResult result =
            scripts->Evaluate(arguments.at("code").get<std::string>());
        if (!result.ok) {
            return Structured(json{{"ok", false}, {"error", result.error}}, /*isError=*/true);
        }

        json values = json::array();
        for (const script::EvalNode& value : result.values) {
            values.push_back(DescribeValue(value));
        }
        return Structured(json{{"ok", true}, {"values", std::move(values)}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["code"],
  "properties": {
    "code": {"type": "string", "description": "Lua source. Tried as an expression first, then as a statement."}
  }
})";
    return tool;
}

// --- lua.script_write -------------------------------------------------------------

ai::ToolDefinition LuaScriptWrite(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "lua.script_write";
    tool.description =
        "Write a Lua script into the project and reload it. The syntax is checked before "
        "anything is written, so a script that does not parse never reaches disk. Use this "
        "instead of file.write for .lua files.";
    tool.permission = ai::ToolPermission::Dangerous;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        script::ScriptRuntime* scripts = app.Context().scripts;
        if (scripts == nullptr) {
            return Text("This editor has no Lua runtime.", /*isError=*/true);
        }
        if (!app.HasProject()) {
            return Text("No project is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        if (!arguments.contains("path") || !arguments.at("path").is_string() ||
            !arguments.contains("text") || !arguments.at("text").is_string()) {
            return Text("\"path\" and \"text\" are both required strings.", /*isError=*/true);
        }

        const std::string relative = arguments.at("path").get<std::string>();
        std::filesystem::path absolute;
        const std::string refused =
            ResolveProjectPath(app.CurrentProject().root, relative, absolute);
        if (!refused.empty()) {
            return Text(refused, /*isError=*/true);
        }
        if (absolute.extension() != ".lua") {
            // Berkas yang tidak berakhiran .lua tidak akan pernah dimuat sebagai
            // skrip, jadi menerimanya berarti melapor berhasil untuk sesuatu
            // yang tidak akan pernah berjalan.
            return Text("A Lua script must end in .lua.", /*isError=*/true);
        }

        const std::string& text = arguments.at("text").get_ref<const std::string&>();
        const std::string syntaxError = scripts->CheckSyntax(text, "=" + relative);
        if (!syntaxError.empty()) {
            return Structured(json{{"ok", false}, {"error", syntaxError}}, /*isError=*/true);
        }

        std::error_code code;
        const bool existed = std::filesystem::exists(absolute, code);
        if (existed && !arguments.value("overwrite", false)) {
            return Text("That file already exists. Pass \"overwrite\": true to replace it.",
                        /*isError=*/true);
        }
        std::filesystem::create_directories(absolute.parent_path(), code);
        std::ofstream file(absolute, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Text("That file cannot be written.", /*isError=*/true);
        }
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();

        RefreshAssets(app);

        // Reload baru mungkin setelah pemindaian: berkas yang baru dibuat belum
        // punya GUID sampai indeksnya melihatnya.
        json result{{"ok", true},
                    {"path", relative},
                    {"bytes", text.size()},
                    {"replaced", existed},
                    {"reloaded", false}};
        if (const assets::AssetDatabase* database = app.Context().assets) {
            std::error_code relativeCode;
            const std::filesystem::path fromAssets =
                std::filesystem::relative(absolute, app.AssetsDirectory(), relativeCode);
            if (!relativeCode && !fromAssets.empty()) {
                if (const assets::AssetRecord* record =
                        database->FindByRelativePath(fromAssets.generic_string())) {
                    result["guid"] = record->guid.ToString();
                    scripts->Reload(record->guid);
                    result["reloaded"] = true;
                }
            }
        }
        return Structured(result);
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["path", "text"],
  "properties": {
    "path": {"type": "string", "description": "Project-relative path ending in .lua, e.g. \"Assets/Scripts/spin.lua\"."},
    "text": {"type": "string"},
    "overwrite": {"type": "boolean", "description": "Required to replace a file that exists. Default false."}
  }
})";
    return tool;
}

#endif  // SIM_WITH_LUA

// --- material.graph_get -----------------------------------------------------------

/// Membaca berkas material sebagai teks. Kosong berarti tidak terbaca.
std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

/// Pin input tiap tipe node yang dipakai graph ini, beserta nilai bawaannya.
///
/// **Tanpa ini agen harus menebak nama pin, dan tebakan yang salah tidak
/// menghasilkan galat.** Material bawaan tidak menulis satu pun `pins` — kunci
/// itu hanya muncul saat nilainya menyimpang dari bawaan katalog — jadi graph
/// yang dibaca agen tidak memuat contoh nama pin sama sekali. Nama yang dipakai
/// juga bukan nama yang akan ditebak siapa pun dari luar: `baseMetalness` dan
/// `specularRoughness`, mengikuti OpenPBR, bukan `metalness` dan `roughness`.
json PinSchema(const material::MaterialGraph& graph) {
    const material::MaterialNodeCatalog& catalog = material::MaterialNodeCatalog::Get();
    json schema = json::object();
    for (const material::MaterialNode& node : graph.nodes) {
        if (schema.contains(node.type)) {
            continue;
        }
        const material::MaterialNodeType* type = catalog.Find(node.type);
        if (type == nullptr) {
            continue;
        }
        json pins = json::array();
        for (const material::MaterialPin* pin : type->Inputs()) {
            json described{{"name", pin->name},
                           {"kind", material::ToString(pin->kind)}};
            if (pin->defaultValue.empty()) {
                // Pin tanpa nilai bawaan wajib tersambung; menyetel literal di
                // sana tidak akan menolongnya.
                described["mustBeLinked"] = true;
            } else {
                described["default"] = pin->defaultValue;
            }
            pins.push_back(std::move(described));
        }
        schema[node.type] = std::move(pins);
    }
    return schema;
}

/// Kunci yang benar-benar dibaca `LoadMaterialFromString` dari sebuah node.
/// Apa pun di luar ini dibuang tanpa suara.
bool IsKnownNodeKey(const std::string& key) {
    return key == "guid" || key == "type" || key == "position" || key == "size" ||
           key == "settings" || key == "pins";
}

/// Memeriksa apa yang akan hilang diam-diam sebelum sesuatu ditulis.
///
/// **Kelas bug yang sama yang berulang sepanjang track ini: tool melapor
/// berhasil untuk sesuatu yang tidak terjadi.** Sebuah node yang membawa
/// `"pinValues"` alih-alih `"pins"`, atau `"metalness"` alih-alih
/// `baseMetalness`, terurai tanpa galat, lolos validasi, dan tersimpan sebagai
/// material yang persis sama dengan sebelumnya. Yang dilihat agen adalah
/// `ok: true`; yang dilihat orang adalah bola putih.
std::vector<std::string> DroppedByLoader(const json& submitted) {
    const material::MaterialNodeCatalog& catalog = material::MaterialNodeCatalog::Get();
    std::vector<std::string> dropped;
    const auto nodes = submitted.find("nodes");
    if (nodes == submitted.end() || !nodes->is_array()) {
        return dropped;
    }
    for (const json& node : *nodes) {
        if (!node.is_object()) {
            continue;
        }
        const std::string type = node.value("type", std::string{});
        for (const auto& [key, value] : node.items()) {
            if (!IsKnownNodeKey(key)) {
                dropped.push_back("node \"" + type + "\" has no \"" + key +
                                  "\"; pin literals go in \"pins\"");
            }
        }
        const auto pins = node.find("pins");
        if (pins == node.end() || !pins->is_object()) {
            continue;
        }
        const material::MaterialNodeType* definition = catalog.Find(type);
        if (definition == nullptr) {
            dropped.push_back("no node type called \"" + type + "\"");
            continue;
        }
        for (const auto& [pin, value] : pins->items()) {
            if (definition->FindPin(pin) == nullptr) {
                dropped.push_back("node \"" + type + "\" has no pin \"" + pin + "\"");
            }
        }
    }
    return dropped;
}

/// Ringkasan graph yang cukup untuk agen menilai hasilnya tanpa membaca ulang
/// seluruh JSON-nya.
json Summarize(const material::MaterialGraph& graph) {
    json nodeTypes = json::object();
    for (const material::MaterialNode& node : graph.nodes) {
        const std::string type = node.type;
        nodeTypes[type] = nodeTypes.value(type, 0) + 1;
    }
    return json{{"nodes", graph.nodes.size()},
                {"links", graph.links.size()},
                {"parameters", graph.parameters.size()},
                {"nodeTypes", std::move(nodeTypes)}};
}

ai::ToolDefinition MaterialGraphGet(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "material.graph_get";
    tool.description =
        "Read a material graph as JSON, exactly as it is stored, together with \"pinSchema\": "
        "the input pins each node type in it accepts, their value kinds, and their defaults. "
        "Read the schema before changing anything — a stored material only lists a pin once "
        "its value differs from the default, so the graph alone shows you almost none of the "
        "names. Edit what you get back and hand it to material.graph_set.";
    tool.permission = ai::ToolPermission::Read;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        const assets::AssetDatabase* database = app.Context().assets;
        if (database == nullptr || !app.HasProject()) {
            return Text("No project is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        const assets::AssetRecord* record = FindAsset(app, arguments);
        if (record == nullptr) {
            return Text("No asset with that GUID or path. Use asset.search to find one.",
                        /*isError=*/true);
        }
        const std::filesystem::path absolute = database->AbsolutePath(*record);
        if (absolute.extension() != ".simmat") {
            return Text("That asset is not a material graph (.simmat).", /*isError=*/true);
        }

        const std::string text = ReadFile(absolute);
        if (text.empty()) {
            return Text("That material cannot be read.", /*isError=*/true);
        }
        material::MaterialGraph graph;
        const material::MaterialIoResult loaded = material::LoadMaterialFromString(graph, text);
        if (!loaded.ok) {
            // Berkasnya ada tapi rusak. Dikatakan apa adanya, karena agen yang
            // menerima "tidak bisa dibaca" akan mencoba jalur lain, sedangkan
            // yang tahu berkasnya rusak akan menulisnya ulang.
            return Structured(json{{"ok", false}, {"error", loaded.error}}, /*isError=*/true);
        }

        json graphJson = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (graphJson.is_discarded()) {
            return Text("That material is not valid JSON.", /*isError=*/true);
        }
        return Structured(json{{"guid", record->guid.ToString()},
                               {"path", record->relativePath},
                               {"summary", Summarize(graph)},
                               {"pinSchema", PinSchema(graph)},
                               {"graph", std::move(graphJson)}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["asset"],
  "properties": {
    "asset": {"type": "string", "description": "Material GUID or project-relative path."}
  }
})";
    return tool;
}

// --- material.graph_set -----------------------------------------------------------

ai::ToolDefinition MaterialGraphSet(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "material.graph_set";
    tool.description =
        "Replace a material graph. Nothing reaches disk unless it would compile. Three "
        "things refuse the write and say which node: keys and pin names the loader would "
        "drop without a word, JSON that does not parse, and a graph that fails validation "
        "(type mismatch, cycle, empty required pin, missing or duplicated output).";
    // **Dangerous karena tidak bisa di-undo, bukan karena bisa ditolak.**
    // Suntingan material sengaja tidak melewati CommandHistory — riwayat undo
    // utama menjanjikan pembatalan perubahan *scene*. Berkas yang tertimpa di
    // sini tidak dikembalikan oleh editor.undo.
    tool.permission = ai::ToolPermission::Dangerous;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        const assets::AssetDatabase* database = app.Context().assets;
        if (database == nullptr || !app.HasProject()) {
            return Text("No project is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        const assets::AssetRecord* record = FindAsset(app, arguments);
        if (record == nullptr) {
            return Text("No asset with that GUID or path. Use asset.search to find one.",
                        /*isError=*/true);
        }
        const std::filesystem::path absolute = database->AbsolutePath(*record);
        if (absolute.extension() != ".simmat") {
            return Text("That asset is not a material graph (.simmat).", /*isError=*/true);
        }

        std::string text;
        if (arguments.contains("graph") && arguments.at("graph").is_object()) {
            text = arguments.at("graph").dump();
        } else if (arguments.contains("text") && arguments.at("text").is_string()) {
            text = arguments.at("text").get<std::string>();
        } else {
            return Text("Pass the graph as \"graph\" (an object) or \"text\" (a string).",
                        /*isError=*/true);
        }

        material::MaterialGraph graph;
        const material::MaterialIoResult loaded = material::LoadMaterialFromString(graph, text);
        if (!loaded.ok) {
            return Structured(json{{"ok", false}, {"stage", "parse"}, {"error", loaded.error}},
                              /*isError=*/true);
        }

        // Diperiksa sebelum validasi, karena yang dicari di sini justru lolos
        // validasi: graph-nya sah, hanya saja bukan graph yang dimaksud agen.
        const json submitted = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (!submitted.is_discarded()) {
            const std::vector<std::string> dropped = DroppedByLoader(submitted);
            if (!dropped.empty()) {
                return Structured(json{{"ok", false},
                                       {"stage", "shape"},
                                       {"dropped", dropped},
                                       {"hint", "Call material.graph_get and read "
                                                "\"pinSchema\" for the names this node "
                                                "type accepts."}},
                                  /*isError=*/true);
            }
        }

        const material::ValidationResult validated = material::ValidateMaterial(graph);
        if (!validated.ok) {
            json issues = json::array();
            for (const material::MaterialIssue& issue : validated.errors) {
                json described{{"message", issue.message}};
                if (issue.node.IsValid()) {
                    described["node"] = issue.node.ToString();
                }
                if (!issue.pin.empty()) {
                    described["pin"] = issue.pin;
                }
                issues.push_back(std::move(described));
            }
            return Structured(
                json{{"ok", false}, {"stage", "validate"}, {"issues", std::move(issues)}},
                /*isError=*/true);
        }

        // Yang ditulis adalah hasil `SaveMaterialToString`, bukan teks yang
        // dikirim agen. Keduanya setara isinya, tapi bentuk kanonis inilah yang
        // ditulis Material Editor — dan berkas yang bentuknya berubah-ubah
        // tergantung siapa yang menyimpannya menghasilkan diff palsu di setiap
        // suntingan.
        const std::string canonical = material::SaveMaterialToString(graph);
        std::ofstream file(absolute, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Text("That material cannot be written.", /*isError=*/true);
        }
        file.write(canonical.data(), static_cast<std::streamsize>(canonical.size()));
        file.close();

        // **Disalin sebelum memindai ulang.** `AssetDatabase::Find` mengembalikan
        // pointer ke dalam `std::vector<AssetRecord>`, dan `ScanNow()` menyusun
        // vektor itu dari awal — `record` menggantung sesudahnya. Ini terbaca
        // benar sampai dijalankan: yang keluar bukan crash melainkan string
        // sampah, dan yang melaporkannya adalah nlohmann yang menolak men-dump
        // byte yang bukan UTF-8.
        const std::string guid = record->guid.ToString();
        const std::string relativePath = record->relativePath;
        RefreshAssets(app);
        return Structured(json{{"ok", true},
                               {"guid", guid},
                               {"path", relativePath},
                               {"bytes", canonical.size()},
                               {"summary", Summarize(graph)}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["asset"],
  "properties": {
    "asset": {"type": "string", "description": "Material GUID or project-relative path."},
    "graph": {"type": "object", "description": "The whole graph, in the shape material.graph_get returns under \"graph\"."},
    "text": {"type": "string", "description": "The graph as raw JSON text. Use this or \"graph\", not both."}
  }
})";
    return tool;
}

// --- material.preview -------------------------------------------------------------

/// Menunggu satu langkah di main thread selesai. `false` berarti editor tidak
/// menjawab — modal, atau sedang tidak menggambar sama sekali.
template <typename Fn>
bool OnMainThread(Fn&& work, bool& answer) {
    std::future<bool> done = MainThreadQueue::Get().Submit(std::forward<Fn>(work));
    if (done.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        return false;
    }
    answer = done.get();
    return true;
}

ai::ToolDefinition MaterialPreview(EditorApp& app, ScreenshotFn screenshot) {
    ai::ToolDefinition tool;
    tool.name = "material.preview";
    tool.description =
        "Open a material in the Material Editor and take a picture of its 3D preview. This is "
        "how you check what a material actually looks like instead of reasoning about its "
        "graph. Compiling a changed material takes a moment; this waits for it.";
    tool.permission = ai::ToolPermission::Read;
    // Alasan yang sama dengan `viewport.capture`: ia harus menunggu beberapa
    // frame, dan menunggu dari dalam main thread berarti menunggu frame yang
    // tidak akan pernah datang.
    tool.needsMainThread = false;
    tool.handler = [&app, screenshot = std::move(screenshot)](std::string_view argumentsJson) {
        const json arguments = ParseArguments(argumentsJson);

        Uuid material;
        std::string assetName;
        std::string failure;
        bool opened = false;
        if (!OnMainThread(
                [&app, &arguments, &material, &assetName, &failure]() {
                    const assets::AssetDatabase* database = app.Context().assets;
                    if (database == nullptr || !app.HasProject()) {
                        failure = "No project is open.";
                        return false;
                    }
                    const assets::AssetRecord* record = FindAsset(app, arguments);
                    if (record == nullptr) {
                        failure =
                            "No asset with that GUID or path. Use asset.search to find one.";
                        return false;
                    }
                    const std::filesystem::path absolute = database->AbsolutePath(*record);
                    if (absolute.extension() != ".simmat" &&
                        absolute.extension() != ".simmatinst") {
                        failure = "That asset is not a material (.simmat or .simmatinst).";
                        return false;
                    }
                    material = record->guid;
                    assetName = record->relativePath;

                    // **Keadaan pratinjau dikosongkan sebelum menunggu.** Panel
                    // yang tertutup, atau berada di belakang tab lain, tidak
                    // menggambar sama sekali — dan keadaan yang tertinggal dari
                    // material sebelumnya akan terbaca sebagai "sudah siap".
                    app.Context().materialPreviewState = {};

                    if (Panel* panel = app.Panels().Find(panel_id::kMaterialEditor)) {
                        panel->SetOpen(true);
                        panel->RequestFocus();
                    }
                    if (!app.Context().openAsset || !app.Context().openAsset(record->guid)) {
                        failure = "No editor accepted that material.";
                        return false;
                    }
                    return true;
                },
                opened)) {
            return Text("The editor did not answer; it may be showing a modal dialog.",
                        /*isError=*/true);
        }
        if (!opened) {
            return Text(failure, /*isError=*/true);
        }

        // Kompilasi shader material berjalan di TaskPool, jadi lamanya tidak
        // bisa ditebak dari sini. Yang ditunggu adalah panel menyatakan sendiri
        // bahwa yang tergambar adalah material ini — bukan sekian milidetik yang
        // dipilih dengan harapan.
        EditorContext::ViewportRect rect;
        std::string status = "The Material Editor never drew the preview.";
        bool ready = false;
        constexpr int kPolls = 80;  // 80 x 250 ms = 20 detik
        for (int poll = 0; poll < kPolls && !ready; ++poll) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            bool answered = false;
            if (!OnMainThread(
                    [&app, &material, &rect, &status]() {
                        const EditorContext::MaterialPreviewState& state =
                            app.Context().materialPreviewState;
                        if (!state.status.empty()) {
                            status = state.status;
                        }
                        if (!state.ready || state.material != material) {
                            return false;
                        }
                        rect = state.rect;
                        return true;
                    },
                    answered)) {
                return Text("The editor did not answer; it may be showing a modal dialog.",
                            /*isError=*/true);
            }
            ready = answered;
        }
        if (!ready) {
            // Dikatakan apa adanya. Gambar tulisan "Compiling…" yang dikirim
            // dengan keterangan "ini materialmu" adalah kebohongan yang tidak
            // akan pernah diperiksa agen.
            return Text("The preview never became ready: " + status, /*isError=*/true);
        }

        std::vector<uint8_t> png;
        std::string error;
        bool captured = false;
        if (!OnMainThread(
                [&screenshot, &rect, &png, &error]() {
                    // Dua langkah, alasan yang sama dengan `viewport.capture`:
                    // skala piksel per satuan logis baru diketahui dari lebar
                    // gambar yang benar-benar ditangkap.
                    std::vector<uint8_t> probe;
                    std::string probeError;
                    if (!screenshot(/*crop=*/nullptr, probe, probeError)) {
                        error = probeError;
                        return false;
                    }
                    if (probe.size() < 24 || rect.mainSize.x <= 0.0f) {
                        png = std::move(probe);
                        return true;
                    }
                    const uint32_t imageWidth =
                        (uint32_t(probe[16]) << 24) | (uint32_t(probe[17]) << 16) |
                        (uint32_t(probe[18]) << 8) | uint32_t(probe[19]);
                    const float scale = static_cast<float>(imageWidth) / rect.mainSize.x;
                    CaptureRect crop;
                    crop.x = static_cast<uint32_t>(std::max(0.0f, rect.position.x * scale));
                    crop.y = static_cast<uint32_t>(std::max(0.0f, rect.position.y * scale));
                    crop.width = static_cast<uint32_t>(std::max(1.0f, rect.size.x * scale));
                    crop.height = static_cast<uint32_t>(std::max(1.0f, rect.size.y * scale));
                    return screenshot(&crop, png, error);
                },
                captured)) {
            return Text("The editor did not answer; it may be showing a modal dialog.",
                        /*isError=*/true);
        }
        if (!captured) {
            return Text("Could not capture the preview: " + error, /*isError=*/true);
        }

        ai::ToolResult result;
        result.text = assetName + " — material preview, " +
                      std::to_string(static_cast<int>(rect.size.x)) + "x" +
                      std::to_string(static_cast<int>(rect.size.y)) + " logical units";
        result.imageBytes = std::move(png);
        result.imageMimeType = "image/png";
        return result;
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["asset"],
  "properties": {
    "asset": {"type": "string", "description": "Material GUID or project-relative path."}
  }
})";
    return tool;
}

}  // namespace

void RegisterAuthoringTools(ai::ToolRegistry& tools, EditorApp& app, ScreenshotFn screenshot) {
#if SIM_WITH_LUA
    tools.Register(LuaEval(app));
    tools.Register(LuaScriptWrite(app));
#endif
    tools.Register(MaterialGraphGet(app));
    tools.Register(MaterialGraphSet(app));
    // **Tanpa penangkap layar, tool ini tidak didaftarkan sama sekali.** Ia
    // menempuh tangkapan jendela lalu dipotong — tidak ada jalur readback GPU di
    // engine ini — jadi editor tanpa kemampuan itu hanya bisa menawarkan tool
    // yang selalu gagal. Aturan yang sama dengan `editor.screenshot`.
    if (screenshot) {
        tools.Register(MaterialPreview(app, std::move(screenshot)));
    }
}

}  // namespace sim::editor
