#include "Sim/ImageIO/ImageIO.h"

#include "Backend.h"

#include "Sim/Core/Log.h"

#include <algorithm>
#include <cctype>
#include <map>

namespace sim::imageio {
namespace {

std::string ToLower(std::string_view text) {
    std::string lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

/// Peta ekstensi → backend yang menanganinya, dibangun sekali.
///
/// **Yang lebih mampu didaftarkan lebih dulu.** Ketika OIIO ada, ia bertindih
/// dengan ketiga backend lain di hampir setiap format — dan yang menang
/// ditentukan di satu tempat ini, bukan oleh urutan berkas di daftar SOURCES.
///
/// Tindihan itu bukan pemborosan melainkan alat: uji lintas-backend membaca
/// berkas yang sama lewat setiap backend yang mengaku bisa, dan menuntut
/// hasilnya identik. Dua pembaca yang tidak sepakat berarti salah satunya
/// salah, dan itu jauh lebih baik diketahui di test daripada di heightmap
/// produksi.
class Registry {
public:
    static const Registry& Get() {
        static const Registry registry;
        return registry;
    }

    const IBackend* Find(std::string_view extension) const {
        const auto found = byExtension_.find(ToLower(extension));
        return found != byExtension_.end() ? found->second : nullptr;
    }

    /// Nullptr bila backend itu tidak ada, atau ada tapi tidak menangani
    /// formatnya. Pemanggil membedakan keduanya lewat `Named` di bawah.
    const IBackend* FindNamed(std::string_view name, std::string_view extension) const {
        const IBackend* backend = Named(name);
        if (backend == nullptr) {
            return nullptr;
        }
        const std::string lower = ToLower(extension);
        const std::vector<std::string>& readable = backend->ReadableExtensions();
        return std::find(readable.begin(), readable.end(), lower) != readable.end() ? backend
                                                                                    : nullptr;
    }

    const IBackend* Named(std::string_view name) const {
        for (const IBackend* backend : backends_) {
            if (name == backend->Name()) {
                return backend;
            }
        }
        return nullptr;
    }

    std::vector<std::string> BackendsReading(std::string_view extension) const {
        const std::string lower = ToLower(extension);
        std::vector<std::string> names;
        for (const IBackend* backend : backends_) {
            const std::vector<std::string>& readable = backend->ReadableExtensions();
            if (std::find(readable.begin(), readable.end(), lower) != readable.end()) {
                names.emplace_back(backend->Name());
            }
        }
        return names;
    }

    /// Backend yang bisa **menulis** ekstensi ini. Terpisah dari `Find` karena
    /// kemampuan baca dan tulis tidak selalu dipegang backend yang sama, dan
    /// menganggapnya sama akan mengirim gambar ke backend yang cuma bisa
    /// membacanya.
    const IBackend* FindWriter(std::string_view extension) const {
        const std::string lower = ToLower(extension);
        for (const IBackend* backend : backends_) {
            const std::vector<std::string>& writable = backend->WritableExtensions();
            if (std::find(writable.begin(), writable.end(), lower) != writable.end()) {
                return backend;
            }
        }
        return nullptr;
    }

    const std::vector<std::string>& Extensions() const { return extensions_; }
    const std::vector<std::string>& Writable() const { return writable_; }
    const std::vector<std::string>& Names() const { return names_; }
    const std::string& Summary() const { return summary_; }

private:
    Registry() {
        // Urutannya menentukan siapa yang menang: yang didaftarkan lebih dulu
        // memegang ekstensinya. OIIO paling dulu karena ia satu-satunya yang
        // membawa metadata colorspace, PSD utuh, dan DDS.
        if (const IBackend* oiio = OiioBackend(); oiio != nullptr) {
            Add(*oiio);
        }
        if (const IBackend* exr = ExrBackend(); exr != nullptr) {
            Add(*exr);
        }
        if (const IBackend* tiff = TiffBackend(); tiff != nullptr) {
            Add(*tiff);
        }
        Add(StbBackend());

        extensions_.reserve(byExtension_.size());
        for (const auto& [extension, backend] : byExtension_) {
            extensions_.push_back(extension);
        }
        // `std::map` sudah terurut, jadi `extensions_` ikut terurut.

        for (const IBackend* backend : backends_) {
            for (const std::string& extension : backend->WritableExtensions()) {
                if (std::find(writable_.begin(), writable_.end(), extension) == writable_.end()) {
                    writable_.push_back(extension);
                }
            }
        }
        std::sort(writable_.begin(), writable_.end());

        for (const IBackend* backend : backends_) {
            std::string entry = backend->Name();
            if (*backend->Version() != '\0') {
                entry += ' ';
                entry += backend->Version();
            }
            summary_ += summary_.empty() ? entry : " + " + entry;
        }
        summary_ += " (" + std::to_string(extensions_.size()) + " format)";
    }

    void Add(const IBackend& backend) {
        backends_.push_back(&backend);
        names_.emplace_back(backend.Name());
        for (const std::string& extension : backend.ReadableExtensions()) {
            byExtension_.emplace(extension, &backend);  // yang pertama menang
        }
    }

    std::vector<const IBackend*> backends_;
    std::map<std::string, const IBackend*> byExtension_;
    std::vector<std::string> extensions_;
    std::vector<std::string> writable_;
    std::vector<std::string> names_;
    std::string summary_;
};

/// Jalur berkas yang tidak ada dijawab di sini, bukan diserahkan ke backend.
///
/// Dua backend akan melaporkannya dengan dua kalimat berbeda, dan pesan galat
/// yang berubah menurut backend yang kebetulan aktif membuat laporan bug sulit
/// dibaca.
ImageIoResult ResolveForRead(const std::filesystem::path& path, const IBackend*& backend) {
    ImageIoResult result;
    const std::string extension = ToLower(path.extension().string());
    backend = Registry::Get().Find(extension);
    if (backend == nullptr) {
        result.error = "no backend reads " +
                       (extension.empty() ? std::string("files without an extension")
                                          : extension) +
                       ": " + path.string();
        return result;
    }
    std::error_code code;
    if (!std::filesystem::exists(path, code)) {
        result.error = "file does not exist: " + path.string();
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace

void LogWarning(const std::string& message) { SIM_WARN("ImageIO", "{}", message); }

ImageIoResult Read(const std::filesystem::path& path, const ReadOptions& options, Image& out) {
    const IBackend* backend = nullptr;
    if (ImageIoResult resolved = ResolveForRead(path, backend); !resolved) {
        return resolved;
    }
    return backend->Read(path, options, out);
}

ImageIoResult Read(std::span<const uint8_t> bytes, std::string_view extensionHint,
                   const ReadOptions& options, Image& out) {
    ImageIoResult result;
    if (bytes.empty()) {
        result.error = "empty buffer";
        return result;
    }
    // Petunjuk yang kosong atau tidak dikenal jatuh ke stb, yang mengenali
    // formatnya dari byte pertamanya sendiri. Itu jalur thumbnail: berkas sudah
    // di memori, dan namanya belum tentu ikut sampai ke sini.
    const IBackend* backend = Registry::Get().Find(extensionHint);
    return (backend != nullptr ? *backend : StbBackend()).Read(bytes, extensionHint, options, out);
}

ImageIoResult Probe(const std::filesystem::path& path, ImageDesc& out) {
    const IBackend* backend = nullptr;
    if (ImageIoResult resolved = ResolveForRead(path, backend); !resolved) {
        return resolved;
    }
    return backend->Probe(path, out);
}

ImageIoResult Write(const std::filesystem::path& path, const Image& image) {
    ImageIoResult result;
    const std::string extension = ToLower(path.extension().string());
    const IBackend* backend = Registry::Get().FindWriter(extension);
    if (backend == nullptr) {
        result.error = "no backend writes " +
                       (extension.empty() ? std::string("files without an extension") : extension) +
                       ": " + path.string();
        return result;
    }
    if (image.desc.width == 0 || image.desc.height == 0) {
        result.error = "refusing to write an empty image: " + path.string();
        return result;
    }
    return backend->Write(path, image);
}

ImageIoResult Encode(const Image& image, std::string_view extension, std::vector<uint8_t>& out) {
    ImageIoResult result;
    const IBackend* backend = Registry::Get().FindWriter(extension);
    if (backend == nullptr) {
        result.error = "no backend writes " + std::string(extension);
        return result;
    }
    if (image.desc.width == 0 || image.desc.height == 0) {
        result.error = "refusing to encode an empty image";
        return result;
    }
    return backend->Encode(image, out);
}

const std::vector<std::string>& ReadableExtensions() { return Registry::Get().Extensions(); }

const std::vector<std::string>& WritableExtensions() { return Registry::Get().Writable(); }

bool CanWrite(std::string_view extension) {
    return Registry::Get().FindWriter(extension) != nullptr;
}

bool CanRead(std::string_view extension) { return Registry::Get().Find(extension) != nullptr; }

std::string_view BackendFor(std::string_view extension) {
    const IBackend* backend = Registry::Get().Find(extension);
    return backend != nullptr ? backend->Name() : std::string_view{};
}

const std::vector<std::string>& BackendNames() { return Registry::Get().Names(); }

std::vector<std::string> BackendsFor(std::string_view extension) {
    return Registry::Get().BackendsReading(extension);
}

ImageIoResult ReadWith(std::string_view backend, const std::filesystem::path& path,
                       const ReadOptions& options, Image& out) {
    ImageIoResult result;
    const std::string extension = ToLower(path.extension().string());
    const IBackend* chosen = Registry::Get().FindNamed(backend, extension);
    if (chosen == nullptr) {
        // Dua sebab yang berbeda, dan membedakannya menghemat pencarian:
        // backend yang tidak dikompilasi menuntut membangun ulang, sementara
        // backend yang tidak menangani formatnya menuntut backend lain.
        result.error = Registry::Get().Named(backend) == nullptr
                           ? "no backend named '" + std::string(backend) + "'"
                           : "backend '" + std::string(backend) + "' does not read " + extension;
        return result;
    }
    std::error_code code;
    if (!std::filesystem::exists(path, code)) {
        result.error = "file does not exist: " + path.string();
        return result;
    }
    return chosen->Read(path, options, out);
}

std::string BackendSummary() { return Registry::Get().Summary(); }

}  // namespace sim::imageio
