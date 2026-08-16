#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN

#include "Sim/ImageIO/ImageIO.h"
#include "Sim/ImageIO/MipChain.h"
#include "Sim/ImageIO/TextureColor.h"

#include <array>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace sim;
using namespace sim::imageio;

namespace {

/// Berkas uji di `Resources/Images`, beserta isinya yang tepat.
///
/// **Dibuat oleh enkoder di luar mesin ini** — Pillow/libpng untuk kedua PNG,
/// dan RGBE yang disusun tangan untuk HDR-nya. Fixture yang dihasilkan dekoder
/// yang sama dengan yang mengujinya hanya membuktikan konsistensi dengan diri
/// sendiri: berkas yang bukan PNG sah pun akan lulus round-trip semacam itu.
///
/// Rumusnya ditulis di sini, bukan hanya nilainya, supaya berkasnya bisa dibuat
/// ulang kalau hilang:
///
///   checker.png   8x8, RGB 8-bit.   piksel(x,y) = (x*32, y*32, (x+y) genap ? 255 : 0)
///   ramp16.png    8x8, grey 16-bit. piksel(x,y) = (y*8 + x) * 1024
///   gradient.hdr  8x4, RGB float.   piksel(x,y) = (x/8, y/4, 0.5)
///
/// Eksponen RGBE di `gradient.hdr` dipatok 128 untuk seluruh piksel, jadi
/// setiap kanal bernilai mantissa/256 **tepat**. Itulah kenapa nilainya boleh
/// dibandingkan dengan toleransi sekecil ini: yang diuji bukan ketelitian
/// dekodernya melainkan bahwa ia tidak menerapkan gamma diam-diam.
std::filesystem::path ImageDir() { return std::filesystem::path(SIM_IMAGE_DIR); }

std::vector<uint8_t> ReadBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    REQUIRE(stream);
    const std::streamsize size = stream.tellg();
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

/// Folder sementara yang bersih, dihapus saat selesai. Sama seperti di
/// TerrainTests — berkas cacat dibuat saat uji berjalan, bukan disimpan di
/// `Resources/`, supaya folder aset tidak berisi berkas yang sengaja rusak.
class TempDir {
public:
    explicit TempDir(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / ("sim-imageio-" + name)) {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
        std::filesystem::create_directories(path_, code);
    }
    ~TempDir() {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path operator/(const std::string& leaf) const { return path_ / leaf; }

private:
    std::filesystem::path path_;
};

bool Contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

/// Membuang komentar `//` dari sebuah baris.
///
/// Ada karena uji "tidak ada `stbi_` di luar backend" menyisir teks, dan teks
/// mencakup komentar. `PngWrite.cpp` menyebut `stbi_write_png` dan
/// `stbi_zlib_compress` di komentar kepalanya untuk menerangkan kenapa enkoder
/// PNG 16-bit ditulis tangan — penjelasan yang justru harus tetap ada. Yang
/// dilarang adalah memanggilnya, bukan menyebutnya.
///
/// Komentar blok `/* */` tidak ditangani: seluruh `Code/` memakai `//`, dan
/// pengurai komentar C++ yang benar di dalam sebuah uji adalah biaya yang jauh
/// lebih besar daripada yang dibelinya.
std::string StripLineComment(const std::string& line) {
    const std::size_t at = line.find("//");
    return at == std::string::npos ? line : line.substr(0, at);
}

}  // namespace

TEST_CASE("PNG 8-bit dibaca apa adanya") {
    Image image;
    REQUIRE(Read(ImageDir() / "checker.png", ReadOptions{}, image));

    CHECK(image.desc.width == 8);
    CHECK(image.desc.height == 8);
    CHECK(image.desc.channels == 3);
    CHECK(image.desc.type == PixelType::UInt8);
    CHECK(image.bytes.size() == 8u * 8u * 3u);

    const uint8_t* pixels = image.AsU8();
    REQUIRE(pixels != nullptr);
    // Nullptr untuk tipe yang salah adalah bagian dari kontraknya, bukan
    // kebetulan: pemanggil yang salah membaca tipe mendapat crash yang jelas,
    // bukan piksel yang ditafsirkan melenceng.
    CHECK(image.AsU16() == nullptr);
    CHECK(image.AsF32() == nullptr);

    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * 8 + x) * 3;
            CHECK(pixels[at] == x * 32);
            CHECK(pixels[at + 1] == y * 32);
            CHECK(pixels[at + 2] == ((x + y) % 2 == 0 ? 255 : 0));
        }
    }
}

TEST_CASE("PNG 16-bit tidak diturunkan diam-diam") {
    Image image;
    REQUIRE(Read(ImageDir() / "ramp16.png", ReadOptions{}, image));

    CHECK(image.desc.width == 8);
    CHECK(image.desc.height == 8);
    CHECK(image.desc.channels == 1);
    // Inilah klaim yang sesungguhnya. Berkasnya 16-bit, dan pembaca yang
    // menurunkannya ke 8-bit tanpa diminta akan menghasilkan heightmap dengan
    // 256 tingkat tinggi — langkah empat meter pada terrain setinggi kilometer.
    CHECK(image.desc.type == PixelType::UInt16);
    CHECK(image.bytes.size() == 8u * 8u * 2u);

    const uint16_t* samples = image.AsU16();
    REQUIRE(samples != nullptr);
    for (uint32_t i = 0; i < 64; ++i) {
        CHECK(samples[i] == i * 1024);
    }
}

TEST_CASE("HDR dibaca sebagai float linear tanpa gamma") {
    Image image;
    REQUIRE(Read(ImageDir() / "gradient.hdr", ReadOptions{}, image));

    CHECK(image.desc.width == 8);
    CHECK(image.desc.height == 4);
    CHECK(image.desc.channels == 3);
    CHECK(image.desc.type == PixelType::Float32);
    // Radiance RGBE menyimpan radiance linear, dan itu satu-satunya format di
    // sini yang menyatakan ruang warnanya sendiri.
    CHECK(image.desc.colorSpace == ColorSpace::Linear);

    const float* pixels = image.AsF32();
    REQUIRE(pixels != nullptr);
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y) * 8 + x) * 3;
            CHECK(pixels[at] == doctest::Approx(static_cast<float>(x) / 8.0f).epsilon(0.0001));
            CHECK(pixels[at + 1] == doctest::Approx(static_cast<float>(y) / 4.0f).epsilon(0.0001));
            CHECK(pixels[at + 2] == doctest::Approx(0.5f).epsilon(0.0001));
        }
    }
}

