#include "Sim/AIBridge/ToolRegistry.h"
#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Assets/AssetTypes.h"
#include "Sim/Editor/AiTools.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/EditorContext.h"
#include "Sim/ImageIO/ImageIO.h"
#include "Sim/ImageIO/MipChain.h"
#include "Sim/Material/MaterialGraph.h"
#include "Sim/Material/MaterialNodeCatalog.h"
#include "Sim/Scene/AssetUsage.h"
#include "Sim/Scene/Project.h"
#include "Sim/Scene/World.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
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

ai::ToolResult Structured(const json& value) {
    ai::ToolResult result;
    result.text = value.dump(2);
    return result;
}

std::string Lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}


/// Memindai ulang indeks aset sekarang juga.
///
/// **Sinkron, bukan menunggu pemantau berkas.** Kriteria terima A2 nomor 3
/// menuntut aset yang dibuat agen muncul di Asset Browser tanpa restart, dan
/// agen yang membuat lalu langsung menanyakan GUID-nya akan mendapat "tidak ada"
/// bila indeksnya baru menyusul beberapa ratus milidetik kemudian.
void RefreshAssets(EditorApp& app) {
    if (assets::AssetDatabase* database = app.Context().assets) {
        database->ScanNow();
    }
}

/// Jalur sebuah berkas relatif terhadap folder aset, memakai '/' — bentuk yang
/// dipakai `AssetRecord::relativePath`.
std::string RelativeToAssets(EditorApp& app, const std::filesystem::path& absolute) {
    std::error_code code;
    const std::filesystem::path relative = std::filesystem::relative(
        absolute, std::filesystem::weakly_canonical(app.AssetsDirectory(), code), code);
    if (code || relative.empty()) {
        return {};
    }
    return relative.generic_string();
}

// --- pengurungan jalur ---------------------------------------------------------

/// Jalur di dalam project yang boleh disentuh, atau kegagalan yang menyebut
/// sebabnya.
struct ResolvedPath {
    std::filesystem::path absolute;
    std::string error;

    bool ok() const { return error.empty(); }
};

/// Menyelesaikan sebuah jalur relatif terhadap akar project, dan menolak apa pun
/// yang keluar darinya.
///
/// **Diselesaikan lewat sistem berkas, bukan lewat teks.** Membuang `..` secara
/// leksikal sudah menutup `../../etc/passwd`, tapi tidak menutup sebuah symlink
/// di dalam project yang menunjuk ke luar — jalur teksnya tetap tampak berada di
/// dalam, dan yang terbaca adalah berkas di seberang. `weakly_canonical`
/// mengikuti symlink, jadi yang dibandingkan adalah tempat berkasnya benar-benar
/// berada.
///
/// **Jalur absolut ditolak seluruhnya, bahkan yang kebetulan berada di dalam
/// project.** Menerimanya berarti agen harus tahu di mana project ini tersimpan
/// di mesin ini, dan pengetahuan itu tidak pernah dibutuhkan untuk apa pun yang
/// sah — yang membutuhkannya adalah percobaan menyentuh sesuatu yang lain.
ResolvedPath ResolveInsideProject(const std::filesystem::path& root,
                                  const std::string& relative) {
    ResolvedPath resolved;
    if (root.empty()) {
        resolved.error = "No project is open.";
        return resolved;
    }
    if (relative.empty()) {
        resolved.error = "The path is empty.";
        return resolved;
    }
    if (std::filesystem::path(relative).is_absolute()) {
        resolved.error = "Give a path relative to the project, not an absolute one.";
        return resolved;
    }

    std::error_code code;
    const std::filesystem::path canonicalRoot = std::filesystem::weakly_canonical(root, code);
    if (code) {
        resolved.error = "The project folder cannot be resolved.";
        return resolved;
    }
    const std::filesystem::path candidate =
        std::filesystem::weakly_canonical(root / relative, code);
    if (code) {
        resolved.error = "That path cannot be resolved.";
        return resolved;
    }

    // Dibandingkan sebagai urutan komponen, bukan sebagai awalan string:
    // "/home/arie/Proyek" adalah awalan string dari "/home/arie/ProyekLain",
    // dan perbandingan teks akan menerimanya.
    auto rootPart = canonicalRoot.begin();
    auto candidatePart = candidate.begin();
    for (; rootPart != canonicalRoot.end(); ++rootPart, ++candidatePart) {
        if (candidatePart == candidate.end() || *candidatePart != *rootPart) {
            resolved.error = "That path is outside the project folder.";
            return resolved;
        }
    }
    resolved.absolute = candidate;
    return resolved;
}

