#include "Sim/Core/FileWatcher.h"

#include "Sim/Core/Log.h"

#include <algorithm>

#if defined(__linux__)
#include <sys/inotify.h>
#include <unistd.h>
#include <cerrno>
#include <unordered_map>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#endif

namespace sim {

#if defined(__linux__)

/// inotify tidak punya mode rekursif: satu watch per direktori, dan direktori
/// baru harus didaftarkan sendiri saat ia muncul. Itulah alasan antarmuka
/// FileWatcher dibentuk mengikuti platform yang lebih sulit — implementasi
/// Windows tinggal mengabaikan sebagian pekerjaan ini.
struct FileWatcher::Impl {
    int fd = -1;
    std::filesystem::path root;
    std::unordered_map<int, std::string> directories;  // watch descriptor -> path relatif

    ~Impl() { Close(); }

    void Close() {
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        directories.clear();
    }

    std::string Relative(const std::filesystem::path& path) const {
        const std::string rootText = root.lexically_normal().string();
        std::string text = path.string();
        // Akar itu sendiri memetakan ke string kosong. Melewatkan kasus ini
        // membuat direktori akar tercatat dengan path absolutnya, dan setiap
        // event di dalamnya dilaporkan sebagai path absolut pula — yang tidak
        // akan pernah cocok dengan path relatif di indeks aset.
        if (text == rootText) {
            return {};
        }
        if (text.size() > rootText.size() + 1 &&
            text.compare(0, rootText.size(), rootText) == 0) {
            text.erase(0, rootText.size() + 1);
        }
        return text;
    }

    bool AddWatch(const std::filesystem::path& directory) {
        constexpr uint32_t kMask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                                   IN_CLOSE_WRITE | IN_DELETE_SELF;
        const int wd = inotify_add_watch(fd, directory.c_str(), kMask);
        if (wd < 0) {
            // ENOSPC berarti max_user_watches habis. Ini kegagalan yang harus
            // dilaporkan, bukan diabaikan: pemantauan separuh pohon jauh lebih
            // berbahaya daripada tidak memantau sama sekali, karena perubahan
            // di bagian yang tak terpantau tidak akan pernah terlihat.
            SIM_WARN("Core", "inotify_add_watch failed for {}: {}", directory.string(),
                     std::strerror(errno));
            return false;
        }
        directories[wd] = Relative(directory);
        return true;
    }

    bool AddWatchesRecursively(const std::filesystem::path& directory) {
        if (!AddWatch(directory)) {
            return false;
        }
        std::error_code error;
        for (std::filesystem::recursive_directory_iterator it(directory, error), end; it != end;
             it.increment(error)) {
            if (error) {
                return false;
            }
            if (it->is_directory(error) && !AddWatch(it->path())) {
                return false;
            }
        }
        return true;
    }
};

FileWatcher::FileWatcher() : impl_(std::make_unique<Impl>()) {}
FileWatcher::~FileWatcher() = default;

bool FileWatcher::Watch(const std::filesystem::path& root) {
    Stop();
    impl_->root = root;
    impl_->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (impl_->fd < 0) {
        SIM_WARN("Core", "inotify_init1 failed: {}", std::strerror(errno));
        return false;
    }
    if (!impl_->AddWatchesRecursively(root)) {
        impl_->Close();
        return false;
    }
    SIM_INFO("Core", "Watching {} ({} directories)", root.string(), impl_->directories.size());
    return true;
}

void FileWatcher::Stop() {
    impl_->Close();
}

bool FileWatcher::IsWatching() const {
    return impl_->fd >= 0;
}

bool FileWatcher::Poll(std::vector<Event>& out) {
    if (impl_->fd < 0) {
        return true;
    }

    // Sejajar dengan alignment inotify_event: pembacaan bisa mengembalikan
    // beberapa event sekaligus dan strukturnya harus dibaca pada batas yang benar.
    alignas(alignof(struct inotify_event)) char buffer[8192];
    bool complete = true;

    for (;;) {
        const ssize_t bytes = ::read(impl_->fd, buffer, sizeof(buffer));
        if (bytes <= 0) {
            if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                SIM_WARN("Core", "inotify read failed: {}", std::strerror(errno));
                complete = false;
            }
            break;
        }

        for (ssize_t offset = 0; offset < bytes;) {
            const auto* event = reinterpret_cast<const struct inotify_event*>(buffer + offset);
            offset += static_cast<ssize_t>(sizeof(struct inotify_event) + event->len);

            if ((event->mask & IN_Q_OVERFLOW) != 0) {
                complete = false;
                continue;
            }
            const auto directory = impl_->directories.find(event->wd);
            if (directory == impl_->directories.end() || event->len == 0) {
                continue;
            }

            // Direktori baru: watch-nya dipasang sekarang, tapi berkas yang
            // sempat masuk sebelum watch terpasang tidak akan pernah dilaporkan.
            // Satu-satunya jawaban jujur adalah meminta pemindaian ulang.
            if ((event->mask & IN_ISDIR) != 0) {
                if ((event->mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
                    impl_->AddWatch(impl_->root / directory->second / event->name);
                }
                complete = false;
                continue;
            }

            Event result;
            if ((event->mask & (IN_CREATE | IN_MOVED_TO)) != 0) {
                result.kind = Event::Kind::Added;
            } else if ((event->mask & (IN_DELETE | IN_MOVED_FROM)) != 0) {
                result.kind = Event::Kind::Removed;
            } else {
                result.kind = Event::Kind::Modified;
            }
            result.path = directory->second.empty()
                              ? std::string(event->name)
                              : directory->second + "/" + event->name;
            out.push_back(std::move(result));
        }
    }
    return complete;
}

#elif defined(_WIN32)

/// ReadDirectoryChangesW memantau seluruh sub-pohon lewat satu handle, jadi
/// tidak ada padanan dari pekerjaan mendaftar per direktori seperti di Linux.
///
/// Konsekuensinya terlihat di test: karena sub-pohon ikut terpantau, direktori
/// baru tidak pernah membuat perubahan di dalamnya hilang, sehingga jalur ini
/// tidak punya alasan meminta pemindaian ulang di tempat inotify memintanya.
/// Yang tetap bisa meminta pemindaian ulang adalah buffer yang meluap
/// (`ERROR_NOTIFY_ENUM_DIR`) — kontrak `Poll()` di header tidak berubah.
struct FileWatcher::Impl {
    HANDLE directory = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};
    std::array<char, 64 * 1024> buffer{};
    bool pending = false;

