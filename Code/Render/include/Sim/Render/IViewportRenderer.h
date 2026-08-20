#pragma once

#include "Sim/Render/TraceBackend.h"
#include "Sim/Assets/MeshData.h"
#include "Sim/Render/Types.h"

#include <cstdint>
#include <string>
#include <vector>
#include <span>
#include <string_view>

namespace sim::render {

/// Waktu GPU satu pass, milidetik.
///
/// **Angkanya tertinggal beberapa frame dari yang terlihat**, dan itu disengaja:
/// membacanya tepat waktu menuntut CPU menunggu GPU, yaitu persis yang tidak
/// boleh dilakukan alat ukur. Untuk angka yang dibaca manusia, keterlambatan itu
/// tidak berarti apa-apa.
///
/// `name` menunjuk penyimpanan milik renderer dan hanya sah sampai `Render()`
/// berikutnya — sama disiplinnya dengan span di `ViewportScene`.
struct PassTiming {
    std::string_view name;
    float milliseconds = 0.0f;
};

/// Hitungan yang menjelaskan angka waktu di sebelahnya.
///
/// **Waktu saja tidak bisa dipakai mengambil keputusan.** Sebuah pass bayangan
/// yang mahal bisa berarti terlalu banyak caster, terlalu banyak muka, atau
/// keduanya — dan ketiga jawaban itu menuntut pekerjaan yang berbeda. Angka di
/// sini yang memisahkannya.
struct RenderStats {
    /// Instance buram yang ada di dalam buffer frame ini. Lebih besar daripada
    /// `opaqueDrawn` karena caster di luar pandangan ikut diunggah — bayangannya
    /// dibutuhkan walaupun bendanya tidak terlihat.
    uint32_t opaqueInstances = 0;
    /// Yang benar-benar digambar pandangan utama.
    uint32_t opaqueDrawn = 0;
    uint32_t transparentDrawn = 0;
    /// Instance yang menjatuhkan bayangan, sebelum penyaringan per muka.
    uint32_t shadowCasters = 0;
    /// Muka bayangan yang kebagian tempat di atlas frame ini.
    uint32_t shadowFaces = 0;
    /// Lampu yang meminta bayangan dan tidak kebagian.
    ///
    /// **Nol bukan sesuatu yang bisa diandaikan.** Atlas yang penuh membuang
    /// lampu menurut kepentingannya, dan itu keputusan yang benar — tetapi yang
    /// menyalakan lampu keenam belas lalu tidak mendapat bayangan tanpa satu pun
    /// petunjuk kenapa.
    uint32_t shadowLightsDropped = 0;
};

/// Batas antara editor dan rendering (seam #1 di docs/ARCHITECTURE.md).
///
/// Panel Viewport hanya melihat antarmuka ini. Sampai E8 implementasinya adalah
/// StubRenderer (clear + grid prosedural); setelah E8 diganti VulkanRenderer
/// tanpa satu baris pun panel berubah. Header ini sengaja tidak meng-include
/// apa pun dari Vulkan — kalau suatu saat ia perlu, berarti ada seam yang bocor.
class IViewportRenderer {
public:
    virtual ~IViewportRenderer() = default;

    /// Menyesuaikan ukuran target render. Aman dipanggil tiap frame dengan
    /// ukuran yang sama — implementasi yang membangun ulang setiap panggilan
    /// akan membuat panel berkedip saat diseret.
    virtual void Resize(uint32_t width, uint32_t height) = 0;

    /// Memuat geometri sebuah berkas mesh, atau mengembalikan yang sudah dimuat.
    ///
    /// **Di-cache per jalur, dan itu wajib bukan optimisasi.** Pemanggilnya
    /// adalah pembangun daftar gambar, yang berjalan tiap frame untuk setiap
    /// entity; tanpa cache, sebuah FBX sebelas megabyte diurai enam puluh kali
    /// per detik per entity — yang muncul bukan sebagai galat melainkan sebagai
    /// editor yang berhenti merespons begitu sebuah mesh ditetapkan.
    ///
    /// Jalur yang gagal dimuat dicatat sebagai gagal dan tidak dicoba lagi:
    /// mencoba ulang tiap frame berarti membaca berkas rusak enam puluh kali per
    /// detik sambil membanjiri log.
    virtual MeshAsset AcquireMesh(std::string_view path) = 0;

    /// Mesh yang datang sebagai data, bukan sebagai berkas.
    ///
    /// **Dibutuhkan geometri yang dikarang mesin, bukan diimpor** — whitebox
    /// lebih dulu, dan nanti kain serta apa pun yang bentuknya berubah saat
    /// dijalankan. `AcquireMesh` di atas hanya menerima path, jadi geometri yang
    /// tidak punya berkas harus menulis berkas sementara hanya untuk dibaca
    /// kembali sedetik kemudian.
    ///
    /// `key` mengenali mesh ini di dalam cache; `version` naik setiap kali
    /// isinya berubah, dan yang berubah diunggah ulang. Tanpa versi, satu-satunya
    /// pilihan adalah mengunggah ulang tiap frame atau tidak pernah — dan
    /// keduanya salah.
    ///
    /// Bawaannya mengembalikan aset kosong: renderer yang tidak menggambar
    /// apa pun tidak perlu tahu soal ini.
    virtual MeshAsset AcquireMeshData(std::string_view key, const assets::MeshData& data,
                                      uint64_t version) {
        (void)key;
        (void)data;
        (void)version;
        return {};
    }

