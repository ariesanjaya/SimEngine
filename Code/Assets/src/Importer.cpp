#include "Sim/Assets/Importer.h"

#include "Sim/Core/Log.h"

#include <nlohmann/json.hpp>

// Jangan tambahkan STBI_NO_STDIO di sini: stb memeriksanya dengan #ifndef, jadi
// mendefinisikannya bahkan bernilai 0 akan mematikan stbi_info() dan seluruh
// jalur baca-dari-berkas.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <fstream>
#include <sstream>

namespace sim::assets {
namespace {

/// Tekstur: hanya membaca kepala berkasnya, bukan seluruh piksel.
///
/// Yang dibutuhkan indeks aset cuma dimensi dan jumlah kanal. Mendekode gambar
/// 8K hanya untuk mengisi dua angka di panel detail adalah pemborosan yang
/// langsung terasa saat folder berisi ribuan tekstur dipindai.
class TextureImporter final : public IImporter {
public:
    const char* Name() const override { return "Texture"; }
    bool Handles(AssetType type) const override { return type == AssetType::Texture; }

    ImportResult Import(const std::filesystem::path& path) const override {
        ImportResult result;
        int width = 0;
        int height = 0;
        int channels = 0;
        if (stbi_info(path.string().c_str(), &width, &height, &channels) == 0) {
            result.error = stbi_failure_reason() != nullptr ? stbi_failure_reason()
                                                            : "unrecognised image";
            return result;
        }
        result.ok = true;
        result.width = static_cast<uint32_t>(width);
        result.height = static_cast<uint32_t>(height);
        result.channels = static_cast<uint32_t>(channels);
        return result;
    }
};

/// Level, prefab, dan material: yang dicari adalah rujukan ke aset lain.
///
/// AssetRef diserialisasi sebagai string GUID, jadi setiap string yang berupa
/// GUID sah adalah calon ketergantungan. Yang bukan GUID aset akan tersaring
/// sendirinya nanti karena tidak ditemukan di indeks — termasuk GUID entity,
/// yang bentuknya sama persis. Menyaringnya di sini mustahil: importer berjalan
/// di thread latar dan tidak boleh melihat indeks yang sedang dibaca UI.
class DocumentImporter final : public IImporter {
public:
    const char* Name() const override { return "Document"; }
    bool Handles(AssetType type) const override {
        return type == AssetType::Level || type == AssetType::Prefab ||
               type == AssetType::Material || type == AssetType::Json;
    }

    ImportResult Import(const std::filesystem::path& path) const override {
        ImportResult result;
        std::ifstream stream(path);
        if (!stream) {
            result.error = "cannot open file";
            return result;
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();

        nlohmann::json document;
        try {
            document = nlohmann::json::parse(buffer.str());
        } catch (const nlohmann::json::exception& exception) {
            result.error = exception.what();
            return result;
        }

        CollectGuids(document, result.dependencies);
        result.ok = true;
        return result;
    }

private:
    static void CollectGuids(const nlohmann::json& node, std::vector<Uuid>& out) {
        if (node.is_string()) {
            const Uuid guid = Uuid::Parse(node.get<std::string>());
            if (guid.IsValid() &&
                std::find(out.begin(), out.end(), guid) == out.end()) {
                out.push_back(guid);
            }
            return;
        }
        if (node.is_object() || node.is_array()) {
            for (const auto& child : node) {
                CollectGuids(child, out);
            }
        }
    }
};

/// Segala yang lain: mesh, skrip, teks. Tidak ada yang perlu diurai di fase
/// editor — mesh baru benar-benar dibaca di E8, dan skrip di E6.
class PassThroughImporter final : public IImporter {
public:
    const char* Name() const override { return "Pass-through"; }
    bool Handles(AssetType type) const override {
        return type == AssetType::Mesh || type == AssetType::Script ||
               type == AssetType::Text || type == AssetType::Unknown;
    }

    ImportResult Import(const std::filesystem::path& path) const override {
        ImportResult result;
        std::error_code error;
        result.ok = std::filesystem::exists(path, error);
        if (!result.ok) {
            result.error = "file disappeared";
        }
        return result;
    }
};

}  // namespace

ImporterRegistry& ImporterRegistry::Get() {
    static ImporterRegistry registry;
    return registry;
}

void ImporterRegistry::Register(std::unique_ptr<IImporter> importer) {
    if (importer != nullptr) {
        importers_.push_back(std::move(importer));
    }
}

const IImporter* ImporterRegistry::Find(AssetType type) const {
    for (const std::unique_ptr<IImporter>& importer : importers_) {
        if (importer->Handles(type)) {
            return importer.get();
        }
    }
    return nullptr;
}

void ImporterRegistry::RegisterBuiltIn() {
    ImporterRegistry& registry = Get();
    if (!registry.importers_.empty()) {
        return;  // aman dipanggil dua kali
    }
    // Urutan penting: PassThrough menangani Unknown, jadi ia harus terakhir.
    registry.Register(std::make_unique<TextureImporter>());
    registry.Register(std::make_unique<DocumentImporter>());
    registry.Register(std::make_unique<PassThroughImporter>());
    SIM_INFO("Assets", "{} importers registered", registry.importers_.size());
}

}  // namespace sim::assets
