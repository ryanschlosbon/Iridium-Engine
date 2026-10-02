#include "editor/EditorAssetSettingsTransactionService.h"

#include <exception>
#include <stdexcept>
#include <utility>

namespace Iridium {

    EditorAssetSettingsTransactionService::
        EditorAssetSettingsTransactionService(SubmitCallback submit)
        : submit_(std::move(submit)) {}

    void EditorAssetSettingsTransactionService::setSubmitCallback(
        SubmitCallback submit) {
        if (pending_) {
            throw std::logic_error(
                "Cannot replace the asset-settings submit callback while a transaction is pending");
        }
        submit_ = std::move(submit);
    }

    EditorAssetSettingsTransactionResult
        EditorAssetSettingsTransactionService::requestApply(
            AssetGuid rootGuid,
            std::string label,
            nlohmann::json before,
            nlohmann::json after) {
        if (pending_) {
            return {
                .outcome = EditorAssetSettingsTransactionOutcome::Pending,
                .rootGuid = rootGuid,
                .diagnostic = "An asset-settings transaction is already pending",
            };
        }
        if (rootGuid.isNil() || label.empty() || !before.is_object() ||
            !after.is_object()) {
            return {
                .outcome = EditorAssetSettingsTransactionOutcome::Invalid,
                .rootGuid = rootGuid,
                .diagnostic = "Asset-settings transactions require a root GUID, label, and object settings",
            };
        }
        if (before == after) {
            return {
                .outcome = EditorAssetSettingsTransactionOutcome::NoChange,
                .rootGuid = rootGuid,
            };
        }

        if (rootGuid_ != std::optional(rootGuid)) clear();
        else if (const nlohmann::json* expected = expectedPublishedSettings();
            expected != nullptr && *expected != before) {
            clear();
        }
        rootGuid_ = rootGuid;
        const Entry entry{
            .rootGuid = rootGuid,
            .label = std::move(label),
            .before = std::move(before),
            .after = std::move(after),
        };
        return submit(EditorAssetSettingsTransactionAction::Apply,
            entry, entry.after);
    }

    EditorAssetSettingsTransactionResult
        EditorAssetSettingsTransactionService::requestUndo(
            AssetGuid rootGuid,
            const nlohmann::json& publishedSettings) {
        if (pending_) {
            return {
                .outcome = EditorAssetSettingsTransactionOutcome::Pending,
                .action = EditorAssetSettingsTransactionAction::Undo,
                .rootGuid = rootGuid,
                .diagnostic = "An asset-settings transaction is already pending",
            };
        }
        if (!canUndo(rootGuid)) {
            return {
                .outcome = EditorAssetSettingsTransactionOutcome::Unavailable,
                .action = EditorAssetSettingsTransactionAction::Undo,
                .rootGuid = rootGuid,
            };
        }
        const Entry& entry = history_[cursor_ - 1];
        if (!publishedSettings.is_object() ||
            publishedSettings != entry.after) {
            clear();
            return {
                .outcome = EditorAssetSettingsTransactionOutcome::Diverged,
                .action = EditorAssetSettingsTransactionAction::Undo,
                .rootGuid = rootGuid,
                .diagnostic = "Published asset settings changed outside transaction history",
            };
        }
        return submit(EditorAssetSettingsTransactionAction::Undo,
            entry, entry.before);
    }

    EditorAssetSettingsTransactionResult
        EditorAssetSettingsTransactionService::requestRedo(
            AssetGuid rootGuid,
            const nlohmann::json& publishedSettings) {
        if (pending_) {
            return {
                .outcome = EditorAssetSettingsTransactionOutcome::Pending,
                .action = EditorAssetSettingsTransactionAction::Redo,
                .rootGuid = rootGuid,
                .diagnostic = "An asset-settings transaction is already pending",
            };
        }
        if (!canRedo(rootGuid)) {
            return {
                .outcome = EditorAssetSettingsTransactionOutcome::Unavailable,
                .action = EditorAssetSettingsTransactionAction::Redo,
                .rootGuid = rootGuid,
            };
        }
        const Entry& entry = history_[cursor_];
        if (!publishedSettings.is_object() ||
            publishedSettings != entry.before) {
            clear();
            return {
                .outcome = EditorAssetSettingsTransactionOutcome::Diverged,
                .action = EditorAssetSettingsTransactionAction::Redo,
                .rootGuid = rootGuid,
                .diagnostic = "Published asset settings changed outside transaction history",
            };
        }
        return submit(EditorAssetSettingsTransactionAction::Redo,
            entry, entry.after);
    }

