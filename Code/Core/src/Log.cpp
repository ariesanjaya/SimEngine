#include "Sim/Core/Log.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <memory>
#include <unordered_map>

namespace sim {
namespace {

constexpr const char* kPattern = "[%H:%M:%S.%e] [%^%-8l%$] [%-10n] %v";

/// Sink yang menyalin setiap pesan ke LogRing supaya panel Console bisa
/// menampilkannya. Sengaja menyimpan hasil format akhir, bukan argumennya,
/// karena argumen bisa berupa referensi ke objek yang sudah mati saat panel
/// menggambar beberapa milidetik kemudian.
class RingSink final : public spdlog::sinks::base_sink<std::mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        // Sengaja tidak memakai formatter_: panel Console punya kolom sendiri
        // untuk level dan channel, jadi yang disimpan cukup payload mentah.
        LogRing::Get().Push(msg.level, std::string(msg.logger_name.begin(), msg.logger_name.end()),
                            std::string(msg.payload.begin(), msg.payload.end()));
    }

    void flush_() override {}
};

struct LogState {
    std::vector<spdlog::sink_ptr> sinks;
    std::unordered_map<std::string, std::shared_ptr<spdlog::logger>> channels;
    std::mutex mutex;
    bool initialized = false;
};

LogState& State() {
    static LogState state;
    return state;
}

}  // namespace

LogRing& LogRing::Get() {
    static LogRing ring;
    return ring;
}

void LogRing::Push(spdlog::level::level_enum level, std::string channel, std::string message) {
    std::lock_guard lock(mutex_);
    entries_.push_back(LogEntry{level, std::move(channel), std::move(message), ++sequence_});
    while (entries_.size() > capacity_) {
        entries_.pop_front();
    }
}

std::vector<LogEntry> LogRing::Snapshot() const {
    std::lock_guard lock(mutex_);
    return std::vector<LogEntry>(entries_.begin(), entries_.end());
}

void LogRing::Clear() {
    std::lock_guard lock(mutex_);
    entries_.clear();
}

uint64_t LogRing::Sequence() const {
    std::lock_guard lock(mutex_);
    return sequence_;
}

void LogRing::SetCapacity(std::size_t capacity) {
    std::lock_guard lock(mutex_);
    capacity_ = capacity == 0 ? 1 : capacity;
    while (entries_.size() > capacity_) {
        entries_.pop_front();
    }
}

void Log::Init(const std::filesystem::path& logFile) {
    LogState& state = State();
    std::lock_guard lock(state.mutex);
    if (state.initialized) {
        return;
    }

    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_pattern(kPattern);
    state.sinks.push_back(std::move(console));

    std::error_code ec;
    if (!logFile.parent_path().empty()) {
        std::filesystem::create_directories(logFile.parent_path(), ec);
    }
    if (!ec) {
        auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>(logFile.string(), true);
        file->set_pattern(kPattern);
        state.sinks.push_back(std::move(file));
    }
    state.sinks.push_back(std::make_shared<RingSink>());

    state.initialized = true;
#if SIM_DEBUG
    spdlog::set_level(spdlog::level::debug);
#else
    spdlog::set_level(spdlog::level::info);
#endif
}

void Log::Shutdown() {
    LogState& state = State();
    std::lock_guard lock(state.mutex);
    for (auto& [name, logger] : state.channels) {
        logger->flush();
    }
    state.channels.clear();
    state.sinks.clear();
    state.initialized = false;
    spdlog::shutdown();
}

spdlog::logger& Log::Channel(const char* name) {
    LogState& state = State();
    std::lock_guard lock(state.mutex);

    auto it = state.channels.find(name);
    if (it != state.channels.end()) {
        return *it->second;
    }

    // Log sebelum Init() (mis. dari inisialisasi statik) tetap harus jalan,
    // kalau tidak kegagalan awal justru tidak terlihat.
    if (!state.initialized) {
        auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        console->set_pattern(kPattern);
        state.sinks.push_back(std::move(console));
        state.sinks.push_back(std::make_shared<RingSink>());
        state.initialized = true;
    }

    auto logger = std::make_shared<spdlog::logger>(name, state.sinks.begin(), state.sinks.end());
    logger->set_level(spdlog::get_level());
    logger->flush_on(spdlog::level::warn);
    it = state.channels.emplace(name, std::move(logger)).first;
    return *it->second;
}

}  // namespace sim
