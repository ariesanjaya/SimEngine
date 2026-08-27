#include "Sim/Material/ShaderCache.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <system_error>

namespace sim::material {
namespace {

constexpr uint32_t kSpirvMagic = 0x07230203u;
/// Header SPIR-V: magic, versi, generator, bound, schema.
constexpr size_t kSpirvHeaderWords = 5;

/// FNV-1a 64-bit, dua lintasan dengan basis berbeda supaya kuncinya 128 bit.
///
/// **Lebarnya yang jadi soal, bukan kekuatan kriptografisnya.** Kunci cache yang
/// bertabrakan tidak pernah muncul sebagai galat: ia muncul sebagai material
/// yang memakai shader milik material lain, dan itu bug yang tidak akan pernah
/// dicurigai orang berasal dari cache. Dengan 128 bit peluangnya berhenti jadi
/// hal yang perlu dipikirkan; dengan 64 bit ia hanya sangat kecil, dan "sangat
/// kecil" tetap harus dijelaskan kepada orang yang menemukannya.
struct Hash128 {
    uint64_t lo = 0xcbf29ce484222325ull;
    uint64_t hi = 0x9e3779b97f4a7c15ull;

    void Feed(const void* data, size_t bytes) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < bytes; ++i) {
            lo = (lo ^ p[i]) * 0x100000001b3ull;
            hi = (hi ^ p[i]) * 0xc2b2ae3d27d4eb4full;
            hi = (hi << 7) | (hi >> 57);
        }
        // Panjangnya ikut, supaya "ab"+"c" dan "a"+"bc" tidak bisa bertemu di
        // kunci yang sama lewat penggabungan medan yang berbeda.
        const auto length = static_cast<uint64_t>(bytes);
        lo ^= length;
        hi ^= length * 0x100000001b3ull;
    }

    void Feed(std::string_view text) { Feed(text.data(), text.size()); }

    std::string Hex() const {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string out(32, '0');
        for (int i = 0; i < 16; ++i) {
            const uint64_t word = i < 8 ? hi : lo;
            const int shift = 60 - (i % 8) * 8;
            out[static_cast<size_t>(i) * 2] = kDigits[(word >> (shift + 4)) & 0xf];
            out[static_cast<size_t>(i) * 2 + 1] = kDigits[(word >> shift) & 0xf];
        }
        return out;
    }
};