    ~Impl() { Close(); }

    void Close() {
        if (directory != INVALID_HANDLE_VALUE) {
            ::CancelIo(directory);
            ::CloseHandle(directory);
            directory = INVALID_HANDLE_VALUE;
        }
        if (overlapped.hEvent != nullptr) {
            ::CloseHandle(overlapped.hEvent);
            overlapped.hEvent = nullptr;
        }
        pending = false;
    }

    bool Issue() {
        constexpr DWORD kFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                  FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE;
        DWORD ignored = 0;
        overlapped.Offset = 0;
        overlapped.OffsetHigh = 0;
        pending = ::ReadDirectoryChangesW(directory, buffer.data(),
                                          static_cast<DWORD>(buffer.size()), TRUE, kFilter,
                                          &ignored, &overlapped, nullptr) != 0;
        return pending;
    }
};

FileWatcher::FileWatcher() : impl_(std::make_unique<Impl>()) {}
FileWatcher::~FileWatcher() = default;

bool FileWatcher::Watch(const std::filesystem::path& root) {
    Stop();
    impl_->directory =
        ::CreateFileW(root.wstring().c_str(), FILE_LIST_DIRECTORY,
                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (impl_->directory == INVALID_HANDLE_VALUE) {
        SIM_WARN("Core", "CreateFileW failed for {}", root.string());
        return false;
    }
    impl_->overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (impl_->overlapped.hEvent == nullptr || !impl_->Issue()) {
        impl_->Close();
        return false;
    }
    SIM_INFO("Core", "Watching {} (recursive)", root.string());
    return true;
}

void FileWatcher::Stop() {
    impl_->Close();
}

bool FileWatcher::IsWatching() const {
    return impl_->directory != INVALID_HANDLE_VALUE;
}

bool FileWatcher::Poll(std::vector<Event>& out) {
    if (impl_->directory == INVALID_HANDLE_VALUE || !impl_->pending) {
        return true;
    }
    DWORD bytes = 0;
    if (::GetOverlappedResult(impl_->directory, &impl_->overlapped, &bytes, FALSE) == 0) {
        // Belum ada apa-apa: itu keadaan normal, bukan kegagalan.
        return ::GetLastError() == ERROR_IO_INCOMPLETE;
    }

    bool complete = true;
    // Nol byte berarti buffer meluap dan kernel membuang isinya. Satu-satunya
    // pemulihan adalah memindai ulang.
    if (bytes == 0) {
        complete = false;
    }

    for (DWORD offset = 0; offset < bytes;) {
        const auto* info =
            reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(impl_->buffer.data() + offset);

        const int length = ::WideCharToMultiByte(
            CP_UTF8, 0, info->FileName,
            static_cast<int>(info->FileNameLength / sizeof(WCHAR)), nullptr, 0, nullptr, nullptr);
        std::string path(static_cast<std::size_t>(length), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, info->FileName,
                              static_cast<int>(info->FileNameLength / sizeof(WCHAR)), path.data(),
                              length, nullptr, nullptr);
        // Path relatif ikut masuk indeks dan berkas favorit, jadi pemisahnya
        // diseragamkan ke '/' seperti di platform lain.
        std::replace(path.begin(), path.end(), '\\', '/');

        Event result;
        switch (info->Action) {
            case FILE_ACTION_ADDED:
            case FILE_ACTION_RENAMED_NEW_NAME: result.kind = Event::Kind::Added; break;
            case FILE_ACTION_REMOVED:
            case FILE_ACTION_RENAMED_OLD_NAME: result.kind = Event::Kind::Removed; break;
            default: result.kind = Event::Kind::Modified; break;
        }
        result.path = std::move(path);
        out.push_back(std::move(result));

        if (info->NextEntryOffset == 0) {
            break;
        }
        offset += info->NextEntryOffset;
    }

    ::ResetEvent(impl_->overlapped.hEvent);
    if (!impl_->Issue()) {
        complete = false;
    }
    return complete;
}

#else

/// Platform tanpa dukungan: pemanggil kembali ke pemindaian berkala.
struct FileWatcher::Impl {};

FileWatcher::FileWatcher() : impl_(std::make_unique<Impl>()) {}
FileWatcher::~FileWatcher() = default;
bool FileWatcher::Watch(const std::filesystem::path&) { return false; }
void FileWatcher::Stop() {}
bool FileWatcher::IsWatching() const { return false; }
bool FileWatcher::Poll(std::vector<Event>&) { return true; }

#endif

}  // namespace sim
