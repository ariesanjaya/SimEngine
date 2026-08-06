#include "Sim/Vegetation/VegetationIo.h"

#include "Sim/Terrain/TerrainIo.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace sim::vegetation {
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

std::string CompanionName(const std::filesystem::path& path, const std::string& suffix) {
    return path.stem().string() + suffix;
}

/// Membaca gambar greyscale 8-bit apa adanya, tanpa menuntut ukurannya.
VegetationIoResult ReadMaskFile(const std::filesystem::path& path, std::vector<uint8_t>& values,
                                int& width, int& height) {
    VegetationIoResult result;
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 1);
    if (pixels == nullptr) {
        result.error = std::string("cannot read ") + path.string() + ": " + stbi_failure_reason();
        return result;
    }
    if (width <= 0 || height <= 0) {
        stbi_image_free(pixels);
        result.error = "image is empty: " + path.string();
        return result;
    }
    values.assign(pixels,
                  pixels + static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    stbi_image_free(pixels);
    result.ok = true;
    return result;
}

Json RulesToJson(const PlacementRules& rules) {
    Json entry;
    entry["minHeight"] = rules.minHeight;
    entry["maxHeight"] = rules.maxHeight;
    entry["minSlope"] = rules.minSlopeDegrees;
    entry["maxSlope"] = rules.maxSlopeDegrees;
    entry["terrainLayer"] = rules.terrainLayer;
    entry["minTerrainWeight"] = rules.minTerrainWeight;
    entry["minDistance"] = rules.minDistance;
    entry["density"] = rules.density;
    entry["seed"] = rules.seed;
    entry["avoidHoles"] = rules.avoidHoles;
    return entry;
}

PlacementRules RulesFromJson(const Json& entry) {
    PlacementRules rules;
    if (!entry.is_object()) {
        return rules;
    }
    rules.minHeight = entry.value("minHeight", rules.minHeight);
    rules.maxHeight = entry.value("maxHeight", rules.maxHeight);
    rules.minSlopeDegrees = entry.value("minSlope", rules.minSlopeDegrees);
    rules.maxSlopeDegrees = entry.value("maxSlope", rules.maxSlopeDegrees);
    rules.terrainLayer = entry.value("terrainLayer", rules.terrainLayer);
    rules.minTerrainWeight = entry.value("minTerrainWeight", rules.minTerrainWeight);
    rules.minDistance = entry.value("minDistance", rules.minDistance);
    rules.density = entry.value("density", rules.density);
    rules.seed = entry.value("seed", rules.seed);
    rules.avoidHoles = entry.value("avoidHoles", rules.avoidHoles);
    return rules;
}

}  // namespace

std::string SaveDocumentToString(const VegetationDocument& document,
                                 const Vegetation& vegetation,
                                 const std::vector<VegetationLayer>& layers) {
    Json root;
    root["version"] = kVegetationSchemaVersion;
    root["name"] = document.name;
    root["terrain"] = document.terrain.guid.ToString();
    root["densityCellSize"] = document.densityCellSize;

    Json array = Json::array();
    for (int index = 0; index < static_cast<int>(layers.size()); ++index) {
        const VegetationLayer& layer = layers[static_cast<std::size_t>(index)];
        Json entry;
        entry["name"] = layer.name;
        entry["model"] = layer.model.guid.ToString();
        entry["color"] = Json::array({layer.color.x, layer.color.y, layer.color.z});
        entry["minScale"] = layer.minScale;
        entry["maxScale"] = layer.maxScale;
        entry["randomYaw"] = layer.randomYaw;
        entry["alignToNormal"] = layer.alignToNormal;
        entry["offsetY"] = layer.offsetY;
        entry["lodDistance"] = layer.lodDistance;
        entry["billboardDistance"] = layer.billboardDistance;
        entry["cullDistance"] = layer.cullDistance;
        entry["visible"] = layer.visible;
        entry["rules"] = RulesToJson(layer.rules);
        entry["densitymap"] = layer.densityFile;

        // Suntingan tangan ditulis sebagai deret angka datar, bukan sebagai
        // objek per instance. Yang dihemat bukan keterbacaan — deretnya memang
        // lebih sulit dibaca mata — melainkan ukurannya: sebuah goresan
        // penghapus mengangkat ribuan instance sekaligus, dan setiap nama field
        // yang diulang di sana adalah puluhan kilobyte yang tidak membawa
        // informasi apa pun.
        Json added = Json::array();
        for (const Instance& instance : vegetation.Added(index)) {
            added.push_back(instance.position.x);
            added.push_back(instance.position.y);
            added.push_back(instance.position.z);
            added.push_back(instance.up.x);
            added.push_back(instance.up.y);
            added.push_back(instance.up.z);
            added.push_back(instance.yaw);
            added.push_back(instance.scale);
        }
        entry["added"] = std::move(added);

        // Kunci hapus dipecah menjadi dua bilangan bulat 32-bit, bukan ditulis
        // sebagai satu 64-bit. JSON tidak punya bilangan bulat 64-bit yang aman:
        // pembaca berbasis double kehilangan bit di atas 2^53, dan yang hilang
        // di situ bukan angka yang salah sedikit melainkan pohon yang salah.
        Json removed = Json::array();
        for (const uint64_t key : vegetation.Removed(index)) {
            int32_t x = 0;
            int32_t z = 0;
            UnpackKey(key, x, z);
            removed.push_back(x);
            removed.push_back(z);
        }
        entry["removed"] = std::move(removed);
        array.push_back(std::move(entry));
    }
    root["layers"] = std::move(array);
    return root.dump(2) + "\n";
}