/// Mengutip sebuah path untuk shell yang akan menjalankannya.
///
/// **Shell-nya berbeda, dan begitu juga aturan kutipnya.** `std::system`
/// menjalankan `/bin/sh` di POSIX dan `cmd /c` di Windows, dan cmd tidak
/// mengenal kutip tunggal sama sekali — `'C:\...\slangc.exe'` dibacanya sebagai
/// nama berkas yang memang mengandung apostrof, lalu menjawab "The filename,
/// directory name, or volume label syntax is incorrect". Yang terlihat dari
/// dalam editor hanyalah slangc yang seolah tidak ada.
#if defined(_WIN32)
std::string Quote(const std::filesystem::path& path) {
    // Windows melarang '"' di dalam nama berkas, jadi tidak ada yang perlu
    // di-escape di dalamnya — berbeda dengan POSIX di bawah.
    return "\"" + path.string() + "\"";
}
#else
std::string Quote(const std::filesystem::path& path) {
    std::string out = "'";
    for (const char c : path.string()) {
        // Kutip tunggal di dalam kutip tunggal harus keluar dulu. Nama direktori
        // yang mengandung apostrof jarang, tapi kalau terjadi ia menjadi
        // perintah shell yang terpotong — bukan pesan galat.
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += '\'';
    return out;
}
#endif

/// Menjalankan perintah shell, mengembalikan kode keluarnya.
///
/// **Bungkus kutip tambahan di Windows bukan hiasan.** `cmd /c` membuang kutip
/// pertama dan kutip terakhir dari seluruh baris begitu baris itu diawali kutip
/// dan memuat karakter khusus seperti `>` — jadi `"exe" -v > "log"` sampai ke
/// cmd sebagai `exe" -v > log`, yang bukan perintah apa pun. Membungkusnya
/// sekali lagi membuat yang dibuang justru bungkus itu, dan yang tersisa persis
/// perintah yang dimaksud.
int RunShell(const std::string& command) {
#if defined(_WIN32)
    return std::system(("\"" + command + "\"").c_str());
#else
    return std::system(command.c_str());
#endif
}

std::string ReadTextFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

std::string Trim(std::string text) {
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    return text;
}

#if defined(_WIN32)
constexpr const char* kExeSuffix = ".exe";
// **Pemisah PATH, dan di Windows ia bukan ':'.** Memakai ':' di sana tidak
// sekadar gagal memecah — ia memecah di tempat yang salah, karena setiap path
// Windows memuat ':' pada huruf drive-nya. `C:\VulkanSDK\1.4.357.0\Bin` menjadi
// potongan `C` dan `\VulkanSDK\1.4.357.0\Bin`, dan tidak satu pun dari keduanya
// menunjuk ke mana-mana. Akibatnya slangc tak pernah ditemukan walau ia ada di
// PATH, dan yang terbaca hanyalah mesh yang digambar dengan shader cadangan.
constexpr char kPathSeparator = ';';
#else
constexpr const char* kExeSuffix = "";
constexpr char kPathSeparator = ':';
#endif

std::filesystem::path ResolveSlangc(const std::filesystem::path& hint) {
    std::error_code ec;
    if (!hint.empty()) {
        return std::filesystem::is_regular_file(hint, ec) ? hint : std::filesystem::path{};
    }
    const std::string name = std::string("slangc") + kExeSuffix;
    if (const char* sdk = std::getenv("VULKAN_SDK"); sdk != nullptr && *sdk != '\0') {
        const std::filesystem::path candidate = std::filesystem::path(sdk) / "bin" / name;
        if (std::filesystem::is_regular_file(candidate, ec)) {
            return candidate;
        }
    }
    if (const char* path = std::getenv("PATH"); path != nullptr) {
        std::string_view rest(path);
        while (!rest.empty()) {
            const size_t split = rest.find(kPathSeparator);
            const std::string_view entry = rest.substr(0, split);
            if (!entry.empty()) {
                const std::filesystem::path candidate = std::filesystem::path(entry) / name;
                if (std::filesystem::is_regular_file(candidate, ec)) {
                    return candidate;
                }
            }
            if (split == std::string_view::npos) {
                break;
            }
            rest.remove_prefix(split + 1);
        }
    }
    return {};
}

}  // namespace

const char* ToString(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:
            return "vertex";
        case ShaderStage::Fragment:
            return "fragment";
    }
    return "fragment";
}

std::array<uint32_t, 3> ShaderVariant::Constants() const {
    return {skinned ? 1u : 0u, instanced ? 1u : 0u, alphaTest ? 1u : 0u};
}

uint32_t ShaderVariant::Mask() const {
    return (skinned ? 1u : 0u) | (instanced ? 2u : 0u) | (alphaTest ? 4u : 0u);
}

bool LooksLikeSpirv(const std::vector<uint32_t>& words) {
    return words.size() >= kSpirvHeaderWords && words[0] == kSpirvMagic;
}

void ShaderCache::Configure(std::filesystem::path directory, std::string compilerIdentity) {
    directory_ = std::move(directory);
    compilerIdentity_ = std::move(compilerIdentity);
    if (!directory_.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(directory_, ec);
        if (ec) {
            SIM_WARN("Material", "shader cache directory '{}' unusable: {}", directory_.string(),
                     ec.message());
            directory_.clear();
        }
    }
}

void ShaderCache::SetCompiler(Compiler compiler) { compiler_ = std::move(compiler); }

std::string ShaderCache::KeyOf(const CompileRequest& request) const {
    Hash128 hash;
    // Setiap medan diberi makan terpisah, dan panjangnya ikut dihitung di dalam
    // Feed — jadi memindahkan satu karakter dari entry point ke sumbernya tidak
    // menghasilkan kunci yang sama.
    hash.Feed(compilerIdentity_);
    hash.Feed(ToString(request.stage));
    hash.Feed(request.entryPoint);
    hash.Feed(request.source);
    return hash.Hex();
}

