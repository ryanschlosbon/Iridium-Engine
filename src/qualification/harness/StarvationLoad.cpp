#include "qualification/harness/StarvationLoad.h"

#include "assets/AssetImport.h"
#include "assets/AssetMetadata.h"
#include "assets/cooker/AssetCooker.h"
#include "assets/cooker/CookedArtifact.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Iridium {

    namespace {

        uint64_t nowNanoseconds() noexcept {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        double seconds(std::chrono::steady_clock::duration duration) {
            return std::chrono::duration<double>(duration).count();
        }

        // Nearest-rank percentile of a copy (end-of-run only).
        double percentile(std::vector<float> values, double fraction) {
            if (values.empty()) return 0.0;
            std::sort(values.begin(), values.end());
            const size_t index = std::min(values.size() - 1,
                static_cast<size_t>(fraction * static_cast<double>(values.size() - 1) + 0.5));
            return values[index];
        }

        nlohmann::ordered_json distribution(const std::vector<float>& values) {
            return {
                { "p50", percentile(values, 0.50) },
                { "p95", percentile(values, 0.95) },
                { "p99", percentile(values, 0.99) },
                { "max", values.empty() ? 0.0 : static_cast<double>(
                    *std::max_element(values.begin(), values.end())) },
            };
        }

        // The editor cook target (AssetIntegration's preparation services).
        CookTarget backgroundCookTarget() {
            return CookTarget{
                .platform = "windows-x64",
                .profile = "editor",
                .qualityPolicy = "reference",
                .artifactContainerVersion = kCookedArtifactContainerVersion,
                .materialSchemaVersion = 2,
            };
        }

        constexpr uint64_t MaxFailuresWithoutCook = 3;

    } // namespace

    // --- BackgroundCookLoad ------------------------------------------------------------

    BackgroundCookLoad::BackgroundCookLoad(Tasks::TaskSystem& tasks,
        std::filesystem::path assetRoot, std::filesystem::path source)
        : tasks_(tasks),
          assetRoot_(std::move(assetRoot)),
          source_(std::move(source)),
          strand_(std::make_unique<Tasks::FunctionStrand>(tasks,
              Tasks::TaskPriority::Background, "qualification.background_cook")) {
        const std::filesystem::path sourcePath = source_.is_absolute()
            ? source_ : assetRoot_ / source_;
        if (!std::filesystem::is_regular_file(sourcePath)) {
            throw std::invalid_argument(
                "--qualification-background-cook source not found: " +
                sourcePath.generic_string());
        }
    }

    BackgroundCookLoad::~BackgroundCookLoad() {
        stop();
    }

    void BackgroundCookLoad::start() {
        if (started_) return;
        started_ = true;
        startTime_ = std::chrono::steady_clock::now();
        (void)strand_->post([this] { cookOnce(); });
    }

    void BackgroundCookLoad::stop() {
        if (stopping_.exchange(true)) return;
        stop_.request_stop();
        strand_->waitIdle();
        std::lock_guard lock(mutex_);
        stopTime_ = std::chrono::steady_clock::now();
    }

    void BackgroundCookLoad::cookOnce() {
        if (stopping_.load()) return;
        const std::stop_token stopToken = stop_.get_token();
        const auto begin = std::chrono::steady_clock::now();
        bool giveUp = false;
        try {
            const std::filesystem::path sourcePath = source_.is_absolute()
                ? source_ : assetRoot_ / source_;
            const std::filesystem::path relative =
                std::filesystem::relative(sourcePath, assetRoot_);
            const AssetMetadataReadResult metadata =
                readAssetMetadata(sourcePath.string() + ".iridium.meta");
            if (!metadata.metadata || metadata.hasErrors()) {
                throw std::runtime_error("Background cook metadata is missing or invalid.");
            }
            // A full cook every time: the source parse and content hashes,
            // then every product with no derived-data cache (no texture-view
            // or parent artifact reuse).
            const ImporterRegistry importers = createStandardAssetImporterRegistry();
            PreparedAssetCook prepared = prepareAssetCook(importers, assetRoot_,
                relative, *metadata.metadata, backgroundCookTarget(),
                "m7r-r5b3-background-cook", stopToken);
            if (!prepared.valid()) {
                throw std::runtime_error("Background cook preparation failed.");
            }
            const CookedArtifactBlob blob = buildPreparedArtifact(prepared, stopToken);
            const double duration = seconds(std::chrono::steady_clock::now() - begin);
            std::lock_guard lock(mutex_);
            if (stopToken.stop_requested()) {
                ++cancelled_;
            }
            else {
                if (completed_ == 0) {
                    firstCookSeconds_ = duration;
                    artifactHash_ = blob.artifactHash;
                }
                else if (blob.artifactHash != artifactHash_) {
                    artifactHashesIdentical_ = false;
                }
                ++completed_;
                completedSeconds_ += duration;
            }
        }
        catch (const std::exception& exception) {
            std::lock_guard lock(mutex_);
            if (stopToken.stop_requested()) {
                ++cancelled_;
            }
            else {
                ++failed_;
                lastError_ = exception.what();
                giveUp = completed_ == 0 && failed_ >= MaxFailuresWithoutCook;
            }
        }
        if (giveUp) {
            return; // a broken source must not spin the strand
        }
        if (!stopping_.load()) {
            (void)strand_->post([this] { cookOnce(); });
        }
    }

    std::string BackgroundCookLoad::reportJson() const {
        std::lock_guard lock(mutex_);
        const auto end = stopTime_ == std::chrono::steady_clock::time_point{}
            ? std::chrono::steady_clock::now() : stopTime_;
        const double runSeconds = started_ ? seconds(end - startTime_) : 0.0;
        nlohmann::ordered_json report{
            { "source", source_.generic_string() },
            { "background_slots", tasks_.backgroundSlotLimit() },
            { "workers", tasks_.workerThreadCount() },
            { "completed", completed_ },
            { "failed", failed_ },
            { "cancelled", cancelled_ },
            { "first_cook_seconds", firstCookSeconds_ },
            { "cook_seconds_total", completedSeconds_ },
            { "cook_seconds_mean", completed_ != 0
                ? completedSeconds_ / static_cast<double>(completed_) : 0.0 },
            { "run_seconds", runSeconds },
            { "cooks_per_minute", runSeconds > 0.0
                ? 60.0 * static_cast<double>(completed_) / runSeconds : 0.0 },
            { "artifact_hash", artifactHash_ },
            { "artifact_hashes_identical", artifactHashesIdentical_ },
        };
        if (!lastError_.empty()) report["last_error"] = lastError_;
        return report.dump();
    }

    // --- FrameTaskProbe ----------------------------------------------------------------

    FrameTaskProbe::Chunks::Chunks()
        : TaskSet(Tasks::TaskPriority::FrameCritical, ChunkCount, 1,
            "qualification.frame_task_probe") {}

    void FrameTaskProbe::Chunks::execute(Tasks::TaskRange range, uint32_t threadIndex) {
        if (threadIndex != 0) {
            uint64_t expected = 0;
            (void)firstWorkerStartNanoseconds.compare_exchange_strong(
                expected, nowNanoseconds(), std::memory_order_relaxed);
            workerChunks.fetch_add(range.end - range.begin, std::memory_order_relaxed);
        }
        const uint64_t perChunk = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ChunkWork).count());
        for (uint32_t chunk = range.begin; chunk < range.end; ++chunk) {
            const uint64_t until = nowNanoseconds() + perChunk;
            while (nowNanoseconds() < until) {
                // Fixed busy work stands in for a slice of parallel extraction.
            }
        }
    }

    FrameTaskProbe::FrameTaskProbe(Tasks::TaskSystem& tasks, size_t reservedFrames)
        : tasks_(tasks) {
        workerStartMicroseconds_.reserve(reservedFrames);
        joinMicroseconds_.reserve(reservedFrames);
        framePeriodMicroseconds_.reserve(reservedFrames);
    }

    void FrameTaskProbe::runFrame(bool measured) {
        const uint64_t frameStart = nowNanoseconds();
        if (lastFrameNanoseconds_ != 0 && !framePeriodMicroseconds_.empty() &&
            framePeriodMicroseconds_.back() == 0.0f) {
            // The previous measured frame's period ends here.
            framePeriodMicroseconds_.back() =
                static_cast<float>(frameStart - lastFrameNanoseconds_) / 1000.0f;
        }
        lastFrameNanoseconds_ = frameStart;

        chunks_.firstWorkerStartNanoseconds.store(0, std::memory_order_relaxed);
        chunks_.workerChunks.store(0, std::memory_order_relaxed);
        chunks_.submitNanoseconds = nowNanoseconds();
        tasks_.submit(chunks_);
        tasks_.wait(chunks_);
        const uint64_t joined = nowNanoseconds();
        if (!measured) return;
        if (joinMicroseconds_.size() == joinMicroseconds_.capacity()) {
            ++droppedSamples_; // never grows in a frame
            return;
        }
        const float join = static_cast<float>(joined - chunks_.submitNanoseconds) / 1000.0f;
        const uint64_t firstWorker =
            chunks_.firstWorkerStartNanoseconds.load(std::memory_order_relaxed);
        const uint32_t workerChunks = chunks_.workerChunks.load(std::memory_order_relaxed);
        workerChunksTotal_ += workerChunks;
        float workerStart = join;
        if (firstWorker == 0) {
            ++framesWithoutWorker_; // the main thread ran every chunk
        }
        else {
            workerStart = static_cast<float>(
                firstWorker - chunks_.submitNanoseconds) / 1000.0f;
        }
        workerStartMicroseconds_.push_back(workerStart);
        joinMicroseconds_.push_back(join);
        framePeriodMicroseconds_.push_back(0.0f);
    }

    std::string FrameTaskProbe::reportJson() const {
        std::vector<float> periods;
        std::vector<size_t> periodFrames;
        for (size_t index = 0; index < framePeriodMicroseconds_.size(); ++index) {
            if (framePeriodMicroseconds_[index] > 0.0f) {
                periods.push_back(framePeriodMicroseconds_[index]);
                periodFrames.push_back(index);
            }
        }
        const double medianPeriod = percentile(periods, 0.5);
        uint64_t slowFrames = 0;
        uint64_t slowFramesFromJoin = 0;
        for (size_t index = 0; index < periods.size(); ++index) {
            if (periods[index] <= 2.0 * medianPeriod) continue;
            ++slowFrames;
            // Caused by the frame task when its wait covers at least half of
            // the excess over the median.
            const double excess = periods[index] - medianPeriod;
            const double expectedJoin = percentile(joinMicroseconds_, 0.5);
            if (joinMicroseconds_[periodFrames[index]] - expectedJoin >= 0.5 * excess) {
                ++slowFramesFromJoin;
            }
        }
        const nlohmann::ordered_json report{
            { "frames", joinMicroseconds_.size() },
            { "chunks_per_frame", ChunkCount },
            { "chunk_work_us", ChunkWork.count() },
            { "worker_start_us", distribution(workerStartMicroseconds_) },
            { "join_us", distribution(joinMicroseconds_) },
            { "frames_without_worker", framesWithoutWorker_ },
            { "worker_chunk_fraction", joinMicroseconds_.empty() ? 0.0
                : static_cast<double>(workerChunksTotal_) /
                    (static_cast<double>(joinMicroseconds_.size()) * ChunkCount) },
            { "frame_period_us", distribution(periods) },
            { "frames_over_2x_median", slowFrames },
            { "frames_over_2x_median_from_frame_task_wait", slowFramesFromJoin },
            { "dropped_samples", droppedSamples_ },
        };
        return report.dump();
    }

} // namespace Iridium
