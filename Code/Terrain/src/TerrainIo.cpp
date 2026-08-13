#include "Sim/Terrain/TerrainIo.h"

#include "Sim/Core/Log.h"
#include "Sim/ImageIO/ImageIO.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
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

/// Nama berkas pendamping baku. Diturunkan dari nama dokumen supaya seluruh
/// berkas satu terrain berkerabat namanya di folder, dan tetap dicatat di JSON
/// supaya pemuatnya tidak perlu menebak.
std::string CompanionName(const std::filesystem::path& path, const std::string& suffix) {
    return path.stem().string() + suffix;
}

/// Membungkus sampel menjadi gambar satu kanal untuk diserahkan ke `Sim::ImageIO`.
///
/// Menyalin, bukan menunjuk. Salinan seukuran heightmap memang tidak gratis,
/// tapi jalur ini berjalan sekali per simpan — dan alternatifnya adalah
/// antarmuka gambar yang memegang pointer milik orang lain, yang harganya
/// dibayar di setiap pemakaian berikutnya.
imageio::Image WrapSamples(const void* data, int width, int height, imageio::PixelType type) {
    imageio::ImageDesc desc;
    desc.width = static_cast<uint32_t>(width);
    desc.height = static_cast<uint32_t>(height);
    desc.channels = 1;
    desc.type = type;
    imageio::Image image;
    image.Allocate(desc);
    std::memcpy(image.bytes.data(), data, image.bytes.size());
    return image;
}

/// Mengubah heightmap float menjadi sampel 16-bit, dan **mencatat bagaimana**.
///
/// Dua sumber yang sama-sama sah datang sebagai TIFF float dan artinya berbeda:
/// World Machine dan Gaea mengekspor 0..1 ternormalisasi, sementara DEM dari
/// sumber GIS berisi meter di atas permukaan laut. Menjepit keduanya ke 0..1
/// akan meratakan yang kedua menjadi dataran tinggi seragam — tanpa satu pun
/// galat, dan terlihat seperti data yang memang datar.
///
/// Jadi rentangnya dibaca dari isinya, lalu dipetakan, lalu **dicatat**. Yang
/// tidak boleh terjadi adalah kuantisasi yang tidak disebut di mana pun.
void QuantiseFloatHeights(const float* values, std::size_t count, const std::string& what,
                          std::vector<Sample>& samples) {
    float low = values[0];
    float high = values[0];
    for (std::size_t i = 1; i < count; ++i) {
        low = std::min(low, values[i]);
        high = std::max(high, values[i]);
    }

    const bool normalised = low >= 0.0f && high <= 1.0f;
    const float scale = normalised ? 1.0f : (high > low ? 1.0f / (high - low) : 0.0f);
    const float bias = normalised ? 0.0f : -low;

    samples.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float unit = std::clamp((values[i] + bias) * scale, 0.0f, 1.0f);
        samples[i] = static_cast<Sample>(std::lround(unit * static_cast<float>(kSampleMax)));
    }

    if (normalised) {
        SIM_INFO("Terrain", "{}: float heights 0..1 quantised to 16-bit ({} levels)", what,
                 static_cast<int>(kSampleMax) + 1);
    } else {
        SIM_INFO("Terrain",
                 "{}: float heights {:.4g}..{:.4g} rescaled to the terrain's full 16-bit range; "
                 "set the terrain's min/max height to match the source units",
                 what, static_cast<double>(low), static_cast<double>(high));
    }
}

TerrainIoResult ReadMaskPng(const std::filesystem::path& path, std::vector<uint8_t>& values,
                            int& width, int& height) {
    TerrainIoResult result;
    imageio::ReadOptions options;
    options.channels = 1;
    options.type = imageio::PixelType::UInt8;

    imageio::Image image;
    const imageio::ImageIoResult decoded = imageio::Read(path, options, image);
    if (!decoded) {
        result.error = decoded.error;
        return result;
    }
    width = static_cast<int>(image.desc.width);
    height = static_cast<int>(image.desc.height);
    values = std::move(image.bytes);
    result.ok = true;
    return result;
}

