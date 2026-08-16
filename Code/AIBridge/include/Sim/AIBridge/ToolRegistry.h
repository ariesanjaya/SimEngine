#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace sim::ai {

/// Seberapa jauh sebuah tool boleh bertindak.
///
/// **Ditetapkan saat registrasi, bukan disimpulkan dari namanya.** Nama tool
/// adalah dokumentasi; tingkat izin adalah aturan. Menyimpulkannya dari awalan
/// nama berarti sebuah tool baru yang lupa mengikuti pola penamaan diam-diam
/// mendapat izin yang salah — dan yang salah ke arah longgar tidak akan terlihat
/// sampai sesudahnya.
enum class ToolPermission : uint8_t {
    /// Hanya membaca. Selalu diizinkan.
    Read,
    /// Mengubah data lewat Command, jadi tercatat dan bisa di-undo.
    Write,
    /// Tidak tertutup oleh undo, atau menyentuh hal di luar project.
    /// Minta persetujuan bahkan di mode `auto`.
    Dangerous,
};

const char* ToString(ToolPermission permission);

/// Seberapa jauh server membiarkan sebuah klien bertindak.
///
/// **Ditegakkan di server, bukan di panel yang menampilkannya.** Kalau yang
/// menegakkannya panel AI Assistant, maka mode ini hanya berlaku untuk klien
/// itu — dan Claude Code yang menyambung ke port yang sama tetap bisa melakukan
/// apa saja. Yang dijanjikan kriteria terima A4 nomor 2 adalah "mode read-only
/// benar-benar menolak semua tool yang mengubah data", dan satu-satunya tempat
/// yang bisa menjanjikan itu untuk semua klien adalah di sini.
enum class PermissionMode : uint8_t {
    /// Hanya tool yang membaca. Yang lain tidak dijalankan, dan tidak muncul di
    /// `tools/list` — agen yang tidak melihatnya tidak membuang panggilan
    /// untuk mencobanya.
    ReadOnly,
    /// Setiap tool yang mengubah data minta persetujuan.
    Ask,
    /// Berjalan sendiri. `Dangerous` tetap minta persetujuan bila ada yang bisa
    /// ditanya — lihat `ToolApprover`.
    Auto,
};

const char* ToString(PermissionMode mode);
/// Mengembalikan false bila teksnya tidak dikenal, dan tidak menyentuh `out`.
///
/// Tidak memilih nilai bawaan untuk teks yang salah: nilai bawaan sebuah
/// pengaturan izin yang dipilih diam-diam adalah persis cara sebuah salah ketik
/// membuka lebih banyak daripada yang diminta.
bool PermissionModeFromString(std::string_view text, PermissionMode& out);

/// Hasil satu pemanggilan tool.
///
/// `isError` di sini adalah **galat tingkat tool**, bukan galat protokol: tool
/// yang menolak argumennya tetap membalas 200 dengan `isError` true, karena yang
/// perlu tahu adalah agen, bukan lapisan transport. Galat protokol (metode tidak
/// dikenal, JSON rusak) diurus `JsonRpc` dan tidak pernah sampai ke sini.
struct ToolResult {
    bool isError = false;
    /// Teks yang dikembalikan ke agen. Untuk hasil terstruktur, ini JSON yang
    /// sudah di-serialisasi; MCP membungkusnya sebagai content bertipe text.
    std::string text;

    /// Bila terisi, hasilnya dikirim sebagai content bertipe image.
    ///
    /// **Byte mentah, bukan base64.** Penyandiannya urusan lapisan protokol,
    /// dan tool yang menyandikan sendiri berarti setiap tool harus tahu bentuk
    /// kawat MCP — sesuatu yang akan berubah tanpa mereka.
    std::vector<uint8_t> imageBytes;
    std::string imageMimeType = "image/png";
};

/// Handler sebuah tool. `argumentsJson` adalah objek JSON `arguments` dari
/// `tools/call`, sebagai teks — "{}" bila agen tidak mengirim apa pun.
using ToolHandler = std::function<ToolResult(std::string_view argumentsJson)>;

struct ToolDefinition {
    std::string name;
    std::string description;
    /// JSON Schema objek argumen, sebagai teks. Wajib objek yang sah — agen
    /// memakainya untuk menyusun panggilan, dan skema yang rusak membuatnya
    /// menebak.
    std::string inputSchemaJson = R"({"type":"object","properties":{}})";
    ToolPermission permission = ToolPermission::Read;

    /// True bila handler-nya menyentuh World, ImGui, atau Vulkan.
    ///
    /// **Ini yang menentukan apakah panggilannya diantrikan ke main thread.**
    /// Menandainya salah ke arah `false` adalah bug yang muncul sebagai crash
    /// acak jauh dari sebabnya, jadi bawaannya `true`: yang harus menyatakan
    /// diri adalah tool yang benar-benar tidak menyentuh apa-apa.
    bool needsMainThread = true;

    ToolHandler handler;
};

/// Ditanya sebelum sebuah tool yang mengubah data dijalankan. `false` menolak.
///
/// **Dipanggil di thread jaringan.** Yang memasangnya bertanggung jawab atas
/// perpindahan thread-nya sendiri: penyetuju di editor memunculkan dialog dan
/// karena itu harus lewat `MainThreadQueue`, sedangkan penyetuju di mode
/// headless membaca sebuah flag dan tidak perlu berpindah ke mana pun.
///
/// Kosong berarti tidak ada yang bisa ditanya. Mode `ask` lalu **menolak**:
/// pengguna yang meminta ditanya tidak sedang meminta agar dilanjutkan diam-diam
/// ketika tidak ada yang bertanya. Mode `auto` sebaliknya melanjutkan, termasuk
/// untuk `Dangerous` — di sana memang tidak ada yang menunggu jawaban.
using ToolApprover = std::function<bool(const ToolDefinition& tool, std::string_view arguments)>;

/// Kumpulan tool yang ditawarkan server ke agen.
///
/// Bukan singleton. Pemiliknya adalah yang menyalakan server — di editor itu
/// `EditorApp`, di SimHeadless nanti sesuatu yang lain — dan keduanya
/// mendaftarkan himpunan tool yang berbeda.
class ToolRegistry {
public:
    /// Mendaftarkan sebuah tool. Nama yang sudah ada **diganti**, bukan
    /// digandakan: `tools/list` yang memuat dua tool bernama sama membuat agen
    /// memilih salah satunya secara sembarang.
    void Register(ToolDefinition definition);

    /// Nullptr bila tidak ada. Pemanggil yang mendapat nullptr membalas
    /// `MethodNotFound`, bukan hasil kosong — "tidak ada tool itu" dan "tool itu
    /// tidak mengembalikan apa-apa" adalah dua jawaban berbeda.
    const ToolDefinition* Find(std::string_view name) const;

    const std::vector<ToolDefinition>& All() const { return tools_; }
    std::size_t Count() const { return tools_.size(); }
    void Clear();

private:
    std::vector<ToolDefinition> tools_;
};

}  // namespace sim::ai
