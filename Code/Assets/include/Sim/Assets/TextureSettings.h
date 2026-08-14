#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

/// Pengaturan sebuah aset tekstur, disimpan di sebelah berkasnya.
///
/// **Berkas tersendiri, bukan di dalam `.meta`** — alasan yang sama persis
/// dengan `MeshSettings`: `.meta` memuat identitas aset, ditulis sekali saat
/// aset ditemukan, dan tidak pernah disentuh lagi. Menumpangkan pengaturan yang
/// berubah-ubah di sana berarti setiap penyimpanan menulis ulang berkas yang
/// memegang satu-satunya hal yang tidak boleh hilang.
namespace sim::assets {

/// Untuk apa sebuah tekstur dipakai. Inilah yang menentukan format blok dan
/// colorspace-nya saat di-bake.
///
/// **Dicatat di asetnya, bukan di slot material yang memakainya.** Slot material
/// memang tahu semantiknya — `tAlbedo`, `tNormal` — tetapi satu tekstur bisa
/// dirujuk dua material pada slot yang berbeda, dan saat itu tidak ada satu
/// jawaban benar. Sebuah berkas normal map adalah normal map di mana pun ia
/// dipakai; itu yang diharapkan artis, dan itu yang dilakukan engine lain.
enum class TextureUsage : uint8_t {
    /// Warna dasar, emissive, apa pun yang dilihat mata sebagai warna. sRGB.
    Color,
    /// Peta normal tangent-space. Linear, dan dua kanal sudah cukup.
    NormalMap,
    /// Roughness, metalness, occlusion, atau topeng lain. Linear, satu kanal.
    Mask,
    /// Gambar rentang dinamis tinggi — peta lingkungan, dan sejenisnya.
    Hdr,
    /// Peta ketinggian. Linear, dan presisi lebih berarti daripada ukuran.
    Height,
};

/// Seberapa keras encoder bekerja. Waktu encode versus PSNR.
enum class TextureQuality : uint8_t {
    Fast,
    Balanced,
    Best,
};

/// Bagaimana alpha diperlakukan. Menentukan BC1 versus BC7 untuk `Color`.
enum class TextureAlpha : uint8_t {
    /// Tidak ada alpha sama sekali.
    None,
    /// Alpha hanya menyala atau mati — dedaunan, pagar kawat.
    PunchThrough,
    /// Alpha bergradasi penuh.
    Full,
};

const char* ToString(TextureUsage usage);
const char* ToString(TextureQuality quality);
const char* ToString(TextureAlpha alpha);

struct TextureSettings {
    TextureUsage usage = TextureUsage::Color;
    TextureQuality quality = TextureQuality::Balanced;
    TextureAlpha alpha = TextureAlpha::None;
    /// Boleh dimatikan per aset untuk yang benar-benar butuh presisi.
    bool compress = true;
    bool generateMips = true;

    bool operator==(const TextureSettings& other) const;
};

/// Tebakan awal `usage` dari nama berkasnya.
///
/// **Tebakan, dan disebut tebakan.** Akhiran `_n`, `_normal`, `_rough` adalah
/// kebiasaan yang dipegang sebagian besar artis dan dilanggar sebagian lainnya;
/// menebaknya menghemat ratusan klik dan salah pada sebagian kecil. Yang
/// membuatnya aman adalah bisa diubah — dan bahwa tebakan tidak pernah ditulis
/// ke disk sampai seseorang benar-benar mengubah sesuatu.
TextureUsage GuessUsageFromName(std::string_view fileName);

/// Pengaturan yang berlaku untuk sebuah tekstur **yang belum punya berkas
/// pengaturan** — yaitu bawaan struct ditambah tebakan `usage` dari namanya.
///
/// **Inilah acuan "tidak ada yang diatur", dan bukan `TextureSettings{}`.**
/// Bedanya menentukan: tanpa ini, tekstur bernama `batu_n.png` yang sengaja
/// disetel pengguna ke `Color` akan tersimpan sebagai "sama dengan bawaan",
/// berkasnya dihapus, dan tebakan namanya kembali memaksanya menjadi
/// `NormalMap`. Pengguna lalu tidak punya cara menolak tebakan itu — dan yang
/// tidak bisa ditolak bukan tebakan lagi.
TextureSettings DefaultTextureSettings(const std::filesystem::path& texturePath);

/// Jalur berkas pengaturan untuk sebuah berkas tekstur.
std::filesystem::path TextureSettingsPath(const std::filesystem::path& texturePath);

/// Memuat pengaturan. Berkas yang belum ada berarti `DefaultTextureSettings`,
/// bukan galat — itu keadaan setiap tekstur yang baru diimpor.
///
/// Mengembalikan false hanya bila berkasnya ada tetapi tidak bisa dibaca.
bool LoadTextureSettings(TextureSettings& settings, const std::filesystem::path& texturePath);

/// Menyimpan pengaturan. Yang sama persis dengan `DefaultTextureSettings`
/// **menghapus** berkasnya alih-alih menulis berkas kosong: berkas pengaturan
/// yang tidak mengatur apa pun hanya menambah satu berkas yang harus ikut
/// kontrol versi.
///
/// Dibandingkan terhadap bawaan **berkas itu**, bukan terhadap bawaan struct —
/// lihat `DefaultTextureSettings`.
bool SaveTextureSettings(const TextureSettings& settings,
                         const std::filesystem::path& texturePath);

}  // namespace sim::assets