VegetationIoResult LoadDocumentFromString(VegetationDocument& document, Vegetation& vegetation,
                                          const std::string& text) {
    VegetationIoResult result;
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

    document = VegetationDocument{};
    result.sourceVersion = root.value("version", kVegetationSchemaVersion);
    document.name = root.value("name", std::string{});
    document.terrain = AssetRef{Uuid::Parse(root.value("terrain", std::string{}))};
    document.densityCellSize =
        root.value("densityCellSize", Vegetation::kDefaultDensityCell);

    std::vector<VegetationLayer> layers;
    std::vector<std::vector<Instance>> added;
    std::vector<std::vector<uint64_t>> removed;
    if (const auto it = root.find("layers"); it != root.end() && it->is_array()) {
        for (const Json& entry : *it) {
            if (!entry.is_object()) {
                continue;
            }
            VegetationLayer layer;
            layer.name = entry.value("name", std::string{"Layer"});
            layer.model = AssetRef{Uuid::Parse(entry.value("model", std::string{}))};
            if (const auto color = entry.find("color");
                color != entry.end() && color->is_array() && color->size() == 3) {
                layer.color = Vec3((*color)[0].get<float>(), (*color)[1].get<float>(),
                                   (*color)[2].get<float>());
            }
            layer.minScale = entry.value("minScale", layer.minScale);
            layer.maxScale = entry.value("maxScale", layer.maxScale);
            layer.randomYaw = entry.value("randomYaw", layer.randomYaw);
            layer.alignToNormal = entry.value("alignToNormal", layer.alignToNormal);
            layer.offsetY = entry.value("offsetY", layer.offsetY);
            layer.lodDistance = entry.value("lodDistance", layer.lodDistance);
            layer.billboardDistance = entry.value("billboardDistance", layer.billboardDistance);
            layer.cullDistance = entry.value("cullDistance", layer.cullDistance);
            layer.visible = entry.value("visible", layer.visible);
            layer.densityFile = entry.value("densitymap", std::string{});
            if (const auto rules = entry.find("rules"); rules != entry.end()) {
                layer.rules = RulesFromJson(*rules);
            }

            std::vector<Instance> instances;
            if (const auto list = entry.find("added");
                list != entry.end() && list->is_array() && list->size() % 8 == 0) {
                for (std::size_t i = 0; i + 7 < list->size(); i += 8) {
                    Instance instance;
                    instance.position = Vec3((*list)[i].get<float>(), (*list)[i + 1].get<float>(),
                                             (*list)[i + 2].get<float>());
                    instance.up = Vec3((*list)[i + 3].get<float>(), (*list)[i + 4].get<float>(),
                                       (*list)[i + 5].get<float>());
                    instance.yaw = (*list)[i + 6].get<float>();
                    instance.scale = (*list)[i + 7].get<float>();
                    instance.manual = true;
                    instances.push_back(instance);
                }
            }
            std::vector<uint64_t> keys;
            if (const auto list = entry.find("removed");
                list != entry.end() && list->is_array() && list->size() % 2 == 0) {
                for (std::size_t i = 0; i + 1 < list->size(); i += 2) {
                    keys.push_back(
                        PackKey((*list)[i].get<int32_t>(), (*list)[i + 1].get<int32_t>()));
                }
            }

            layers.push_back(std::move(layer));
            added.push_back(std::move(instances));
            removed.push_back(std::move(keys));
        }
    }

    vegetation = Vegetation{};
    vegetation.SetDensityCellSize(document.densityCellSize);
    vegetation.SetLayers(layers);
    for (int index = 0; index < vegetation.LayerCount(); ++index) {
        vegetation.SetManual(index, added[static_cast<std::size_t>(index)],
                             removed[static_cast<std::size_t>(index)]);
    }
    result.ok = true;
    return result;
}

