#pragma once

#include "assets/model/ModelRuntimeProduct.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <functional>
#include <optional>

namespace Iridium {
    // One in-flight CPU job. Callers retain/coalesce newer requests, and reject
    // stale completions by serial before touching any renderer resource.
    class MaterialPreviewCompileQueue {
    public:
        struct Completion {
            uint64_t serial = 0;
            RuntimeCanonicalMaterialResult result;
            std::string diagnostic;
        };
        ~MaterialPreviewCompileQueue() {
            worker_.request_stop();
            condition_.notify_all();
        }
        [[nodiscard]] bool busy() const {
            std::lock_guard lock(mutex_);
            return running_ || task_.has_value() || completion_.has_value();
        }
        bool submit(uint64_t serial, std::function<RuntimeCanonicalMaterialResult()> compile) {
            std::lock_guard lock(mutex_);
            if (running_ || task_ || completion_) return false;
            if (!worker_.joinable()) worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
            task_ = Task{serial, std::move(compile)};
            condition_.notify_one();
            return true;
        }
        [[nodiscard]] std::optional<Completion> poll() {
            std::lock_guard lock(mutex_);
            auto result = std::move(completion_);
            completion_.reset();
            return result;
        }
    private:
        struct Task {
            uint64_t serial;
            std::function<RuntimeCanonicalMaterialResult()> compile;
        };
        void run(std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::optional<Task> task;
                {
                    std::unique_lock lock(mutex_);
                    if (!condition_.wait(lock, stop, [this] { return task_.has_value(); })) return;
                    task = std::move(task_);
                    task_.reset();
                    running_ = true;
                }
                Completion result{.serial = task->serial};
                try { result.result = task->compile(); }
                catch (const std::exception& error) { result.diagnostic = error.what(); }
                catch (...) { result.diagnostic = "Unknown material preview compilation failure"; }
                {
                    std::lock_guard lock(mutex_);
                    completion_ = std::move(result);
                    running_ = false;
                }
            }
        }
        mutable std::mutex mutex_;
        std::condition_variable_any condition_;
        std::optional<Task> task_;
        std::optional<Completion> completion_;
        bool running_ = false;
        // Declared last so shutdown joins before destroying mutex/state. This
        // worker is reused across drags, not recreated for every slider update.
        std::jthread worker_;
    };
}