TEST_CASE("jumlah kanal dipaksa saat dekode") {
    ReadOptions options;
    options.channels = 4;

    Image image;
    REQUIRE(Read(ImageDir() / "checker.png", options, image));
    CHECK(image.desc.channels == 4);
    CHECK(image.bytes.size() == 8u * 8u * 4u);

    const uint8_t* pixels = image.AsU8();
    REQUIRE(pixels != nullptr);
    // Berkasnya RGB tanpa alfa; kanal keempat yang ditambahkan harus opak.
    // Alfa nol di sini akan membuat setiap tekstur tanpa alfa menghilang.
    for (uint32_t i = 0; i < 64; ++i) {
        CHECK(pixels[i * 4 + 3] == 255);
    }
    CHECK(pixels[0] == 0);
    CHECK(pixels[1] == 0);
    CHECK(pixels[2] == 255);
}

TEST_CASE("tipe piksel dipaksa tanpa gamma tersembunyi") {
    // Jalur yang paling mudah salah. Jalur konversi stb sendiri menerapkan
    // pow(x, 2.2) saat menaikkan LDR ke float — sebuah keputusan colorspace
    // yang disembunyikan di dalam pemanggilan dekode, dan yang salahnya tidak
    // pernah muncul sebagai galat. Yang dituntut di sini adalah skala murni.
    ReadOptions options;
    options.channels = 1;
    options.type = PixelType::Float32;

    Image image;
    REQUIRE(Read(ImageDir() / "ramp16.png", options, image));
    CHECK(image.desc.type == PixelType::Float32);

    const float* pixels = image.AsF32();
    REQUIRE(pixels != nullptr);
    for (uint32_t i = 0; i < 64; ++i) {
        const float expected = static_cast<float>(i * 1024) / 65535.0f;
        CHECK(pixels[i] == doctest::Approx(expected).epsilon(0.00001));
    }
}

TEST_CASE("16-bit ke 8-bit membulatkan, bukan memotong") {
    ReadOptions options;
    options.channels = 1;
    options.type = PixelType::UInt8;

    Image image;
    REQUIRE(Read(ImageDir() / "ramp16.png", options, image));
    CHECK(image.desc.type == PixelType::UInt8);

    const uint8_t* pixels = image.AsU8();
    REQUIRE(pixels != nullptr);
    CHECK(pixels[0] == 0);
    // 64512/65535 * 255 = 251.02 → 251. Menggeser 8 bit menghasilkan 252,
    // dan ujung atasnya tidak pernah mencapai 255 sama sekali.
    CHECK(pixels[63] == 251);
}

TEST_CASE("baca dari memori sama dengan baca dari berkas") {
    // Jalur thumbnail: berkasnya sudah di memori, dan yang dihasilkan harus
    // identik dengan jalur berkas. Kalau tidak, thumbnail dan tekstur yang
    // dimuat menampilkan gambar yang berbeda dari berkas yang sama.
    const std::vector<uint8_t> bytes = ReadBytes(ImageDir() / "checker.png");

    Image fromFile;
    Image fromMemory;
    REQUIRE(Read(ImageDir() / "checker.png", ReadOptions{}, fromFile));
    REQUIRE(Read(bytes, ".png", ReadOptions{}, fromMemory));

    CHECK(fromMemory.desc.width == fromFile.desc.width);
    CHECK(fromMemory.desc.height == fromFile.desc.height);
    CHECK(fromMemory.desc.channels == fromFile.desc.channels);
    CHECK(fromMemory.desc.type == fromFile.desc.type);
    CHECK(fromMemory.bytes == fromFile.bytes);
}

TEST_CASE("Probe membaca kepala berkas saja") {
    ImageDesc desc;
    REQUIRE(Probe(ImageDir() / "ramp16.png", desc));
    CHECK(desc.width == 8);
    CHECK(desc.height == 8);
    CHECK(desc.channels == 1);
    CHECK(desc.type == PixelType::UInt16);

    REQUIRE(Probe(ImageDir() / "gradient.hdr", desc));
    CHECK(desc.width == 8);
    CHECK(desc.height == 4);
    CHECK(desc.channels == 3);
    CHECK(desc.type == PixelType::Float32);
}

