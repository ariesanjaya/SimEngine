#include "Sim/Editor/ProjectLibrary.h"

#include "Sim/Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>

namespace sim::editor {
namespace {

using Json = nlohmann::ordered_json;

constexpr const char* kProjectFileName = "project.simproj";

/// Aksara yang ditolak sistem berkas Windows, ditambah pemisah jalur.
bool IsForbidden(char c) {
    const auto value = static_cast<unsigned char>(c);
    if (value < 0x20) {
        return true;
    }
    return std::string_view(R"(<>:"/\|?*)").find(c) != std::string_view::npos;
}

}  // namespace

bool RecentProject::Exists() const {
    std::error_code error;
    return !path.empty() && std::filesystem::exists(path / kProjectFileName, error);
}

std::string ProjectLibrary::SanitizeFolderName(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        out.push_back(IsForbidden(c) ? '_' : c);
    }
    // Spasi dan titik di **kedua** ujung dibuang. Di ujung belakang karena
    // Windows menghapusnya diam-diam saat membuat folder, jadi nama yang diminta
    // dan nama yang jadi akan berbeda — dan project yang foldernya bukan yang
    // tertulis di daftar tidak bisa dibuka lagi dari sana. Di ujung depan karena
    // titik di awal membuat folder tersembunyi di sistem mirip-Unix, dan karena
    // sebuah nama tidak boleh bisa menjadi `..`: pemisah jalurnya sudah diganti
    // di atas, dan ini yang menutup sisanya.
    const auto notTrimmable = [](char c) { return c != ' ' && c != '.'; };
    const auto begin = std::find_if(out.begin(), out.end(), notTrimmable);
    const auto end = std::find_if(out.rbegin(), out.rend(), notTrimmable).base();
    out = begin < end ? std::string(begin, end) : std::string{};
    return out;
}

void ProjectLibrary::Load(const std::filesystem::path& file) {
    recent_.clear();
    std::ifstream stream(file);
    if (!stream) {
        return;
    }
    Json root;
    try {
        stream >> root;
    } catch (const nlohmann::json::exception& error) {
        SIM_WARN("Editor", "Cannot read the project list {}: {}", file.string(), error.what());
        return;
    }
    const auto projects = root.find("projects");
    if (projects == root.end() || !projects->is_array()) {
        return;
    }
    for (const Json& entry : *projects) {
        if (!entry.is_object()) {
            continue;
        }
        RecentProject project;
        project.name = entry.value("name", std::string{});
        project.path = std::filesystem::path(entry.value("path", std::string{}));
        project.lastOpened = entry.value("lastOpened", int64_t{0});
        if (!project.path.empty()) {
            recent_.push_back(std::move(project));
        }
    }
    // Diurutkan saat dibaca, bukan dipercaya dari berkasnya: berkas yang
    // disunting tangan tetap harus menghasilkan daftar yang benar.
    std::stable_sort(recent_.begin(), recent_.end(),
                     [](const RecentProject& a, const RecentProject& b) {
                         return a.lastOpened > b.lastOpened;
                     });
}

bool ProjectLibrary::Save(const std::filesystem::path& file) const {
    Json root;
    root["version"] = 1;
    Json projects = Json::array();
    std::size_t written = 0;
    for (const RecentProject& project : recent_) {
        if (written++ >= kMaxRecent) {
            break;
        }
        projects.push_back(Json{{"name", project.name},
                                {"path", project.path.generic_string()},
                                {"lastOpened", project.lastOpened}});
    }
    root["projects"] = std::move(projects);

    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream stream(file);
    if (!stream) {
        SIM_WARN("Editor", "Cannot write the project list {}", file.string());
        return false;
    }
    stream << root.dump(2) << '\n';
    return stream.good();
}

void ProjectLibrary::Remember(const scene::Project& project, int64_t nowUnix) {
    if (project.root.empty()) {
        return;
    }
    std::error_code error;
    // Dinormalkan supaya "folder yang sama lewat jalur yang berbeda" tidak
    // menjadi dua entri. `weakly_canonical` tetap bekerja untuk jalur yang
    // belum ada, jadi ia aman dipanggil sebelum foldernya sempat dibuat.
    const std::filesystem::path root = std::filesystem::weakly_canonical(project.root, error);
    const std::filesystem::path key = error ? project.root : root;

    const auto same = [&key](const RecentProject& candidate) { return candidate.path == key; };
    recent_.erase(std::remove_if(recent_.begin(), recent_.end(), same), recent_.end());

    RecentProject entry;
    entry.name = project.name;
    entry.path = key;
    entry.lastOpened = nowUnix;
    recent_.insert(recent_.begin(), std::move(entry));
}

bool ProjectLibrary::Forget(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
    const std::filesystem::path key = error ? path : canonical;
    const auto tail = std::remove_if(recent_.begin(), recent_.end(),
                                     [&key](const RecentProject& candidate) {
                                         return candidate.path == key;
                                     });
    if (tail == recent_.end()) {
        return false;
    }
    recent_.erase(tail, recent_.end());
    return true;
}

bool ProjectLibrary::Create(const std::filesystem::path& parent, std::string_view name,
                            scene::Project& out, std::string& error) const {
    error.clear();
    const std::string folder = SanitizeFolderName(name);
    if (folder.empty()) {
        error = "the project name is empty once the characters a folder cannot hold are removed";
        return false;
    }

    const std::filesystem::path root = parent / folder;
    std::error_code code;
    if (std::filesystem::exists(root, code)) {
        error = root.string() + " already exists";
        return false;
    }

    scene::Project project;
    project.name = std::string(name);
    project.root = root;
    for (const std::filesystem::path& directory :
         {project.AssetsDirectory(), project.LevelsDirectory(), project.PrefabsDirectory()}) {
        std::filesystem::create_directories(directory, code);
        if (code) {
            error = "cannot create " + directory.string() + ": " + code.message();
            return false;
        }
    }
    if (!scene::SaveProject(project, root)) {
        error = "cannot write " + (root / kProjectFileName).string();
        return false;
    }
    out = std::move(project);
    return true;
}

bool ProjectLibrary::Open(const std::filesystem::path& path, scene::Project& out,
                          std::string& error) const {
    error.clear();
    if (path.empty()) {
        error = "no path given";
        return false;
    }
    if (!scene::LoadProject(out, path, error)) {
        return false;
    }
    // Folder isinya dibuat kalau belum ada. Project yang di-checkout dari
    // kontrol versi kerap kehilangan folder kosong — git tidak menyimpannya —
    // dan editor yang menolak membukanya karena itu menolak justru pada keadaan
    // yang paling lazim.
    std::error_code code;
    for (const std::filesystem::path& directory :
         {out.AssetsDirectory(), out.LevelsDirectory(), out.PrefabsDirectory()}) {
        std::filesystem::create_directories(directory, code);
    }
    return true;
}

}  // namespace sim::editor