/// Ukuran peta pendamping harus cocok dengan terrainnya, dengan alasan yang sama
/// seperti heightmap: menskala ulang diam-diam menghasilkan sesuatu yang terlihat
/// masuk akal dan tetap salah.
bool SizeMatches(const Terrain& terrain, int width, int height, TerrainIoResult& result) {
    if (width == terrain.SamplesX() && height == terrain.SamplesY()) {
        return true;
    }
    result.ok = false;
    result.error = "map is " + std::to_string(width) + "x" + std::to_string(height) +
                   ", terrain expects " + std::to_string(terrain.SamplesX()) + "x" +
                   std::to_string(terrain.SamplesY());
    return false;
}

}  // namespace

std::string SaveDocumentToString(const TerrainDocument& document,
                                 const std::vector<TerrainLayer>& layers) {
    Json root;
    root["version"] = kTerrainSchemaVersion;
    root["name"] = document.name;
    root["heightmap"] = document.heightmapFile;
    root["holes"] = document.holeFile;
    root["tileSamples"] = document.desc.tileSamples;
    root["tilesX"] = document.desc.tilesX;
    root["tilesY"] = document.desc.tilesY;
    root["sampleSpacing"] = document.desc.sampleSpacing;
    root["minHeight"] = document.desc.minHeight;
    root["maxHeight"] = document.desc.maxHeight;
    root["baseHeight"] = document.desc.baseHeight;

    Json array = Json::array();
    for (const TerrainLayer& layer : layers) {
        Json entry;
        entry["name"] = layer.name;
        entry["material"] = layer.material.guid.ToString();
        entry["color"] = Json::array({layer.color.x, layer.color.y, layer.color.z});
        entry["tileSize"] = layer.tileSize;
        entry["weightmap"] = layer.weightFile;
        array.push_back(std::move(entry));
    }
    root["layers"] = std::move(array);
    return root.dump(2) + "\n";
}

TerrainIoResult LoadDocumentFromString(TerrainDocument& document,
                                       std::vector<TerrainLayer>& layers,
                                       const std::string& text) {
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
    document.holeFile = root.value("holes", std::string{});
    document.desc.tileSamples = root.value("tileSamples", 512);
    document.desc.tilesX = root.value("tilesX", 4);
    document.desc.tilesY = root.value("tilesY", 4);
    document.desc.sampleSpacing = root.value("sampleSpacing", 1.0f);
    document.desc.minHeight = root.value("minHeight", 0.0f);
    document.desc.maxHeight = root.value("maxHeight", 1000.0f);
    document.desc.baseHeight = root.value("baseHeight", 0.0f);

    layers.clear();
    if (const auto it = root.find("layers"); it != root.end() && it->is_array()) {
        for (const Json& entry : *it) {
            if (!entry.is_object()) {
                continue;
            }
            TerrainLayer layer;
            layer.name = entry.value("name", std::string{"Layer"});
            layer.material = AssetRef{Uuid::Parse(entry.value("material", std::string{}))};
            if (const auto color = entry.find("color");
                color != entry.end() && color->is_array() && color->size() == 3) {
                layer.color = Vec3((*color)[0].get<float>(), (*color)[1].get<float>(),
                                   (*color)[2].get<float>());
            }
            layer.tileSize = entry.value("tileSize", 8.0f);
            layer.weightFile = entry.value("weightmap", std::string{});
            layers.push_back(std::move(layer));
        }
    }
    if (layers.empty()) {
        // Berkas versi 1 tidak punya daftar layer, dan berkas yang daftarnya
        // kosong adalah berkas yang rusak. Keduanya dilayani sama: satu layer
        // dasar, yang persis keadaan terrain versi 1.
        TerrainLayer base;
        base.name = "Base";
        layers.push_back(std::move(base));
    }
    result.ok = true;
    return result;
}

