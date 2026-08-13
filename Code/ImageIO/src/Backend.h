#pragma once

#include "Sim/ImageIO/ImageIO.h"

#include <string>
#include <vector>

namespace sim::imageio {

/// Satu implementasi dekode, di balik antarmuka yang sama.
///
/// Virtual, bukan `#if` bertebaran di titik panggilan. Backend EXR dan TIFF
/// tidak selalu dikompilasi, dan cara paling murah membuat "tidak selalu ada"
/// tidak menular ke seluruh berkas adalah menaruhnya di satu daftar yang isinya
/// ditentukan saat build.
///
/// Yang dibelinya ternyata lebih dari itu: pustaka di balik backend EXR dan TIFF
/// pernah diganti seluruhnya tanpa satu pun titik panggil berubah.
class IBackend {
public:
    virtual ~IBackend() = default;

    virtual const char* Name() const = 0;

    /// Versi pustakanya, atau kosong bagi backend yang di-vendor bersama pohon
    /// ini. Muncul di log startup dan di DEPENDENCIES.md — nama backend saja tidak
    /// cukup untuk menjelaskan kenapa satu berkas terbaca berbeda di dua mesin.
    virtual const char* Version() const { return ""; }

    /// Ekstensi huruf kecil dengan titik, terurut. Yang dikembalikan adalah
    /// yang **dikurasi SimEngine**, bukan seluruh yang bisa dibaca pustakanya:
    /// setiap ekstensi di sini adalah janji dukungan, dan janji yang tidak bisa
    /// ditepati lebih buruk daripada format yang tidak ada.
    virtual const std::vector<std::string>& ReadableExtensions() const = 0;

    virtual ImageIoResult Read(const std::filesystem::path& path, const ReadOptions& options,
                               Image& out) const = 0;
    virtual ImageIoResult Read(std::span<const uint8_t> bytes, std::string_view extensionHint,
                               const ReadOptions& options, Image& out) const = 0;
    virtual ImageIoResult Probe(const std::filesystem::path& path, ImageDesc& out) const = 0;

    /// Bawaannya kosong: backend yang tidak menulis tidak perlu mengatakannya.
    virtual const std::vector<std::string>& WritableExtensions() const {
        static const std::vector<std::string> kNone;
        return kNone;
    }

    virtual ImageIoResult Write(const std::filesystem::path& path, const Image& image) const {
        (void)image;
        ImageIoResult result;
        result.error = std::string(Name()) + " cannot write " + path.extension().string();
        return result;
    }

    virtual ImageIoResult Encode(const Image& image, std::vector<uint8_t>& out) const {
        (void)image;
        (void)out;
        ImageIoResult result;
        result.error = std::string(Name()) + " cannot write to memory";
        return result;
    }
};

/// Selalu ada. Menangani PNG, JPEG, TGA, BMP, HDR, dan PSD terbatas.
const IBackend& StbBackend();

/// OpenImageIO. Nullptr bila SIM_WITH_OIIO tidak aktif.
///
/// **Didahulukan bila ada**, karena ia satu-satunya yang membawa metadata
/// colorspace, PSD utuh, dan DDS — bukan karena lebih cepat.
const IBackend* OiioBackend();

/// OpenEXR lewat tinyexr. Nullptr bila SIM_WITH_TINYEXR tidak aktif.
const IBackend* ExrBackend();

/// TIFF lewat libtiff. Nullptr bila SIM_WITH_LIBTIFF tidak aktif.
const IBackend* TiffBackend();

/// Mencatat peringatan lewat `Sim::Core`.
///
/// Dibungkus di sini, bukan dipanggil langsung dari masing-masing backend,
/// supaya TU backend tidak perlu menarik spdlog beserta seluruh isinya hanya
/// untuk satu baris peringatan.
void LogWarning(const std::string& message);

}  // namespace sim::imageio