std::filesystem::path ShaderCache::PathFor(const std::string& key) const {
    return directory_ / (key + ".spv");
}

bool ShaderCache::Load(const std::string& key, std::vector<uint32_t>& spirv) {
    if (directory_.empty()) {
        return false;
    }
    const std::filesystem::path path = PathFor(key);
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return false;
    }
    // Berkas yang bukan kelipatan 4 byte tidak mungkin SPIR-V yang utuh, dan
    // membacanya sebagai larik kata akan diam-diam membuang sisanya.
    if (size == 0 || size % sizeof(uint32_t) != 0) {
        ++stats_.rejected;
        std::filesystem::remove(path, ec);
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    spirv.assign(static_cast<size_t>(size) / sizeof(uint32_t), 0u);
    file.read(reinterpret_cast<char*>(spirv.data()), static_cast<std::streamsize>(size));
    if (!file || !LooksLikeSpirv(spirv)) {
        ++stats_.rejected;
        spirv.clear();
        std::filesystem::remove(path, ec);
        return false;
    }
    return true;
}

bool ShaderCache::Store(const std::string& key, const std::vector<uint32_t>& spirv) const {
    if (directory_.empty()) {
        return false;
    }
    const std::filesystem::path final = PathFor(key);
    const std::filesystem::path scratch = directory_ / (key + ".spv.tmp");
    {
        std::ofstream file(scratch, std::ios::binary | std::ios::trunc);
        if (!file) {
            return false;
        }
        file.write(reinterpret_cast<const char*>(spirv.data()),
                   static_cast<std::streamsize>(spirv.size() * sizeof(uint32_t)));
        if (!file) {
            return false;
        }
    }
    // Tulis ke nama sementara lalu rename. Menulis langsung ke nama akhirnya
    // berarti proses yang mati di tengah meninggalkan berkas separuh yang
    // pemanggil berikutnya cari dan temukan.
    std::error_code ec;
    std::filesystem::rename(scratch, final, ec);
    if (ec) {
        std::filesystem::remove(scratch, ec);
        return false;
    }
    return true;
}

CompileOutput ShaderCache::Get(const CompileRequest& request) {
    CompileOutput out;
    const std::string key = KeyOf(request);

    if (Load(key, out.spirv)) {
        ++stats_.hits;
        out.ok = true;
        return out;
    }
    ++stats_.misses;

    if (!compiler_) {
        out.error = "no shader compiler configured";
        return out;
    }
    ++stats_.compiles;
    out = compiler_(request);
    if (!out.ok) {
        return out;
    }
    // Hasil kompilasi pun diperiksa sebelum disimpan. Kompilator yang mengaku
    // berhasil tapi menyerahkan keluaran kosong akan mengisi cache dengan entri
    // yang gagal setiap kali dibaca, dan menyalahkan pembacanya.
    if (!LooksLikeSpirv(out.spirv)) {
        out.ok = false;
        out.spirv.clear();
        out.error = "compiler reported success but produced no valid SPIR-V";
        return out;
    }
    Store(key, out.spirv);
    return out;
}