// --- project.info ---------------------------------------------------------------

ai::ToolDefinition ProjectInfo(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "project.info";
    tool.description =
        "What project is open and how it is laid out: its folders, its levels, and how many "
        "assets are indexed. Paths are relative to the project — that is the only form the "
        "file tools accept.";
    tool.permission = ai::ToolPermission::Read;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view) {
        if (!app.HasProject()) {
            return Text("No project is open.", /*isError=*/true);
        }
        const scene::Project& project = app.CurrentProject();

        json levels = json::array();
        std::error_code code;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(app.LevelsDirectory(), code)) {
            if (entry.is_regular_file() && entry.path().extension() == ".simlevel") {
                levels.push_back(entry.path().stem().string());
            }
        }
        std::sort(levels.begin(), levels.end());

        const assets::AssetDatabase* database = app.Context().assets;
        return Structured(json{
            {"name", project.name},
            // Akar absolutnya disebut supaya manusia bisa menemukannya, tapi
            // tool berkas tetap hanya menerima jalur relatif.
            {"root", project.root.string()},
            {"folders",
             json{{"assets", project.assetsPath},
                  {"levels", project.levelsPath},
                  {"prefabs", project.prefabsPath}}},
            {"levels", std::move(levels)},
            {"currentLevel", app.Context().levelName},
            {"assetCount", database != nullptr ? json(database->All().size()) : json(nullptr)}});
    };
    return tool;
}

// --- asset.search ----------------------------------------------------------------

json DescribeAsset(const assets::AssetRecord& record) {
    json described{{"guid", record.guid.ToString()},
                   {"name", record.name},
                   {"path", record.relativePath},
                   {"type", assets::ToString(record.type)},
                   {"bytes", record.fileSize}};
    // Keadaan impor disebut hanya bila ia bukan "siap": aset yang gagal diimpor
    // terlihat persis seperti aset yang baik dari daftar namanya saja, dan agen
    // yang memakainya akan menyalahkan tempat lain.
    if (record.state != assets::ImportState::Ready) {
        described["importState"] = record.state == assets::ImportState::Failed ? "failed"
                                   : record.state == assets::ImportState::Importing ? "importing"
                                                                                    : "pending";
        if (!record.error.empty()) {
            described["importError"] = record.error;
        }
    }
    if (record.type == assets::AssetType::Texture && record.width > 0) {
        described["size"] = json{{"width", record.width}, {"height", record.height},
                                 {"channels", record.channels}};
    }
    return described;
}

