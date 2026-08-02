#include "Sim/Editor/Command.h"

#include "Sim/Core/Assert.h"

#include <utility>

namespace sim::editor {
namespace {

/// Membungkus beberapa command jadi satu entri undo.
class TransactionCommand final : public ICommand {
public:
    TransactionCommand(std::string name, std::vector<std::unique_ptr<ICommand>> commands)
        : name_(std::move(name)), commands_(std::move(commands)) {}

    void Do() override {
        for (auto& command : commands_) {
            command->Do();
        }
    }

    void Undo() override {
        // Terbalik: command belakangan bisa bergantung pada hasil command
        // sebelumnya, jadi membatalkannya harus dari yang paling akhir.
        for (auto it = commands_.rbegin(); it != commands_.rend(); ++it) {
            (*it)->Undo();
        }
    }

    std::string Name() const override { return name_; }

    std::size_t MemoryCost() const override {
        std::size_t total = sizeof(TransactionCommand);
        for (const auto& command : commands_) {
            total += command->MemoryCost();
        }
        return total;
    }

private:
    std::string name_;
    std::vector<std::unique_ptr<ICommand>> commands_;
};

}  // namespace

void CommandHistory::Execute(std::unique_ptr<ICommand> command) {
    if (command == nullptr) {
        return;
    }
    command->Do();

    if (transactionDepth_ > 0) {
        transactionCommands_.push_back(std::move(command));
        return;
    }

    // Coba gabungkan ke command teratas selama kelompoknya masih terbuka.
    if (mergeOpen_ && cursor_ > 0 && cursor_ == static_cast<int>(commands_.size())) {
        ICommand& top = *commands_[static_cast<std::size_t>(cursor_) - 1];
        if (top.MergeWith(*command)) {
            Entry& entry = entries_[static_cast<std::size_t>(cursor_) - 1];
            memoryUsed_ -= entry.memoryCost;
            entry.memoryCost = top.MemoryCost();
            entry.name = top.Name();
            memoryUsed_ += entry.memoryCost;
            Changed();
            return;
        }
    }

    Push(std::move(command));
}

void CommandHistory::Push(std::unique_ptr<ICommand> command) {
    // Membuang cabang redo. Setelah undo lalu melakukan sesuatu yang baru,
    // langkah yang tadi dibatalkan tidak bisa dicapai lagi.
    while (static_cast<int>(commands_.size()) > cursor_) {
        memoryUsed_ -= entries_.back().memoryCost;
        commands_.pop_back();
        entries_.pop_back();
    }
    // Titik "tersimpan" ikut hilang bila berada di cabang yang dibuang.
    if (savedCursor_ > cursor_) {
        savedCursor_ = -1;
    }

    Entry entry;
    entry.name = command->Name();
    entry.memoryCost = command->MemoryCost();
    memoryUsed_ += entry.memoryCost;

    commands_.push_back(std::move(command));
    entries_.push_back(std::move(entry));
    ++cursor_;
    mergeOpen_ = true;

    TrimToBudget();
    Changed();
}

bool CommandHistory::Undo() {
    if (!CanUndo()) {
        return false;
    }
    --cursor_;
    commands_[static_cast<std::size_t>(cursor_)]->Undo();
    mergeOpen_ = false;
    Changed();
    return true;
}

bool CommandHistory::Redo() {
    if (!CanRedo()) {
        return false;
    }
    commands_[static_cast<std::size_t>(cursor_)]->Do();
    ++cursor_;
    mergeOpen_ = false;
    Changed();
    return true;
}

void CommandHistory::JumpTo(int index) {
    const int target = index < 0 ? 0
                                 : (index > static_cast<int>(commands_.size())
                                        ? static_cast<int>(commands_.size())
                                        : index);
    while (cursor_ > target) {
        --cursor_;
        commands_[static_cast<std::size_t>(cursor_)]->Undo();
    }
    while (cursor_ < target) {
        commands_[static_cast<std::size_t>(cursor_)]->Do();
        ++cursor_;
    }
    mergeOpen_ = false;
    Changed();
}

void CommandHistory::BeginTransaction(std::string name) {
    if (transactionDepth_ == 0) {
        transactionName_ = std::move(name);
        transactionCommands_.clear();
    }
    ++transactionDepth_;
}

void CommandHistory::EndTransaction() {
    SIM_ASSERT(transactionDepth_ > 0, "EndTransaction without a matching BeginTransaction");
    if (transactionDepth_ == 0) {
        return;
    }
    --transactionDepth_;
    if (transactionDepth_ > 0) {
        return;
    }
    if (transactionCommands_.empty()) {
        return;
    }
    // Command di dalam transaksi sudah dijalankan saat Execute(); yang dibuat
    // di sini hanya pembungkusnya, jadi Do() tidak boleh dipanggil lagi.
    Push(std::make_unique<TransactionCommand>(transactionName_,
                                              std::move(transactionCommands_)));
    transactionCommands_.clear();
}

void CommandHistory::Clear() {
    commands_.clear();
    entries_.clear();
    transactionCommands_.clear();
    transactionDepth_ = 0;
    cursor_ = 0;
    savedCursor_ = 0;
    memoryUsed_ = 0;
    mergeOpen_ = false;
    Changed();
}

void CommandHistory::SetMemoryBudget(std::size_t bytes) {
    memoryBudget_ = bytes;
    TrimToBudget();
}

void CommandHistory::TrimToBudget() {
    if (memoryBudget_ == 0) {
        return;
    }
    // Membuang dari yang terlama. Entri yang dibuang tidak bisa di-undo lagi,
    // jadi titik "tersimpan" ikut bergeser supaya dirty flag tidak berbohong.
    while (memoryUsed_ > memoryBudget_ && commands_.size() > 1 && cursor_ > 0) {
        memoryUsed_ -= entries_.front().memoryCost;
        commands_.erase(commands_.begin());
        entries_.erase(entries_.begin());
        --cursor_;
        if (savedCursor_ > 0) {
            --savedCursor_;
        } else {
            savedCursor_ = -1;
        }
    }
}

void CommandHistory::Changed() {
    if (onChanged_) {
        onChanged_();
    }
}

}  // namespace sim::editor