TerrainIoResult SaveTerrain(const Terrain& terrain, const TerrainDocument& document,
                            const std::filesystem::path& path) {
    TerrainDocument copy = document;
    if (copy.heightmapFile.empty()) {
        copy.heightmapFile = CompanionName(path, "_height.png");
    }
    // Deskriptornya selalu mencatat desc terrain yang sungguh disimpan, bukan
    // yang kebetulan ada di dokumen: keduanya berbeda berarti berkas yang dimuat
    // ulang tidak cocok dengan heightmap di sebelahnya.
    copy.desc = terrain.Desc();

    const TerrainIoResult heights = SaveHeightmapImage(terrain, path.parent_path() / copy.heightmapFile);
    if (!heights.ok) {
        return heights;
    }

    // Daftar layernya disalin, bukan disunting di tempat: nama berkas bawaan
    // dibangkitkan di sini, dan membangkitkannya ke dalam terrain berarti
    // menyimpan mengubah dokumen yang sedang dibuka.
    std::vector<TerrainLayer> layers;
    layers.reserve(static_cast<std::size_t>(terrain.LayerCount()));
    for (int index = 0; index < terrain.LayerCount(); ++index) {
        TerrainLayer layer = terrain.Layer(index);
        if (!terrain.LayerPainted(index)) {
            // Layer yang belum pernah dicat tidak punya berkas, dan karena itu
            // tidak punya nama berkas. Nama yang tetap dicatat adalah nama yang
            // menunjuk berkas yang tidak ada.
            layer.weightFile.clear();
            layers.push_back(std::move(layer));
            continue;
        }
        if (layer.weightFile.empty()) {
            layer.weightFile = CompanionName(path, "_w" + std::to_string(index) + ".png");
        }
        const TerrainIoResult weights =
            SaveWeightPng(terrain, index, path.parent_path() / layer.weightFile);
        if (!weights.ok) {
            return weights;
        }
        layers.push_back(std::move(layer));
    }

    if (terrain.HoleCount() == 0) {
        copy.holeFile.clear();
    } else {
        if (copy.holeFile.empty()) {
            copy.holeFile = CompanionName(path, "_holes.png");
        }
        const TerrainIoResult holes = SaveHolePng(terrain, path.parent_path() / copy.holeFile);
        if (!holes.ok) {
            return holes;
        }
    }

    TerrainIoResult result;
    const std::string text = SaveDocumentToString(copy, layers);
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

    std::vector<TerrainLayer> layers;
    TerrainIoResult result = LoadDocumentFromString(document, layers, buffer.str());
    if (!result.ok) {
        return result;
    }

    terrain = Terrain(document.desc);
    terrain.SetLayers(layers);

    if (!document.heightmapFile.empty()) {
        const TerrainIoResult heights =
            LoadHeightmapImage(terrain, path.parent_path() / document.heightmapFile);
        if (!heights.ok) {
            return heights;
        }
    }
    for (int index = 1; index < terrain.LayerCount(); ++index) {
        if (terrain.Layer(index).weightFile.empty()) {
            continue;
        }
        const TerrainIoResult weights =
            LoadWeightPng(terrain, index, path.parent_path() / terrain.Layer(index).weightFile);
        if (!weights.ok) {
            return weights;
        }
    }
    // Dinormalkan setelah SELURUH layer masuk, bukan per berkas. Dinormalkan per
    // berkas, layer yang dimuat lebih dulu akan menggerus layer berikutnya hanya
    // karena urutan bacanya — dan hasil pemuatan bergantung pada urutan berkas.
    terrain.NormalizeWeights();

    if (!document.holeFile.empty()) {
        const TerrainIoResult holes =
            LoadHolePng(terrain, path.parent_path() / document.holeFile);
        if (!holes.ok) {
            return holes;
        }
    }
    return result;
}

TerrainIoResult SaveWeightPng(const Terrain& terrain, int layer,
                              const std::filesystem::path& path) {
    TerrainIoResult result;
    std::vector<Weight> weights;
    terrain.ReadWeights(layer, weights);
    if (weights.empty()) {
        result.error = "layer " + std::to_string(layer) + " has no weight map";
        return result;
    }
    const std::vector<unsigned char> png =
        EncodeMaskPng(weights.data(), terrain.SamplesX(), terrain.SamplesY());
    if (png.empty()) {
        result.error = "PNG encode failed";
        return result;
    }
    result.ok = WriteFile(path, png.data(), png.size(), result.error);
    return result;
}

TerrainIoResult LoadWeightPng(Terrain& terrain, int layer, const std::filesystem::path& path) {
    std::vector<uint8_t> values;
    int width = 0;
    int height = 0;
    TerrainIoResult result = ReadMaskPng(path, values, width, height);
    if (!result.ok || !SizeMatches(terrain, width, height, result)) {
        return result;
    }
    terrain.WriteWeights(layer, values.data());
    return result;
}

TerrainIoResult SaveHolePng(const Terrain& terrain, const std::filesystem::path& path) {
    TerrainIoResult result;
    std::vector<uint8_t> holes;
    terrain.ReadHoles(holes);
    const std::vector<unsigned char> png =
        EncodeMaskPng(holes.data(), terrain.SamplesX(), terrain.SamplesY());
    if (png.empty()) {
        result.error = "PNG encode failed";
        return result;
    }
    result.ok = WriteFile(path, png.data(), png.size(), result.error);
    return result;
}