    EditorAssetSettingsTransactionResult
        EditorAssetSettingsTransactionService::complete(
            uint64_t jobSerial,
            bool succeeded,
            std::string_view diagnostic) {
        if (!pending_ || pending_->jobSerial != jobSerial) {
            return {
                .outcome =
                    EditorAssetSettingsTransactionOutcome::IgnoredCompletion,
                .jobSerial = jobSerial,
            };
        }

        Pending completed = std::move(*pending_);
        pending_.reset();
        if (!succeeded) {
            return {
                .outcome = EditorAssetSettingsTransactionOutcome::JobFailed,
                .action = completed.action,
                .rootGuid = completed.entry.rootGuid,
                .jobSerial = jobSerial,
                .diagnostic = diagnostic.empty()
                    ? "Asset settings cook failed"
                    : std::string(diagnostic),
            };
        }

        if (completed.action == EditorAssetSettingsTransactionAction::Apply) {
            discardRedoBranch();
            history_.push_back(std::move(completed.entry));
            cursor_ = history_.size();
        }
        else if (completed.action ==
                EditorAssetSettingsTransactionAction::Undo) {
            --cursor_;
        }
        else {
            ++cursor_;
        }
        return {
            .outcome = EditorAssetSettingsTransactionOutcome::Completed,
            .action = completed.action,
            .rootGuid = completed.entry.rootGuid,
            .jobSerial = jobSerial,
        };
    }

    bool EditorAssetSettingsTransactionService::canUndo(
        AssetGuid rootGuid) const noexcept {
        return !pending_ && rootGuid_ == std::optional(rootGuid) &&
            cursor_ != 0;
    }

    bool EditorAssetSettingsTransactionService::canRedo(
        AssetGuid rootGuid) const noexcept {
        return !pending_ && rootGuid_ == std::optional(rootGuid) &&
            cursor_ != history_.size();
    }

    std::string_view EditorAssetSettingsTransactionService::undoLabel(
        AssetGuid rootGuid) const noexcept {
        return canUndo(rootGuid)
            ? std::string_view(history_[cursor_ - 1].label)
            : std::string_view{};
    }

    std::string_view EditorAssetSettingsTransactionService::redoLabel(
        AssetGuid rootGuid) const noexcept {
        return canRedo(rootGuid)
            ? std::string_view(history_[cursor_].label)
            : std::string_view{};
    }

    void EditorAssetSettingsTransactionService::clear() noexcept {
        rootGuid_.reset();
        history_.clear();
        cursor_ = 0;
        pending_.reset();
    }

    EditorAssetSettingsTransactionResult
        EditorAssetSettingsTransactionService::submit(
            EditorAssetSettingsTransactionAction action,
            const Entry& entry,
            const nlohmann::json& settings) {
        if (!submit_) {
            return {
                .outcome =
                    EditorAssetSettingsTransactionOutcome::SubmitFailed,
                .action = action,
                .rootGuid = entry.rootGuid,
                .diagnostic = "Asset-settings submission is unavailable",
            };
        }
        try {
            const uint64_t serial = submit_(entry.rootGuid, settings);
            if (serial == 0) {
                return {
                    .outcome =
                        EditorAssetSettingsTransactionOutcome::SubmitFailed,
                    .action = action,
                    .rootGuid = entry.rootGuid,
                    .diagnostic = "Asset-settings submission returned an invalid job serial",
                };
            }
            pending_ = Pending{
                .action = action,
                .jobSerial = serial,
                .entry = entry,
            };
            return {
                .outcome = EditorAssetSettingsTransactionOutcome::Queued,
                .action = action,
                .rootGuid = entry.rootGuid,
                .jobSerial = serial,
            };
        }
        catch (const std::exception& exception) {
            return {
                .outcome =
                    EditorAssetSettingsTransactionOutcome::SubmitFailed,
                .action = action,
                .rootGuid = entry.rootGuid,
                .diagnostic = exception.what(),
            };
        }
        catch (...) {
            return {
                .outcome =
                    EditorAssetSettingsTransactionOutcome::SubmitFailed,
                .action = action,
                .rootGuid = entry.rootGuid,
                .diagnostic = "Asset-settings submission raised an unknown exception",
            };
        }
    }

    const nlohmann::json*
        EditorAssetSettingsTransactionService::expectedPublishedSettings()
            const noexcept {
        if (history_.empty()) return nullptr;
        return cursor_ == 0
            ? &history_.front().before
            : &history_[cursor_ - 1].after;
    }

    void EditorAssetSettingsTransactionService::discardRedoBranch() {
        history_.erase(history_.begin() +
            static_cast<std::ptrdiff_t>(cursor_), history_.end());
    }

} // namespace Iridium
