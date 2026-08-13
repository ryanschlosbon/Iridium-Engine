#include "editor/EditorTransactionService.h"

#include <exception>
#include <utility>

namespace Iridium {
    namespace {

        [[nodiscard]] EditorMutationResult invokeMutation(
            const EditorTransactionOperation::Callback& callback) noexcept {
            try {
                return callback();
            }
            catch (const std::exception& exception) {
                return EditorMutationResult::failure(exception.what());
            }
            catch (...) {
                return EditorMutationResult::failure(
                    "Editor mutation raised an unknown exception");
            }
        }

        void appendDiagnostic(std::string& destination,
            std::string_view prefix, const EditorMutationResult& result) {
            if (!destination.empty()) destination += "; ";
            destination += prefix;
            if (!result.diagnostic.empty()) {
                destination += ": ";
                destination += result.diagnostic;
            }
        }

    } // namespace

    EditorTransactionService::EditorTransactionService(
        EditorSceneDocumentService& document)
        : document_(document), expectedState_(document.currentState()) {
        document_.setHistoryObserver(this);
    }

    EditorTransactionService::~EditorTransactionService() {
        document_.removeHistoryObserver(this);
    }

    EditorTransactionResult EditorTransactionService::execute(
        EditorTransaction transaction) {
        diagnostic_.clear();
        if (!documentStateMatches()) resetToDocumentState();
        if (transaction.label.empty()) {
            diagnostic_ = "A transaction label is required";
            return {
                .outcome = EditorTransactionOutcome::Invalid,
                .state = expectedState_,
                .diagnostic = diagnostic_,
            };
        }
        if (transaction.operations.empty()) {
            return {
                .outcome = EditorTransactionOutcome::NoChange,
                .state = expectedState_,
            };
        }
        for (const EditorTransactionOperation& operation :
            transaction.operations) {
            if (!operation.apply || !operation.revert) {
                diagnostic_ = "Every transaction operation requires apply and "
                    "revert callbacks";
                return {
                    .outcome = EditorTransactionOutcome::Invalid,
                    .state = expectedState_,
                    .diagnostic = diagnostic_,
                };
            }
        }

        size_t appliedCount = 0;
        for (size_t operationIndex = 0;
            operationIndex < transaction.operations.size(); ++operationIndex) {
            EditorTransactionOperation& operation =
                transaction.operations[operationIndex];
            const EditorMutationResult result = invokeMutation(operation.apply);
            if (result.outcome == EditorMutationOutcome::NoChange) continue;
            if (!result) {
                appendDiagnostic(diagnostic_, "Apply failed", result);
                bool rollbackFailed = false;
                while (appliedCount != 0) {
                    --appliedCount;
                    const EditorMutationResult rollback =
                        invokeMutation(transaction.operations[
                            appliedCount].revert);
                    if (!rollback) {
                        rollbackFailed = true;
                        appendDiagnostic(diagnostic_, "Rollback failed", rollback);
                    }
                }
                return {
                    .outcome = rollbackFailed
                        ? EditorTransactionOutcome::RollbackFailed
                        : EditorTransactionOutcome::Failed,
                    .state = expectedState_,
                    .diagnostic = diagnostic_,
                };
            }
            if (appliedCount != operationIndex) {
                transaction.operations[appliedCount] = std::move(operation);
            }
            ++appliedCount;
        }
        if (appliedCount == 0) {
            return {
                .outcome = EditorTransactionOutcome::NoChange,
                .state = expectedState_,
            };
        }
        transaction.operations.resize(appliedCount);

        discardRedoBranch();
        const SceneDocumentStateToken before = expectedState_;
        const SceneDocumentStateToken after = document_.advanceState();
        expectedState_ = after;

        const bool coalesces = !transaction.coalescingKey.empty() &&
            transaction.coalescingSession != 0 && cursor_ != 0 &&
            cursor_ == history_.size() &&
            history_.back().coalescingKey == transaction.coalescingKey &&
            history_.back().coalescingSession == transaction.coalescingSession;
        if (coalesces) {
            HistoryEntry& entry = history_.back();
            estimatedHistoryBytes_ -= entry.estimatedBytes;
            entry.afterState = after;
            entry.operations.insert(entry.operations.end(),
                std::make_move_iterator(transaction.operations.begin()),
                std::make_move_iterator(transaction.operations.end()));
            entry.estimatedBytes = estimateEntryBytes(entry);
            estimatedHistoryBytes_ += entry.estimatedBytes;
        }
        else {
            HistoryEntry entry;
            entry.label = std::move(transaction.label);
            entry.coalescingKey = std::move(transaction.coalescingKey);
            entry.coalescingSession = transaction.coalescingSession;
            entry.beforeState = before;
            entry.afterState = after;
            entry.operations = std::move(transaction.operations);
            entry.estimatedBytes = estimateEntryBytes(entry);
            estimatedHistoryBytes_ += entry.estimatedBytes;
            history_.push_back(std::move(entry));
            cursor_ = history_.size();
        }
        return {
            .outcome = EditorTransactionOutcome::Committed,
            .state = after,
        };
    }

