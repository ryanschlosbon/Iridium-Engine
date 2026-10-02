#pragma once

#include "assets/AssetGuid.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace Iridium {

    enum class EditorAssetSettingsTransactionAction : uint8_t {
        Apply,
        Undo,
        Redo,
    };

    enum class EditorAssetSettingsTransactionOutcome : uint8_t {
        Queued,
        NoChange,
        Completed,
        Unavailable,
        Pending,
        Invalid,
        Diverged,
        SubmitFailed,
        JobFailed,
        IgnoredCompletion,
    };

    struct EditorAssetSettingsTransactionResult {
        EditorAssetSettingsTransactionOutcome outcome =
            EditorAssetSettingsTransactionOutcome::Invalid;
        EditorAssetSettingsTransactionAction action =
            EditorAssetSettingsTransactionAction::Apply;
        AssetGuid rootGuid;
        uint64_t jobSerial = 0;
        std::string diagnostic;

        [[nodiscard]] explicit operator bool() const noexcept {
            return outcome == EditorAssetSettingsTransactionOutcome::Queued ||
                outcome == EditorAssetSettingsTransactionOutcome::NoChange ||
                outcome == EditorAssetSettingsTransactionOutcome::Completed;
        }
    };

    class EditorAssetSettingsTransactionService {
    public:
        using SubmitCallback = std::function<uint64_t(
            AssetGuid, nlohmann::json)>;

        explicit EditorAssetSettingsTransactionService(
            SubmitCallback submit = {});

        void setSubmitCallback(SubmitCallback submit);

        [[nodiscard]] EditorAssetSettingsTransactionResult requestApply(
            AssetGuid rootGuid,
            std::string label,
            nlohmann::json before,
            nlohmann::json after);
        [[nodiscard]] EditorAssetSettingsTransactionResult requestUndo(
            AssetGuid rootGuid,
            const nlohmann::json& publishedSettings);
        [[nodiscard]] EditorAssetSettingsTransactionResult requestRedo(
            AssetGuid rootGuid,
            const nlohmann::json& publishedSettings);
        [[nodiscard]] EditorAssetSettingsTransactionResult complete(
            uint64_t jobSerial,
            bool succeeded,
            std::string_view diagnostic = {});

        [[nodiscard]] bool pending() const noexcept {
            return pending_.has_value();
        }
        [[nodiscard]] bool canUndo(AssetGuid rootGuid) const noexcept;
        [[nodiscard]] bool canRedo(AssetGuid rootGuid) const noexcept;
        [[nodiscard]] std::string_view undoLabel(
            AssetGuid rootGuid) const noexcept;
        [[nodiscard]] std::string_view redoLabel(
            AssetGuid rootGuid) const noexcept;
        [[nodiscard]] size_t historyEntryCount() const noexcept {
            return history_.size();
        }
        [[nodiscard]] size_t appliedEntryCount() const noexcept {
            return cursor_;
        }
        void clear() noexcept;

    private:
        struct Entry {
            AssetGuid rootGuid;
            std::string label;
            nlohmann::json before;
            nlohmann::json after;
        };

        struct Pending {
            EditorAssetSettingsTransactionAction action =
                EditorAssetSettingsTransactionAction::Apply;
            uint64_t jobSerial = 0;
            Entry entry;
        };

        [[nodiscard]] EditorAssetSettingsTransactionResult submit(
            EditorAssetSettingsTransactionAction action,
            const Entry& entry,
            const nlohmann::json& settings);
        [[nodiscard]] const nlohmann::json* expectedPublishedSettings()
            const noexcept;
        void discardRedoBranch();

        SubmitCallback submit_;
        std::optional<AssetGuid> rootGuid_;
        std::vector<Entry> history_;
        size_t cursor_ = 0;
        std::optional<Pending> pending_;
    };

} // namespace Iridium