VegetationIoResult SaveVegetation(const Vegetation& vegetation,
                                  const VegetationDocument& document,
                                  const std::filesystem::path& path) {
    VegetationDocument copy = document;
    copy.densityCellSize = vegetation.DensityCellSize();

    // Hanya deskriptornya yang disalin, bukan seluruh `Vegetation`-nya: sejuta
    // instance adalah puluhan megabyte yang tidak ada hubungannya dengan nama
    // berkas yang sedang dibangkitkan di sini.
    std::vector<VegetationLayer> layers;
    layers.reserve(static_cast<std::size_t>(vegetation.LayerCount()));
    for (int index = 0; index < vegetation.LayerCount(); ++index) {
        VegetationLayer layer = vegetation.Layer(index);
        if (!vegetation.Density(index).Painted()) {
            // Peta yang belum pernah dicat tidak punya berkas, dan karena itu
            // tidak punya nama berkas.
            layer.densityFile.clear();
            layers.push_back(std::move(layer));
            continue;
        }
        if (layer.densityFile.empty()) {
            layer.densityFile = CompanionName(path, "_d" + std::to_string(index) + ".png");
        }
        const VegetationIoResult density =
            SaveDensityPng(vegetation, index, path.parent_path() / layer.densityFile);
        if (!density.ok) {
            return density;
        }
        layers.push_back(std::move(layer));
    }

    VegetationIoResult result;
    const std::string text = SaveDocumentToString(copy, vegetation, layers);
    result.ok = WriteFile(path, text.data(), text.size(), result.error);
    return result;
}

VegetationIoResult LoadVegetation(Vegetation& vegetation, VegetationDocument& document,
                                  const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        VegetationIoResult result;
        result.error = "cannot open " + path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    VegetationIoResult result = LoadDocumentFromString(document, vegetation, buffer.str());
    if (!result.ok) {
        return result;
    }
    for (int index = 0; index < vegetation.LayerCount(); ++index) {
        if (vegetation.Layer(index).densityFile.empty()) {
            continue;
        }
        const VegetationIoResult density =
            LoadDensityPng(vegetation, index, path.parent_path() / vegetation.Layer(index).densityFile);
        if (!density.ok) {
            return density;
        }
    }
    return result;
}

VegetationIoResult SaveDensityPng(const Vegetation& vegetation, int layer,
                                  const std::filesystem::path& path) {
    VegetationIoResult result;
    const DensityMap& map = vegetation.Density(layer);
    if (!map.Painted()) {
        result.error = "layer has no painted density map";
        return result;
    }
    const std::vector<unsigned char> png =
        terrain::EncodeMaskPng(map.Cells().data(), map.Width(), map.Height());
    if (png.empty()) {
        result.error = "cannot encode " + path.string();
        return result;
    }
    result.ok = WriteFile(path, png.data(), png.size(), result.error);
    return result;
}

