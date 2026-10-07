#pragma once

// M7R R5b.3 cook-while-render starvation test (ADR-0015, design section 4.2):
// harness-only load and probe, enabled by qualification flags.
//
//   BackgroundCookLoad (--qualification-background-cook PATH) re-cooks one
//   source asset with the DDC bypassed, one full cook after another, as
//   Background work on the engine task system for the whole run, and reports
//   how many cooks completed and whether every cook produced the same bytes.
//
//   FrameTaskProbe (--qualification-frame-task-probe) joins a fixed
//   frame-critical parallelFor every frame, as R5c's parallel extraction will,
//   and records when the first worker started it and how long the join took.
//   Run it on both sides of a starvation pair so the comparison is fair.
//
// Neither allocates in a steady frame (the probe's samples are reserved up
// front); both print one IRIDIUM_* line at the end of the run.

#include "core/tasks/TaskSystem.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <vector>

namespace Iridium {

    class BackgroundCookLoad final {
    public:
        // `source` is absolute or relative to `assetRoot`; its metadata is the
        // sidecar next to it.
        BackgroundCookLoad(Tasks::TaskSystem& tasks,
            std::filesystem::path assetRoot, std::filesystem::path source);
        ~BackgroundCookLoad();

        BackgroundCookLoad(const BackgroundCookLoad&) = delete;
        BackgroundCookLoad& operator=(const BackgroundCookLoad&) = delete;

        void start();
        // Cancels the cook in progress (it observes the stop token) and waits
        // for it. Idempotent.
        void stop();
        // One JSON object: completed/failed/cancelled cooks, their summed and
        // mean duration, the run time and the artifact hash agreement.
        [[nodiscard]] std::string reportJson() const;

    private:
        void cookOnce();

        Tasks::TaskSystem& tasks_;
        std::filesystem::path assetRoot_;
        std::filesystem::path source_;
        std::stop_source stop_;
        std::atomic<bool> stopping_{ false };
        bool started_ = false;
        mutable std::mutex mutex_;
        uint64_t completed_ = 0;
        uint64_t failed_ = 0;
        uint64_t cancelled_ = 0;
        double completedSeconds_ = 0.0;
        double firstCookSeconds_ = 0.0;
        std::string artifactHash_;
        bool artifactHashesIdentical_ = true;
        std::string lastError_;
        std::chrono::steady_clock::time_point startTime_{};
        std::chrono::steady_clock::time_point stopTime_{};
        // Declared last: drained by stop() before the state above goes.
        std::unique_ptr<Tasks::FunctionStrand> strand_;
    };

    class FrameTaskProbe final {
    public:
        static constexpr uint32_t ChunkCount = 32;
        static constexpr std::chrono::microseconds ChunkWork{ 20 };

        // Reserves samples for `reservedFrames` measured frames.
        FrameTaskProbe(Tasks::TaskSystem& tasks, size_t reservedFrames);

        FrameTaskProbe(const FrameTaskProbe&) = delete;
        FrameTaskProbe& operator=(const FrameTaskProbe&) = delete;

        // Main thread, once per frame: submit, join and (for measured frames)
        // record the worker start latency, the join time and the frame period.
        void runFrame(bool measured);
        [[nodiscard]] std::string reportJson() const;

    private:
        class Chunks final : public Tasks::TaskSet {
        public:
            Chunks();
            uint64_t submitNanoseconds = 0;
            std::atomic<uint64_t> firstWorkerStartNanoseconds{ 0 };
            std::atomic<uint32_t> workerChunks{ 0 };

        private:
            void execute(Tasks::TaskRange range, uint32_t threadIndex) override;
        };

        Tasks::TaskSystem& tasks_;
        Chunks chunks_;
        std::vector<float> workerStartMicroseconds_;
        std::vector<float> joinMicroseconds_;
        std::vector<float> framePeriodMicroseconds_;
        uint64_t framesWithoutWorker_ = 0;
        uint64_t workerChunksTotal_ = 0;
        uint64_t droppedSamples_ = 0;
        uint64_t lastFrameNanoseconds_ = 0;
    };

} // namespace Iridium
