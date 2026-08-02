#pragma once

#include <spdlog/spdlog.h>

#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace sim {

/// Satu baris log yang disimpan untuk panel Console.
struct LogEntry {
    spdlog::level::level_enum level = spdlog::level::info;
    std::string channel;
    std::string message;
    uint64_t sequence = 0;  ///< urutan global, dipakai panel untuk deteksi log baru
};

/// Penampung log berkapasitas tetap yang dibaca panel Console.
///
/// Ditulis dari thread mana pun (job import, file watcher, server MCP), dibaca
/// dari main thread. Panel mengambil salinan lewat Snapshot() supaya tidak
/// menahan lock selama menggambar — daftar log bisa panjang dan menggambar
/// ribuan baris sambil memegang mutex akan menghambat thread penulis.
class LogRing {
public:
    static LogRing& Get();

    void Push(spdlog::level::level_enum level, std::string channel, std::string message);
    std::vector<LogEntry> Snapshot() const;
    void Clear();

    uint64_t Sequence() const;
    void SetCapacity(std::size_t capacity);

private:
    mutable std::mutex mutex_;
    std::deque<LogEntry> entries_;
    std::size_t capacity_ = 8192;
    uint64_t sequence_ = 0;
};

/// Titik masuk logging. Channel dipakai supaya panel Console bisa memfilter
/// per subsistem ("RHI", "Asset", "Lua", "MCP").
class Log {
public:
    static void Init(const std::filesystem::path& logFile);
    static void Shutdown();

    /// Mengembalikan logger untuk sebuah channel, membuatnya bila perlu.
    static spdlog::logger& Channel(const char* name);
};

}  // namespace sim

#define SIM_TRACE(channel, ...) ::sim::Log::Channel(channel).trace(__VA_ARGS__)
#define SIM_DEBUG_LOG(channel, ...) ::sim::Log::Channel(channel).debug(__VA_ARGS__)
#define SIM_INFO(channel, ...) ::sim::Log::Channel(channel).info(__VA_ARGS__)
#define SIM_WARN(channel, ...) ::sim::Log::Channel(channel).warn(__VA_ARGS__)
#define SIM_ERROR(channel, ...) ::sim::Log::Channel(channel).error(__VA_ARGS__)
#define SIM_CRITICAL(channel, ...) ::sim::Log::Channel(channel).critical(__VA_ARGS__)
