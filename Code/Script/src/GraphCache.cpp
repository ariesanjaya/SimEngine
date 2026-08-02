#include "Sim/Script/GraphCache.h"

#include "Sim/Core/Log.h"
#include "Sim/Script/Graph.h"

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

std::filesystem::path GraphCache::EnsureCompiled(const Uuid& guid,
                                                 const std::filesystem::path& source) {
    if (directory_.empty()) {
        return {};
    }
    const std::filesystem::path output = PathFor(guid);

    std::error_code ec;
    const auto sourceTime = std::filesystem::last_write_time(source, ec);
    if (ec) {
        SIM_WARN("Lua", "Graph {} is missing", source.string());
        return {};
    }
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
    Graph graph;
    const GraphIoResult read = LoadGraphFromFile(graph, source);
    if (!read.ok) {
        CompileResult failure;
        failure.errors.push_back({Uuid{}, read.error});
        results_[guid] = std::move(failure);
        SIM_ERROR("Lua", "{}: {}", source.filename().string(), read.error);
        return {};
    }

    CompileResult result = CompileGraph(graph, source.filename().string());
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
