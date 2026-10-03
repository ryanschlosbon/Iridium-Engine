#pragma once

// M7R R4c.0: scripted mid-run scene changes for the hitch scenario
// (--qualification-scripted-changes, tools/m7r/Run-HitchScenario.ps1). This
// header is the device-free part: the scenario file parser, the deterministic
// per-measured-frame schedule and the per-frame drain timeline that the harness
// samples from the CPU profiler. The profiler keeps only
// CpuProfiler::CompletedFrameCapacity detailed frames, so the harness records
// its own compact record for every measured frame and appends the records to
// the profile JSON Lines (tools/m7r/Analyze-Hitches.py reads them).
//
// Scenario file (UTF-8 JSON):
//
//   {
//     "schema": "iridium.qualification.scripted_changes.v1",
//     "id": "<scenario id>",
//     "description": "...",            optional
//     "runner": { ... },               optional, ignored by the engine
//     "events": [
//       { "frame": 500, "action": "add_instances", "count": 256 },
//       { "frame": 1500, "action": "publish_constant_environment",
//         "color": [0.02, 0.03, 0.05] },
//       { "frame": 2500, "action": "set_capture_probe_resolution",
//         "resolution": 512 }
//     ]
//   }
//
// "frame" is the zero-based measured frame; events apply at
// FrameBeginPhase::PreSceneUpdate of that frame, in file order. Frames must be
// non-decreasing. Unknown keys are errors.

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium {

    class CpuProfiler;
    struct CpuFrameProfile;

    inline constexpr std::string_view kScriptedChangesSchema =
        "iridium.qualification.scripted_changes.v1";

    enum class ScriptedChangeAction : uint8_t {
        // Benchmark instanced-grid copies of the startup model (GPU scene,
        // opaque/view culler and imported-buffer capacity).
        AddInstances,
        RemoveInstances,          // LIFO, scripted instances only
        // Unshadowed point lights (GPU light records, kInitialGpuLightCapacity).
        AddLights,
        RemoveLights,             // LIFO, scripted lights only
        // Canonical material records (GPU material table).
        AddMaterials,
        // Asset-environment reflection probes (probe record capacity); require
        // --validate-reflection-probes.
        AddEnvironmentProbes,
        RemoveEnvironmentProbes,  // LIFO, scripted probes only
        // The runtime capture probe owner (captured environment slots).
        RemoveCaptureProbe,
        AddCaptureProbe,
        SetCaptureProbeResolution,
        // A new constant scene environment (probe environment table and the
        // scene image-based lighting).
        PublishConstantEnvironment,
    };

    [[nodiscard]] std::string_view scriptedChangeActionName(
        ScriptedChangeAction action) noexcept;

    struct ScriptedChangeEvent {
        uint64_t measuredFrame = 0;
        ScriptedChangeAction action = ScriptedChangeAction::AddInstances;
        // Add/remove count, or the capture resolution for
        // SetCaptureProbeResolution; 0 for the others.
        uint32_t count = 0;
        // PublishConstantEnvironment: linear Rec.709 radiance.
        float color[3]{ 0.0f, 0.0f, 0.0f };
    };

    struct ScriptedChangeScenario {
        std::string id;
        std::vector<ScriptedChangeEvent> events;
    };

    // Throws std::invalid_argument ("scripted changes: ...") on malformed input.
    [[nodiscard]] ScriptedChangeScenario parseScriptedChangeScenario(
        std::string_view json);
    [[nodiscard]] ScriptedChangeScenario loadScriptedChangeScenario(
        const std::filesystem::path& path);

    // Hands out each event exactly once, in order, at the first measured frame
    // at or after its scheduled frame.
    class ScriptedChangeSchedule {
    public:
        ScriptedChangeSchedule() = default;
        explicit ScriptedChangeSchedule(
            std::span<const ScriptedChangeEvent> events) noexcept
            : events_(events) {}

        // The not yet issued events with measuredFrame <= `measuredFrame`.
        [[nodiscard]] std::span<const ScriptedChangeEvent> take(
            uint64_t measuredFrame) noexcept;
        [[nodiscard]] size_t issuedCount() const noexcept { return cursor_; }
        [[nodiscard]] bool finished() const noexcept {
            return cursor_ == events_.size();
        }

    private:
        std::span<const ScriptedChangeEvent> events_;
        size_t cursor_ = 0;
    };

    // One profiled frame, reduced to what the hitch analysis needs.
    struct ScriptedFrameSample {
        uint64_t frameId = 0;           // CpuProfiler frame id (application frame + 1)
        uint64_t cpuFrameNanoseconds = 0;   // cpu.frame.total
        uint64_t drainNanoseconds = 0;      // cpu.renderer.drain_all_frames
        uint64_t uploadWaitNanoseconds = 0; // cpu.renderer.upload_wait
        uint32_t drainCount = 0;
        uint32_t uploadWaitCount = 0;
    };

    [[nodiscard]] ScriptedFrameSample sampleScriptedFrame(
        const CpuFrameProfile& frame) noexcept;

    // Scope detail for a frame whose cpu.frame.total reaches the slow-frame
    // threshold, so isolated spikes outside the profiler's detailed-frame window
    // can still be attributed. Keeps the longest kEventCapacity scopes.
    struct ScriptedSlowFrame {
        struct Event {
            const char* name = nullptr; // profiler scope names are static strings
            uint64_t eventId = 0;
            uint64_t parentEventId = 0;
            uint64_t durationNanoseconds = 0;
        };
        static constexpr size_t kEventCapacity = 48;
        uint64_t frameId = 0;
        uint32_t eventCount = 0;
        std::array<Event, kEventCapacity> events{};
    };

    // Samples the profiler's latest completed frame once per frame id into
    // reserved storage (no steady-frame allocations once reserved).
    class ScriptedFrameTimeline {
    public:
        static constexpr size_t kSlowFrameCapacity = 32;
        static constexpr uint64_t kDefaultSlowFrameNanoseconds = 250'000'000;

        void reserve(size_t frames) {
            samples_.reserve(frames);
            slowFrames_.reserve(kSlowFrameCapacity);
        }
        void setSlowFrameThreshold(uint64_t nanoseconds) noexcept {
            slowFrameNanoseconds_ = nanoseconds;
        }
        // Returns true when a new frame was recorded.
        bool observe(const CpuProfiler& profiler);
        bool observe(const CpuFrameProfile& frame);
        [[nodiscard]] std::span<const ScriptedFrameSample> samples()
            const noexcept { return samples_; }
        // The first kSlowFrameCapacity frames at or above the threshold.
        [[nodiscard]] std::span<const ScriptedSlowFrame> slowFrames()
            const noexcept { return slowFrames_; }

    private:
        std::vector<ScriptedFrameSample> samples_;
        std::vector<ScriptedSlowFrame> slowFrames_;
        uint64_t slowFrameNanoseconds_ = kDefaultSlowFrameNanoseconds;
        uint64_t lastFrameId_ = 0;
    };

    // One applied event; `result` is the action's resulting population (scripted
    // instances/lights/materials/probes, the capture resolution, or the loaded
    // environment count).
    struct AppliedScriptedChange {
        size_t eventIndex = 0;
        uint64_t measuredFrame = 0;
        uint64_t applicationFrame = 0;
        uint64_t applyNanoseconds = 0;
        uint64_t result = 0;
    };

    // JSON object for one applied event (also printed as IRIDIUM_SCRIPTED_CHANGE).
    [[nodiscard]] std::string scriptedChangeJson(
        const ScriptedChangeEvent& event, const AppliedScriptedChange& applied);

    // Appends {"type":"scripted_changes",...} followed by one
    // {"type":"scripted_frame",...} record per sample and one
    // {"type":"scripted_slow_frame",...} record per slow frame. Measured frame
    // m = frameId - 1 - warmupFrames.
    void writeScriptedChangeJsonLines(std::ostream& output,
        const ScriptedChangeScenario& scenario, std::string_view scenarioPath,
        uint64_t warmupFrames, std::span<const AppliedScriptedChange> applied,
        std::span<const ScriptedFrameSample> samples,
        std::span<const ScriptedSlowFrame> slowFrames = {});

} // namespace Iridium