TerrainIoResult LoadHolePng(Terrain& terrain, const std::filesystem::path& path) {
    std::vector<uint8_t> values;
    int width = 0;
    int height = 0;
    TerrainIoResult result = ReadMaskPng(path, values, width, height);
    if (!result.ok || !SizeMatches(terrain, width, height, result)) {
        return result;
    }
    terrain.WriteHoles(values.data());
    return result;
}

TerrainIoResult SaveHeightmapImage(const Terrain& terrain, const std::filesystem::path& path) {
    TerrainIoResult result;
    std::vector<Sample> samples;
    terrain.ReadAll(samples);

    // Formatnya dari ekstensinya — PNG 16-bit lewat enkoder sendiri, TIFF
    // 16-bit lewat libtiff. Keduanya tanpa kehilangan, dan uji I3 menuntut
    // round-trip lewat keduanya identik bit per bit.
    const imageio::Image image = WrapSamples(samples.data(), terrain.SamplesX(),
                                             terrain.SamplesY(), imageio::PixelType::UInt16);
    const imageio::ImageIoResult written = imageio::Write(path, image);
    result.ok = written.ok;
    result.error = written.error;
    return result;
}

TerrainIoResult ReadHeightmapImage(const std::filesystem::path& path,
                                   std::vector<Sample>& samples, int& width, int& height) {
    TerrainIoResult result;
    // **Tipenya tidak dipaksa di sini.** Yang ada di berkas dibaca apa adanya,
    // lalu diterjemahkan di bawah — karena terjemahannya berbeda menurut
    // tipenya, dan menyerahkannya ke konversi umum akan menjepit heightmap
    // float bermeter ke 0..1 tanpa ada yang menyebutnya.
    imageio::ReadOptions options;
    options.channels = 1;

    imageio::Image image;
    const imageio::ImageIoResult decoded = imageio::Read(path, options, image);
    if (!decoded) {
        result.error = decoded.error;
        return result;
    }
    width = static_cast<int>(image.desc.width);
    height = static_cast<int>(image.desc.height);
    const std::size_t count = image.desc.SampleCount();

    switch (image.desc.type) {
        case imageio::PixelType::UInt16:
            samples.assign(image.AsU16(), image.AsU16() + count);
            break;
        case imageio::PixelType::UInt8: {
            // 8-bit dinaikkan, bukan ditolak: peta 8-bit tetap dipakai sebagai
            // sketsa kasar. Yang dijaga adalah bahwa putih tetap menjadi puncak
            // penuh — 255 × 257 = 65535, bukan 65280.
            const uint8_t* values = image.AsU8();
            samples.resize(count);
            for (std::size_t i = 0; i < count; ++i) {
                samples[i] = static_cast<Sample>(static_cast<uint32_t>(values[i]) * 257u);
            }
            SIM_INFO("Terrain", "{}: 8-bit heightmap widened to 16-bit; only 256 distinct heights",
                     path.string());
            break;
        }
        case imageio::PixelType::Float32:
            QuantiseFloatHeights(image.AsF32(), count, path.string(), samples);
            break;
    }
    result.ok = true;
    return result;
}

std::vector<unsigned char> EncodeHeightmapPng(const Sample* samples, int width, int height) {
    if (samples == nullptr || width <= 0 || height <= 0) {
        return {};
    }
    std::vector<unsigned char> bytes;
    const imageio::Image image =
        WrapSamples(samples, width, height, imageio::PixelType::UInt16);
    imageio::Encode(image, ".png", bytes);
    return bytes;
}

std::vector<unsigned char> EncodeMaskPng(const uint8_t* values, int width, int height) {
    if (values == nullptr || width <= 0 || height <= 0) {
        return {};
    }
    std::vector<unsigned char> bytes;
    const imageio::Image image = WrapSamples(values, width, height, imageio::PixelType::UInt8);
    imageio::Encode(image, ".png", bytes);
    return bytes;
}

TerrainIoResult LoadHeightmapImage(Terrain& terrain, const std::filesystem::path& path) {
    std::vector<Sample> samples;
    int width = 0;
    int height = 0;
    TerrainIoResult result = ReadHeightmapImage(path, samples, width, height);
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
