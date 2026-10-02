#pragma once

#include <nlohmann/json.hpp>
#include <vector>
#include <optional>
#include <cstdint>

namespace Iridium {
    // Coalesce a slider/color-picker gesture into one editor-local undo entry.
    // This does not enqueue asset cooking or affect published settings history.
    class EditorMaterialDraftHistory {
    public:
        using Json = nlohmann::json;
        void reset(const Json& value) {
            committed_ = value;
            pending_ = value;
            undo_.clear();
            redo_.clear();
            activeControl_ = 0;
        }
        // Zero means no active control. A new activation is a boundary even
        // when the same slider is dragged again without an observed idle frame.
        void observe(const Json& value, uint64_t activeControl, bool newlyActivated = false) {
            if (!committed_) reset(value);
            if (activeControl != 0 && (activeControl != activeControl_ || newlyActivated))
                commitGesture();
            activeControl_ = activeControl;
            pending_ = value;
            if (activeControl == 0) commitGesture();
        }
        [[nodiscard]] bool canUndo() const { return !undo_.empty() || (committed_ && pending_ != *committed_); }
        [[nodiscard]] bool canRedo() const { return !redo_.empty() && committed_ && pending_ == *committed_; }
        [[nodiscard]] std::optional<Json> undo() {
            commitGesture();
            activeControl_ = 0;
            if (undo_.empty()) return {};
            redo_.push_back(*committed_);
            committed_ = std::move(undo_.back());
            undo_.pop_back();
            pending_ = *committed_;
            return committed_;
        }
        [[nodiscard]] std::optional<Json> redo() {
            if (!canRedo()) return {};
            activeControl_ = 0;
            undo_.push_back(*committed_);
            committed_ = std::move(redo_.back());
            redo_.pop_back();
            pending_ = *committed_;
            return committed_;
        }
    private:
        void commitGesture() {
            if (!committed_ || pending_ == *committed_) return;
            if (undo_.size() == 128) undo_.erase(undo_.begin());
            undo_.push_back(*committed_);
            committed_ = pending_;
            redo_.clear();
        }
        std::optional<Json> committed_;
        Json pending_;
        uint64_t activeControl_ = 0;
        std::vector<Json> undo_, redo_;
    };
}
