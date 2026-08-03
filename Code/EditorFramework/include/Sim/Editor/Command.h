#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sim::editor {

/// Satu perubahan data yang bisa dibatalkan.
///
/// Seam #3 di docs/ARCHITECTURE.md. Panel tidak pernah menulis langsung ke
/// data; panel membuat command dan menyerahkannya ke CommandHistory. Aturan ini
/// berlaku sama untuk panel, script Lua, dan tool MCP — tanpa pengecualian,
/// karena satu jalur tulis yang lolos sudah cukup membuat undo tidak bisa
/// dipercaya.
class ICommand {
public:
    virtual ~ICommand() = default;

    virtual void Do() = 0;
    virtual void Undo() = 0;

    /// Nama yang tampil di panel History. Sebaiknya menyebut objeknya juga
    /// ("Move Shader Ball"), bukan hanya jenis operasinya.
    virtual std::string Name() const = 0;

    /// Menyerap `next` ke dalam command ini bila keduanya bagian dari satu
    /// gerakan pengguna. Dipakai supaya menyeret slider atau gizmo menghasilkan
    /// satu entri undo, bukan ratusan.
    ///
    /// Hanya dipanggil bila kelompok penggabungan masih terbuka — lihat
    /// CommandHistory::CloseMergeGroup().
    virtual bool MergeWith(const ICommand& next) {
        (void)next;
        return false;
    }

    /// Perkiraan memori yang ditahan command ini. Command yang menyimpan data
    /// besar — subtree entity pada DeleteEntitiesCommand dan PasteEntitiesCommand
    /// — wajib meng-override ini, kalau tidak batas memori history jadi tidak
    /// berarti.
    ///
    /// Sempat direncanakan bahwa patch heightmap E7.3 lewat sini juga. Ternyata
    /// tidak bisa: terrain adalah dokumen yang dibuka dan ditutup, sedangkan
    /// CommandHistory tidak punya cakupan dokumen. Membuka terrain lain akan
    /// meninggalkan command yang membatalkan goresan pada heightmap yang sama
    /// sekali berbeda. Karena itu Terrain menyimpan jurnalnya sendiri, sama
    /// seperti Material, Graph, dan Script Editor.
    virtual std::size_t MemoryCost() const { return sizeof(ICommand) + 64; }
};

/// Command sederhana dari sepasang lambda. Untuk operasi yang tidak menyimpan
/// keadaan besar dan tidak perlu digabung.
class LambdaCommand final : public ICommand {
public:
    LambdaCommand(std::string name, std::function<void()> doFn, std::function<void()> undoFn)
        : name_(std::move(name)), do_(std::move(doFn)), undo_(std::move(undoFn)) {}

    void Do() override { do_(); }
    void Undo() override { undo_(); }
    std::string Name() const override { return name_; }

private:
    std::string name_;
    std::function<void()> do_;
    std::function<void()> undo_;
};

/// Tumpukan undo/redo editor.
///
/// Dirty flag ikut di sini, bukan di dokumen: satu-satunya cara data berubah
/// adalah lewat command, jadi di sinilah tempat yang tahu apakah ada perubahan
/// yang belum disimpan.
class CommandHistory {
public:
    struct Entry {
        std::string name;
        std::size_t memoryCost = 0;
    };

    void Execute(std::unique_ptr<ICommand> command);

    template <class T, class... Args>
    void Execute(Args&&... args) {
        Execute(std::make_unique<T>(std::forward<Args>(args)...));
    }

    /// Menutup kelompok penggabungan: command berikutnya pasti membuat entri
    /// baru. Dipanggil saat pengguna melepas slider atau gizmo.
    void CloseMergeGroup() { mergeOpen_ = false; }

    bool CanUndo() const { return cursor_ > 0; }
    bool CanRedo() const { return cursor_ < static_cast<int>(entries_.size()); }
    bool Undo();
    bool Redo();

    /// Melompat ke titik mana pun di history. `index` adalah jumlah command
    /// yang diterapkan: 0 berarti keadaan awal.
    void JumpTo(int index);

    /// Menggabungkan semua command sampai EndTransaction() jadi satu entri.
    /// Aman bersarang; hanya lapisan terluar yang membuat entri.
    void BeginTransaction(std::string name);
    void EndTransaction();

    void Clear();

    /// Membuang entri terlama saat total memori melewati batas. 0 = tanpa batas.
    void SetMemoryBudget(std::size_t bytes);
    std::size_t MemoryUsed() const { return memoryUsed_; }

    bool IsDirty() const { return cursor_ != savedCursor_; }
    void MarkSaved() { savedCursor_ = cursor_; }

    const std::vector<Entry>& Entries() const { return entries_; }
    /// Jumlah command yang sedang diterapkan.
    int Cursor() const { return cursor_; }

    /// Dipanggil setiap kali isi history berubah. Dipakai shell untuk
    /// memperbarui judul jendela dan panel History.
    void SetOnChanged(std::function<void()> callback) { onChanged_ = std::move(callback); }

private:
    void Push(std::unique_ptr<ICommand> command);
    void TrimToBudget();
    void Changed();

    std::vector<std::unique_ptr<ICommand>> commands_;
    std::vector<Entry> entries_;
    /// Jumlah command yang diterapkan. commands_[cursor_-1] adalah yang teratas.
    int cursor_ = 0;
    int savedCursor_ = 0;
    bool mergeOpen_ = false;

    std::size_t memoryBudget_ = 64u * 1024u * 1024u;
    std::size_t memoryUsed_ = 0;

    int transactionDepth_ = 0;
    std::string transactionName_;
    std::vector<std::unique_ptr<ICommand>> transactionCommands_;

    std::function<void()> onChanged_;
};

}  // namespace sim::editor
