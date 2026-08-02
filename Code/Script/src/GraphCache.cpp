#include "Sim/Script/GraphCache.h"

#include "Sim/Core/Log.h"
#include "Sim/Script/Graph.h"

#include <algorithm>
#include <fstream>

namespace sim::script {

void GraphCache::Initialize(std::filesystem::path directory) {
    directory_ = std::move(directory);
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
}

std::string GraphCache::ChunkName(const std::filesystem::path& source) {
    return source.filename().string() + ".lua";
}

std::filesystem::path GraphCache::PathFor(const Uuid& guid) const {
    // Dinamai menurut GUID, bukan nama berkasnya: dua graph bernama sama di
    // folder berbeda tidak boleh saling menimpa hasil kompilasi, dan mengganti
    // nama graph tidak boleh membuang cache-nya.
    return directory_ / (guid.ToString() + ".lua");
}

const CompileResult* GraphCache::LastResult(const Uuid& guid) const {
    const auto it = results_.find(guid);
    return it == results_.end() ? nullptr : &it->second;
}

void GraphCache::SetSourceResolver(std::function<std::filesystem::path(const Uuid&)> resolver) {
    sourceResolver_ = std::move(resolver);
}

const Graph* GraphCache::Find(const Uuid& guid) const {
    if (const auto known = parsed_.find(guid); known != parsed_.end()) {
        return &known->second;
    }
    if (!sourceResolver_) {
        return nullptr;
    }
    const std::filesystem::path source = sourceResolver_(guid);
    if (source.empty()) {
        return nullptr;
    }
    Graph graph;
    if (!LoadGraphFromFile(graph, source).ok) {
        return nullptr;
    }
    return &parsed_.emplace(guid, std::move(graph)).first->second;
}

std::string GraphCache::NameOf(const Uuid& guid) const {
    if (!sourceResolver_) {
        return guid.ToString();
    }
    const std::filesystem::path source = sourceResolver_(guid);
    return source.empty() ? guid.ToString() : source.stem().string();
}

std::filesystem::file_time_type GraphCache::NewestSourceTime(
    const Uuid& guid, const std::filesystem::path& source, std::vector<Uuid>& seen) const {
    std::error_code ec;
    std::filesystem::file_time_type newest = std::filesystem::last_write_time(source, ec);
    if (ec) {
        return {};
    }
    // Lingkar antar-graph ditolak kompiler, tapi penelusuran waktu ini berjalan
    // SEBELUM kompilasi — jadi ia harus bisa berhenti sendiri.
    if (std::find(seen.begin(), seen.end(), guid) != seen.end()) {
        return newest;
    }
    seen.push_back(guid);

    const Graph* graph = Find(guid);
    if (graph == nullptr || !sourceResolver_) {
        return newest;
    }
    for (const GraphNode& node : graph->nodes) {
        if (node.type != "graph.call") {
            continue;
        }
        const Uuid referenced = Uuid::Parse(node.Setting("graph"));
        const std::filesystem::path path = sourceResolver_(referenced);
        if (path.empty()) {
            continue;
        }
        newest = std::max(newest, NewestSourceTime(referenced, path, seen));
    }
    return newest;
}

void GraphCache::SetBreakpoints(const Uuid& guid, std::vector<Uuid> nodes) {
    breakpoints_[guid] = std::move(nodes);
}

const std::vector<Uuid>& GraphCache::Breakpoints(const Uuid& guid) const {
    static const std::vector<Uuid> kNone;
    const auto it = breakpoints_.find(guid);
    return it == breakpoints_.end() ? kNone : it->second;
}

std::filesystem::path GraphCache::EnsureCompiled(const Uuid& guid,
                                                 const std::filesystem::path& source) {
    if (directory_.empty()) {
        return {};
    }
    const std::filesystem::path output = PathFor(guid);

    std::error_code ec;
    if (!std::filesystem::exists(source, ec)) {
        SIM_WARN("Lua", "Graph {} is missing", source.string());
        return {};
    }
    std::vector<Uuid> seen;
    const auto sourceTime = NewestSourceTime(guid, source, seen);
    const auto outputTime = std::filesystem::last_write_time(output, ec);
    // Hasil yang masih lebih baru dipakai apa adanya. Inilah yang membuat
    // memuat level tidak mengompilasi apa pun: yang berjalan adalah `.lua` yang
    // sudah ada di cache, sama seperti aset lain yang sudah diimpor.
    //
    // Hasil kompilasi terakhirnya tidak ikut dimuat ke `results_` — peta sumber
    // hanya dibutuhkan editor, dan editor akan mengompilasi ulang saat graph-nya
    // dibuka. Runtime cukup dengan berkasnya.
    if (!ec && outputTime >= sourceTime) {
        return output;
    }
    return Compile(guid, source);
}

std::filesystem::path GraphCache::Rebuild(const Uuid& guid,
                                          const std::filesystem::path& source) {
    return directory_.empty() ? std::filesystem::path{} : Compile(guid, source);
}

std::filesystem::path GraphCache::Compile(const Uuid& guid,
                                          const std::filesystem::path& source) {
    // Hasil urai lama dibuang: yang memicu kompilasi ulang justru berkas yang
    // berubah, dan memakai salinan lama berarti mengompilasi isi yang sudah
    // tidak ada.
    parsed_.clear();

    Graph graph;
    const GraphIoResult read = LoadGraphFromFile(graph, source);
    if (!read.ok) {
        CompileResult failure;
        failure.errors.push_back({Uuid{}, read.error});
        results_[guid] = std::move(failure);
        SIM_ERROR("Lua", "{}: {}", source.filename().string(), read.error);
        return {};
    }

    CompileOptions options;
    options.breakpoints = Breakpoints(guid);
    options.library = this;
    CompileResult result = CompileGraph(graph, source.filename().string(), options);
    if (!result.ok) {
        // Berkas lama sengaja TIDAK ditimpa saat kompilasi gagal. Menimpanya
        // dengan hasil setengah jadi akan membuat Play berikutnya gagal dengan
        // kesalahan Lua yang tidak ada hubungannya dengan yang sebenarnya salah.
        for (const CompileError& error : result.errors) {
            SIM_ERROR("Lua", "{}: {}", source.filename().string(), error.message);
        }
        results_[guid] = std::move(result);
        return {};
    }

    const std::filesystem::path output = PathFor(guid);
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file) {
        SIM_ERROR("Lua", "Cannot write compiled graph to {}", output.string());
        return {};
    }
    file << result.lua;
    file.close();

    SIM_INFO("Lua", "Compiled {} ({} lines)", source.filename().string(),
             result.sourceMap.empty() ? 0 : result.sourceMap.back().line);
    results_[guid] = std::move(result);
    return output;
}

}  // namespace sim::script
