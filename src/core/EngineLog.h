#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium {

    enum class EngineLogSeverity : uint8_t {
        Info,
        Warning,
        Error,
    };

    struct EngineLogEntry {
        uint64_t sequence = 0;
        std::chrono::system_clock::time_point timestamp;
        EngineLogSeverity severity = EngineLogSeverity::Info;
        std::string category;
        std::string message;
    };

    // Thread-safe, bounded diagnostic history shared by engine/editor services.
    // The UI snapshots only when revision changes, so an open console has no
    // per-frame string-copy cost while idle.
    class EngineLog {
    public:
        explicit EngineLog(size_t capacity = 2'048);

        uint64_t write(
            EngineLogSeverity severity,
            std::string_view category,
            std::string message);
        uint64_t info(
            std::string_view category,
            std::string message);
        uint64_t warning(
            std::string_view category,
            std::string message);
        uint64_t error(
            std::string_view category,
            std::string message);

        [[nodiscard]] uint64_t revision() const noexcept;
        [[nodiscard]] std::vector<EngineLogEntry> snapshot() const;
        void clear() noexcept;

    private:
        const size_t capacity_;
        mutable std::mutex mutex_;
        std::deque<EngineLogEntry> entries_;
        uint64_t nextSequence_ = 1;
        uint64_t revision_ = 0;
    };

} // namespace Iridium