    /// Tekstur dari berkas, di-cache dan siap diikat sebagai albedo sebuah
    /// ruas.
    ///
    /// Nol berarti tidak ada — berkasnya tidak bisa dibaca, atau jalurnya
    /// kosong. Yang gagal dicatat gagal dan tidak dicoba lagi, aturan yang sama
    /// dengan `AcquireMesh`: berkas rusak yang diurai ulang tiap frame
    /// membanjiri log dengan pesan yang sama sampai tidak ada pesan lain yang
    /// terbaca.
    ///
    /// **Nomor set dan binding-nya sengaja sama dengan yang dihasilkan kompiler
    /// graph material** — set 2, parameter di binding 0, tekstur dan sampler
    /// berselang mulai binding 1. Begitu pipeline material menggantikan
    /// `box.frag`, yang berganti adalah shader-nya, bukan pipa ini.
    /// `path` adalah sebuah `.ktx2` yang **sudah di-bake**, bukan berkas
    /// sumber. Yang membangunnya adalah `assets::TextureBakery`; renderer tidak
    /// pernah mendekode gambar.
    virtual TextureHandle AcquireTexture(std::string_view path) {
        (void)path;
        return kInvalidTexture;
    }

    /// Tekstur placeholder untuk ruas yang punya tekstur tetapi hasil bake-nya
    /// belum ada. Magenta, dan sengaja tidak bisa dikira apa pun yang lain.
    ///
    /// Berbeda arti dari `kInvalidTexture`, yang berarti "ruas ini memang tidak
    /// bertekstur" dan tergambar putih — nilai satuan perkalian.
    virtual TextureHandle PendingTexture() const { return kInvalidTexture; }

    /// Program material untuk pass forward: shader fragmen beserta datanya.
    ///
    /// **SPIR-V, bukan graph** — bentuk yang sama persis dengan
    /// `MaterialPreviewShaders`, dan alasannya sama: yang merakit dan
    /// mengompilasi material adalah editor, dan `Sim::Render` tidak pernah
    /// mengenal `Sim::Material`. Yang digambar viewport adalah shader yang sama
    /// persis dengan yang dilihat pratinjau.
    ///
    /// Tahap vertexnya **tidak** ada di sini: pass forward memakai `box.vert`
    /// miliknya sendiri, jadi yang berganti hanya shader fragmen.
    struct MaterialProgram {
        std::span<const uint32_t> fragmentSpirv;
        /// Blok uniform material, sudah terisi nilai instance-nya.
        std::span<const uint8_t> parameters;
        /// Tekstur tiap slot, urutannya sama dengan deklarasi materialnya.
        /// Slot yang kosong diisi tekstur putih 1x1 oleh renderer.
        std::span<const TextureHandle> textures;
    };

    /// Membangun (atau mengambil kembali) pipeline sebuah material.
    ///
    /// `key` harus berubah setiap kali shader atau datanya berubah — GUID
    /// material ditambah revisinya. Ia yang membuat pemanggil boleh memanggil
    /// ini tiap frame: yang kuncinya sudah dikenal tidak membangun apa pun.
    ///
    /// Mengembalikan `kInvalidMaterial` bila pipeline-nya gagal dibangun, dan
    /// ruas yang menerimanya digambar jalur mundur `box.frag` — bukan tidak
    /// digambar sama sekali.
    virtual MaterialHandle AcquireMaterial(std::string_view key,
                                           const MaterialProgram& program) {
        (void)key;
        (void)program;
        return kInvalidMaterial;
    }

    /// Byte tekstur material yang sedang berada di GPU.
    ///
    /// Ada untuk satu alasan yang ditulis rencananya: penghematan VRAM dari
    /// kompresi blok harus **terukur**, bukan diyakini. Angkanya dijumlahkan
    /// dari yang sungguh diunggah tiap tekstur.
    virtual uint64_t TextureBytes() const { return 0; }

    virtual void Render(const ViewportDesc& desc, const ViewportScene& scene) = 0;

