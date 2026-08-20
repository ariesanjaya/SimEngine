#pragma once

#include "Sim/RHI/Device.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace sim::rhi {

/// Pengukur waktu GPU per pass, berbasis timestamp query.
///
/// **Timestamp, bukan fence di sekeliling submit.** Fence memberi tahu kapan
/// seluruh submit selesai; biaya sebuah pass menuntut penanda yang ditulis ke
/// dalam command buffer di antara perintah-perintahnya. Menjumlahkan waktu
/// submit lalu membaginya rata antar-pass adalah angka yang terlihat masuk akal
/// dan tidak mengukur apa pun.
///
/// **Hasilnya dibaca terlambat beberapa frame, dan tidak pernah ditunggu.**
/// `vkGetQueryPoolResults` dengan `WAIT_BIT` menghentikan CPU sampai GPU selesai
/// — yaitu persis yang tidak boleh dilakukan alat ukur, karena ia mengubah hal
/// yang sedang diukurnya. Yang ditampilkan karena itu adalah frame beberapa
/// langkah ke belakang; untuk angka yang dibaca manusia, itu tidak berarti apa-apa.
class GpuProfiler {
public:
    struct Scope {
        std::string name;
        double milliseconds = 0.0;
    };

    /// Mengembalikan false bila perangkatnya tidak mendukung timestamp pada
    /// antrean grafis. Bukan kegagalan fatal — profiler yang mati hanya
    /// menghasilkan daftar kosong, dan renderer tetap jalan.
    bool Create(Device& device, uint32_t maxScopesPerFrame = 32);
    void Destroy();

    bool IsValid() const { return pool_ != VK_NULL_HANDLE; }

    /// Dipanggil sekali di awal command buffer sebuah frame.
    void BeginFrame(VkCommandBuffer cmd);
    /// Membuka dan menutup satu lingkup ukur. Tidak bersarang — pass frame graph
    /// memang datar, dan lingkup bersarang menuntut pohon yang tidak ada yang
    /// membutuhkannya.
    void BeginScope(VkCommandBuffer cmd, std::string_view name);
    void EndScope(VkCommandBuffer cmd);
    /// Dipanggil sekali di akhir command buffer, sesudah seluruh lingkup ditutup.
    void EndFrame(VkCommandBuffer cmd);

    /// Hasil frame terakhir yang sudah selesai di GPU. Kosong sampai beberapa
    /// frame pertama lewat.
    std::span<const Scope> Results() const { return results_; }
    /// Naik tepat sekali setiap kali `Results()` benar-benar berganti isi.
    ///
    /// **Ada karena pemungutan boleh gagal, dan gagalnya diam.** `Collect`
    /// melewati hasil yang belum siap alih-alih menunggunya, jadi dua frame
    /// berturut-turut bisa membaca angka yang sama persis. Untuk angka yang
    /// dibaca manusia itu tidak berarti apa-apa; untuk yang merata-ratakan
    /// ratusan frame, itu berarti satu frame ikut berhitung dua kali dan
    /// bobotnya berlipat tanpa ada yang menyadarinya. Yang mengukur karena itu
    /// mencatat sampel hanya saat angka ini berpindah.
    uint64_t ResultsSerial() const { return resultsSerial_; }
    /// Jumlah seluruh lingkup, milidetik.
    double TotalMilliseconds() const;

private:
    static constexpr uint32_t kFrameCount = 3;

    struct Frame {
        std::vector<std::string> names;
        uint32_t first = 0;
        uint32_t count = 0;
        bool pending = false;
    };

    void Collect(uint32_t frame);

    Device* device_ = nullptr;
    VkQueryPool pool_ = VK_NULL_HANDLE;
    uint32_t maxScopes_ = 0;
    /// Nanodetik per tik timestamp. **Wajib dipakai.** Tanpa mengalikannya,
    /// angkanya terlihat masuk akal di satu GPU dan meleset ratusan kali lipat
    /// di GPU lain — dan tidak ada yang akan mencurigai satuannya.
    double nanosecondsPerTick_ = 1.0;
    uint32_t frameIndex_ = 0;
    std::array<Frame, kFrameCount> frames_{};
    std::vector<uint64_t> readback_;
    std::vector<Scope> results_;
    uint64_t resultsSerial_ = 0;
    int openScope_ = -1;
};

}  // namespace sim::rhi
