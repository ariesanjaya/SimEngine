#pragma once

#include <string>
#include <vector>

namespace sim::editor {

enum class NotificationLevel {
    Info,
    Success,
    Warning,
    Error,
};

/// Pesan singkat di pojok kanan-bawah.
///
/// Dipakai untuk hasil operasi yang pengguna picu sendiri ("Level saved",
/// "Import failed"). Bukan pengganti Console: apa pun yang mungkin perlu dibaca
/// ulang nanti harus tetap masuk log, karena toast menghilang.
class Notifications {
public:
    void Post(NotificationLevel level, std::string message, float seconds = 4.0f);
    void Info(std::string message) { Post(NotificationLevel::Info, std::move(message)); }
    void Success(std::string message) { Post(NotificationLevel::Success, std::move(message)); }
    void Warning(std::string message) { Post(NotificationLevel::Warning, std::move(message)); }
    /// Error bertahan lebih lama: kalau terlewat, pengguna tidak tahu ada yang
    /// gagal sampai menemukannya sendiri.
    void Error(std::string message) { Post(NotificationLevel::Error, std::move(message), 8.0f); }

    /// Menggambar seluruh toast yang masih hidup dan membuang yang kedaluwarsa.
    void Draw(float deltaSeconds);

    std::size_t Count() const { return items_.size(); }
    void Clear() { items_.clear(); }

private:
    struct Item {
        NotificationLevel level = NotificationLevel::Info;
        std::string message;
        float remaining = 0.0f;
        float total = 0.0f;
    };

    std::vector<Item> items_;
};

}  // namespace sim::editor
