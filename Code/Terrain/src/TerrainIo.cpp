#include "Sim/Terrain/TerrainIo.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <fstream>
#include <sstream>

namespace sim::terrain {
namespace {

using Json = nlohmann::ordered_json;

bool WriteFile(const std::filesystem::path& path, const void* data, std::size_t bytes,
               std::string& error) {
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        error = "cannot open " + path.string();
        return false;
    }
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    if (!stream) {
        error = "write failed: " + path.string();
        return false;
    }
    return true;
}

}  // namespace

std::string SaveDocumentToString(const TerrainDocument& document) {
    Json root;
    root["version"] = kTerrainSchemaVersion;
    root["name"] = document.name;
    root["heightmap"] = document.heightmapFile;
    root["tileSamples"] = document.desc.tileSamples;
    root["tilesX"] = document.desc.tilesX;
    root["tilesY"] = document.desc.tilesY;
    root["sampleSpacing"] = document.desc.sampleSpacing;
    root["minHeight"] = document.desc.minHeight;
    root["maxHeight"] = document.desc.maxHeight;
    root["baseHeight"] = document.desc.baseHeight;
    return root.dump(2) + "\n";
}

TerrainIoResult LoadDocumentFromString(TerrainDocument& document, const std::string& text) {
    TerrainIoResult result;
    Json root;
    try {
        root = Json::parse(text);
    } catch (const nlohmann::json::exception& error) {
        result.error = error.what();
        return result;
    }
    if (!root.is_object()) {
        result.error = "root is not an object";
        return result;
    }

    document = TerrainDocument{};
    result.sourceVersion = root.value("version", kTerrainSchemaVersion);
    document.name = root.value("name", std::string{});
    document.heightmapFile = root.value("heightmap", std::string{});
    document.desc.tileSamples = root.value("tileSamples", 512);
    document.desc.tilesX = root.value("tilesX", 4);
    document.desc.tilesY = root.value("tilesY", 4);
    document.desc.sampleSpacing = root.value("sampleSpacing", 1.0f);
    document.desc.minHeight = root.value("minHeight", 0.0f);
    document.desc.maxHeight = root.value("maxHeight", 1000.0f);
    document.desc.baseHeight = root.value("baseHeight", 0.0f);
    result.ok = true;
    return result;
}

TerrainIoResult SaveTerrain(const Terrain& terrain, const TerrainDocument& document,
                            const std::filesystem::path& path) {
    TerrainDocument copy = document;
    if (copy.heightmapFile.empty()) {
        copy.heightmapFile = path.stem().string() + "_height.png";
    }
    // Deskriptornya selalu mencatat desc terrain yang sungguh disimpan, bukan
    // yang kebetulan ada di dokumen: keduanya berbeda berarti berkas yang dimuat
    // ulang tidak cocok dengan heightmap di sebelahnya.
    copy.desc = terrain.Desc();

    const TerrainIoResult heights = SaveHeightmapPng(terrain, path.parent_path() / copy.heightmapFile);
    if (!heights.ok) {
        return heights;
    }

    TerrainIoResult result;
    const std::string text = SaveDocumentToString(copy);
    result.ok = WriteFile(path, text.data(), text.size(), result.error);
    return result;
}

TerrainIoResult LoadTerrain(Terrain& terrain, TerrainDocument& document,
                            const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        TerrainIoResult result;
        result.error = "cannot open " + path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    TerrainIoResult result = LoadDocumentFromString(document, buffer.str());
    if (!result.ok) {
        return result;
    }

    terrain = Terrain(document.desc);
    if (document.heightmapFile.empty()) {
        return result;  // terrain rata, tanpa pendamping
    }
    const TerrainIoResult heights =
        LoadHeightmapPng(terrain, path.parent_path() / document.heightmapFile);
    if (!heights.ok) {
        return heights;
    }
    return result;
}

TerrainIoResult SaveHeightmapPng(const Terrain& terrain, const std::filesystem::path& path) {
    TerrainIoResult result;
    std::vector<Sample> samples;
    terrain.ReadAll(samples);
    const std::vector<unsigned char> png =
        EncodeHeightmapPng(samples.data(), terrain.SamplesX(), terrain.SamplesY());
    if (png.empty()) {
        result.error = "PNG encode failed";
        return result;
    }
    result.ok = WriteFile(path, png.data(), png.size(), result.error);
    return result;
}

TerrainIoResult ReadHeightmapPng(const std::filesystem::path& path, std::vector<Sample>& samples,
                                 int& width, int& height) {
    TerrainIoResult result;
    int channels = 0;
    // Dibaca dengan stb, bukan dengan dekoder tandingan buatan sendiri. Menulis
    // dan membaca dengan implementasi yang sama akan membuat round-trip lulus
    // walaupun berkasnya bukan PNG yang sah — yang diuji hanya konsistensi
    // dengan diri sendiri.
    stbi_us* pixels = stbi_load_16(path.string().c_str(), &width, &height, &channels, 1);
    if (pixels == nullptr) {
        result.error = std::string("cannot read ") + path.string() + ": " + stbi_failure_reason();
        return result;
    }
    samples.assign(pixels, pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    stbi_image_free(pixels);
    result.ok = true;
    return result;
}

TerrainIoResult LoadHeightmapPng(Terrain& terrain, const std::filesystem::path& path) {
    std::vector<Sample> samples;
    int width = 0;
    int height = 0;
    TerrainIoResult result = ReadHeightmapPng(path, samples, width, height);
    if (!result.ok) {
        return result;
    }
    if (width != terrain.SamplesX() || height != terrain.SamplesY()) {
        result.ok = false;
        result.error = "heightmap is " + std::to_string(width) + "x" + std::to_string(height) +
                       ", terrain expects " + std::to_string(terrain.SamplesX()) + "x" +
                       std::to_string(terrain.SamplesY());
        return result;
    }
    terrain.WriteAll(samples.data());
    return result;
}

TerrainIoResult SaveHeightmapRaw(const Terrain& terrain, const std::filesystem::path& path) {
    TerrainIoResult result;
    std::vector<Sample> samples;
    terrain.ReadAll(samples);
    // Little-endian eksplisit, bukan tata letak memori mesin ini. RAW ditukar
    // antar-alat; menuliskan endianness host membuat berkasnya hanya berlaku di
    // arsitektur tempat ia dibuat.
    std::vector<unsigned char> bytes(samples.size() * 2u);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        bytes[i * 2u] = static_cast<unsigned char>(samples[i] & 0xffU);
        bytes[i * 2u + 1u] = static_cast<unsigned char>(samples[i] >> 8);
    }
    result.ok = WriteFile(path, bytes.data(), bytes.size(), result.error);
    return result;
}

TerrainIoResult LoadHeightmapRaw(Terrain& terrain, const std::filesystem::path& path) {
    TerrainIoResult result;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        result.error = "cannot open " + path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string blob = buffer.str();

    const std::size_t expected = static_cast<std::size_t>(terrain.SamplesX()) *
                                 static_cast<std::size_t>(terrain.SamplesY()) * 2u;
    if (blob.size() != expected) {
        result.error = "raw heightmap is " + std::to_string(blob.size()) +
                       " bytes, terrain expects " + std::to_string(expected);
        return result;
    }

    std::vector<Sample> samples(expected / 2u);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<Sample>(static_cast<unsigned char>(blob[i * 2u]) |
                                         (static_cast<unsigned char>(blob[i * 2u + 1u]) << 8));
    }
    terrain.WriteAll(samples.data());
    result.ok = true;
    return result;
}

}  // namespace sim::terrain
