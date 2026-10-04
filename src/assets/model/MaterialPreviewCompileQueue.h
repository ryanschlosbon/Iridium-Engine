#pragma once

#include "assets/model/ModelRuntimeProduct.h"
#include "core/tasks/TaskSystem.h"

#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace Iridium {
    // One in-flight CPU job. Callers retain/coalesce newer requests, and reject
    // stale completions by serial before touching any renderer resource.
    //
    // M7R R5b.2 (ADR-0015): the job is a Normal-priority task on the engine task
    // system (one slot, latest wins at the caller), not a lazily started thread.
    // Without a task system (tools, tests) a submitted job compiles inline.
    class MaterialPreviewCompileQueue {
    public:
        struct Completion {
            uint64_t serial = 0;
            RuntimeCanonicalMaterialResult result;
            std::string diagnostic;
        };

        MaterialPreviewCompileQueue() = default;
        ~MaterialPreviewCompileQueue() { waitIdle(); }

        MaterialPreviewCompileQueue(const MaterialPreviewCompileQueue&) = delete;
        MaterialPreviewCompileQueue& operator=(const MaterialPreviewCompileQueue&) = delete;

        // Main thread, while no job is in flight.
        void setTaskSystem(Tasks::TaskSystem* tasks) {
            waitIdle();
            tasks_ = tasks;
        }
        [[nodiscard]] bool busy() const {
            std::lock_guard lock(mutex_);
            return running_ || completion_.has_value();
        }
        bool submit(uint64_t serial, std::function<RuntimeCanonicalMaterialResult()> compile) {
            {
                std::lock_guard lock(mutex_);
                if (running_ || completion_) return false;
                running_ = true;
            }
            // running_ was false, so the previous job's execute() has returned;
            // its task becomes complete right after (a short window).
            while (!job_.isComplete()) std::this_thread::yield();
            job_.serial = serial;
            job_.compile = std::move(compile);
            if (tasks_ != nullptr) tasks_->submit(job_);
            else job_.runInline();
            return true;
        }
        [[nodiscard]] std::optional<Completion> poll() {
            std::lock_guard lock(mutex_);
            auto result = std::move(completion_);
            completion_.reset();
            return result;
        }

    private:
        class Job final : public Tasks::TaskSet {
        public:
            explicit Job(MaterialPreviewCompileQueue& owner)
                : TaskSet(Tasks::TaskPriority::Normal, 1, 1, "asset.material_preview.compile"),
                  owner_(owner) {}

            void runInline() { run(); }

            uint64_t serial = 0;
            std::function<RuntimeCanonicalMaterialResult()> compile;

        private:
            void execute(Tasks::TaskRange, uint32_t) override { run(); }
            void run() {
                Completion result{ .serial = serial };
                try { result.result = compile(); }
                catch (const std::exception& error) { result.diagnostic = error.what(); }
                catch (...) { result.diagnostic = "Unknown material preview compilation failure"; }
                compile = {};
                std::lock_guard lock(owner_.mutex_);
                owner_.completion_ = std::move(result);
                owner_.running_ = false;
            }

            MaterialPreviewCompileQueue& owner_;
        };

        void waitIdle() {
            if (tasks_ != nullptr && !job_.isComplete()) tasks_->wait(job_);
        }

        mutable std::mutex mutex_;
        std::optional<Completion> completion_;
        bool running_ = false;
        Tasks::TaskSystem* tasks_ = nullptr;
        // Declared last: destroyed first, after the destructor waited for it.
        Job job_{ *this };
    };
}