void ShaderCache::Purge() {
    if (directory_.empty()) {
        return;
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
        const std::string name = entry.path().filename().string();
        // Hanya berkas yang pola namanya milik cache. Direktori cache yang
        // ternyata ditunjuk ke tempat lain tidak boleh menjadi rm -rf.
        if (name.size() > 4 && (name.ends_with(".spv") || name.ends_with(".spv.tmp"))) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

std::string_view SlangArguments() {
    // `-matrix-layout-column-major` menyamakan tata letak matriks di memori
    // dengan glm, yang column-major. Tanpanya `mul(m, v)` membaca matriks yang
    // diunggah C++ dalam urutan terbalik — yang bukan galat melainkan transpose,
    // dan transpose sebuah matriks rotasi adalah rotasi ke arah sebaliknya.
    return "-target spirv -profile spirv_1_5 -emit-spirv-directly "
           "-matrix-layout-column-major";
}

std::string SlangCompilerIdentity(const std::filesystem::path& slangcPath) {
    const std::filesystem::path exe = ResolveSlangc(slangcPath);
    if (exe.empty()) {
        return {};
    }
    std::error_code ec;
    const std::filesystem::path out =
        std::filesystem::temp_directory_path(ec) / "sim-slangc-version.txt";
    const std::string command = Quote(exe) + " -v > " + Quote(out) + " 2>&1";
    if (RunShell(command) != 0) {
        std::filesystem::remove(out, ec);
        return {};
    }
    std::string text = Trim(ReadTextFile(out));
    std::filesystem::remove(out, ec);
    if (const size_t newline = text.find('\n'); newline != std::string::npos) {
        text.resize(newline);
    }
    text = Trim(std::move(text));
    if (text.empty()) {
        return {};
    }
    return text + " " + std::string(SlangArguments());
}

ShaderCache::Compiler MakeSlangCompiler(std::filesystem::path slangcPath) {
    const std::filesystem::path exe = ResolveSlangc(slangcPath);
    if (exe.empty()) {
        // Kompilator yang gagal dengan jelas, bukan yang tidak ada. Pemanggil
        // yang menerima nullptr harus mengingat memeriksanya di setiap tempat;
        // yang menerima ini mendapat pesan yang menyebut apa yang hilang, di
        // tempat kegagalannya benar-benar terasa.
        const std::string reason = slangcPath.empty()
                                       ? "slangc not found in VULKAN_SDK or PATH"
                                       : "slangc not found at " + slangcPath.string();
        return [reason](const CompileRequest&) {
            CompileOutput out;
            out.error = reason;
            return out;
        };
    }

    return [exe](const CompileRequest& request) {
        CompileOutput out;
        std::error_code ec;
        // Berkas perantara di direktori sementara sistem, bukan di direktori
        // cache. Yang harus atomik adalah penyimpanan hasilnya, dan itu terjadi
        // di `Store` — di mana berkas sementara dan berkas akhirnya memang
        // sengaja bertetangga supaya `rename`-nya tidak melintasi sistem berkas.
        const std::filesystem::path scratch = std::filesystem::temp_directory_path(ec);
        Hash128 unique;
        unique.Feed(request.source);
        unique.Feed(request.entryPoint);
        const std::string stem = "sim-slang-" + unique.Hex();
        const std::filesystem::path in = scratch / (stem + ".slang");
        const std::filesystem::path spv = scratch / (stem + ".spv");
        const std::filesystem::path log = scratch / (stem + ".log");

        {
            std::ofstream file(in, std::ios::binary | std::ios::trunc);
            if (!file) {
                out.error = "cannot write temporary Slang source to " + in.string();
                return out;
            }
            file << request.source;
        }

        const std::string entry = request.entryPoint.empty() ? std::string("main")
                                                             : request.entryPoint;
        const std::string command = Quote(exe) + " " + Quote(in) + " " +
                                    std::string(SlangArguments()) + " -entry " + entry +
                                    " -stage " + ToString(request.stage) + " -o " + Quote(spv) +
                                    " > " + Quote(log) + " 2>&1";
        const int status = RunShell(command);
        out.error = Trim(ReadTextFile(log));

        if (status == 0) {
            std::ifstream file(spv, std::ios::binary);
            const auto size = std::filesystem::file_size(spv, ec);
            if (file && !ec && size > 0 && size % sizeof(uint32_t) == 0) {
                out.spirv.assign(static_cast<size_t>(size) / sizeof(uint32_t), 0u);
                file.read(reinterpret_cast<char*>(out.spirv.data()),
                          static_cast<std::streamsize>(size));
                out.ok = static_cast<bool>(file);
            }
            if (!out.ok && out.error.empty()) {
                out.error = "slangc succeeded but wrote no usable SPIR-V";
            }
        } else if (out.error.empty()) {
            out.error = "slangc exited with status " + std::to_string(status);
        }

        std::filesystem::remove(in, ec);
        std::filesystem::remove(spv, ec);
        std::filesystem::remove(log, ec);
        return out;
    };
}

}  // namespace sim::material