ai::ToolDefinition AssetSearch(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "asset.search";
    tool.description =
        "Find assets in the open project by name substring, type, or folder. Returns GUIDs and "
        "project-relative paths, paginated. Use this to find something by what it is rather "
        "than by remembering its filename.";
    tool.permission = ai::ToolPermission::Read;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        const assets::AssetDatabase* database = app.Context().assets;
        if (database == nullptr || !app.HasProject()) {
            return Text("No project is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        const std::string name = Lowered(arguments.value("name", std::string{}));
        const std::string folder = arguments.value("folder", std::string{});
        const std::string wantedType = Lowered(arguments.value("type", std::string{}));

        const std::size_t limit =
            static_cast<std::size_t>(std::clamp(arguments.value("limit", 50), 1, 500));
        const std::size_t offset =
            static_cast<std::size_t>(std::max(arguments.value("offset", 0), 0));

        std::vector<const assets::AssetRecord*> matched;
        for (const assets::AssetRecord& record : database->All()) {
            if (!name.empty() && Lowered(record.name).find(name) == std::string::npos) {
                continue;
            }
            if (!wantedType.empty() &&
                Lowered(assets::ToString(record.type)) != wantedType) {
                continue;
            }
            if (!folder.empty() && record.relativePath.rfind(folder, 0) != 0) {
                continue;
            }
            matched.push_back(&record);
        }

        json results = json::array();
        for (std::size_t i = offset; i < matched.size() && results.size() < limit; ++i) {
            results.push_back(DescribeAsset(*matched[i]));
        }
        return Structured(json{{"total", matched.size()},
                               {"offset", offset},
                               {"returned", results.size()},
                               {"assets", std::move(results)}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "properties": {
    "name": {"type": "string", "description": "Case-insensitive substring of the asset name."},
    "type": {"type": "string", "description": "Asset type, e.g. \"Texture\", \"Mesh\", \"Material\"."},
    "folder": {"type": "string", "description": "Project-relative folder prefix, e.g. \"Textures/\"."},
    "limit": {"type": "integer", "minimum": 1, "maximum": 500, "description": "Default 50."},
    "offset": {"type": "integer", "minimum": 0}
  }
})";
    return tool;
}

// --- asset.info --------------------------------------------------------------------

ai::ToolDefinition AssetInfo(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "asset.info";
    tool.description =
        "Everything known about one asset, including which entities in the open level use it "
        "and which other assets refer to it. Check this before deleting or replacing "
        "something.";
    tool.permission = ai::ToolPermission::Read;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        const assets::AssetDatabase* database = app.Context().assets;
        if (database == nullptr || !app.HasProject()) {
            return Text("No project is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);

        const assets::AssetRecord* record = nullptr;
        if (arguments.contains("asset") && arguments.at("asset").is_string()) {
            const std::string key = arguments.at("asset").get<std::string>();
            // GUID atau jalur relatif: agen menemukan aset lewat `asset.search`,
            // yang mengembalikan keduanya, dan memaksanya memilih satu berarti
            // memaksanya mengingat mana yang diterima tool ini.
            record = database->Find(Uuid::Parse(key));
            if (record == nullptr) {
                record = database->FindByRelativePath(key);
            }
        }
        if (record == nullptr) {
            return Text("No asset with that GUID or path. Use asset.search to find one.",
                        /*isError=*/true);
        }

        json described = DescribeAsset(*record);
        described["absolutePath"] = database->AbsolutePath(*record).string();

        json usedBy = json::array();
        if (const scene::World* world = app.Context().world) {
            for (const scene::Entity entity : scene::EntitiesUsingAsset(*world, record->guid)) {
                usedBy.push_back(json{{"guid", world->GuidOf(entity).ToString()},
                                      {"name", world->NameOf(entity)}});
            }
        }
        described["usedByEntities"] = std::move(usedBy);

        // Pemakai di luar scene yang terbuka — material yang merujuk tekstur,
        // prefab yang merujuk mesh. Tanpa ini, "tidak ada yang memakainya"
        // berarti "tidak ada yang memakainya di level yang kebetulan terbuka",
        // dan itu jawaban yang menyesatkan justru saat seseorang hendak
        // menghapus sesuatu.
        json externalUsers = json::array();
        if (app.Context().findExternalAssetUsers) {
            for (const std::string& user : app.Context().findExternalAssetUsers(record->guid)) {
                externalUsers.push_back(user);
            }
        }
        described["usedByAssets"] = std::move(externalUsers);
        return Structured(described);
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

// --- file.read ------------------------------------------------------------------------

ai::ToolDefinition FileRead(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "file.read";
    tool.description =
        "Read a text file inside the project — a Lua script, a material, a level. The path is "
        "relative to the project folder; anything outside it is refused.";
    tool.permission = ai::ToolPermission::Read;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        if (!app.HasProject()) {
            return Text("No project is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        if (!arguments.contains("path") || !arguments.at("path").is_string()) {
            return Text("\"path\" is required and must be a string.", /*isError=*/true);
        }
        const ResolvedPath resolved = ResolveInsideProject(
            app.CurrentProject().root, arguments.at("path").get<std::string>());
        if (!resolved.ok()) {
            return Text(resolved.error, /*isError=*/true);
        }
        std::error_code code;
        if (!std::filesystem::is_regular_file(resolved.absolute, code)) {
            return Text("That path is not a file.", /*isError=*/true);
        }

        // **Dibatasi, dan batasnya disebut saat tercapai.** Berkas puluhan
        // megabyte yang ditarik utuh menghabiskan konteks agen untuk sesuatu
        // yang hampir pasti tidak ia butuhkan seluruhnya — dan pemotongan yang
        // tidak disebut membuatnya menyimpulkan berkasnya memang sependek itu.
        constexpr std::uintmax_t kMaxBytes = 256u * 1024u;
        const std::uintmax_t size = std::filesystem::file_size(resolved.absolute, code);
        std::ifstream file(resolved.absolute, std::ios::binary);
        if (!file) {
            return Text("That file cannot be opened.", /*isError=*/true);
        }
        std::string contents(std::min<std::uintmax_t>(size, kMaxBytes), '\0');
        file.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        contents.resize(static_cast<std::size_t>(file.gcount()));

        json result{{"path", arguments.at("path")},
                    {"bytes", size},
                    {"text", contents}};
        if (size > kMaxBytes) {
            result["truncated"] = true;
            result["note"] = "Only the first " + std::to_string(kMaxBytes) +
                             " bytes are returned.";
        }
        return Structured(result);
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["path"],
  "properties": {
    "path": {"type": "string", "description": "Project-relative path, e.g. \"Assets/Scripts/player.lua\"."}
  }
})";
    return tool;
}

// --- file.write -------------------------------------------------------------------

ai::ToolDefinition FileWrite(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "file.write";
    tool.description =
        "Write a text file inside the project. Creates parent folders. The path is relative to "
        "the project folder; anything outside it is refused. Existing files are only replaced "
        "when you say so.";
    // **Dangerous, dan bukan karena jalurnya bisa keluar project — itu sudah
    // ditutup.** Ia tidak melewati Command, jadi tidak ada undo yang bisa
    // mengembalikan berkas yang tertimpa. Yang tidak bisa dibatalkan tidak boleh
    // berjalan tanpa persetujuan.
    tool.permission = ai::ToolPermission::Dangerous;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        if (!app.HasProject()) {
            return Text("No project is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        if (!arguments.contains("path") || !arguments.at("path").is_string() ||
            !arguments.contains("text") || !arguments.at("text").is_string()) {
            return Text("\"path\" and \"text\" are both required strings.", /*isError=*/true);
        }
        const ResolvedPath resolved = ResolveInsideProject(
            app.CurrentProject().root, arguments.at("path").get<std::string>());
        if (!resolved.ok()) {
            return Text(resolved.error, /*isError=*/true);
        }

        std::error_code code;
        const bool existed = std::filesystem::exists(resolved.absolute, code);
        if (existed && !arguments.value("overwrite", false)) {
            // Menimpa diam-diam adalah cara paling sunyi kehilangan pekerjaan.
            return Text("That file already exists. Pass \"overwrite\": true to replace it.",
                        /*isError=*/true);
        }
        if (existed && !std::filesystem::is_regular_file(resolved.absolute, code)) {
            return Text("That path is not a file.", /*isError=*/true);
        }

        std::filesystem::create_directories(resolved.absolute.parent_path(), code);
        std::ofstream file(resolved.absolute, std::ios::binary | std::ios::trunc);
        if (!file) {
            return Text("That file cannot be written.", /*isError=*/true);
        }
        const std::string& text = arguments.at("text").get_ref<const std::string&>();
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.close();

        RefreshAssets(app);
        return Structured(json{{"path", arguments.at("path")},
                               {"bytes", text.size()},
                               {"replaced", existed}});
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["path", "text"],
  "properties": {
    "path": {"type": "string", "description": "Project-relative path."},
    "text": {"type": "string"},
    "overwrite": {"type": "boolean", "description": "Required to replace a file that exists. Default false."}
  }
})";
    return tool;
}

// --- asset.create -----------------------------------------------------------------

ai::ToolDefinition AssetCreate(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "asset.create";
    tool.description =
        "Create a new asset in the project: a material, or a Lua script. The result is a valid "
        "asset ready to edit, not an empty file — and it appears in the Asset Browser "
        "immediately.";
    tool.permission = ai::ToolPermission::Write;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        if (!app.HasProject()) {
            return Text("No project is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        if (!arguments.contains("type") || !arguments.at("type").is_string() ||
            !arguments.contains("path") || !arguments.at("path").is_string()) {
            return Text("\"type\" and \"path\" are both required.", /*isError=*/true);
        }
        const std::string kind = Lowered(arguments.at("type").get<std::string>());
        std::string relative = arguments.at("path").get<std::string>();

        const char* extension = kind == "material" ? ".simmat" : kind == "script" ? ".lua" : nullptr;
        if (extension == nullptr) {
            return Text("Unknown asset type \"" + kind + "\". Known: material, script.",
                        /*isError=*/true);
        }
        // Ekstensi ditambahkan hanya bila belum ada: "Batu.simmat" tidak sedang
        // meminta "Batu.simmat.simmat".
        if (std::filesystem::path(relative).extension() != extension) {
            relative += extension;
        }

        const ResolvedPath resolved = ResolveInsideProject(app.CurrentProject().root, relative);
        if (!resolved.ok()) {
            return Text(resolved.error, /*isError=*/true);
        }
        std::error_code code;
        if (std::filesystem::exists(resolved.absolute, code)) {
            return Text("Something already exists at that path.", /*isError=*/true);
        }
        std::filesystem::create_directories(resolved.absolute.parent_path(), code);

        if (kind == "material") {
            // **Material kosong yang sudah sah, bukan berkas kosong.** Satu
            // simpul keluaran permukaan, sama dengan yang dibuat menu editor —
            // berkas tanpa isi baru menjadi material sesudah disunting, dan yang
            // membukanya sebelum itu akan mengira asetnya rusak.
            material::MaterialGraph graph;
            material::MaterialNode output;
            output.guid = Uuid::Generate();
            output.type = std::string(material::kSurfaceOutputType);
            output.position = Vec2(240.0f, 80.0f);
            graph.nodes.push_back(std::move(output));
            if (!material::SaveMaterialToFile(graph, resolved.absolute).ok) {
                return Text("Could not write the material.", /*isError=*/true);
            }
        } else {
            std::ofstream file(resolved.absolute, std::ios::binary | std::ios::trunc);
            if (!file) {
                return Text("Could not write the script.", /*isError=*/true);
            }
            const std::string body = arguments.value("text", std::string{});
            file << (body.empty() ? "-- " + resolved.absolute.stem().string() + "\n" : body);
        }

        RefreshAssets(app);
        json created{{"path", relative}, {"type", kind}};
        // GUID-nya disebut kalau indeksnya sudah melihatnya: itu yang dibutuhkan
        // panggilan berikutnya, dan tanpanya agen harus mencarinya sendiri.
        if (const assets::AssetDatabase* database = app.Context().assets) {
            const std::string underAssets = RelativeToAssets(app, resolved.absolute);
            if (const assets::AssetRecord* record =
                    database->FindByRelativePath(underAssets)) {
                created["guid"] = record->guid.ToString();
            }
        }
        return Structured(created);
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["type", "path"],
  "properties": {
    "type": {"type": "string", "enum": ["material", "script"]},
    "path": {"type": "string", "description": "Project-relative path. The extension is added if you leave it off."},
    "text": {"type": "string", "description": "Initial contents. Scripts only."}
  }
})";
    return tool;
}

// --- asset.import ------------------------------------------------------------------

ai::ToolDefinition AssetImport(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "asset.import";
    tool.description =
        "Copy a file from somewhere on this machine into the project's asset folder. This is "
        "the one tool whose source is deliberately outside the project; the destination is "
        "not.";
    tool.permission = ai::ToolPermission::Dangerous;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        if (!app.HasProject()) {
            return Text("No project is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        if (!arguments.contains("source") || !arguments.at("source").is_string()) {
            return Text("\"source\" is required: a path on this machine to copy from.",
                        /*isError=*/true);
        }
        const std::filesystem::path source = arguments.at("source").get<std::string>();
        std::error_code code;
        if (!std::filesystem::is_regular_file(source, code)) {
            return Text("There is no file at that source path.", /*isError=*/true);
        }

        // Tujuan bawaannya folder aset dengan nama berkas aslinya. Yang
        // menyebutkan `destination` tetap dikurung seperti tool berkas lain —
        // sumbernya boleh di luar, tujuannya tidak pernah.
        std::string relative = arguments.value("destination", std::string{});
        if (relative.empty()) {
            relative = app.CurrentProject().assetsPath + "/" + source.filename().string();
        }
        const ResolvedPath resolved = ResolveInsideProject(app.CurrentProject().root, relative);
        if (!resolved.ok()) {
            return Text(resolved.error, /*isError=*/true);
        }
        if (std::filesystem::exists(resolved.absolute, code) &&
            !arguments.value("overwrite", false)) {
            return Text("Something already exists at that destination. Pass \"overwrite\": true "
                        "to replace it.",
                        /*isError=*/true);
        }

        std::filesystem::create_directories(resolved.absolute.parent_path(), code);
        std::filesystem::copy_file(source, resolved.absolute,
                                   std::filesystem::copy_options::overwrite_existing, code);
        if (code) {
            return Text("Could not copy the file: " + code.message(), /*isError=*/true);
        }

        RefreshAssets(app);
        json imported{{"source", source.string()},
                      {"path", relative},
                      {"bytes", std::filesystem::file_size(resolved.absolute, code)}};
        if (const assets::AssetDatabase* database = app.Context().assets) {
            const std::string underAssets = RelativeToAssets(app, resolved.absolute);
            if (const assets::AssetRecord* record = database->FindByRelativePath(underAssets)) {
                imported["guid"] = record->guid.ToString();
                imported["type"] = assets::ToString(record->type);
            }
        }
        return Structured(imported);
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["source"],
  "properties": {
    "source": {"type": "string", "description": "Absolute path on this machine to copy from."},
    "destination": {"type": "string", "description": "Project-relative destination. Defaults to the asset folder, keeping the file name."},
    "overwrite": {"type": "boolean", "description": "Required to replace an existing file. Default false."}
  }
})";
    return tool;
}

// --- asset.thumbnail ----------------------------------------------------------------

ai::ToolDefinition AssetThumbnail(EditorApp& app) {
    ai::ToolDefinition tool;
    tool.name = "asset.thumbnail";
    tool.description =
        "A small picture of a texture asset, so you can see what it looks like before using it. "
        "Only textures: everything else would need the renderer, and this tool says so rather "
        "than sending you a blank image.";
    tool.permission = ai::ToolPermission::Read;
    tool.needsMainThread = true;
    tool.handler = [&app](std::string_view argumentsJson) {
        const assets::AssetDatabase* database = app.Context().assets;
        if (database == nullptr || !app.HasProject()) {
            return Text("No project is open.", /*isError=*/true);
        }
        const json arguments = ParseArguments(argumentsJson);
        if (!arguments.contains("asset") || !arguments.at("asset").is_string()) {
            return Text("\"asset\" is required: a GUID or project-relative path.",
                        /*isError=*/true);
        }
        const std::string key = arguments.at("asset").get<std::string>();
        const assets::AssetRecord* record = database->Find(Uuid::Parse(key));
        if (record == nullptr) {
            record = database->FindByRelativePath(key);
        }
        if (record == nullptr) {
            return Text("No asset with that GUID or path. Use asset.search to find one.",
                        /*isError=*/true);
        }
        if (record->type != assets::AssetType::Texture) {
            // **Ditolak dengan menyebut jenisnya.** Gambar kosong yang dikirim
            // sebagai thumbnail membuat agen menyimpulkan asetnya yang kosong.
            return Text(std::string("Only textures have a thumbnail here; \"") + record->name +
                            "\" is a " + assets::ToString(record->type) +
                            ". Use viewport.capture to look at something in the scene.",
                        /*isError=*/true);
        }

        imageio::Image source;
        imageio::ReadOptions options;
        // Empat kanal apa pun isi berkasnya: rantai mip dan penyandi PNG
        // keduanya bekerja pada bentuk yang seragam, dan tekstur satu kanal yang
        // dikirim apa adanya terbaca sebagai gambar abu-abu yang salah.
        options.channels = 4;
        options.type = imageio::PixelType::UInt8;
        const std::filesystem::path path = database->AbsolutePath(*record);
        const imageio::ImageIoResult read = imageio::Read(path, options, source);
        if (!read.ok) {
            return Text("Could not read that texture: " + read.error, /*isError=*/true);
        }

        const uint32_t wanted = static_cast<uint32_t>(
            std::clamp(arguments.value("size", 256), 16, 1024));
        imageio::MipOptions mip;
        // Tekstur di sini dianggap warna kecuali namanya menyebut sebaliknya.
        // Menyaring warna di ruang non-linear menggelapkan setiap level, dan
        // yang terlihat hanyalah thumbnail yang lebih gelap dari asetnya.
        mip.usage = imageio::TextureUsage::Color;
        const std::vector<imageio::Image> chain = imageio::BuildMipChain(source, mip);
        const imageio::Image* chosen = &source;
        for (const imageio::Image& level : chain) {
            // Level terkecil yang masih setidaknya sebesar yang diminta:
            // mengecilkan lebih jauh membuang detail yang justru dicari, dan
            // mengirim level yang lebih besar membuang byte.
            if (std::max(level.desc.width, level.desc.height) >= wanted) {
                chosen = &level;
            }
        }

        std::vector<uint8_t> png;
        const imageio::ImageIoResult encoded = imageio::Encode(*chosen, ".png", png);
        if (!encoded.ok) {
            return Text("Could not encode the thumbnail: " + encoded.error, /*isError=*/true);
        }

        ai::ToolResult result;
        result.text = record->name + " — " + std::to_string(chosen->desc.width) + "x" +
                      std::to_string(chosen->desc.height) + " (source " +
                      std::to_string(source.desc.width) + "x" +
                      std::to_string(source.desc.height) + ")";
        result.imageBytes = std::move(png);
        result.imageMimeType = "image/png";
        return result;
    };
    tool.inputSchemaJson = R"({
  "type": "object",
  "required": ["asset"],
  "properties": {
    "asset": {"type": "string", "description": "Asset GUID or project-relative path."},
    "size": {"type": "integer", "minimum": 16, "maximum": 1024, "description": "Longest edge, roughly. Default 256."}
  }
})";
    return tool;
}

}  // namespace

std::string ResolveProjectPathForTest(const std::filesystem::path& root,
                                      const std::string& relative,
                                      std::filesystem::path& absolute) {
    const ResolvedPath resolved = ResolveInsideProject(root, relative);
    absolute = resolved.absolute;
    return resolved.error;
}

void RegisterAssetTools(ai::ToolRegistry& tools, EditorApp& app) {
    tools.Register(ProjectInfo(app));
    tools.Register(AssetSearch(app));
    tools.Register(AssetInfo(app));
    tools.Register(FileRead(app));
    tools.Register(FileWrite(app));
    tools.Register(AssetCreate(app));
    tools.Register(AssetImport(app));
    tools.Register(AssetThumbnail(app));
}

}  // namespace sim::editor
