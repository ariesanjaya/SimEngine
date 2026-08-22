#include "Sim/AIBridge/ToolRegistry.h"
#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Core/MainThreadQueue.h"
#include "Sim/Editor/AiTools.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialNodeCatalog.h"
#include "Sim/Material/MaterialValidation.h"
#include "Sim/Animation/AnimationIo.h"
#include "Sim/Animation/Clip.h"
#include "Sim/Particle/ParticleEffect.h"
#include "Sim/Terrain/Terrain.h"
#include "Sim/Terrain/TerrainBrush.h"
#include "Sim/SceneView/TerrainStore.h"
#include "Sim/Editor/Panel.h"
#include "Sim/Editor/PanelIds.h"
#include "Sim/Editor/PanelManager.h"
#include "Sim/Scene/Project.h"

#if SIM_WITH_LUA
#include "Sim/Script/ScriptRuntime.h"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <utility>
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

std::string Lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
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

/// Kunci yang dikirim tapi tidak muncul lagi setelah berkasnya ditulis ulang.
///
/// **Pemeriksaan yang tidak perlu tahu formatnya.** Ia mengurai kiriman agen,
/// menuliskannya kembali lewat penulis kanonis format itu, lalu mencari kunci
/// yang hilang di perjalanan. Apa pun yang diabaikan pemuat — salah nama, salah
/// sarang, field yang sudah tidak ada — muncul di sini.
///
/// Yang dicari hanya kunci yang **hilang**, bukan nilai yang berbeda. Penulis
/// kanonis berhak menormalkan angka (`1` jadi `1.0`) dan mengisi bawaan, dan
/// melaporkan itu sebagai kehilangan akan membanjiri agen dengan galat palsu
/// justru pada kiriman yang benar.
void CollectMissingKeys(const json& sent, const json& stored, const std::string& path,
                        std::vector<std::string>& missing) {
    if (sent.is_object()) {
        for (const auto& [key, value] : sent.items()) {
            const std::string here = path.empty() ? key : path + "." + key;
            if (!stored.is_object() || !stored.contains(key)) {
                missing.push_back(here);
                continue;
            }
            CollectMissingKeys(value, stored.at(key), here, missing);
        }
        return;
    }
    if (sent.is_array() && stored.is_array()) {
        const std::size_t shared = std::min(sent.size(), stored.size());
        for (std::size_t at = 0; at < shared; ++at) {
            CollectMissingKeys(sent[at], stored[at], path + "[" + std::to_string(at) + "]",
                               missing);
        }
        if (sent.size() > stored.size()) {
            missing.push_back(path + " has " + std::to_string(sent.size()) + " entries but " +
                              std::to_string(stored.size()) + " were kept");
        }
    }
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

/// Membuka sebuah aset di editor dokumennya, menunggu pratinjaunya benar-benar
/// menggambar aset itu, lalu memotret petaknya.
///
/// **Satu jalur untuk material dan partikel.** Keduanya menempuh langkah yang
/// sama persis, dan yang membedakan hanya panel mana yang dibuka dan bidang
/// keadaan mana yang dibaca. Menulisnya dua kali berarti perbaikan pada satu
/// salinan yang tidak pernah sampai ke salinan lainnya.
using PreviewStateOf = EditorContext::DocumentPreviewState& (*)(EditorContext&);

struct PreviewKind {
    const char* toolName;
    const char* description;
    const char* panelId;
    /// Ekstensi yang diterima, diakhiri nullptr.
    const char* const* extensions;
    const char* wrongType;
    PreviewStateOf state;
    /// Berapa lama pantas ditunggu. Material harus mengompilasi shader dulu;
    /// pratinjau partikel digambar draw list ImGui dan siap pada frame berikutnya.
    int polls;
};

ai::ToolDefinition DocumentPreview(EditorApp& app, ScreenshotFn screenshot, PreviewKind kind) {
    ai::ToolDefinition tool;
    tool.name = kind.toolName;
    tool.description = kind.description;
    tool.permission = ai::ToolPermission::Read;
    // Alasan yang sama dengan `viewport.capture`: ia harus menunggu beberapa
    // frame, dan menunggu dari dalam main thread berarti menunggu frame yang
    // tidak akan pernah datang.
    tool.needsMainThread = false;
    tool.handler = [&app, screenshot = std::move(screenshot),
                    kind](std::string_view argumentsJson) {
        const json arguments = ParseArguments(argumentsJson);

        Uuid asset;
        std::string assetName;
        std::string failure;
        bool opened = false;
        if (!OnMainThread(
                [&app, &arguments, &asset, &assetName, &failure, &kind]() {
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
                    bool accepted = false;
                    for (const char* const* at = kind.extensions; *at != nullptr; ++at) {
                        accepted = accepted || absolute.extension() == *at;
                    }
                    if (!accepted) {
                        failure = kind.wrongType;
                        return false;
                    }
                    asset = record->guid;
                    assetName = record->relativePath;

                    // **Keadaan pratinjau dikosongkan sebelum menunggu.** Panel
                    // yang tertutup, atau berada di belakang tab lain, tidak
                    // menggambar sama sekali — dan keadaan yang tertinggal dari
                    // aset sebelumnya akan terbaca sebagai "sudah siap".
                    kind.state(app.Context()) = {};

                    if (Panel* panel = app.Panels().Find(kind.panelId)) {
                        panel->SetOpen(true);
                        panel->RequestFocus();
                    }
                    if (!app.Context().openAsset || !app.Context().openAsset(record->guid)) {
                        failure = "No editor accepted that asset.";
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

        // Yang ditunggu adalah panel menyatakan sendiri bahwa yang tergambar
        // adalah aset ini — bukan sekian milidetik yang dipilih dengan harapan.
        EditorContext::ViewportRect rect;
        std::string status = "The editor never drew the preview.";
        bool ready = false;
        for (int poll = 0; poll < kind.polls && !ready; ++poll) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            bool answered = false;
            if (!OnMainThread(
                    [&app, &asset, &rect, &status, &kind]() {
                        const EditorContext::DocumentPreviewState& state =
                            kind.state(app.Context());
                        if (!state.status.empty()) {
                            status = state.status;
                        }
                        if (!state.ready || state.asset != asset) {
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
        result.text = assetName + " — preview, " +
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
    "asset": {"type": "string", "description": "Asset GUID or project-relative path."}
  }
})";
    return tool;
}

constexpr const char* kMaterialExtensions[] = {".simmat", ".simmatinst", nullptr};
constexpr const char* kEffectExtensions[] = {".simfx", nullptr};
constexpr const char* kClipExtensions[] = {".simanim", nullptr};

ai::ToolDefinition MaterialPreview(EditorApp& app, ScreenshotFn screenshot) {
    return DocumentPreview(
        app, std::move(screenshot),
        PreviewKind{"material.preview",
                    "Open a material in the Material Editor and take a picture of its 3D "
                    "preview. This is how you check what a material actually looks like "
                    "instead of reasoning about its graph. Compiling a changed material takes "
                    "a moment; this waits for it.",
                    panel_id::kMaterialEditor, kMaterialExtensions,
                    "That asset is not a material (.simmat or .simmatinst).",
                    [](EditorContext& context) -> EditorContext::DocumentPreviewState& {
                        return context.materialPreviewState;
                    },
                    /*polls=*/80});  // 20 detik: shader-nya dikompilasi dulu
}

ai::ToolDefinition AnimationPreview(EditorApp& app, ScreenshotFn screenshot) {
    return DocumentPreview(
        app, std::move(screenshot),
        PreviewKind{"animation.preview",
                    "Open an animation clip in the Animation Editor and take a picture of its "
                    "skeleton preview at the current playhead. Use it to see whether a pose "
                    "reads the way you meant instead of reasoning about key values.",
                    panel_id::kAnimationEditor, kClipExtensions,
                    "That asset is not an animation clip (.simanim).",
                    [](EditorContext& context) -> EditorContext::DocumentPreviewState& {
                        return context.animationPreviewState;
                    },
                    /*polls=*/16});  // 4 detik: rangkanya digambar draw list ImGui
}

ai::ToolDefinition ParticlePreview(EditorApp& app, ScreenshotFn screenshot) {
    return DocumentPreview(
        app, std::move(screenshot),
        PreviewKind{"particle.preview",
                    "Open a particle effect in the Particle Editor and take a picture of its "
                    "preview, with the effect playing. Use it to check what an effect actually "
                    "does instead of reasoning about its modules.",
                    panel_id::kParticleEditor, kEffectExtensions,
                    "That asset is not a particle effect (.simfx).",
                    [](EditorContext& context) -> EditorContext::DocumentPreviewState& {
                        return context.particlePreviewState;
                    },
                    /*polls=*/16});  // 4 detik: tidak ada yang dikompilasi
}

// --- partikel -----------------------------------------------------------------------

/// Ringkasan efek: cukup untuk agen menilai hasilnya tanpa membaca ulang JSON-nya.
json Summarize(const particle::ParticleEffect& effect) {
    json emitters = json::array();
    for (const particle::ParticleEmitter& emitter : effect.emitters) {
        // Modul disimpan sebagai bidang bernama, bukan sebagai daftar — jadi
        // yang diringkas di sini adalah jenis mana yang menyala, lewat
        // `IsEnabled`, bukan iterasi atas sebuah koleksi.
        json modules = json::array();
        for (const particle::ModuleKind kind :
             {particle::ModuleKind::Spawn, particle::ModuleKind::Shape,
              particle::ModuleKind::Initial, particle::ModuleKind::OverLifetime,
              particle::ModuleKind::Force, particle::ModuleKind::Collision,
              particle::ModuleKind::Render}) {
            if (emitter.IsEnabled(kind)) {
                modules.push_back(particle::ToString(kind));
            }
        }
        emitters.push_back(json{{"name", emitter.name},
                                {"enabled", emitter.enabled},
                                {"maxParticles", emitter.maxParticles},
                                {"modules", std::move(modules)}});
    }
    return json{{"name", effect.name},
                {"emitters", std::move(emitters)}};
}

ai::ToolDefinition ParticleGet(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "particle.get";
    tool.description =
        "Read a particle effect (.simfx) as JSON, exactly as it is stored, with a summary of "
        "its emitters and the modules each one has switched on. Edit what you get back and "
        "hand it to particle.set.";
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
        if (absolute.extension() != ".simfx") {
            return Text("That asset is not a particle effect (.simfx).", /*isError=*/true);
        }

        const std::string text = ReadFile(absolute);
        if (text.empty()) {
            return Text("That effect cannot be read.", /*isError=*/true);
        }
        particle::ParticleEffect effect;
        const particle::EffectIoResult loaded = particle::LoadEffectFromString(effect, text);
        if (!loaded.ok) {
            return Structured(json{{"ok", false}, {"error", loaded.error}}, /*isError=*/true);
        }
        json effectJson = json::parse(text, nullptr, /*allow_exceptions=*/false);
        if (effectJson.is_discarded()) {
            return Text("That effect is not valid JSON.", /*isError=*/true);
        }
        return Structured(json{{"guid", record->guid.ToString()},
                               {"path", record->relativePath},
                               {"summary", Summarize(effect)},
                               {"effect", std::move(effectJson)}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["asset"],
  "properties": {
    "asset": {"type": "string", "description": "Effect GUID or project-relative path."}
  }
})";
    return tool;
}

ai::ToolDefinition ParticleSet(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "particle.set";
    tool.description =
        "Replace a particle effect. What you send is parsed, written back out through the "
        "engine's own writer, and compared: anything the loader would have dropped without a "
        "word — a misspelled key, a field nested in the wrong place — refuses the write and "
        "comes back named.";
    // Dangerous karena tidak bisa di-undo: suntingan aset tidak melewati
    // CommandHistory, alasan yang sama dengan material.
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
        if (absolute.extension() != ".simfx") {
            return Text("That asset is not a particle effect (.simfx).", /*isError=*/true);
        }

        std::string text;
        if (arguments.contains("effect") && arguments.at("effect").is_object()) {
            text = arguments.at("effect").dump();
        } else if (arguments.contains("text") && arguments.at("text").is_string()) {
            text = arguments.at("text").get<std::string>();
        } else {
            return Text("Pass the effect as \"effect\" (an object) or \"text\" (a string).",
                        /*isError=*/true);
        }

        particle::ParticleEffect effect;
        const particle::EffectIoResult loaded = particle::LoadEffectFromString(effect, text);
        if (!loaded.ok) {
            return Structured(json{{"ok", false}, {"stage", "parse"}, {"error", loaded.error}},
                              /*isError=*/true);
        }

        const std::string canonical = particle::SaveEffectToString(effect);
        const json sent = json::parse(text, nullptr, /*allow_exceptions=*/false);
        const json stored = json::parse(canonical, nullptr, /*allow_exceptions=*/false);
        if (!sent.is_discarded() && !stored.is_discarded()) {
            std::vector<std::string> missing;
            CollectMissingKeys(sent, stored, {}, missing);
            if (!missing.empty()) {
                return Structured(json{{"ok", false},
                                       {"stage", "shape"},
                                       {"dropped", missing},
                                       {"hint", "Call particle.get and edit what it returns; "
                                                "these keys did not survive being written "
                                                "back out."}},
                                  /*isError=*/true);
            }
        }

        std::ofstream file(absolute, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Text("That effect cannot be written.", /*isError=*/true);
        }
        file.write(canonical.data(), static_cast<std::streamsize>(canonical.size()));
        file.close();

        // Disalin sebelum memindai ulang: `record` menggantung setelah ScanNow.
        const std::string guid = record->guid.ToString();
        const std::string relativePath = record->relativePath;
        RefreshAssets(app);
        return Structured(json{{"ok", true},
                               {"guid", guid},
                               {"path", relativePath},
                               {"bytes", canonical.size()},
                               {"summary", Summarize(effect)}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["asset"],
  "properties": {
    "asset": {"type": "string", "description": "Effect GUID or project-relative path."},
    "effect": {"type": "object", "description": "The whole effect, in the shape particle.get returns under \"effect\"."},
    "text": {"type": "string", "description": "The effect as raw JSON text. Use this or \"effect\", not both."}
  }
})";
    return tool;
}

// --- terrain -------------------------------------------------------------------------

/// Terrain yang sedang dipegang editor untuk sebuah aset, dimuat bila perlu.
///
/// **Lewat `TerrainStore`, bukan memuat berkasnya sendiri.** Sebuah terrain
/// berukuran ratusan megabyte, dan salinan kedua bukan sekadar tidak sepakat
/// dengan yang sedang disunting orang — ia juga tidak muat.
terrain::Terrain* OpenTerrain(EditorApp& app, const json& arguments, std::string& failure,
                              Uuid& guid, std::string& relativePath) {
    const assets::AssetDatabase* database = app.Context().assets;
    if (database == nullptr || !app.HasProject()) {
        failure = "No project is open.";
        return nullptr;
    }
    TerrainStore* store = app.Context().terrains;
    if (store == nullptr) {
        failure = "This editor has no terrain store.";
        return nullptr;
    }
    const assets::AssetRecord* record = FindAsset(app, arguments);
    if (record == nullptr) {
        failure = "No asset with that GUID or path. Use asset.search to find one.";
        return nullptr;
    }
    const std::filesystem::path absolute = database->AbsolutePath(*record);
    if (absolute.extension() != ".simterrain") {
        failure = "That asset is not a terrain (.simterrain).";
        return nullptr;
    }
    guid = record->guid;
    relativePath = record->relativePath;
    terrain::Terrain* loaded = store->Get(guid, absolute);
    if (loaded == nullptr) {
        failure = "That terrain cannot be read.";
    }
    return loaded;
}

/// Petak sampel yang diminta, dijepit ke terrain. Kosong berarti seluruhnya.
terrain::SampleRect ReadRect(const json& arguments, const terrain::Terrain& value) {
    terrain::SampleRect rect{0, 0, value.SamplesX(), value.SamplesY()};
    const auto region = arguments.find("region");
    if (region == arguments.end() || !region->is_object()) {
        return rect;
    }
    rect.x0 = std::clamp(region->value("x", 0), 0, value.SamplesX());
    rect.y0 = std::clamp(region->value("y", 0), 0, value.SamplesY());
    rect.x1 = std::clamp(rect.x0 + region->value("width", value.SamplesX()), rect.x0,
                         value.SamplesX());
    rect.y1 = std::clamp(rect.y0 + region->value("height", value.SamplesY()), rect.y0,
                         value.SamplesY());
    return rect;
}

json DescribeTerrain(const terrain::Terrain& value, const terrain::SampleRect& rect) {
    return json{{"samplesX", value.SamplesX()},
                {"samplesY", value.SamplesY()},
                {"worldWidth", value.WorldWidth()},
                {"worldDepth", value.WorldDepth()},
                {"region", json{{"x", rect.x0},
                                {"y", rect.y0},
                                {"width", rect.Width()},
                                {"height", rect.Height()}}}};
}

ai::ToolDefinition TerrainHeightmapGet(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "terrain.heightmap_get";
    tool.description =
        "Read a rectangle of a terrain heightmap. Returns heights in metres, plus the min, "
        "max and mean over the rectangle. Ask for a region — a whole terrain is millions of "
        "samples and will be refused.";
    tool.permission = ai::ToolPermission::Read;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        const json arguments = ParseArguments(argumentsJson);
        std::string failure;
        Uuid guid;
        std::string relativePath;
        terrain::Terrain* value = OpenTerrain(app, arguments, failure, guid, relativePath);
        if (value == nullptr) {
            return Text(failure, /*isError=*/true);
        }
        const terrain::SampleRect rect = ReadRect(arguments, *value);
        if (rect.Empty()) {
            return Text("That region is empty.", /*isError=*/true);
        }
        // **Dibatasi, dan yang menolak menyebut angkanya.** Terrain 4096² adalah
        // 16 juta sampel; menjawabnya sebagai JSON berarti balasan ratusan
        // megabyte yang tidak muat di jendela konteks agen mana pun.
        constexpr int kMaxSamples = 256 * 256;
        if (rect.Width() * rect.Height() > kMaxSamples) {
            return Text("That region has " +
                            std::to_string(rect.Width() * rect.Height()) +
                            " samples; the limit is " + std::to_string(kMaxSamples) +
                            ". Ask for a smaller region.",
                        /*isError=*/true);
        }

        json heights = json::array();
        float lowest = std::numeric_limits<float>::max();
        float highest = std::numeric_limits<float>::lowest();
        double total = 0.0;
        for (int y = rect.y0; y < rect.y1; ++y) {
            json row = json::array();
            for (int x = rect.x0; x < rect.x1; ++x) {
                const float metres = value->ToMeters(value->RawAt(x, y));
                row.push_back(metres);
                lowest = std::min(lowest, metres);
                highest = std::max(highest, metres);
                total += metres;
            }
            heights.push_back(std::move(row));
        }

        json described = DescribeTerrain(*value, rect);
        described["guid"] = guid.ToString();
        described["path"] = relativePath;
        described["minHeight"] = lowest;
        described["maxHeight"] = highest;
        described["meanHeight"] =
            static_cast<float>(total / (rect.Width() * rect.Height()));
        described["heights"] = std::move(heights);
        return Structured(described);
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["asset"],
  "properties": {
    "asset": {"type": "string", "description": "Terrain GUID or project-relative path."},
    "region": {
      "type": "object",
      "description": "Sample rectangle. Omit for the whole terrain, which is usually too large.",
      "properties": {
        "x": {"type": "integer"}, "y": {"type": "integer"},
        "width": {"type": "integer"}, "height": {"type": "integer"}
      }
    }
  }
})";
    return tool;
}

ai::ToolDefinition TerrainHeightmapSet(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "terrain.heightmap_set";
    tool.description =
        "Write heights, in metres, into a rectangle of a terrain heightmap. The rows must "
        "match the region exactly; a mismatch is refused rather than padded, because a "
        "half-written region looks like a cliff nobody asked for.";
    tool.permission = ai::ToolPermission::Write;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        const json arguments = ParseArguments(argumentsJson);
        std::string failure;
        Uuid guid;
        std::string relativePath;
        terrain::Terrain* value = OpenTerrain(app, arguments, failure, guid, relativePath);
        if (value == nullptr) {
            return Text(failure, /*isError=*/true);
        }
        const terrain::SampleRect rect = ReadRect(arguments, *value);
        if (rect.Empty()) {
            return Text("That region is empty.", /*isError=*/true);
        }
        const auto heights = arguments.find("heights");
        if (heights == arguments.end() || !heights->is_array()) {
            return Text("\"heights\" is required: an array of rows, each an array of metres.",
                        /*isError=*/true);
        }
        // **Diperiksa seluruhnya sebelum satu sampel pun ditulis.** Menolak di
        // tengah meninggalkan setengah petak tertulis, dan yang terlihat adalah
        // tebing yang tidak diminta siapa pun.
        if (static_cast<int>(heights->size()) != rect.Height()) {
            return Text("\"heights\" has " + std::to_string(heights->size()) +
                            " rows but the region is " + std::to_string(rect.Height()) +
                            " tall.",
                        /*isError=*/true);
        }
        for (const json& row : *heights) {
            if (!row.is_array() || static_cast<int>(row.size()) != rect.Width()) {
                return Text("Every row must have " + std::to_string(rect.Width()) +
                                " numbers, to match the region width.",
                            /*isError=*/true);
            }
        }

        // Satu goresan di jurnal undo terrain, jadi Ctrl+Z dari sisi orang
        // mengembalikan seluruh petak sekaligus — bukan sampel demi sampel.
        value->BeginStroke();
        for (int y = rect.y0; y < rect.y1; ++y) {
            const json& row = heights->at(static_cast<std::size_t>(y - rect.y0));
            for (int x = rect.x0; x < rect.x1; ++x) {
                const float metres = row.at(static_cast<std::size_t>(x - rect.x0)).get<float>();
                value->SetRawAt(x, y, value->ToSample(metres));
            }
        }
        value->EndStroke();
        app.Context().terrains->MarkDirty(guid);

        json described = DescribeTerrain(*value, rect);
        described["ok"] = true;
        described["guid"] = guid.ToString();
        described["path"] = relativePath;
        described["samplesWritten"] = rect.Width() * rect.Height();
        return Structured(described);
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["asset", "region", "heights"],
  "properties": {
    "asset": {"type": "string", "description": "Terrain GUID or project-relative path."},
    "region": {
      "type": "object",
      "properties": {
        "x": {"type": "integer"}, "y": {"type": "integer"},
        "width": {"type": "integer"}, "height": {"type": "integer"}
      }
    },
    "heights": {
      "type": "array",
      "description": "Rows of heights in metres, outer array = rows (y), inner = columns (x).",
      "items": {"type": "array", "items": {"type": "number"}}
    }
  }
})";
    return tool;
}

ai::ToolDefinition TerrainSculpt(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "terrain.sculpt";
    tool.description =
        "Run brush dabs over a terrain: raise, lower, flatten, smooth, or noise. Give world "
        "positions in metres. The whole call is one stroke, so one undo from the human side "
        "takes back everything the call did.";
    tool.permission = ai::ToolPermission::Write;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        const json arguments = ParseArguments(argumentsJson);
        std::string failure;
        Uuid guid;
        std::string relativePath;
        terrain::Terrain* value = OpenTerrain(app, arguments, failure, guid, relativePath);
        if (value == nullptr) {
            return Text(failure, /*isError=*/true);
        }

        static const std::map<std::string, terrain::BrushKind> kKinds{
            {"raise", terrain::BrushKind::Raise},
            {"lower", terrain::BrushKind::Lower},
            {"flatten", terrain::BrushKind::Flatten},
            {"smooth", terrain::BrushKind::Smooth},
            {"noise", terrain::BrushKind::Noise}};
        const std::string kindName = Lowered(arguments.value("kind", std::string("raise")));
        const auto kind = kKinds.find(kindName);
        if (kind == kKinds.end()) {
            return Text("Unknown brush \"" + kindName +
                            "\". Known: raise, lower, flatten, smooth, noise.",
                        /*isError=*/true);
        }

        terrain::Brush brush;
        brush.kind = kind->second;
        brush.radius = arguments.value("radius", 10.0f);
        brush.strength = arguments.value("strength", 5.0f);
        brush.falloff = std::clamp(arguments.value("falloff", 0.5f), 0.0f, 1.0f);
        brush.targetHeight = arguments.value("target_height", 0.0f);
        brush.noiseFrequency = arguments.value("noise_frequency", 0.05f);
        brush.seed = arguments.value("seed", 1u);
        if (brush.radius <= 0.0f) {
            return Text("\"radius\" must be greater than zero.", /*isError=*/true);
        }

        const auto at = arguments.find("at");
        if (at == arguments.end() || !at->is_array() || at->empty()) {
            return Text("\"at\" is required: an array of [x, z] world positions in metres.",
                        /*isError=*/true);
        }
        std::vector<std::pair<float, float>> dabs;
        for (const json& point : *at) {
            if (!point.is_array() || point.size() != 2) {
                return Text("Every entry in \"at\" must be [x, z] in metres.",
                            /*isError=*/true);
            }
            dabs.emplace_back(point[0].get<float>(), point[1].get<float>());
        }

        // `dt` adalah detik sejak sentuhan sebelumnya; sebuah sentuhan terprogram
        // tidak punya waktu nyata, jadi angkanya adalah "berapa lama kuas ini
        // ditahan" dan agen yang menentukannya.
        const float dt = arguments.value("dwell", 1.0f);

        value->BeginStroke();
        for (const auto& [x, z] : dabs) {
            terrain::ApplyDab(*value, brush, x, z, dt);
        }
        value->EndStroke();
        app.Context().terrains->MarkDirty(guid);

        return Structured(json{{"ok", true},
                               {"guid", guid.ToString()},
                               {"path", relativePath},
                               {"brush", terrain::ToString(brush.kind)},
                               {"dabs", dabs.size()},
                               {"undoDepth", value->UndoDepth()}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["asset", "at"],
  "properties": {
    "asset": {"type": "string", "description": "Terrain GUID or project-relative path."},
    "kind": {"type": "string", "enum": ["raise", "lower", "flatten", "smooth", "noise"], "description": "Default raise."},
    "at": {
      "type": "array",
      "description": "World positions in metres to dab, each [x, z].",
      "items": {"type": "array", "items": {"type": "number"}}
    },
    "radius": {"type": "number", "description": "Metres. Default 10."},
    "strength": {"type": "number", "description": "Raise/lower: metres per second at the centre. Flatten/smooth: convergence rate. Noise: amplitude. Default 5."},
    "falloff": {"type": "number", "description": "0 = hard edge, 1 = soft. Default 0.5."},
    "target_height": {"type": "number", "description": "Flatten only, metres."},
    "noise_frequency": {"type": "number", "description": "Noise only, cycles per metre."},
    "seed": {"type": "integer", "description": "Noise only."},
    "dwell": {"type": "number", "description": "Seconds each dab is held. Default 1."}
  }
})";
    return tool;
}

// --- animasi -------------------------------------------------------------------------

/// Membaca klip `.simanim` dari disk. Kosongnya `failure` berarti berhasil.
bool OpenClip(EditorApp& app, const json& arguments, animation::Clip& clip,
              animation::ClipDocument& document, std::filesystem::path& absolute,
              Uuid& guid, std::string& relativePath, std::string& failure) {
    const assets::AssetDatabase* database = app.Context().assets;
    if (database == nullptr || !app.HasProject()) {
        failure = "No project is open.";
        return false;
    }
    const assets::AssetRecord* record = FindAsset(app, arguments);
    if (record == nullptr) {
        failure = "No asset with that GUID or path. Use asset.search to find one.";
        return false;
    }
    absolute = database->AbsolutePath(*record);
    if (absolute.extension() != ".simanim") {
        failure = "That asset is not an animation clip (.simanim).";
        return false;
    }
    guid = record->guid;
    relativePath = record->relativePath;
    const animation::AnimationIoResult loaded = animation::LoadClip(clip, document, absolute);
    if (!loaded.ok) {
        failure = loaded.error;
        return false;
    }
    return true;
}

ai::ToolDefinition AnimationClipInfo(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "animation.clip_info";
    tool.description =
        "Describe an animation clip: length, frame rate, looping, root motion, and every "
        "track it holds with the bone and channel it drives and how many keys it has. Read "
        "this before animation.key_set — channel names are fixed and guessing them wastes a "
        "call.";
    tool.permission = ai::ToolPermission::Read;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        const json arguments = ParseArguments(argumentsJson);
        animation::Clip clip;
        animation::ClipDocument document;
        std::filesystem::path absolute;
        Uuid guid;
        std::string relativePath;
        std::string failure;
        if (!OpenClip(app, arguments, clip, document, absolute, guid, relativePath, failure)) {
            return Text(failure, /*isError=*/true);
        }

        json tracks = json::array();
        for (const animation::Track& track : clip.Tracks()) {
            float lowest = 0.0f;
            float highest = 0.0f;
            track.curve.ValueRange(lowest, highest);
            tracks.push_back(json{{"bone", track.bone},
                                  {"channel", animation::ToString(track.channel)},
                                  {"keys", track.curve.Keys().size()},
                                  {"minValue", lowest},
                                  {"maxValue", highest}});
        }
        // **Track rotasi disebut terpisah, bukan digabung ke daftar di atas.**
        // Ia bukan tiga kanal Euler yang kebetulan bersama: kuaternion tidak
        // bisa disunting per komponen, dan menyamarkannya sebagai kanal biasa
        // akan mengundang `animation.key_set` yang merusak rotasinya.
        json rotationTracks = json::array();
        for (int at = 0; at < clip.RotationTrackCount(); ++at) {
            const animation::RotationTrack& track = clip.RotationTrackAt(at);
            rotationTracks.push_back(
                json{{"bone", track.bone}, {"keys", track.keys.size()}});
        }

        return Structured(json{{"guid", guid.ToString()},
                               {"path", relativePath},
                               {"name", clip.name},
                               {"duration", clip.duration},
                               {"frameRate", clip.frameRate},
                               {"looping", clip.looping},
                               {"extractRootMotion", clip.extractRootMotion},
                               {"rootBone", clip.rootBone},
                               {"tracks", std::move(tracks)},
                               {"rotationTracks", std::move(rotationTracks)},
                               {"note", "Rotation tracks are quaternion keys and cannot be "
                                        "edited channel by channel; animation.key_set only "
                                        "touches the scalar channels listed in \"tracks\"."}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["asset"],
  "properties": {
    "asset": {"type": "string", "description": "Clip GUID or project-relative path."}
  }
})";
    return tool;
}

ai::ToolDefinition AnimationKeySet(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "animation.key_set";
    tool.description =
        "Replace the keys on one scalar channel of one bone in a clip. Times are seconds. "
        "The track is created if it does not exist. Quaternion rotation tracks are refused: "
        "x, y, z and w are bound together by unit length, and editing one alone stops being "
        "a rotation.";
    // Dangerous karena tidak bisa di-undo: suntingan aset tidak melewati
    // CommandHistory, alasan yang sama dengan material dan partikel.
    tool.permission = ai::ToolPermission::Dangerous;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        const json arguments = ParseArguments(argumentsJson);
        animation::Clip clip;
        animation::ClipDocument document;
        std::filesystem::path absolute;
        Uuid guid;
        std::string relativePath;
        std::string failure;
        if (!OpenClip(app, arguments, clip, document, absolute, guid, relativePath, failure)) {
            return Text(failure, /*isError=*/true);
        }

        if (!arguments.contains("bone") || !arguments.at("bone").is_string() ||
            !arguments.contains("channel") || !arguments.at("channel").is_string()) {
            return Text("\"bone\" and \"channel\" are both required strings. Call "
                        "animation.clip_info for the names this clip uses.",
                        /*isError=*/true);
        }
        const std::string bone = arguments.at("bone").get<std::string>();
        const std::string channelName = arguments.at("channel").get<std::string>();
        const animation::Channel channel = animation::ChannelFromString(channelName);
        // `ChannelFromString` mengembalikan kanal pertama untuk teks yang tidak
        // dikenal, jadi ia diperiksa dengan menerjemahkannya balik — sebuah
        // salah ketik yang diam-diam jadi TranslationX adalah kunci yang
        // mendarat di kanal yang salah tanpa satu pun tanda.
        if (channelName != animation::ToString(channel)) {
            std::string known;
            for (const animation::Channel each :
                 {animation::Channel::TranslationX, animation::Channel::TranslationY,
                  animation::Channel::TranslationZ, animation::Channel::RotationX,
                  animation::Channel::RotationY, animation::Channel::RotationZ,
                  animation::Channel::ScaleX, animation::Channel::ScaleY,
                  animation::Channel::ScaleZ}) {
                known += (known.empty() ? "" : ", ");
                known += animation::ToString(each);
            }
            return Text("Unknown channel \"" + channelName + "\". Known: " + known + ".",
                        /*isError=*/true);
        }

        const auto keys = arguments.find("keys");
        if (keys == arguments.end() || !keys->is_array()) {
            return Text("\"keys\" is required: an array of {\"time\", \"value\"}.",
                        /*isError=*/true);
        }
        // Seluruhnya diperiksa sebelum track disentuh: menolak di tengah
        // meninggalkan kurva separuh terisi, yang lebih buruk daripada kurva
        // lama yang utuh.
        Curve replacement;
        for (const json& key : *keys) {
            if (!key.is_object() || !key.contains("time") || !key.contains("value")) {
                return Text("Every key needs \"time\" (seconds) and \"value\".",
                            /*isError=*/true);
            }
            CurveKey built;
            built.time = key.at("time").get<float>();
            built.value = key.at("value").get<float>();
            built.inTangent = key.value("in_tangent", 0.0f);
            built.outTangent = key.value("out_tangent", 0.0f);
            // Namanya diambil dari `InterpolationFromString`, bukan dari dua
            // bendera boolean: satu daftar nama berarti satu tempat yang harus
            // ikut berubah kalau modenya bertambah.
            const std::string mode = key.value("interpolation", std::string("bezier"));
            built.interpolation = InterpolationFromString(mode);
            if (Lowered(mode) != Lowered(ToString(built.interpolation))) {
                // `InterpolationFromString` memilih satu mode untuk teks yang
                // tidak dikenal, jadi salah ketik akan diam-diam mengubah bentuk
                // kurvanya. Diterjemahkan balik untuk memeriksanya.
                return Text("Unknown interpolation \"" + mode +
                                "\". Known: constant, linear, bezier.",
                            /*isError=*/true);
            }
            replacement.AddKey(built);
        }

        const int track = clip.EnsureTrack(bone, channel);
        clip.TrackAt(track).curve = std::move(replacement);
        if (arguments.contains("duration")) {
            clip.duration = std::max(arguments.at("duration").get<float>(), 0.0001f);
        }

        const animation::AnimationIoResult saved = animation::SaveClip(clip, document, absolute);
        if (!saved.ok) {
            return Text("That clip cannot be written: " + saved.error, /*isError=*/true);
        }
        RefreshAssets(app);

        return Structured(json{{"ok", true},
                               {"guid", guid.ToString()},
                               {"path", relativePath},
                               {"bone", bone},
                               {"channel", animation::ToString(channel)},
                               {"keys", clip.TrackAt(track).curve.Keys().size()},
                               {"duration", clip.duration}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["asset", "bone", "channel", "keys"],
  "properties": {
    "asset": {"type": "string", "description": "Clip GUID or project-relative path."},
    "bone": {"type": "string", "description": "Bone name, as animation.clip_info reports it."},
    "channel": {"type": "string", "description": "Scalar channel name, as animation.clip_info reports it."},
    "keys": {
      "type": "array",
      "description": "The whole channel, replacing whatever was there.",
      "items": {
        "type": "object",
        "required": ["time", "value"],
        "properties": {
          "time": {"type": "number", "description": "Seconds."},
          "value": {"type": "number"},
          "in_tangent": {"type": "number"}, "out_tangent": {"type": "number"},
          "interpolation": {"type": "string", "enum": ["constant", "linear", "bezier"], "description": "Default bezier."}
        }
      }
    },
    "duration": {"type": "number", "description": "Optional new clip length, seconds."}
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
    tools.Register(ParticleGet(app));
    tools.Register(ParticleSet(app));
    tools.Register(TerrainHeightmapGet(app));
    tools.Register(TerrainHeightmapSet(app));
    tools.Register(TerrainSculpt(app));
    tools.Register(AnimationClipInfo(app));
    tools.Register(AnimationKeySet(app));
    // **Tanpa penangkap layar, tool ini tidak didaftarkan sama sekali.** Ia
    // menempuh tangkapan jendela lalu dipotong — tidak ada jalur readback GPU di
    // engine ini — jadi editor tanpa kemampuan itu hanya bisa menawarkan tool
    // yang selalu gagal. Aturan yang sama dengan `editor.screenshot`.
    if (screenshot) {
        tools.Register(MaterialPreview(app, screenshot));
        tools.Register(ParticlePreview(app, screenshot));
        tools.Register(AnimationPreview(app, std::move(screenshot)));
    }
}

}  // namespace sim::editor
