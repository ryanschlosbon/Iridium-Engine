#include "qualification/harness/ScriptedChanges.h"

#include "profiling/CpuProfiler.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace Iridium {

    namespace {

        using Json = nlohmann::json;

        struct ActionInfo {
            ScriptedChangeAction action;
            std::string_view name;
            // Which payload key the action requires.
            enum class Payload : uint8_t { None, Count, Resolution, Color } payload;
        };

        constexpr std::array<ActionInfo, 11> kActions{ {
            { ScriptedChangeAction::AddInstances, "add_instances",
                ActionInfo::Payload::Count },
            { ScriptedChangeAction::RemoveInstances, "remove_instances",
                ActionInfo::Payload::Count },
            { ScriptedChangeAction::AddLights, "add_lights",
                ActionInfo::Payload::Count },
            { ScriptedChangeAction::RemoveLights, "remove_lights",
                ActionInfo::Payload::Count },
            { ScriptedChangeAction::AddMaterials, "add_materials",
                ActionInfo::Payload::Count },
            { ScriptedChangeAction::AddEnvironmentProbes,
                "add_environment_probes", ActionInfo::Payload::Count },
            { ScriptedChangeAction::RemoveEnvironmentProbes,
                "remove_environment_probes", ActionInfo::Payload::Count },
            { ScriptedChangeAction::RemoveCaptureProbe, "remove_capture_probe",
                ActionInfo::Payload::None },
            { ScriptedChangeAction::AddCaptureProbe, "add_capture_probe",
                ActionInfo::Payload::None },
            { ScriptedChangeAction::SetCaptureProbeResolution,
                "set_capture_probe_resolution", ActionInfo::Payload::Resolution },
            { ScriptedChangeAction::PublishConstantEnvironment,
                "publish_constant_environment", ActionInfo::Payload::Color },
        } };

        // Bounds keep a malformed scenario from asking for absurd work.
        constexpr uint64_t kMaximumCount = 65'536;
        constexpr uint64_t kMaximumResolution = 4'096;

        [[noreturn]] void fail(const std::string& message) {
            throw std::invalid_argument("scripted changes: " + message);
        }

        uint64_t requireUnsigned(const Json& object, const char* key,
            const std::string& where) {
            const auto found = object.find(key);
            if (found == object.end())
                fail(where + " requires \"" + key + "\"");
            if (!found->is_number_unsigned())
                fail(where + " \"" + key + "\" must be an unsigned integer");
            return found->get<uint64_t>();
        }

        ScriptedChangeEvent parseEvent(const Json& value, size_t index) {
            const std::string where = "event " + std::to_string(index);
            if (!value.is_object()) fail(where + " must be an object");
            const auto actionValue = value.find("action");
            if (actionValue == value.end() || !actionValue->is_string())
                fail(where + " requires a string \"action\"");
            const std::string actionName = actionValue->get<std::string>();
            const ActionInfo* info = nullptr;
            for (const ActionInfo& candidate : kActions)
                if (candidate.name == actionName) info = &candidate;
            if (info == nullptr)
                fail(where + " has unknown action \"" + actionName + "\"");

            std::set<std::string> allowed{ "frame", "action" };
            ScriptedChangeEvent event{};
            event.action = info->action;
            event.measuredFrame = requireUnsigned(value, "frame", where);
            switch (info->payload) {
            case ActionInfo::Payload::None:
                break;
            case ActionInfo::Payload::Count: {
                allowed.insert("count");
                const uint64_t count = requireUnsigned(value, "count", where);
                if (count == 0 || count > kMaximumCount)
                    fail(where + " \"count\" must be 1.." +
                        std::to_string(kMaximumCount));
                event.count = static_cast<uint32_t>(count);
                break;
            }
            case ActionInfo::Payload::Resolution: {
                allowed.insert("resolution");
                const uint64_t resolution =
                    requireUnsigned(value, "resolution", where);
                if (resolution < 16 || resolution > kMaximumResolution ||
                    (resolution & (resolution - 1u)) != 0u)
                    fail(where + " \"resolution\" must be a power of two in 16.." +
                        std::to_string(kMaximumResolution));
                event.count = static_cast<uint32_t>(resolution);
                break;
            }
            case ActionInfo::Payload::Color: {
                allowed.insert("color");
                const auto color = value.find("color");
                if (color == value.end() || !color->is_array() ||
                    color->size() != 3u)
                    fail(where + " requires \"color\" as three numbers");
                for (size_t channel = 0; channel < 3u; ++channel) {
                    const Json& component = (*color)[channel];
                    if (!component.is_number())
                        fail(where + " \"color\" must contain numbers");
                    const double v = component.get<double>();
                    if (!std::isfinite(v) || v < 0.0 || v > 1'000.0)
                        fail(where + " \"color\" components must be 0..1000");
                    event.color[channel] = static_cast<float>(v);
                }
                break;
            }
            }
            for (const auto& [key, unused] : value.items()) {
                (void)unused;
                if (!allowed.contains(key))
                    fail(where + " has unknown key \"" + key + "\"");
            }
            return event;
        }

    } // namespace

    std::string_view scriptedChangeActionName(
        ScriptedChangeAction action) noexcept {
        for (const ActionInfo& info : kActions)
            if (info.action == action) return info.name;
        return "unknown";
    }

    ScriptedChangeScenario parseScriptedChangeScenario(std::string_view text) {
        Json root;
        try {
            root = Json::parse(text.begin(), text.end());
        }
        catch (const Json::parse_error& error) {
            fail(std::string("invalid JSON: ") + error.what());
        }
        if (!root.is_object()) fail("the scenario must be a JSON object");
        for (const auto& [key, unused] : root.items()) {
            (void)unused;
            if (key != "schema" && key != "id" && key != "description" &&
                key != "runner" && key != "events")
                fail("unknown top-level key \"" + key + "\"");
        }
        const auto schema = root.find("schema");
        if (schema == root.end() || !schema->is_string() ||
            schema->get<std::string>() != kScriptedChangesSchema)
            fail("\"schema\" must be \"" + std::string(kScriptedChangesSchema) +
                "\"");
        const auto id = root.find("id");
        if (id == root.end() || !id->is_string() ||
            id->get<std::string>().empty())
            fail("\"id\" must be a non-empty string");
        const auto description = root.find("description");
        if (description != root.end() && !description->is_string())
            fail("\"description\" must be a string");
        const auto runner = root.find("runner");
        if (runner != root.end() && !runner->is_object())
            fail("\"runner\" must be an object");
        const auto events = root.find("events");
        if (events == root.end() || !events->is_array() || events->empty())
            fail("\"events\" must be a non-empty array");

        ScriptedChangeScenario scenario{};
        scenario.id = id->get<std::string>();
        scenario.events.reserve(events->size());
        for (size_t index = 0; index < events->size(); ++index) {
            ScriptedChangeEvent event = parseEvent((*events)[index], index);
            if (!scenario.events.empty() &&
                event.measuredFrame < scenario.events.back().measuredFrame)
                fail("event " + std::to_string(index) +
                    " frame is earlier than the previous event's");
            scenario.events.push_back(event);
        }
        return scenario;
    }

    ScriptedChangeScenario loadScriptedChangeScenario(
        const std::filesystem::path& path) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            fail("cannot open " + path.generic_string());
        const std::string text{ std::istreambuf_iterator<char>(stream),
            std::istreambuf_iterator<char>() };
        return parseScriptedChangeScenario(text);
    }

    std::span<const ScriptedChangeEvent> ScriptedChangeSchedule::take(
        uint64_t measuredFrame) noexcept {
        const size_t first = cursor_;
        while (cursor_ < events_.size() &&
            events_[cursor_].measuredFrame <= measuredFrame)
            ++cursor_;
        return events_.subspan(first, cursor_ - first);
    }

    ScriptedFrameSample sampleScriptedFrame(
        const CpuFrameProfile& frame) noexcept {
        ScriptedFrameSample sample{};
        sample.frameId = frame.frameId;
        for (const CpuProfileEvent& event : frame.events) {
            if (event.name == nullptr) continue;
            const std::string_view name(event.name);
            if (name == "cpu.frame.total") {
                sample.cpuFrameNanoseconds += event.durationNanoseconds;
            }
            else if (name == "cpu.renderer.drain_all_frames") {
                ++sample.drainCount;
                sample.drainNanoseconds += event.durationNanoseconds;
            }
            else if (name == "cpu.renderer.upload_wait") {
                ++sample.uploadWaitCount;
                sample.uploadWaitNanoseconds += event.durationNanoseconds;
            }
        }
        return sample;
    }

    bool ScriptedFrameTimeline::observe(const CpuProfiler& profiler) {
        const CpuFrameProfile* latest = profiler.latestCompletedFrame();
        return latest != nullptr && observe(*latest);
    }

    bool ScriptedFrameTimeline::observe(const CpuFrameProfile& frame) {
        if (frame.frameId == 0 || frame.frameId == lastFrameId_) return false;
        lastFrameId_ = frame.frameId;
        samples_.push_back(sampleScriptedFrame(frame));
        return true;
    }

    std::string scriptedChangeJson(const ScriptedChangeEvent& event,
        const AppliedScriptedChange& applied) {
        Json object{
            { "index", applied.eventIndex },
            { "frame", event.measuredFrame },
            { "action", std::string(scriptedChangeActionName(event.action)) },
        };
        if (event.action == ScriptedChangeAction::SetCaptureProbeResolution)
            object["resolution"] = event.count;
        else if (event.action == ScriptedChangeAction::PublishConstantEnvironment)
            object["color"] = { event.color[0], event.color[1], event.color[2] };
        else if (event.count != 0)
            object["count"] = event.count;
        object["applied_measured_frame"] = applied.measuredFrame;
        object["application_frame"] = applied.applicationFrame;
        object["apply_ns"] = applied.applyNanoseconds;
        object["result"] = applied.result;
        return object.dump();
    }

    void writeScriptedChangeJsonLines(std::ostream& output,
        const ScriptedChangeScenario& scenario, std::string_view scenarioPath,
        uint64_t warmupFrames, std::span<const AppliedScriptedChange> applied,
        std::span<const ScriptedFrameSample> samples) {
        Json events = Json::array();
        for (const AppliedScriptedChange& record : applied) {
            if (record.eventIndex >= scenario.events.size()) continue;
            events.push_back(Json::parse(scriptedChangeJson(
                scenario.events[record.eventIndex], record)));
        }
        const Json header{
            { "type", "scripted_changes" },
            { "schema", std::string(kScriptedChangesSchema) },
            { "scenario_id", scenario.id },
            { "scenario_path", std::string(scenarioPath) },
            { "warmup_frames", warmupFrames },
            { "scheduled_events", scenario.events.size() },
            { "applied_events", events },
            { "frame_samples", samples.size() },
        };
        output << header.dump() << '\n';
        // Compact per-frame records: m measured frame, cpu cpu.frame.total ns,
        // drain/upload counts and summed ns within the frame.
        for (const ScriptedFrameSample& sample : samples) {
            const int64_t measured = static_cast<int64_t>(sample.frameId) - 1 -
                static_cast<int64_t>(warmupFrames);
            output << "{\"type\":\"scripted_frame\",\"m\":" << measured
                << ",\"cpu_ns\":" << sample.cpuFrameNanoseconds
                << ",\"drain\":" << sample.drainCount
                << ",\"drain_ns\":" << sample.drainNanoseconds
                << ",\"upload_wait\":" << sample.uploadWaitCount
                << ",\"upload_wait_ns\":" << sample.uploadWaitNanoseconds
                << "}\n";
        }
    }

} // namespace Iridium