    EditorTransactionResult EditorTransactionService::undo() {
        diagnostic_.clear();
        if (!documentStateMatches()) {
            resetToDocumentState();
            diagnostic_ = "Document state changed outside transaction history";
            return {
                .outcome = EditorTransactionOutcome::HistoryDiverged,
                .state = expectedState_,
                .diagnostic = diagnostic_,
            };
        }
        if (cursor_ == 0) {
            return {
                .outcome = EditorTransactionOutcome::Unavailable,
                .state = expectedState_,
            };
        }

        HistoryEntry& entry = history_[cursor_ - 1];
        size_t revertedBegin = entry.operations.size();
        for (size_t index = entry.operations.size(); index != 0; --index) {
            const EditorMutationResult result =
                invokeMutation(entry.operations[index - 1].revert);
            if (!result) {
                appendDiagnostic(diagnostic_, "Undo failed", result);
                bool rollbackFailed = false;
                for (size_t restore = revertedBegin;
                    restore < entry.operations.size(); ++restore) {
                    const EditorMutationResult rollback =
                        invokeMutation(entry.operations[restore].apply);
                    if (!rollback) {
                        rollbackFailed = true;
                        appendDiagnostic(diagnostic_,
                            "Undo rollback failed", rollback);
                    }
                }
                return {
                    .outcome = rollbackFailed
                        ? EditorTransactionOutcome::RollbackFailed
                        : EditorTransactionOutcome::Failed,
                    .state = expectedState_,
                    .diagnostic = diagnostic_,
                };
            }
            revertedBegin = index - 1;
        }
        --cursor_;
        expectedState_ = entry.beforeState;
        document_.adoptState(expectedState_);
        return {
            .outcome = EditorTransactionOutcome::Undone,
            .state = expectedState_,
        };
    }

    EditorTransactionResult EditorTransactionService::redo() {
        diagnostic_.clear();
        if (!documentStateMatches()) {
            resetToDocumentState();
            diagnostic_ = "Document state changed outside transaction history";
            return {
                .outcome = EditorTransactionOutcome::HistoryDiverged,
                .state = expectedState_,
                .diagnostic = diagnostic_,
            };
        }
        if (cursor_ == history_.size()) {
            return {
                .outcome = EditorTransactionOutcome::Unavailable,
                .state = expectedState_,
            };
        }

        HistoryEntry& entry = history_[cursor_];
        size_t appliedCount = 0;
        for (EditorTransactionOperation& operation : entry.operations) {
            const EditorMutationResult result = invokeMutation(operation.apply);
            if (!result) {
                appendDiagnostic(diagnostic_, "Redo failed", result);
                bool rollbackFailed = false;
                while (appliedCount != 0) {
                    --appliedCount;
                    const EditorMutationResult rollback = invokeMutation(
                        entry.operations[appliedCount].revert);
                    if (!rollback) {
                        rollbackFailed = true;
                        appendDiagnostic(diagnostic_,
                            "Redo rollback failed", rollback);
                    }
                }
                return {
                    .outcome = rollbackFailed
                        ? EditorTransactionOutcome::RollbackFailed
                        : EditorTransactionOutcome::Failed,
                    .state = expectedState_,
                    .diagnostic = diagnostic_,
                };
            }
            ++appliedCount;
        }
        ++cursor_;
        expectedState_ = entry.afterState;
        document_.adoptState(expectedState_);
        return {
            .outcome = EditorTransactionOutcome::Redone,
            .state = expectedState_,
        };
    }

    bool EditorTransactionService::canUndo() const noexcept {
        return documentStateMatches() && cursor_ != 0;
    }

    bool EditorTransactionService::canRedo() const noexcept {
        return documentStateMatches() && cursor_ != history_.size();
    }

    std::string_view EditorTransactionService::undoLabel() const noexcept {
        return canUndo() ? std::string_view(history_[cursor_ - 1].label) :
            std::string_view{};
    }

    std::string_view EditorTransactionService::redoLabel() const noexcept {
        return canRedo() ? std::string_view(history_[cursor_].label) :
            std::string_view{};
    }

    void EditorTransactionService::clear() {
        history_.clear();
        history_.shrink_to_fit();
        cursor_ = 0;
        estimatedHistoryBytes_ = 0;
        expectedState_ = document_.currentState();
        diagnostic_.clear();
    }

    void EditorTransactionService::onSceneDocumentCommitted(
        SceneDocumentStateToken state) noexcept {
        history_.clear();
        cursor_ = 0;
        estimatedHistoryBytes_ = 0;
        expectedState_ = state;
        diagnostic_.clear();
    }

    bool EditorTransactionService::documentStateMatches() const noexcept {
        return document_.currentState() == expectedState_;
    }

    void EditorTransactionService::resetToDocumentState() noexcept {
        history_.clear();
        cursor_ = 0;
        estimatedHistoryBytes_ = 0;
        expectedState_ = document_.currentState();
    }

    void EditorTransactionService::discardRedoBranch() {
        while (history_.size() > cursor_) {
            estimatedHistoryBytes_ -= history_.back().estimatedBytes;
            history_.pop_back();
        }
    }

    size_t EditorTransactionService::estimateEntryBytes(
        const HistoryEntry& entry) noexcept {
        size_t bytes = sizeof(HistoryEntry) + entry.label.size() +
            entry.coalescingKey.size() +
            entry.operations.size() * sizeof(EditorTransactionOperation);
        for (const EditorTransactionOperation& operation : entry.operations) {
            bytes += operation.target.size() + operation.estimatedPayloadBytes;
        }
        return bytes;
    }

} // namespace Iridium