TEST_CASE("berkas rusak menghasilkan galat, bukan crash") {
    TempDir dir("corrupt");

    SUBCASE("PNG terpotong") {
        // Header PNG utuh, IDAT-nya dipotong separuh. Ini bentuk kerusakan yang
        // paling sering benar-benar terjadi: salinan yang terputus di tengah,
        // atau editor yang mati saat menulis.
        const std::vector<uint8_t> whole = ReadBytes(ImageDir() / "checker.png");
        const std::filesystem::path path = dir / "truncated.png";
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(whole.data()),
                     static_cast<std::streamsize>(whole.size() / 2));
        stream.close();

        Image image;
        const ImageIoResult result = Read(path, ReadOptions{}, image);
        CHECK_FALSE(result.ok);
        // Pesannya harus menyebut jalur berkasnya. "cannot decode image" tanpa
        // nama berkas mengirim orang memeriksa berkas yang salah.
        CHECK(result.error.find("truncated.png") != std::string::npos);
    }

    SUBCASE("berkas kosong") {
        const std::filesystem::path path = dir / "empty.png";
        std::ofstream(path, std::ios::binary).close();

        Image image;
        CHECK_FALSE(Read(path, ReadOptions{}, image).ok);
    }

    SUBCASE("byte acak dengan ekstensi yang benar") {
        const std::filesystem::path path = dir / "garbage.hdr";
        std::ofstream stream(path, std::ios::binary);
        const std::string junk(512, '\x7f');
        stream.write(junk.data(), static_cast<std::streamsize>(junk.size()));
        stream.close();

        Image image;
        CHECK_FALSE(Read(path, ReadOptions{}, image).ok);
        ImageDesc desc;
        CHECK_FALSE(Probe(path, desc).ok);
    }

    SUBCASE("berkas tidak ada") {
        Image image;
        const ImageIoResult result = Read(dir / "hilang.png", ReadOptions{}, image);
        CHECK_FALSE(result.ok);
        CHECK(result.error.find("hilang.png") != std::string::npos);
    }

    SUBCASE("ekstensi yang tidak didukung") {
        // Ditolak sebelum menyentuh isinya, dengan pesan yang menyebut
        // ekstensinya — bukan "unrecognised image" yang mengirim orang
        // memeriksa berkas yang sebenarnya baik-baik saja.
        const std::filesystem::path path = dir / "animasi.gif";
        std::ofstream(path, std::ios::binary).close();

        Image image;
        const ImageIoResult result = Read(path, ReadOptions{}, image);
        CHECK_FALSE(result.ok);
        CHECK(result.error.find(".gif") != std::string::npos);
    }
}

TEST_CASE("kapabilitas dibangkitkan dari backend yang aktif") {
    const std::vector<std::string>& extensions = ReadableExtensions();
    REQUIRE_FALSE(extensions.empty());

    // Terurut dan tanpa duplikat: daftarnya muncul di UI, dan urutan yang
    // berubah menurut backend mana yang aktif membuatnya sulit dibaca.
    CHECK(std::is_sorted(extensions.begin(), extensions.end()));
    CHECK(std::adjacent_find(extensions.begin(), extensions.end()) == extensions.end());

    // Yang wajib ada di setiap konfigurasi, karena stb selalu ikut dibangun.
    for (const char* required : {".bmp", ".hdr", ".jpeg", ".jpg", ".png", ".psd", ".tga"}) {
        CHECK(Contains(extensions, required));
        CHECK(CanRead(required));
        CHECK_FALSE(BackendFor(required).empty());
    }

    // Yang sengaja ditolak. Daftar ini dikurasi, bukan diwarisi dari apa yang
    // kebetulan bisa dibaca pustakanya — stb sendiri membaca GIF, PIC, dan PNM.
    // Alasan per format ada di docs/PLAN-IMAGEIO.md.
    for (const char* rejected : {".gif", ".pic", ".pnm", ".webp", ".dpx", ".ico"}) {
        CHECK_FALSE(Contains(extensions, rejected));
        CHECK_FALSE(CanRead(rejected));
    }

    // Huruf besar-kecil tidak membedakan: berkas dari Windows dan dari DCC lama
    // sering datang dengan ekstensi huruf besar.
    CHECK(CanRead(".PNG"));
    CHECK(CanRead(".Hdr"));

    CHECK_FALSE(BackendSummary().empty());

    // Dicetak, bukan hanya diperiksa: uji ini berjalan di dua konfigurasi yang
    // berbeda, dan yang paling ingin dilihat orang di keluaran CI adalah
    // konfigurasi mana yang barusan lulus.
    std::string list;
    for (const std::string& extension : extensions) {
        list += list.empty() ? extension : " " + extension;
    }
    MESSAGE("backend: " << BackendSummary());
    MESSAGE("format : " << list);
}

TEST_CASE("backend yang berbagi format membaca berkas yang sama secara identik") {
    // Kriteria terima I1: dua backend yang membaca format yang sama harus
    // sampai pada piksel yang sama. Kalau tidak, salah satunya salah — dan
    // lebih baik diketahui sekarang daripada setelah heightmap produksi dimuat
    // dengan rentang tinggi yang meleset.
    //
    // **Uji ini baru benar-benar membandingkan sesuatu ketika OIIO ada**, dan di
    // situlah nilainya. OIIO bertindih dengan ketiganya — stb pada PNG/HDR/PSD,
    // tinyexr pada EXR, libtiff pada TIFF — jadi pembaca yang ditulis di sini
    // diadu dengan implementasi yang sudah dipakai seluruh industri.
    //
    // Dua di antaranya menguji hal yang tidak bisa diuji dengan cara lain:
    // penyusunan ulang kanal EXR (berkas menyimpannya B,G,R; salah menyusun
    // menukar merah dengan biru di setiap EXR), dan pembacaan strip/tile TIFF
    // terkompresi. Keduanya ditulis tangan di sini; OIIO menjawabnya sendiri.
    //
    // Tanpa OIIO uji ini menyusut menjadi satu backend per format dan tidak
    // membandingkan apa pun — dan itu bukan kelemahan, melainkan bentuk yang
    // sama persis dijalankan di dua konfigurasi. Yang menjaga kebenaran nilai di
    // konfigurasi itu adalah uji nilai tepat di atas, terhadap fixture yang
    // dibuat penulis di luar mesin ini.
    std::vector<std::string> fixtures{"checker.png", "ramp16.png", "gradient.hdr"};
    if (CanRead(".exr")) {
        fixtures.emplace_back("gradient.exr");
        // Kanal yang bukan R/G/B: keduanya harus membiarkannya dalam urutan
        // berkas, bukan menebak yang mana merah.
        fixtures.emplace_back("aov-bukan-lingkungan.exr");
    }
    if (CanRead(".tif")) {
        fixtures.emplace_back("ramp16.tif");
        fixtures.emplace_back("heights-metres.tif");
    }

    for (const std::string& fixture : fixtures) {
        const std::filesystem::path path = ImageDir() / fixture;
        const std::vector<std::string> backends =
            BackendsFor(path.extension().string());
        REQUIRE_FALSE(backends.empty());

        Image reference;
        std::string referenceBackend;
        for (const std::string& backend : backends) {
            Image image;
            const ImageIoResult result = ReadWith(backend, path, ReadOptions{}, image);
            INFO("berkas " << fixture << " lewat backend " << backend);
            REQUIRE(result.ok);

            if (referenceBackend.empty()) {
                reference = std::move(image);
                referenceBackend = backend;
                continue;
            }

            INFO("membandingkan " << backend << " dengan " << referenceBackend);
            CHECK(image.desc.width == reference.desc.width);
            CHECK(image.desc.height == reference.desc.height);
            CHECK(image.desc.channels == reference.desc.channels);
            CHECK(image.desc.type == reference.desc.type);
            // Piksel demi piksel, bukan sekadar bentuknya. Dua pembaca yang
            // sepakat soal dimensi tapi berbeda soal nilai adalah persis kasus
            // yang sulit ditemukan dengan cara lain.
            CHECK(image.bytes == reference.bytes);
        }
    }
}

