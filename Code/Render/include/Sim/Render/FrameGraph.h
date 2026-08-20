#pragma once

#include "Sim/Core/Math.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sim::render {

/// Bagaimana sebuah pass memakai sebuah resource.
///
/// Inilah satu-satunya hal yang dideklarasikan pass tentang resource-nya, dan
/// dari sinilah seluruh barrier diturunkan. Daftarnya sengaja pendek: setiap
/// nilai tambahan adalah satu baris lagi di tabel transisi, dan tabel transisi
/// yang besar adalah tabel yang salah di sudut yang jarang dipakai.
enum class Access : uint8_t {
    /// Tidak dipakai. Keadaan awal sebuah resource transien.
    None,
    ColorWrite,
    DepthWrite,
    /// Depth dibaca sebagai depth (mis. depth test tanpa tulis), bukan sebagai
    /// tekstur.
    DepthRead,
    ShaderRead,
    ShaderWrite,
    TransferRead,
    TransferWrite,
    /// Dibaca `vkCmdDraw*Indirect` sebagai perintah gambar.
    ///
    /// **Tahapnya `DRAW_INDIRECT`, bukan tahap shader mana pun**, dan itu yang
    /// membuatnya harus punya nilai sendiri: perintah dibaca sebelum tahap
    /// vertex berjalan, jadi barrier yang menunggu tahap vertex terlambat satu
    /// tahap penuh — dan yang terbaca adalah perintah frame sebelumnya.
    IndirectRead,
    /// Dibaca sebagai hasil akhir — dipakai target yang diserahkan ke ImGui.
    Present,
};

const char* ToString(Access access);
/// Akses yang tidak menulis apa pun. Dua pass yang membaca dengan cara yang sama
/// tidak perlu dipisahkan barrier — itu yang membuat pembacaan bisa berjalan
/// berbarengan.
bool IsReadOnly(Access access);

enum class ResourceKind : uint8_t {
    Texture,
    Buffer,
};

/// Ukuran sebuah resource transien.
///
/// Formatnya sengaja sebuah kode buram (`uint32_t`), bukan `VkFormat`: header ini
/// dipakai test dan modul yang tidak boleh melihat Vulkan sama sekali. Backend
/// yang menjalankannya menerjemahkan kodenya sendiri.
struct ResourceDesc {
    ResourceKind kind = ResourceKind::Texture;
    uint32_t width = 0;
    uint32_t height = 0;
    /// Kode format milik backend. Nol berarti "sama dengan target akhir".
    uint32_t format = 0;
    uint32_t samples = 1;
    /// Ukuran buffer dalam byte. Hanya untuk `ResourceKind::Buffer`.
    uint64_t bytes = 0;

    /// Dua resource bisa berbagi memori hanya kalau bentuknya sama persis.
    bool SameShape(const ResourceDesc& other) const;
};

using ResourceId = uint32_t;
using PassId = uint32_t;
inline constexpr ResourceId kInvalidResource = 0xFFFFFFFFu;
inline constexpr PassId kInvalidPass = 0xFFFFFFFFu;

/// Perpindahan keadaan sebuah resource yang harus terjadi sebelum sebuah pass.
struct Barrier {
    ResourceId resource = kInvalidResource;
    Access from = Access::None;
    Access to = Access::None;
};

/// Pass yang benar-benar dijalankan, beserta barrier yang **mendahuluinya**.
///
/// Mendahului, bukan mengikuti — dan itu harus dipegang satu arah saja. Barrier
/// yang artinya "sebelum" di satu tempat dan "sesudah" di tempat lain adalah
/// barrier yang dipasang di sisi yang salah tepat pada transisi yang paling
/// jarang dilalui. Transisi yang memang harus terjadi sesudah seluruh pass
/// selesai punya tempatnya sendiri: `CompiledGraph::finalBarriers`.
struct CompiledPass {
    PassId pass = kInvalidPass;
    /// Nama pass, disalin dari graph. Ada supaya hasil kompilasi bisa
    /// menjelaskan dirinya sendiri — pengukur waktu GPU dan pesan galat sama-
    /// sama menyebut pass menurut namanya, bukan menurut nomornya.
    std::string name;
    std::vector<Barrier> barriers;
};

