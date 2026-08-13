#include "core/EngineLog.h"

#include <algorithm>
#include <utility>

namespace Iridium {

    EngineLog::EngineLog(size_t capacity)
        : capacity_(std::max<size_t>(1, capacity)) {}

    uint64_t EngineLog::write(
        EngineLogSeverity severity,
        std::string_view category,
        std::string message) {
        std::lock_guard lock(mutex_);
        const uint64_t sequence = nextSequence_++;
        entries_.push_back({
            .sequence = sequence,
            .timestamp =
                std::chrono::system_clock::now(),
            .severity = severity,
            .category = std::string(category),
            .message = std::move(message),
        });
        while (entries_.size() > capacity_) {
            entries_.pop_front();
        }
        ++revision_;
        return sequence;
    }

    uint64_t EngineLog::info(
        std::string_view category,
        std::string message) {
        return write(
            EngineLogSeverity::Info,
            category, std::move(message));
    }

    uint64_t EngineLog::warning(
        std::string_view category,
        std::string message) {
        return write(
            EngineLogSeverity::Warning,
            category, std::move(message));
    }

    uint64_t EngineLog::error(
        std::string_view category,
        std::string message) {
        return write(
            EngineLogSeverity::Error,
            category, std::move(message));
    }

    uint64_t EngineLog::revision() const noexcept {
        std::lock_guard lock(mutex_);
        return revision_;
    }

    std::vector<EngineLogEntry>
        EngineLog::snapshot() const {
        std::lock_guard lock(mutex_);
        return {
            entries_.begin(),
            entries_.end(),
        };
    }

    void EngineLog::clear() noexcept {
        std::lock_guard lock(mutex_);
        entries_.clear();
        ++revision_;
    }

} // namespace Iridium