    /// Handle tekstur hasil render terakhir, siap dilempar ke ImGui::Image().
    /// Bisa berubah setelah Resize().
    /// Menyalin gambar terakhir yang digambar ke RGBA8 di memori CPU, tepat
    /// seukuran area viewport.
    ///
    /// **Ada supaya sebuah gambar bisa keluar dari engine tanpa lewat jendela.**
    /// Sampai ini ada, satu-satunya jalan adalah menangkap swapchain dan
    /// memotongnya — yang berarti setiap tool yang mengembalikan gambar mati di
    /// mode headless, dan yang tidak mati pun mengembalikan potongan yang
    /// bergantung pada tata letak panel.
    ///
    /// Bawaannya menolak: perender uji dan perender masa depan tidak wajib
    /// bisa, dan yang tidak bisa harus mengatakannya alih-alih mengembalikan
    /// gambar kosong.
    /// Menyalin seluruh isi kaskade clipmap SDF ke memori host, kaskade demi
    /// kaskade, untuk dibandingkan.
    ///
    /// **Alat verifikasi, bukan alat produksi.** Ia menunggu queue idle. Yang
    /// membutuhkannya: memeriksa bahwa komposit compute menghasilkan voxel yang
    /// sama persis dengan komposit CPU. Perbandingan gambar tidak bisa menjawab
    /// itu — satu-satunya pembaca kaskade SDF adalah penelusuran GI, dan GI
    /// tidak deterministik, jadi selisih apa pun tenggelam dalam derau yang
    /// besarnya tidak diketahui.
    ///
    /// Bawaannya menolak, alasan yang sama dengan `CapturePixels`.
    virtual bool CaptureSdf(std::vector<uint8_t>& out, std::string& error) {
        (void)out;
        error = "this renderer has no SDF clipmap";
        return false;
    }

    virtual bool CapturePixels(std::vector<uint8_t>& outRgba, uint32_t& outWidth,
                               uint32_t& outHeight, std::string& error) {
        (void)outRgba;
        (void)outWidth;
        (void)outHeight;
        error = "this renderer cannot read its pixels back";
        return false;
    }

    virtual TextureHandle ColorTarget() const = 0;

    /// Koordinat tekstur pojok kanan-bawah dari bagian yang benar-benar
    /// digambar.
    ///
    /// Tidak selalu (1,1): implementasi boleh mengalokasikan gambar lebih besar
    /// daripada yang diminta agar tidak perlu mengalokasi ulang setiap panel
    /// digeser sedikit. UI wajib memakai nilai ini saat menggambar, kalau tidak
    /// bagian gambar yang belum diisi ikut terlihat.
    virtual Vec2 ColorTargetUvMax() const = 0;

    virtual uint32_t Width() const = 0;
    virtual uint32_t Height() const = 0;

    /// Name implementasi, ditampilkan di pojok viewport supaya jelas bahwa yang
    /// terlihat masih preview stub, bukan hasil rendering sungguhan.
    virtual const char* Name() const = 0;

    /// Waktu GPU per pass frame yang terakhir selesai. Kosong bila perangkatnya
    /// tidak mendukung timestamp, atau selama beberapa frame pertama.
    virtual std::span<const PassTiming> PassTimings() const { return {}; }

    /// Naik setiap kali `PassTimings()` berganti isi; tetap saat angkanya
    /// terulang karena pemungutan sebuah frame terlewat. Yang merata-ratakan
    /// banyak frame memakai ini untuk tidak menghitung satu frame dua kali.
    virtual uint64_t TimingSerial() const { return 0; }

    /// Waktu CPU per tahap frame terakhir, dalam bentuk yang sama dengan
    /// `PassTimings`.
    ///
    /// **Terpisah dari yang GPU dan memang harus terpisah.** Keduanya mengukur
    /// hal yang berbeda pada frame yang berbeda: angka GPU datang beberapa
    /// frame terlambat karena pemungutannya tidak pernah menunggu, sementara
    /// angka CPU adalah frame yang baru saja lewat. Menjumlahkannya menjadi satu
    /// tabel menghasilkan total yang tidak pernah menjadi waktu frame mana pun.
    ///
    /// Alasan keberadaannya sama dengan `SdfUpdateMilliseconds` di bawah, dan
    /// suatu saat menelannya: biaya yang tidak muncul di tabel mana pun adalah
    /// biaya yang tidak ada yang mengawasinya.
    virtual std::span<const PassTiming> CpuTimings() const { return {}; }

    /// Hitungan frame terakhir. Nol semua bila perendernya tidak melacaknya.
    virtual RenderStats Stats() const { return {}; }

    /// Biaya pembaruan clipmap SDF di CPU, milidetik, beserta jumlah voxel
    /// yang ditulis frame terakhir.
    ///
    /// **CPU, bukan GPU, dan karena itu tidak muncul di `PassTimings`.**
    /// Komposit clipmap masih berjalan di CPU sampai ia pindah ke compute, dan
    /// biaya yang tidak muncul di tabel mana pun adalah biaya yang tidak ada
    /// yang mengawasinya — sementara justru angka inilah yang dibatasi anggaran
    /// 0,4 ms rencana GI.
    virtual float SdfUpdateMilliseconds() const { return 0.0f; }
    virtual uint64_t SdfVoxelsWritten() const { return 0; }

    /// Backend trace yang **benar-benar dipakai**, beserta alasannya. Berbeda
    /// dari yang diminta lewat `ViewportDesc::gi` bila permintaannya tidak bisa
    /// dipenuhi perangkat ini.
    virtual TraceBackendSelection GiBackend() const { return {}; }
};

}  // namespace sim::render