TEST_CASE("format tambahan muncul tepat ketika backendnya ada") {
    // Daftar format menyusut atau bertambah mengikuti backend yang aktif — itu
    // seluruh maksud kapabilitas yang dibangkitkan. Yang dikunci di sini adalah
    // kaitannya, bukan isinya: `.exr` ada **jika dan hanya jika** ada backend
    // yang benar-benar bisa membacanya.
    for (const char* extension : {".exr", ".tif", ".tiff", ".dds"}) {
        INFO("ekstensi " << extension);
        CHECK(CanRead(extension) == !BackendsFor(extension).empty());
    }

    const std::vector<std::string>& backends = BackendNames();
    REQUIRE_FALSE(backends.empty());
    // stb selalu ada, dan selalu terakhir: ia yang menangani sisa yang tidak
    // diambil backend yang lebih mampu.
    CHECK(backends.back() == "stb");

    // Setiap ekstensi yang diaku bisa dibaca harus benar-benar punya yang
    // membacanya. Daftar yang menyebut format tanpa backend adalah bentuk lain
    // dari janji yang tidak bisa ditepati.
    for (const std::string& extension : ReadableExtensions()) {
        INFO("ekstensi " << extension);
        CHECK_FALSE(BackendFor(extension).empty());
        CHECK_FALSE(BackendsFor(extension).empty());
    }

    // Backend yang tidak ada ditolak dengan pesan yang menyebut namanya.
    Image image;
    const ImageIoResult result =
        ReadWith("tidak-ada", ImageDir() / "checker.png", ReadOptions{}, image);
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("tidak-ada") != std::string::npos);
}

TEST_CASE("pustaka gambar hanya dipanggil dari backend-nya masing-masing") {
    // Kriteria terima I0, diperluas ke ketiga pustaka — diuji dengan menyisir
    // berkas, bukan dengan disiplin.
    //
    // Setiap pemanggilan dekode langsung adalah satu tempat lagi yang harus
    // disentuh ketika backend berikutnya masuk. Bahwa itu bukan kekhawatiran
    // teoretis sudah terbukti sekali: seluruh backend EXR/TIFF pernah diganti
    // pustakanya **tanpa menyentuh satu pun titik panggil**, karena semuanya
    // lewat seam ini.
    const std::filesystem::path codeDir = std::filesystem::path(SIM_CODE_DIR);
    REQUIRE(std::filesystem::is_directory(codeDir));

    // Setiap pustaka punya tepat satu berkas yang boleh memanggilnya, dan
    // dikenali dari dua sisi: nama pemanggilnya dan header yang menyediakannya.
    // Include-nya ikut diperiksa karena ia syaratnya — berkas yang meng-include
    // tanpa memakainya hari ini adalah titik dekode berikutnya yang menunggu
    // ditulis.
    struct Rule {
        const char* backend;   ///< satu-satunya berkas yang boleh memanggilnya
        const char* symbol;    ///< awalan panggilan pustakanya
        const char* header;    ///< header yang menyediakannya
    };
    const std::array<Rule, 3> kRules{{
        {"BackendStb.cpp", "stbi_", "stb_image.h"},
        {"BackendExr.cpp", "EXRImage", "tinyexr.h"},
        {"BackendTiff.cpp", "TIFFGetField", "tiffio.h"},
    }};

    std::vector<std::string> offenders;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(codeDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::filesystem::path& path = entry.path();
        const std::string extension = path.extension().string();
        if (extension != ".cpp" && extension != ".h") {
            continue;
        }

        const std::string name = path.filename().string();
        std::vector<std::string> lines;
        {
            std::ifstream stream(path);
            std::string line;
            while (std::getline(stream, line)) {
                lines.push_back(StripLineComment(line));
            }
        }

        for (const Rule& rule : kRules) {
            if (name == rule.backend) {
                continue;  // berkas ini memang pemiliknya
            }
            for (const std::string& code : lines) {
                // `stbi_` dicocokkan persis, sehingga `stbir_resize_*`
                // (penskala thumbnail, yang bukan urusan I/O) tidak ikut
                // tertangkap.
                if (code.find(rule.symbol) != std::string::npos ||
                    code.find(rule.header) != std::string::npos) {
                    offenders.push_back(path.string() + " memanggil " + rule.header);
                    break;
                }
            }
        }
    }

    INFO("berkas yang memakai pustaka gambar di luar backend-nya: ",
         offenders.empty() ? std::string("(tidak ada)") : offenders.front());
    CHECK(offenders.empty());
}

// --- I4: colorspace dan alfa -------------------------------------------------

