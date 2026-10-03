// M7R R4c.0: the scripted mid-run change scenario (device-free): parser,
// per-measured-frame schedule, profile-frame sampling, the drain timeline and its
// JSON Lines records, and the tracked hitch scenarios under assets/benchmarks/m7r.

#include "qualification/harness/ScriptedChanges.h"

#include "profiling/CpuProfiler.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using namespace Iridium;
    using Action = ScriptedChangeAction;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    std::string scenario(std::string_view events) {
        return std::string(R"({"schema":"iridium.qualification.scripted_changes.v1",)"
            R"("id":"t","events":[)") + std::string(events) + "]}";
    }

    // The parser's error message, or "" when the text parses.
    std::string parseError(const std::string& text) {
        try {
            (void)parseScriptedChangeScenario(text);
            return {};
        }
        catch (const std::invalid_argument& error) {
            return error.what();
        }
    }

    bool rejected(const std::string& text, std::string_view fragment) {
        const std::string message = parseError(text);
        if (message.find(fragment) == std::string::npos) {
            std::cerr << "  expected \"" << fragment << "\", got \"" << message
                << "\"\n";
            return false;
        }
        return message.starts_with("scripted changes: ");
    }

    bool testParsesEveryAction() {
        const ScriptedChangeScenario parsed = parseScriptedChangeScenario(
            R"({"schema":"iridium.qualification.scripted_changes.v1","id":"all",)"
            R"("description":"d","runner":{"route":"H-x","anything":[1,2]},"events":[)"
            R"({"frame":0,"action":"add_instances","count":256},)"
            R"({"frame":0,"action":"remove_instances","count":2},)"
            R"({"frame":3,"action":"add_lights","count":300},)"
            R"({"frame":3,"action":"remove_lights","count":1},)"
            R"({"frame":4,"action":"add_materials","count":65536},)"
            R"({"frame":5,"action":"add_environment_probes","count":8},)"
            R"({"frame":6,"action":"remove_environment_probes","count":8},)"
            R"({"frame":7,"action":"remove_capture_probe"},)"
            R"({"frame":8,"action":"add_capture_probe"},)"
            R"({"frame":9,"action":"set_capture_probe_resolution","resolution":512},)"
            R"({"frame":10,"action":"publish_constant_environment","color":[0.5,0,2]}]})");
        CHECK(parsed.id == "all");
        CHECK(parsed.events.size() == 11u);
        const Action expected[] = { Action::AddInstances, Action::RemoveInstances,
            Action::AddLights, Action::RemoveLights, Action::AddMaterials,
            Action::AddEnvironmentProbes, Action::RemoveEnvironmentProbes,
            Action::RemoveCaptureProbe, Action::AddCaptureProbe,
            Action::SetCaptureProbeResolution, Action::PublishConstantEnvironment };
        for (size_t index = 0; index < parsed.events.size(); ++index) {
            CHECK(parsed.events[index].action == expected[index]);
            // Names round-trip.
            const std::string name(scriptedChangeActionName(expected[index]));
            CHECK(parseError(scenario(R"({"frame":1,"action":")" + name +
                R"(","count":1,"resolution":16,"color":[0,0,0]})")).find(
                    "unknown key") != std::string::npos);
        }
        CHECK(parsed.events[0].count == 256u);
        CHECK(parsed.events[2].measuredFrame == 3u);
        CHECK(parsed.events[4].count == 65'536u);
        CHECK(parsed.events[7].count == 0u);
        CHECK(parsed.events[9].count == 512u);
        CHECK(parsed.events[10].color[0] == 0.5f);
        CHECK(parsed.events[10].color[1] == 0.0f);
        CHECK(parsed.events[10].color[2] == 2.0f);
        return true;
    }

    bool testRejectsMalformedScenarios() {
        CHECK(rejected("{", "invalid JSON"));
        CHECK(rejected("[]", "must be a JSON object"));
        CHECK(rejected(R"({"schema":"other","id":"t","events":[{"frame":0,"action":"add_capture_probe"}]})",
            "\"schema\" must be"));
        CHECK(rejected(R"({"schema":"iridium.qualification.scripted_changes.v1","events":[{"frame":0,"action":"add_capture_probe"}]})",
            "\"id\""));
        CHECK(rejected(scenario(""), "non-empty array"));
        CHECK(rejected(R"({"schema":"iridium.qualification.scripted_changes.v1","id":"t","extra":1,"events":[]})",
            "unknown top-level key \"extra\""));
        CHECK(rejected(scenario(R"({"frame":0,"action":"explode"})"),
            "unknown action \"explode\""));
        CHECK(rejected(scenario(R"({"action":"add_capture_probe"})"),
            "requires \"frame\""));
        CHECK(rejected(scenario(R"({"frame":-1,"action":"add_capture_probe"})"),
            "\"frame\" must be an unsigned integer"));
        CHECK(rejected(scenario(R"({"frame":0,"action":"add_lights"})"),
            "requires \"count\""));
        CHECK(rejected(scenario(R"({"frame":0,"action":"add_lights","count":0})"),
            "\"count\" must be 1..65536"));
        CHECK(rejected(scenario(R"({"frame":0,"action":"add_lights","count":65537})"),
            "\"count\" must be 1..65536"));
        CHECK(rejected(scenario(R"({"frame":0,"action":"add_lights","count":2.5})"),
            "unsigned integer"));
        CHECK(rejected(scenario(R"({"frame":0,"action":"remove_capture_probe","count":1})"),
            "unknown key \"count\""));
        CHECK(rejected(scenario(R"({"frame":0,"action":"set_capture_probe_resolution","resolution":500})"),
            "power of two"));
        CHECK(rejected(scenario(R"({"frame":0,"action":"set_capture_probe_resolution","resolution":8})"),
            "power of two"));
        CHECK(rejected(scenario(R"({"frame":0,"action":"publish_constant_environment","color":[1,2]})"),
            "three numbers"));
        CHECK(rejected(scenario(R"({"frame":0,"action":"publish_constant_environment","color":[1,-2,0]})"),
            "0..1000"));
        CHECK(rejected(scenario(R"({"frame":0,"action":"publish_constant_environment","color":[1,"x",0]})"),
            "numbers"));
        CHECK(rejected(scenario(R"({"frame":5,"action":"add_capture_probe"},{"frame":4,"action":"remove_capture_probe"})"),
            "event 1 frame is earlier"));
        CHECK(rejected(scenario(R"(7)"), "event 0 must be an object"));
        return true;
    }

    bool testScheduleIssuesEachEventOnce() {
        const ScriptedChangeScenario parsed = parseScriptedChangeScenario(scenario(
            R"({"frame":2,"action":"add_lights","count":1},)"
            R"({"frame":2,"action":"add_lights","count":2},)"
            R"({"frame":5,"action":"add_lights","count":3},)"
            R"({"frame":9,"action":"add_lights","count":4})"));
        ScriptedChangeSchedule schedule(parsed.events);
        CHECK(schedule.take(0).empty());
        CHECK(schedule.take(1).empty());
        const auto two = schedule.take(2);
        CHECK(two.size() == 2u && two[0].count == 1u && two[1].count == 2u);
        CHECK(schedule.take(2).empty());
        CHECK(schedule.issuedCount() == 2u);
        // A skipped measured frame catches up, in order, exactly once.
        const auto late = schedule.take(7);
        CHECK(late.size() == 1u && late[0].count == 3u);
        CHECK(!schedule.finished());
        const auto last = schedule.take(9);
        CHECK(last.size() == 1u && last[0].count == 4u);
        CHECK(schedule.finished());
        CHECK(schedule.take(100).empty());
        CHECK(ScriptedChangeSchedule{}.finished());
        return true;
    }

    bool testFrameSampling() {
        CpuFrameProfile frame{};
        frame.frameId = 42;
        frame.events = {
            { "cpu.frame.total", 1, 0, 0, 0, 10'000'000 },
            { "cpu.renderer.drain_all_frames", 2, 1, 0, 10, 2'000'000 },
            { "cpu.renderer.upload_wait", 3, 1, 0, 20, 300'000 },
            { "cpu.renderer.drain_all_frames", 4, 1, 0, 30, 1'000'000 },
            { "cpu.scene.transforms", 5, 1, 0, 40, 7 },
            { nullptr, 6, 1, 0, 50, 9 },
        };
        const ScriptedFrameSample sample = sampleScriptedFrame(frame);
        CHECK(sample.frameId == 42u);
        CHECK(sample.cpuFrameNanoseconds == 10'000'000u);
        CHECK(sample.drainCount == 2u);
        CHECK(sample.drainNanoseconds == 3'000'000u);
        CHECK(sample.uploadWaitCount == 1u);
        CHECK(sample.uploadWaitNanoseconds == 300'000u);
        const ScriptedFrameSample empty = sampleScriptedFrame(CpuFrameProfile{});
        CHECK(empty.cpuFrameNanoseconds == 0u && empty.drainCount == 0u);
        return true;
    }

    bool testTimelineSamplesEachProfiledFrameOnce() {
        CpuProfiler profiler(true);
        ScriptedFrameTimeline timeline;
        timeline.reserve(4);
        CHECK(!timeline.observe(profiler));
        for (uint64_t frameId = 11; frameId <= 13; ++frameId) {
            CHECK(profiler.beginFrame(frameId));
            {
                CpuScope total(profiler, "cpu.frame.total");
                if (frameId == 12) {
                    CpuScope drain(profiler, "cpu.renderer.drain_all_frames");
                }
            }
            CHECK(profiler.endFrame());
            CHECK(timeline.observe(profiler));
            CHECK(!timeline.observe(profiler));
        }
        const auto samples = timeline.samples();
        CHECK(samples.size() == 3u);
        CHECK(samples[0].frameId == 11u && samples[0].drainCount == 0u);
        CHECK(samples[1].frameId == 12u && samples[1].drainCount == 1u);
        CHECK(samples[2].frameId == 13u);
        CHECK(samples[1].cpuFrameNanoseconds >= samples[1].drainNanoseconds);
        return true;
    }

    bool testJsonLinesRecords() {
        const ScriptedChangeScenario parsed = parseScriptedChangeScenario(scenario(
            R"({"frame":0,"action":"add_instances","count":256},)"
            R"({"frame":1,"action":"publish_constant_environment","color":[0.25,0.5,1]},)"
            R"({"frame":2,"action":"set_capture_probe_resolution","resolution":256})"));
        const std::vector<AppliedScriptedChange> applied{
            { .eventIndex = 0, .measuredFrame = 0, .applicationFrame = 500,
                .applyNanoseconds = 7, .result = 512 },
            { .eventIndex = 2, .measuredFrame = 2, .applicationFrame = 502,
                .applyNanoseconds = 9, .result = 256 },
        };
        const std::vector<ScriptedFrameSample> samples{
            { .frameId = 501, .cpuFrameNanoseconds = 5, .drainNanoseconds = 3,
                .uploadWaitNanoseconds = 1, .drainCount = 2, .uploadWaitCount = 1 },
            { .frameId = 502, .cpuFrameNanoseconds = 6 },
        };
        std::ostringstream stream;
        writeScriptedChangeJsonLines(stream, parsed, "a/b.json", 500, applied,
            samples);
        std::istringstream lines(stream.str());
        std::vector<nlohmann::json> records;
        for (std::string line; std::getline(lines, line);)
            records.push_back(nlohmann::json::parse(line));
        CHECK(records.size() == 3u);
        const nlohmann::json& header = records[0];
        CHECK(header["type"] == "scripted_changes");
        CHECK(header["schema"] == "iridium.qualification.scripted_changes.v1");
        CHECK(header["scenario_id"] == "t");
        CHECK(header["scenario_path"] == "a/b.json");
        CHECK(header["warmup_frames"] == 500);
        CHECK(header["scheduled_events"] == 3);
        CHECK(header["frame_samples"] == 2);
        const nlohmann::json& events = header["applied_events"];
        CHECK(events.size() == 2u);
        CHECK(events[0]["action"] == "add_instances");
        CHECK(events[0]["count"] == 256);
        CHECK(events[0]["frame"] == 0);
        CHECK(events[0]["application_frame"] == 500);
        CHECK(events[0]["result"] == 512);
        CHECK(events[1]["index"] == 2);
        CHECK(events[1]["resolution"] == 256);
        CHECK(!events[1].contains("count"));
        CHECK(records[1]["type"] == "scripted_frame");
        CHECK(records[1]["m"] == 0);   // frame id 501 = application frame 500
        CHECK(records[1]["cpu_ns"] == 5);
        CHECK(records[1]["drain"] == 2);
        CHECK(records[1]["drain_ns"] == 3);
        CHECK(records[1]["upload_wait"] == 1);
        CHECK(records[1]["upload_wait_ns"] == 1);
        CHECK(records[2]["m"] == 1);
        const nlohmann::json color = nlohmann::json::parse(
            scriptedChangeJson(parsed.events[1], { .eventIndex = 1 }))["color"];
        CHECK(color.size() == 3u && color[2] == 1.0);
        return true;
    }

    // The tracked hitch routes parse and keep the R4c.0 schedule: 500 warmup
    // frames, then an event every 500..1000 frames within 10,000 measured frames.
    bool testTrackedHitchScenarios() {
        const std::filesystem::path directory =
            std::filesystem::path(PROJECT_ROOT_DIR) / "assets" / "benchmarks" / "m7r";
        for (const char* name : { "hitch-stress.v1.json", "hitch-probe.v1.json" }) {
            const ScriptedChangeScenario parsed =
                loadScriptedChangeScenario(directory / name);
            CHECK(parsed.events.size() >= 10u);
            CHECK(parsed.events.front().measuredFrame >= 500u);
            CHECK(parsed.events.back().measuredFrame < 10'000u);
            for (size_t index = 1; index < parsed.events.size(); ++index) {
                const uint64_t gap = parsed.events[index].measuredFrame -
                    parsed.events[index - 1].measuredFrame;
                CHECK(gap >= 500u && gap <= 1'000u);
            }
        }
        try {
            (void)loadScriptedChangeScenario(directory / "missing.json");
            CHECK(false);
        }
        catch (const std::invalid_argument& error) {
            CHECK(std::string_view(error.what()).find("cannot open") !=
                std::string_view::npos);
        }
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };

    constexpr TestCase tests[] = {
        { "Parses every action", testParsesEveryAction },
        { "Rejects malformed scenarios", testRejectsMalformedScenarios },
        { "Schedule issues each event once", testScheduleIssuesEachEventOnce },
        { "Frame sampling", testFrameSampling },
        { "Timeline samples each profiled frame once",
            testTimelineSamplesEachProfiledFrameOnce },
        { "JSON Lines records", testJsonLinesRecords },
        { "Tracked hitch scenarios", testTrackedHitchScenarios },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " << test.name << '\n';
            }
            else {
                ++failures;
                std::cerr << "[FAIL] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }

    constexpr size_t testCount = sizeof(tests) / sizeof(tests[0]);
    std::cout << testCount - failures << '/' << testCount << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