/// Hasil kompilasi: urutan jalan, barrier, dan pembagian memori transien.
struct CompiledGraph {
    bool ok = false;
    std::string error;
    std::vector<CompiledPass> order;
    /// Barrier yang dijalankan **sesudah** pass terakhir, untuk mengembalikan
    /// keluaran ke keadaan yang dijanjikannya. Tanpa ini, target yang diserahkan
    /// ke ImGui masih berada dalam keadaan tulis — dan yang terlihat adalah
    /// gambar frame sebelumnya, kadang.
    std::vector<Barrier> finalBarriers;
    /// Untuk tiap resource: slot memori yang dipakainya. Resource yang umurnya
    /// tidak bertumpang tindih boleh berbagi slot. `kInvalidResource` untuk
    /// resource impor, yang memorinya bukan milik graph.
    std::vector<ResourceId> memorySlot;
    /// Jumlah slot yang benar-benar dipakai. Dipakai test anggaran memori dan
    /// panel statistik.
    uint32_t slotCount = 0;
    /// Pass yang dibuang karena tidak ada yang membaca hasilnya.
    std::vector<PassId> culled;
};

/// Frame graph: pass dan resource dideklarasikan, urutan dan barrier
/// disimpulkan.
///
/// **Barrier tidak ditulis tangan.** Barrier yang ditulis tangan benar pada hari
/// ia ditulis dan salah setelah pass ketiga disisipkan di antaranya — dan
/// salahnya tidak kelihatan sebagai kesalahan melainkan sebagai kedipan yang
/// muncul di satu kartu grafis saja. Di sini tiap pass hanya menyatakan apa yang
/// dibacanya dan apa yang ditulisnya; perpindahan keadaannya diturunkan dari
/// deklarasi itu, jadi menyisipkan pass tidak bisa meninggalkan barrier basi.
///
/// **Pass yang hasilnya tidak dibaca siapa pun dibuang.** Tanpa itu, tiap fitur
/// yang bisa dimatikan — bayangan, SSAO, garis bantu debug — menuntut `if` di
/// dalam kode frame, dan kode frame yang penuh cabang adalah kode yang tiap
/// cabangnya harus diuji sendiri. Dengan pembuangan otomatis, mematikan sebuah
/// fitur cukup berarti tidak ada yang membaca keluarannya.
///
/// **Graph dibangun ulang tiap frame.** Karena itu id-nya indeks, bukan pointer:
/// pointer ke dalam vektor yang tumbuh adalah bug menggantung yang paling
/// klasik, dan graph yang dibangun ulang tiap frame menumbuhkan vektornya tiap
/// frame.
class FrameGraph {
public:
    void Clear();

    /// Resource transien: dimiliki graph, boleh berbagi memori dengan transien
    /// lain yang umurnya tidak bertumpang tindih.
    ResourceId CreateTexture(std::string name, const ResourceDesc& desc);
    ResourceId CreateBuffer(std::string name, uint64_t bytes);
    /// Resource dari luar — target viewport, tekstur aset. Graph tidak memiliki
    /// memorinya dan tidak pernah mengalias-kannya.
    ResourceId Import(std::string name, Access initial);

    PassId AddPass(std::string name);
    /// Menyatakan pass membaca sebuah resource. Pass yang membaca resource yang
    /// tidak pernah ditulis siapa pun adalah kesalahan yang dilaporkan saat
    /// `Compile`, bukan pembacaan diam-diam atas isi sampah.
    void Read(PassId pass, ResourceId resource, Access access);
    void Write(PassId pass, ResourceId resource, Access access);
    /// Keluaran yang harus bertahan. Apa pun yang tidak menyumbang ke salah satu
    /// keluaran akan dibuang.
    void SetOutput(ResourceId resource, Access finalAccess);

    /// Pass yang harus selalu dijalankan walaupun keluarannya tidak dibaca —
    /// mis. pass yang menulis ke resource impor milik orang lain. Dipakai
    /// hemat: pass yang tidak boleh dibuang adalah pass yang tidak ikut menikmati
    /// guna utama graph ini.
    void SetSideEffect(PassId pass);

    CompiledGraph Compile() const;

    int PassCount() const { return static_cast<int>(passes_.size()); }
    int ResourceCount() const { return static_cast<int>(resources_.size()); }
    std::string_view PassName(PassId pass) const;
    std::string_view ResourceName(ResourceId resource) const;
    const ResourceDesc& Desc(ResourceId resource) const;
    bool Imported(ResourceId resource) const;

private:
    struct Use {
        ResourceId resource = kInvalidResource;
        Access access = Access::None;
        bool write = false;
    };

    struct Pass {
        std::string name;
        std::vector<Use> uses;
        bool sideEffect = false;
    };

    struct Resource {
        std::string name;
        ResourceDesc desc;
        bool imported = false;
        Access initial = Access::None;
        bool output = false;
        Access finalAccess = Access::None;
    };

    std::vector<Pass> passes_;
    std::vector<Resource> resources_;
};

}  // namespace sim::render
