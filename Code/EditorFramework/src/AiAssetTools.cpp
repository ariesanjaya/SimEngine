#include "Sim/AIBridge/ToolRegistry.h"
#include "Sim/Assets/AssetDatabase.h"
#include "Sim/Assets/AssetTypes.h"
#include "Sim/Editor/AiTools.h"
#include "Sim/Editor/EditorApp.h"
#include "Sim/Editor/EditorContext.h"
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
}

}  // namespace sim::editor
