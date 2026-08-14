#include "Sim/Assets/TextureSettings.h"

#include "Sim/Core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <string>

namespace sim::assets {
namespace {

using Json = nlohmann::ordered_json;

constexpr const char* kExtension = ".simtexcfg";
constexpr int kSchemaVersion = 1;

/// Nama enum ditulis sebagai teks, bukan angka.
///
/// Berkas pengaturan ikut kontrol versi dan dibaca manusia saat menelusuri
/// perbedaan; `"usage": 1` menuntut membuka header untuk tahu artinya, dan
/// nomor yang bergeser saat sebuah nilai disisipkan mengubah arti setiap berkas
/// yang sudah ada tanpa satu pun tanda.
constexpr std::array<std::pair<TextureUsage, const char*>, 5> kUsageNames{{
    {TextureUsage::Color, "color"},
    {TextureUsage::NormalMap, "normal"},
    {TextureUsage::Mask, "mask"},
    {TextureUsage::Hdr, "hdr"},
    {TextureUsage::Height, "height"},
}};

constexpr std::array<std::pair<TextureQuality, const char*>, 3> kQualityNames{{
    {TextureQuality::Fast, "fast"},
    {TextureQuality::Balanced, "balanced"},
    {TextureQuality::Best, "best"},
}};

constexpr std::array<std::pair<TextureAlpha, const char*>, 3> kAlphaNames{{
    {TextureAlpha::None, "none"},
    {TextureAlpha::PunchThrough, "punchThrough"},
    {TextureAlpha::Full, "full"},
}};

template <typename Enum, std::size_t N>
Enum FromName(const std::array<std::pair<Enum, const char*>, N>& table, const std::string& text,
              Enum fallback) {
    for (const auto& [value, name] : table) {
        if (text == name) {
            return value;
        }
    }
    // Nama yang tidak dikenali jatuh ke bawaan, bukan menggagalkan pembacaan:
    // berkas dari versi yang lebih baru harus tetap bisa dibuka, dan satu medan
    // yang tidak terbaca bukan alasan membuang seluruh pengaturannya.
    return fallback;
}

std::string Lowercase(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

/// Apakah `stem` berakhir dengan salah satu akhiran, sebagai **kata**.
///
/// Sebagai kata, bukan sebagai potongan huruf: `_n` yang dicocokkan apa adanya
/// akan ikut mengenai `kayu_batan`, dan yang tertandai normal map adalah tekstur
/// warna yang lalu tampak biru pekat.
bool EndsWithToken(const std::string& stem, std::string_view token) {
    if (stem.size() < token.size()) {
        return false;
    }
    if (stem.compare(stem.size() - token.size(), token.size(), token) != 0) {
        return false;
    }
    // Yang tepat sepanjang akhirannya sendiri diterima; selebihnya harus dipisah
    // sebuah pemisah.
    if (stem.size() == token.size()) {
        return true;
    }
    const char before = stem[stem.size() - token.size() - 1];
    return before == '_' || before == '-' || before == '.';
}

}  // namespace

const char* ToString(TextureUsage usage) {
    for (const auto& [value, name] : kUsageNames) {
        if (value == usage) {
            return name;
        }
    }
    return "color";
}

const char* ToString(TextureQuality quality) {
    for (const auto& [value, name] : kQualityNames) {
        if (value == quality) {
            return name;
        }
    }
    return "balanced";
}

const char* ToString(TextureAlpha alpha) {
    for (const auto& [value, name] : kAlphaNames) {
        if (value == alpha) {
            return name;
        }
    }
    return "none";
}

bool TextureSettings::operator==(const TextureSettings& other) const {
    return usage == other.usage && quality == other.quality && alpha == other.alpha &&
           compress == other.compress && generateMips == other.generateMips;
}

TextureSettings DefaultTextureSettings(const std::filesystem::path& texturePath) {
    TextureSettings settings;
    settings.usage = GuessUsageFromName(texturePath.filename().string());
    return settings;
}

TextureUsage GuessUsageFromName(std::string_view fileName) {
    // Ekstensinya dibuang lebih dulu: `batu_n.png` berakhiran `.png`, dan yang
    // menebak dari nama utuh tidak akan pernah mengenali satu pun akhiran.
    std::string stem = Lowercase(fileName);
    const std::size_t dot = stem.rfind('.');
    if (dot != std::string::npos) {
        stem = stem.substr(0, dot);
    }

    // Urutannya menentukan: `_nrm` diperiksa sebelum `_n` hanya untuk kejelasan
    // — keduanya menjawab hal yang sama — sedangkan HDR diperiksa sebelum yang
    // lain karena berkas `.hdr` kerap dinamai `langit_hdr`.
    for (const std::string_view token : {"hdr", "hdri", "env"}) {
        if (EndsWithToken(stem, token)) {
            return TextureUsage::Hdr;
        }
    }
    for (const std::string_view token : {"n", "nrm", "norm", "normal", "normalmap"}) {
        if (EndsWithToken(stem, token)) {
            return TextureUsage::NormalMap;
        }
    }
    for (const std::string_view token :
         {"r", "rough", "roughness", "m", "metal", "metalness", "ao", "mask", "orm", "occlusion"}) {
        if (EndsWithToken(stem, token)) {
            return TextureUsage::Mask;
        }
    }
    for (const std::string_view token : {"h", "height", "disp", "displacement", "bump"}) {
        if (EndsWithToken(stem, token)) {
            return TextureUsage::Height;
        }
    }
    return TextureUsage::Color;
}

std::filesystem::path TextureSettingsPath(const std::filesystem::path& texturePath) {
    // Ditempelkan, bukan menggantikan ekstensinya — alasan yang sama dengan
    // `MeshSettingsPath`: `batu.png` dan `batu.tga` adalah dua aset yang
    // berbeda, dan mengganti ekstensi membuat keduanya berbagi satu berkas
    // pengaturan.
    return texturePath.string() + kExtension;
}

bool LoadTextureSettings(TextureSettings& settings, const std::filesystem::path& texturePath) {
    settings = DefaultTextureSettings(texturePath);
    std::ifstream stream(TextureSettingsPath(texturePath));
    if (!stream) {
        // Belum ada berarti bawaan, bukan galat: itu keadaan setiap tekstur yang
        // baru diimpor, dan melaporkannya sebagai kegagalan akan membuat setiap
        // pembaca harus membedakan dua hal yang tidak perlu dibedakan.
        return true;
    }

    Json root;
    try {
        stream >> root;
    } catch (const nlohmann::json::exception& error) {
        SIM_WARN("Assets", "Damaged texture settings for {}: {}", texturePath.string(),
                 error.what());
        return false;
    }

    // Bawaan tiap medan adalah nilai yang sudah terisi di atas — yaitu tebakan
    // namanya untuk `usage`. Berkas yang tidak menyebut sebuah medan berarti
    // "biarkan seperti yang berlaku", bukan "kembalikan ke nol".
    settings.usage =
        FromName(kUsageNames, root.value("usage", std::string(ToString(settings.usage))),
                 settings.usage);
    settings.quality = FromName(kQualityNames, root.value("quality", std::string("balanced")),
                                TextureQuality::Balanced);
    settings.alpha =
        FromName(kAlphaNames, root.value("alpha", std::string("none")), TextureAlpha::None);
    settings.compress = root.value("compress", true);
    settings.generateMips = root.value("generateMips", true);
    return true;
}

bool SaveTextureSettings(const TextureSettings& settings,
                         const std::filesystem::path& texturePath) {
    const std::filesystem::path path = TextureSettingsPath(texturePath);
    std::error_code error;

    if (settings == DefaultTextureSettings(texturePath)) {
        // Berkas yang tidak mengatur apa pun hanya menambah satu berkas yang
        // harus ikut kontrol versi. Menghapusnya juga yang membuat "kembalikan
        // ke bawaan" meninggalkan folder seperti sebelum ada yang menyentuhnya.
        std::filesystem::remove(path, error);
        return true;
    }

    // Seluruh medan ditulis, termasuk yang bernilai bawaan. Berkas yang hanya
    // memuat yang berbeda memaksa pembacanya tahu bawaan versi mana yang berlaku
    // — dan bawaan yang berubah di kemudian hari akan diam-diam mengubah arti
    // setiap berkas yang sudah ada.
    Json root;
    root["version"] = kSchemaVersion;
    root["usage"] = ToString(settings.usage);
    root["quality"] = ToString(settings.quality);
    root["alpha"] = ToString(settings.alpha);
    root["compress"] = settings.compress;
    root["generateMips"] = settings.generateMips;

    std::ofstream stream(path);
    if (!stream) {
        SIM_WARN("Assets", "Cannot write texture settings {}", path.string());
        return false;
    }
    stream << root.dump(2) << '\n';
    return static_cast<bool>(stream);
}

}  // namespace sim::assets