VegetationIoResult LoadDensityPng(Vegetation& vegetation, int layer,
                                  const std::filesystem::path& path) {
    std::vector<uint8_t> values;
    int width = 0;
    int height = 0;
    VegetationIoResult result = ReadMaskFile(path, values, width, height);
    if (!result.ok) {
        return result;
    }
    result.ok = false;

    if (vegetation.DensityWidth() <= 0 || vegetation.DensityHeight() <= 0) {
        vegetation.SetDensityGrid(width, height);
    } else if (width != vegetation.DensityWidth() || height != vegetation.DensityHeight()) {
        // **Ditolak di sini, dicuplik ulang di `ImportDensityPng`.** Jalur ini
        // membaca berkas pendamping yang ditulis editor sendiri, jadi ukuran
        // yang tidak cocok bukan gambar yang perlu disesuaikan melainkan tanda
        // bahwa berkasnya milik dokumen lain — dan mencuplik ulang milik dokumen
        // lain menghasilkan sesuatu yang terlihat masuk akal dan tetap salah.
        result.error = "density map is " + std::to_string(width) + "x" +
                       std::to_string(height) + ", grid expects " +
                       std::to_string(vegetation.DensityWidth()) + "x" +
                       std::to_string(vegetation.DensityHeight());
        return result;
    }
    vegetation.Density(layer).SetCells(values.data());
    result.ok = true;
    return result;
}

VegetationIoResult ImportDensityPng(Vegetation& vegetation, int layer,
                                    const std::filesystem::path& path) {
    std::vector<uint8_t> values;
    int width = 0;
    int height = 0;
    VegetationIoResult result = ReadMaskFile(path, values, width, height);
    if (!result.ok) {
        return result;
    }
    result.sourceWidth = width;
    result.sourceHeight = height;
    result.ok = false;

    if (vegetation.DensityWidth() <= 0 || vegetation.DensityHeight() <= 0) {
        // Belum ada kisi berarti belum ada terrain yang menentukannya; gambarnya
        // yang menentukan, sama seperti pada pemuatan berkas.
        vegetation.SetDensityGrid(width, height);
    }
    const int targetWidth = vegetation.DensityWidth();
    const int targetHeight = vegetation.DensityHeight();
    if (targetWidth <= 0 || targetHeight <= 0) {
        result.error = "density grid has no size yet";
        return result;
    }

    std::vector<uint8_t> resampled(static_cast<std::size_t>(targetWidth) *
                                   static_cast<std::size_t>(targetHeight));
    const auto source = [&](int x, int y) {
        const int cx = std::clamp(x, 0, width - 1);
        const int cy = std::clamp(y, 0, height - 1);
        return static_cast<float>(
            values[static_cast<std::size_t>(cy) * static_cast<std::size_t>(width) +
                   static_cast<std::size_t>(cx)]);
    };
    // Bilinear, dan sudut kisi dipetakan ke sudut gambar — bukan pusat piksel ke
    // pusat sel. Mask menutupi dunia dari tepi ke tepi, jadi yang harus berimpit
    // adalah tepinya; menyelaraskan pusat piksel menggeser seluruh mask setengah
    // sel, dan setengah sel di tepi hutan terlihat.
    const float scaleX = targetWidth > 1 ? static_cast<float>(width - 1) /
                                               static_cast<float>(targetWidth - 1)
                                         : 0.0f;
    const float scaleY = targetHeight > 1 ? static_cast<float>(height - 1) /
                                                static_cast<float>(targetHeight - 1)
                                          : 0.0f;
    for (int y = 0; y < targetHeight; ++y) {
        const float fy = static_cast<float>(y) * scaleY;
        const int y0 = static_cast<int>(std::floor(fy));
        const float ty = fy - static_cast<float>(y0);
        for (int x = 0; x < targetWidth; ++x) {
            const float fx = static_cast<float>(x) * scaleX;
            const int x0 = static_cast<int>(std::floor(fx));
            const float tx = fx - static_cast<float>(x0);
            const float mixed =
                (source(x0, y0) * (1.0f - tx) + source(x0 + 1, y0) * tx) * (1.0f - ty) +
                (source(x0, y0 + 1) * (1.0f - tx) + source(x0 + 1, y0 + 1) * tx) * ty;
            resampled[static_cast<std::size_t>(y) * static_cast<std::size_t>(targetWidth) +
                      static_cast<std::size_t>(x)] =
                static_cast<uint8_t>(std::clamp(mixed + 0.5f, 0.0f, 255.0f));
        }
    }
    vegetation.Density(layer).SetCells(resampled.data());
    result.ok = true;
    return result;
}

}  // namespace sim::vegetation
