#pragma once

#include "editor/EditorSceneDocumentService.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Iridium {

    enum class EditorMutationOutcome : uint8_t {
        Applied,
        NoChange,
        Failed,
    };

    struct EditorMutationResult {
        EditorMutationOutcome outcome = EditorMutationOutcome::Applied;
        std::string diagnostic;

        [[nodiscard]] explicit operator bool() const noexcept {
            return outcome != EditorMutationOutcome::Failed;
        }

        [[nodiscard]] static EditorMutationResult applied() {
            return {};
        }
        [[nodiscard]] static EditorMutationResult noChange() {
            return { .outcome = EditorMutationOutcome::NoChange };
        }
        [[nodiscard]] static EditorMutationResult failure(
            std::string message) {
            return {
                .outcome = EditorMutationOutcome::Failed,
                .diagnostic = std::move(message),
            };
        }
    };

    struct EditorTransactionOperation {
        using Callback = std::function<EditorMutationResult()>;

        std::string target;
        Callback apply;
        Callback revert;
        size_t estimatedPayloadBytes = 0;
    };

    template<typename Value, typename Resolver,
        typename Equality = std::equal_to<Value>>
    [[nodiscard]] EditorTransactionOperation makeEditorValueOperation(
        std::string target, Resolver resolver, Value before, Value after,
        Equality equality = {}) {
        struct State {
            std::string target;
            Resolver resolver;
            Value before;
            Value after;
            Equality equality;

            [[nodiscard]] EditorMutationResult write(
                const Value& expected, const Value& replacement) {
                Value* current = std::invoke(resolver);
                if (!current) {
                    return EditorMutationResult::failure(
                        target + " is no longer available");
                }
                if (std::invoke(equality, *current, replacement)) {
                    return EditorMutationResult::noChange();
                }
                if (!std::invoke(equality, *current, expected)) {
                    return EditorMutationResult::failure(
                        target + " changed outside transaction history");
                }
                *current = replacement;
                return EditorMutationResult::applied();
            }
        };
        auto state = std::make_shared<State>(State{
            .target = target,
            .resolver = std::move(resolver),
            .before = std::move(before),
            .after = std::move(after),
            .equality = std::move(equality),
        });
        return {
            .target = std::move(target),
            .apply = [state] {
                return state->write(state->before, state->after);
            },
            .revert = [state] {
                return state->write(state->after, state->before);
            },
            .estimatedPayloadBytes = sizeof(State),
        };
    }

    struct EditorTransaction {
        std::string label;
        std::string coalescingKey;
        uint64_t coalescingSession = 0;
        std::vector<EditorTransactionOperation> operations;
    };

    enum class EditorTransactionOutcome : uint8_t {
        Committed,
        NoChange,
        Undone,
        Redone,
        Unavailable,
        Invalid,
        Failed,
        RollbackFailed,
        HistoryDiverged,
    };

    struct EditorTransactionResult {
        EditorTransactionOutcome outcome = EditorTransactionOutcome::Committed;
        SceneDocumentStateToken state = 0;
        std::string diagnostic;

        [[nodiscard]] explicit operator bool() const noexcept {
            return outcome == EditorTransactionOutcome::Committed ||
                outcome == EditorTransactionOutcome::NoChange ||
                outcome == EditorTransactionOutcome::Undone ||
                outcome == EditorTransactionOutcome::Redone;
        }
    };

    class EditorTransactionService final :
        public EditorSceneHistoryObserver {
    public:
        explicit EditorTransactionService(
            EditorSceneDocumentService& document);
        ~EditorTransactionService() override;

        EditorTransactionService(const EditorTransactionService&) = delete;
        EditorTransactionService& operator=(
            const EditorTransactionService&) = delete;

        [[nodiscard]] EditorTransactionResult execute(
            EditorTransaction transaction);
        [[nodiscard]] EditorTransactionResult undo();
        [[nodiscard]] EditorTransactionResult redo();

        [[nodiscard]] bool canUndo() const noexcept;
        [[nodiscard]] bool canRedo() const noexcept;
        [[nodiscard]] std::string_view undoLabel() const noexcept;
        [[nodiscard]] std::string_view redoLabel() const noexcept;
        [[nodiscard]] size_t historyEntryCount() const noexcept {
            return history_.size();
        }
        [[nodiscard]] size_t appliedEntryCount() const noexcept {
            return cursor_;
        }
        [[nodiscard]] size_t estimatedHistoryBytes() const noexcept {
            return estimatedHistoryBytes_;
        }
        [[nodiscard]] const std::string& diagnostic() const noexcept {
            return diagnostic_;
        }

        void clear();
        void onSceneDocumentCommitted(
            SceneDocumentStateToken state) noexcept override;

    private:
        struct HistoryEntry {
            std::string label;
            std::string coalescingKey;
            uint64_t coalescingSession = 0;
            SceneDocumentStateToken beforeState = 0;
            SceneDocumentStateToken afterState = 0;
            std::vector<EditorTransactionOperation> operations;
            size_t estimatedBytes = 0;
        };

        [[nodiscard]] bool documentStateMatches() const noexcept;
        void resetToDocumentState() noexcept;
        void discardRedoBranch();
        [[nodiscard]] static size_t estimateEntryBytes(
            const HistoryEntry& entry) noexcept;

        EditorSceneDocumentService& document_;
        std::vector<HistoryEntry> history_;
        size_t cursor_ = 0;
        size_t estimatedHistoryBytes_ = 0;
        SceneDocumentStateToken expectedState_ = 0;
        std::string diagnostic_;
    };

} // namespace Iridium