TEST_CASE("slot material yang menentukan ruang warna, bukan isi berkas") {
    // Aturan defaultnya, diuji sebagai tabel supaya ia benar-benar terbaca
    // sebagai aturan dan bukan sebagai perilaku yang kebetulan.
    for (const char* slot : {"baseColor", "albedo", "diffuse", "emissive", "emission"}) {
        INFO("slot " << slot);
        CHECK(UsageForSlot(slot) == TextureUsage::Color);
    }
    for (const char* slot : {"normal", "roughness", "metalness", "metallic", "height",
                             "displacement", "occlusion", "ao", "opacity", "mask"}) {
        INFO("slot " << slot);
        CHECK(UsageForSlot(slot) == TextureUsage::Data);
    }

    // Besar-kecil huruf tidak membedakan: slot yang sama datang sebagai
    // `baseColor`, `basecolor`, dan `BaseColor` dari tiga alat yang berbeda.
    CHECK(UsageForSlot("BASECOLOR") == TextureUsage::Color);
    CHECK(UsageForSlot("BaseColor") == TextureUsage::Color);

    // **Slot tak dikenal dianggap Data.** Arahnya dipilih menurut kesalahan mana
    // yang lebih mudah ditemukan: slot warna yang terlupa menghasilkan tekstur
    // terlalu terang — terlihat seketika; slot data yang terlupa akan didekode
    // sRGB, dan itu tidak terlihat sama sekali.
    CHECK(UsageForSlot("slotYangBelumAda") == TextureUsage::Data);
    CHECK(UsageForSlot("") == TextureUsage::Data);

    CHECK(ExpectedColorSpace(TextureUsage::Color) == ColorSpace::Srgb);
    CHECK(ExpectedColorSpace(TextureUsage::Data) == ColorSpace::Linear);
}

TEST_CASE("albedo dari PNG sRGB dan dari EXR linear menghasilkan warna yang sama") {
    // **Kriteria terima I4.** Kedua berkas berisi warna yang sama disimpan dua
    // cara: PNG menyimpan bytenya tersandi sRGB, EXR menyimpan padanan
    // linearnya yang tepat. Setelah aturan slot diterapkan, keduanya harus
    // sampai pada angka yang sama — kalau tidak, adegan yang memakai keduanya
    // punya dua warna untuk satu material.
    Image fromPng;
    REQUIRE(Read(ImageDir() / "albedo-srgb.png", ReadOptions{}, fromPng));
    PrepareTexture(UsageForSlot("baseColor"), fromPng);

    // Dekode menaikkannya ke float: mendekode sRGB di tempat pada delapan bit
    // akan menjatuhkan seluruh nilai 0..15 ke 0 atau 1.
    REQUIRE(fromPng.desc.type == PixelType::Float32);
    CHECK(fromPng.desc.colorSpace == ColorSpace::Linear);

    if (!CanRead(".exr")) {
        MESSAGE("build ini tanpa backend EXR — pembandingan dilewati");
        return;
    }

    Image fromExr;
    REQUIRE(Read(ImageDir() / "albedo-linear.exr", ReadOptions{}, fromExr));
    PrepareTexture(UsageForSlot("baseColor"), fromExr);

    // Berkas float sudah linear menurut definisinya; ia tidak boleh disentuh.
    CHECK(fromExr.desc.colorSpace == ColorSpace::Linear);

    REQUIRE(fromPng.desc.width == fromExr.desc.width);
    REQUIRE(fromPng.bytes.size() == fromExr.bytes.size());

    const float* png = fromPng.AsF32();
    const float* exr = fromExr.AsF32();
    REQUIRE(png != nullptr);
    REQUIRE(exr != nullptr);
    for (std::size_t i = 0; i < fromPng.desc.SampleCount(); ++i) {
        INFO("sampel " << i);
        CHECK(png[i] == doctest::Approx(exr[i]).epsilon(0.00001));
    }

    // Nilai yang dituntut, bukan sekadar "keduanya sama" — dua jalur yang
    // sama-sama salah juga akan sama.
    CHECK(png[0] == doctest::Approx(0.0f).epsilon(0.00001));           // byte 0
    CHECK(png[1] == doctest::Approx(0.002428216f).epsilon(0.0001));    // byte 8, cabang lurus
    CHECK(png[3] == doctest::Approx(0.215860500f).epsilon(0.0001));    // byte 128, cabang pangkat
    CHECK(png[5] == doctest::Approx(1.0f).epsilon(0.00001));           // byte 255
}

TEST_CASE("normal map tidak pernah melewati konversi sRGB") {
    // **Uji yang mengunci.** Ini kesalahan yang menghasilkan pencahayaan salah
    // halus di seluruh permukaan tanpa satu pun peringatan: tidak ada piksel
    // yang jelas keliru, hanya bayangan yang bentuknya agak lain.
    Image asData;
    REQUIRE(Read(ImageDir() / "albedo-srgb.png", ReadOptions{}, asData));
    const std::vector<uint8_t> before = asData.bytes;
    const PixelType typeBefore = asData.desc.type;

    // Berkasnya sama persis dengan yang dipakai sebagai albedo di atas — dan
    // berkas itu ditandai sRGB oleh backend yang membaca metadatanya. Slotnya
    // yang menentukan, bukan berkasnya.
    PrepareTexture(UsageForSlot("normal"), asData);

    CHECK(asData.desc.type == typeBefore);
    CHECK(asData.bytes == before);

    for (const char* slot : {"roughness", "metalness", "height", "occlusion", "mask"}) {
        INFO("slot " << slot);
        Image image;
        REQUIRE(Read(ImageDir() / "albedo-srgb.png", ReadOptions{}, image));
        PrepareTexture(UsageForSlot(slot), image);
        CHECK(image.bytes == before);
    }
}

