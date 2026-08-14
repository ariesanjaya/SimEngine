#pragma once

#include "Sim/ImageIO/Image.h"
#include "Sim/ImageIO/TextureColor.h"

#include <cstdint>
#include <vector>

/// Pembangkitan rantai mip.
///
/// **Di sini, bukan di baker.** Yang dikerjakannya hanyalah piksel masuk dan
/// piksel keluar — tidak ada berkas, tidak ada pengaturan aset, tidak ada GPU —
/// dan pertanyaan yang paling menentukan hasilnya, "apakah angka-angka ini warna
/// atau bilangan", sudah punya jawabannya di `TextureColor.h` sebelah.
namespace sim::imageio {

struct MipOptions {
    /// Menentukan apakah penyaringan dilakukan di ruang linear. Inilah satu
    /// keputusan yang salahnya paling mahal di seluruh jalur tekstur.
    TextureUsage usage = TextureUsage::Data;

    /// Menormalkan ulang tiap level. Hanya untuk normal map.
    ///
    /// Rata-rata dua vektor satuan **bukan** vektor satuan — dan makin jauh
    /// keduanya menyimpang, makin pendek hasilnya. Tanpa penormalan ulang, mip
    /// yang dalam pada permukaan berlekuk menjadi berisi normal yang memendek,
    /// dan permukaan yang jauh perlahan tampak makin datar dan makin gelap.
    /// Itu terlihat seperti masalah pencahayaan, dan dicari di tempat yang salah.
    bool renormalize = false;
};

/// Jumlah level rantai mip penuh, sampai 1×1.
uint32_t MipLevelCount(uint32_t width, uint32_t height);

/// Membangun rantai mip lengkap. Level 0 adalah salinan sumbernya.
///
/// **Disaring di ruang linear untuk `Color`.** Merata-ratakan nilai sRGB apa
/// adanya menghitung rata-rata angka yang tersandi, bukan rata-rata cahaya:
/// hitam 0 dan putih 255 merata-rata menjadi 128, padahal setengah cahaya antara
/// keduanya adalah sRGB 188. Kesalahannya menggelapkan setiap mip, makin dalam
/// makin gelap, dan tidak pernah muncul sebagai galat — yang terlihat hanyalah
/// permukaan yang menggelap saat kamera menjauh.
///
/// **Tiap level dikecilkan dari level sebelumnya**, bukan dari level 0. Yang
/// dari level 0 lebih tepat pada ukuran bukan kelipatan dua — pengecilan
/// berurutan menggeser gambar sampai setengah teksel per level di sana — tetapi
/// menuntut membaca seluruh sumbernya sekali per level, dan untuk tekstur 4K itu
/// sepuluh kali lipat kerjanya. Untuk ukuran kelipatan dua keduanya identik.
///
/// Rantainya kosong bila sumbernya kosong.
std::vector<Image> BuildMipChain(const Image& source, const MipOptions& options);

}  // namespace sim::imageio