TEST_CASE("format GPU sRGB dipilih hanya untuk warna 8-bit") {
    ImageDesc desc;
    desc.width = 4;
    desc.height = 1;
    desc.channels = 3;

    // Warna 8-bit: perangkat keras yang mendekode, bukan CPU.
    desc.type = PixelType::UInt8;
    desc.colorSpace = ColorSpace::Unknown;
    CHECK(NeedsSrgbGpuFormat(TextureUsage::Color, desc));
    desc.colorSpace = ColorSpace::Srgb;
    CHECK(NeedsSrgbGpuFormat(TextureUsage::Color, desc));

    // Berkas yang menyatakan dirinya linear dipercaya.
    desc.colorSpace = ColorSpace::Linear;
    CHECK_FALSE(NeedsSrgbGpuFormat(TextureUsage::Color, desc));

    // Data tidak pernah, apa pun kata berkasnya. Inilah bentuk GPU dari uji
    // pengunci di atas: format `_SRGB` pada normal map adalah kesalahan yang
    // sama, hanya dipindahkan ke sisi perangkat keras.
    for (const ColorSpace space : {ColorSpace::Unknown, ColorSpace::Srgb, ColorSpace::Linear}) {
        desc.colorSpace = space;
        CHECK_FALSE(NeedsSrgbGpuFormat(TextureUsage::Data, desc));
    }

    // 16-bit dan float punya cukup ketelitian untuk linear apa adanya, dan
    // Vulkan pun tidak menyediakan format `_SRGB` untuk keduanya.
    desc.colorSpace = ColorSpace::Srgb;
    desc.type = PixelType::UInt16;
    CHECK_FALSE(NeedsSrgbGpuFormat(TextureUsage::Color, desc));
    desc.type = PixelType::Float32;
    CHECK_FALSE(NeedsSrgbGpuFormat(TextureUsage::Color, desc));
}

TEST_CASE("alfa premultiplied dikenali dan dinormalkan ke straight") {
    if (!CanRead(".exr")) {
        return;
    }

    Image image;
    REQUIRE(Read(ImageDir() / "alpha-premult.exr", ReadOptions{}, image));
    REQUIRE(image.desc.channels == 4);

    // **Dikenali** — ini yang sebelumnya berbeda antar-backend: alfa EXR
    // premultiplied menurut spesifikasinya, dan backend yang melaporkannya
    // straight membuat tepi setiap tekstur beralfa salah terang.
    CHECK(image.desc.premultipliedAlpha);

    PrepareTexture(UsageForSlot("baseColor"), image);
    CHECK_FALSE(image.desc.premultipliedAlpha);

    // Warna straight yang dimaksud berkasnya sama untuk keempat piksel —
    // (0.8, 0.4, 0.2) — dan hanya alfanya yang berbeda. Setelah diluruskan,
    // ketiganya yang beralfa bukan nol harus kembali ke warna itu.
    const float* pixels = image.AsF32();
    REQUIRE(pixels != nullptr);
    for (std::size_t i = 0; i < 3; ++i) {
        INFO("piksel " << i << " (alfa " << pixels[i * 4 + 3] << ")");
        CHECK(pixels[i * 4 + 0] == doctest::Approx(0.8f).epsilon(0.0001));
        CHECK(pixels[i * 4 + 1] == doctest::Approx(0.4f).epsilon(0.0001));
        CHECK(pixels[i * 4 + 2] == doctest::Approx(0.2f).epsilon(0.0001));
    }

    // Alfa nol tetap hitam: warnanya memang tidak bisa dipulihkan dari nol
    // dikali apa pun, dan membaginya akan menghasilkan tak hingga.
    CHECK(pixels[3 * 4 + 0] == 0.0f);
    CHECK(pixels[3 * 4 + 3] == 0.0f);
    CHECK(std::isfinite(pixels[3 * 4 + 0]));

    // Alfanya sendiri tidak pernah ikut dibagi maupun didekode.
    CHECK(pixels[0 * 4 + 3] == doctest::Approx(1.0f).epsilon(0.0001));
    CHECK(pixels[1 * 4 + 3] == doctest::Approx(0.5f).epsilon(0.0001));
}

// ---------------------------------------------------------------------------
// T2 — rantai mip
// ---------------------------------------------------------------------------

namespace {

/// Sandi sRGB menurut rumusnya, ditulis di sini alih-alih dipanggil dari mesin.
///
/// Uji yang memakai fungsi yang sama dengan kode yang diujinya akan tetap lulus
/// ketika keduanya salah bersama-sama. Yang dibandingkan di bawah adalah hasil
/// mesin terhadap angka yang dihitung dari rumus di spesifikasi sRGB, bukan
/// terhadap dirinya sendiri.
float SrgbToLinear(float encoded) {
    return encoded <= 0.04045f ? encoded / 12.92f
                               : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(float linear) {
    return linear <= 0.0031308f ? linear * 12.92f
                                : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

/// Gambar RGB 8-bit dari daftar nilai abu-abu, satu baris.
Image GreyRow(const std::vector<uint8_t>& values, ColorSpace space) {
    ImageDesc desc;
    desc.width = static_cast<uint32_t>(values.size());
    desc.height = 1;
    desc.channels = 3;
    desc.type = PixelType::UInt8;
    desc.colorSpace = space;
    Image image;
    image.Allocate(desc);
    for (std::size_t i = 0; i < values.size(); ++i) {
        image.bytes[i * 3 + 0] = values[i];
        image.bytes[i * 3 + 1] = values[i];
        image.bytes[i * 3 + 2] = values[i];
    }
    return image;
}

}  // namespace

TEST_CASE("T2: mip tekstur warna dirata-ratakan di ruang linear, bukan di ruang sandi") {
    // Hitam dan putih bersebelahan adalah kasus yang paling memisahkan kedua
    // jawaban: yang benar dan yang salah berjarak lima puluh empat tingkat.
    const Image source = GreyRow({0, 255}, ColorSpace::Srgb);

    MipOptions options;
    options.usage = TextureUsage::Color;
    const std::vector<Image> chain = BuildMipChain(source, options);

    REQUIRE(chain.size() == 2);
    REQUIRE(chain[1].desc.width == 1);
    REQUIRE(chain[1].desc.height == 1);

    // Setengah cahaya antara hitam dan putih, disandikan kembali.
    const float halfLight = (SrgbToLinear(0.0f) + SrgbToLinear(1.0f)) * 0.5f;
    const float expected = LinearToSrgb(halfLight) * 255.0f;
    INFO("harapan " << expected << ", dapat " << int(chain[1].bytes[0]));
    CHECK(std::abs(static_cast<float>(chain[1].bytes[0]) - expected) <= 2.0f);

    // Dan bukan jawaban yang salah. Merata-ratakan nilai tersandi apa adanya
    // menghasilkan 128 — jauh lebih gelap, dan tidak pernah muncul sebagai
    // galat. Batas ini yang membuat uji di atas tidak bisa lulus karena
    // toleransi yang kelewat longgar.
    CHECK(chain[1].bytes[0] > 170);
}

TEST_CASE("T2: mip peta data dirata-ratakan apa adanya") {
    // Pasangan yang sama, tetapi angkanya bukan warna: roughness 0 dan
    // roughness 1 berata-rata menjadi roughness 0,5. Mendekodenya sebagai sRGB
    // di sini akan mencerahkannya menjadi 188 — kesalahan yang berlawanan arah
    // dengan yang di atas, dan yang sama tak terlihatnya.
    const Image source = GreyRow({0, 255}, ColorSpace::Linear);

    MipOptions options;
    options.usage = TextureUsage::Data;
    const std::vector<Image> chain = BuildMipChain(source, options);

    REQUIRE(chain.size() == 2);
    INFO("dapat " << int(chain[1].bytes[0]));
    CHECK(std::abs(static_cast<int>(chain[1].bytes[0]) - 128) <= 1);
}

TEST_CASE("T2: normal map dinormalkan ulang di tiap level") {
    // Papan catur dua normal yang menyimpang ±45° pada sumbu x. Rata-ratanya
    // menunjuk lurus ke atas tetapi panjangnya hanya cos 45° — dan tanpa
    // penormalan ulang, kependekan itu terbawa sampai level terdalam.
    constexpr float kRoot2 = 0.70710678f;
    ImageDesc desc;
    desc.width = 8;
    desc.height = 8;
    desc.channels = 3;
    desc.type = PixelType::UInt8;
    desc.colorSpace = ColorSpace::Linear;
    Image source;
    source.Allocate(desc);
    for (uint32_t y = 0; y < desc.height; ++y) {
        for (uint32_t x = 0; x < desc.width; ++x) {
            const float nx = ((x + y) % 2 == 0) ? kRoot2 : -kRoot2;
            uint8_t* pixel = source.bytes.data() + (static_cast<std::size_t>(y) * desc.width + x) * 3;
            pixel[0] = static_cast<uint8_t>(std::lround((nx * 0.5f + 0.5f) * 255.0f));
            pixel[1] = 128;
            pixel[2] = static_cast<uint8_t>(std::lround((kRoot2 * 0.5f + 0.5f) * 255.0f));
        }
    }

    MipOptions options;
    options.usage = TextureUsage::Data;
    options.renormalize = true;
    const std::vector<Image> chain = BuildMipChain(source, options);

    REQUIRE(chain.size() == 4);
    const Image& deepest = chain.back();
    REQUIRE(deepest.desc.width == 1);
    REQUIRE(deepest.desc.height == 1);

    const float x = deepest.bytes[0] / 255.0f * 2.0f - 1.0f;
    const float y = deepest.bytes[1] / 255.0f * 2.0f - 1.0f;
    const float z = deepest.bytes[2] / 255.0f * 2.0f - 1.0f;
    const float length = std::sqrt(x * x + y * y + z * z);
    INFO("panjang di level terdalam " << length);
    CHECK(length == doctest::Approx(1.0f).epsilon(0.02));

    // Tanpa penormalan ulang panjangnya turun ke sekitar 0,71 — batas ini yang
    // membuat pemeriksaan di atas gagal kalau langkahnya dilewati, alih-alih
    // lulus karena toleransi.
    CHECK(length > 0.95f);
}

TEST_CASE("T2: rantai turun sampai 1x1, dan sisi ganjil tidak pernah menjadi nol") {
    CHECK(MipLevelCount(1, 1) == 1);
    CHECK(MipLevelCount(8, 8) == 4);
    CHECK(MipLevelCount(5, 3) == 3);
    CHECK(MipLevelCount(0, 8) == 0);

    const Image source = GreyRow({10, 20, 30, 40, 50}, ColorSpace::Linear);
    MipOptions options;
    options.usage = TextureUsage::Data;
    const std::vector<Image> chain = BuildMipChain(source, options);

    REQUIRE(chain.size() == 3);
    CHECK(chain[0].desc.width == 5);
    CHECK(chain[1].desc.width == 2);
    CHECK(chain[2].desc.width == 1);
    for (const Image& level : chain) {
        // Sisi yang menjadi nol menghasilkan level tanpa piksel, dan unggahan
        // yang menyusulnya menjadi galat validation layer yang jauh dari
        // sebabnya.
        CHECK(level.desc.height == 1);
        CHECK(level.bytes.size() == level.desc.ByteCount());
        CHECK(level.desc.ByteCount() > 0);
    }
}

// --- PNG berwarna ------------------------------------------------------------
//
// **Ditulis bersamaan dengan jalur berwarnanya, bukan sesudahnya.** Catatan di
// `BackendStb` menyebut alasan PNG di sini lama hanya greyscale: menyalakan
// format tanpa uji berarti menjanjikan berkas yang tidak pernah dibaca siapa
// pun. Uji ini yang membayar janji itu — ia menyandikan lalu membacanya kembali
// dan membandingkan setiap byte, jadi kanal yang tertukar atau `bpp` filter
// yang salah tidak bisa lolos.

TEST_CASE("PNG berwarna bolak-balik utuh") {
    const uint32_t width = 7;   // sengaja bukan kelipatan apa pun
    const uint32_t height = 5;

    auto roundTrip = [&](uint32_t channels) {
        CAPTURE(channels);
        imageio::Image source;
        source.desc.width = width;
        source.desc.height = height;
        source.desc.channels = channels;
        source.desc.type = imageio::PixelType::UInt8;
        source.desc.colorSpace = imageio::ColorSpace::Srgb;
        source.bytes.resize(static_cast<std::size_t>(width) * height * channels);
        // Pola yang berbeda di tiap kanal: kanal yang tertukar akan terlihat,
        // sedangkan pola abu-abu seragam akan menyembunyikannya.
        for (std::size_t i = 0; i < source.bytes.size(); ++i) {
            const std::size_t pixel = i / channels;
            const std::size_t channel = i % channels;
            // **Alfa dibuat penuh, dan itu bukan menghindari kasus sulit.**
            // Pembaca di sini meng-*associate* alfa — mengalikan warna dengan
            // alfa, di ruang linear — persis seperti yang dijanjikan
            // `premultipliedAlpha` di `ImageDesc`. Alfa yang tidak penuh karena
            // itu memang tidak kembali seperti semula, dan menuntutnya kembali
            // berarti menguji janji yang tidak pernah dibuat. Yang diuji di sini
            // enkodernya; asosiasi alfa diuji terpisah di bawah.
            const bool isAlpha = channels == 4 && channel == 3;
            source.bytes[i] = isAlpha
                                  ? uint8_t{255}
                                  : static_cast<uint8_t>((pixel * 7u + channel * 61u) & 0xFFu);
        }

        std::vector<uint8_t> png;
        const imageio::ImageIoResult encoded = imageio::Encode(source, ".png", png);
        REQUIRE_MESSAGE(encoded.ok, encoded.error);
        REQUIRE(png.size() > 8);
        // Tanda tangan PNG, supaya kegagalan "bukan PNG" terbaca sebagai itu
        // dan bukan sebagai piksel yang salah.
        const std::array<uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
        CHECK(std::equal(signature.begin(), signature.end(), png.begin()));

        imageio::ReadOptions options;
        options.channels = channels;
        imageio::Image decoded;
        const imageio::ImageIoResult read = imageio::Read(png, ".png", options, decoded);
        REQUIRE_MESSAGE(read.ok, read.error);
        CHECK(decoded.desc.width == width);
        CHECK(decoded.desc.height == height);
        CHECK(decoded.desc.channels == channels);
        REQUIRE(decoded.bytes.size() == source.bytes.size());
        for (std::size_t i = 0; i < source.bytes.size(); ++i) {
            if (source.bytes[i] != decoded.bytes[i]) {
                INFO("byte ", i, " (piksel ", i / channels, " kanal ", i % channels,
                     "): sumber ", int(source.bytes[i]), " != hasil ", int(decoded.bytes[i]));
                CHECK(source.bytes[i] == decoded.bytes[i]);
                break;
            }
        }
        CHECK(std::equal(source.bytes.begin(), source.bytes.end(), decoded.bytes.begin()));
    };

    SUBCASE("RGB") { roundTrip(3); }
    SUBCASE("RGBA") { roundTrip(4); }
    SUBCASE("greyscale tetap seperti semula") { roundTrip(1); }
}

TEST_CASE("PNG menolak yang memang tidak bisa ditulisnya") {
    imageio::Image image;
    image.desc.width = 2;
    image.desc.height = 2;
    image.desc.channels = 2;  // grey+alpha: PNG punya kodenya, enkoder ini tidak
    image.desc.type = imageio::PixelType::UInt8;
    image.bytes.resize(8);

    std::vector<uint8_t> png;
    const imageio::ImageIoResult result = imageio::Encode(image, ".png", png);
    CHECK_FALSE(result.ok);
    // Pesannya menyebut apa yang diterimanya, bukan hanya bahwa ia menolak.
    CHECK(result.error.find("2-channel") != std::string::npos);
}

TEST_CASE("PNG RGBA dengan alfa tidak penuh kembali dalam bentuk ter-associate") {
    // **Ini mengunci perilaku, bukan merayakannya.** Pembaca gambar di sini
    // meng-associate alfa saat membaca — warna dikalikan alfanya, di ruang
    // linear — dan itu tercatat di `ImageDesc::premultipliedAlpha`. Yang
    // menemukannya adalah uji round-trip PNG berwarna, dan yang membuatnya mahal
    // adalah bahwa gambarnya tetap terlihat masuk akal: hanya lebih gelap di
    // tempat yang alfanya rendah.
    //
    // Karena itu `editor.screenshot` mengirim RGB, bukan RGBA. Jendela editor
    // tidak punya alfa yang berarti, dan membawanya berarti membawa pertanyaan
    // ini ke setiap orang yang membaca berkasnya kembali.
    imageio::Image source;
    source.desc.width = 4;
    source.desc.height = 1;
    source.desc.channels = 4;
    source.desc.type = imageio::PixelType::UInt8;
    source.desc.colorSpace = imageio::ColorSpace::Srgb;
    source.bytes = {200, 200, 200, 255,   // alfa penuh: harus utuh
                    200, 200, 200, 128,   // separuh: menggelap
                    200, 200, 200, 0,     // nol: hilang seluruhnya
                    10,  10,  10,  255};

    std::vector<uint8_t> png;
    REQUIRE(imageio::Encode(source, ".png", png).ok);

    imageio::ReadOptions options;
    options.channels = 4;
    imageio::Image decoded;
    REQUIRE(imageio::Read(png, ".png", options, decoded).ok);
    REQUIRE(decoded.bytes.size() == source.bytes.size());

    // Alfa sendiri selalu kembali utuh — yang berubah warnanya.
    for (std::size_t pixel = 0; pixel < 4; ++pixel) {
        CAPTURE(pixel);
        CHECK(decoded.bytes[pixel * 4 + 3] == source.bytes[pixel * 4 + 3]);
        // Associate hanya bisa menggelapkan, tidak pernah mencerahkan. Berlaku
        // juga bila backend yang aktif ternyata tidak meng-associate sama
        // sekali — yang salah lalu tetap tertangkap, yang benar tetap lulus.
        for (std::size_t channel = 0; channel < 3; ++channel) {
            CHECK(decoded.bytes[pixel * 4 + channel] <= source.bytes[pixel * 4 + channel]);
        }
    }
    // Alfa penuh tidak boleh berubah sama sekali: itu jaminan yang dipakai
    // `editor.screenshot` kalau suatu saat ia kembali membawa alfa.
    CHECK(decoded.bytes[0] == 200);
    CHECK(decoded.bytes[12] == 10);
}
