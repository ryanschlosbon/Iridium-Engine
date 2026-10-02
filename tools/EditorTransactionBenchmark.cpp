#include "editor/EditorTransactionService.h"
#include "profiling/CpuAllocationProfile.h"
#include "scene/SceneWorld.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

    using Clock = std::chrono::steady_clock;
    using json = nlohmann::json;
    constexpr uint32_t kSamples = 30;

    [[nodiscard]] double percentile(
        const std::vector<double>& sorted, double fraction) {
        const size_t index = static_cast<size_t>(std::ceil(
            fraction * static_cast<double>(sorted.size() - 1)));
        return sorted[index];
    }

    [[nodiscard]] json statistics(std::vector<double> values) {
        std::ranges::sort(values);
        return {
            { "min", values.front() },
            { "median", percentile(values, 0.50) },
            { "p95", percentile(values, 0.95) },
            { "p99", percentile(values, 0.99) },
            { "max", values.back() },
        };
    }

    struct Measurements {
        std::vector<double> nanoseconds;
        std::vector<double> nanosecondsPerTarget;
        std::vector<double> allocationCount;
        std::vector<double> allocationBytes;

        void add(Clock::duration duration,
            Iridium::CpuAllocationFrameSample allocations,
            size_t targetCount) {
            const double ns = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    duration).count());
            nanoseconds.push_back(ns);
            nanosecondsPerTarget.push_back(ns /
                static_cast<double>(targetCount));
            allocationCount.push_back(
                static_cast<double>(allocations.allocationCount));
            allocationBytes.push_back(
                static_cast<double>(allocations.requestedBytes));
        }

        [[nodiscard]] json report() const {
            return {
                { "nanoseconds", statistics(nanoseconds) },
                { "nanosecondsPerTarget", statistics(nanosecondsPerTarget) },
                { "allocationCount", statistics(allocationCount) },
                { "allocationBytes", statistics(allocationBytes) },
            };
        }
    };

    [[nodiscard]] json runWorkload(size_t targetCount) {
        Measurements apply;
        Measurements undo;
        Measurements redo;
        std::vector<double> historyBytes;
        for (uint32_t sample = 0; sample < kSamples; ++sample) {
            Iridium::SceneWorld world;
            Iridium::EditorSceneDocumentService document(world);
            Iridium::EditorTransactionService history(document);
            std::vector<int> values(targetCount, 0);
            Iridium::EditorTransaction transaction;
            transaction.label = "Benchmark multi-edit";
            transaction.operations.reserve(targetCount);
            for (size_t index = 0; index < targetCount; ++index) {
                transaction.operations.push_back(
                    Iridium::makeEditorValueOperation<int>(
                        "benchmark/value", [&values, index] {
                            return &values[index];
                        }, 0, 1));
            }

            Iridium::beginCpuAllocationFrame();
            const auto applyStart = Clock::now();
            const auto applied = history.execute(std::move(transaction));
            const auto applyEnd = Clock::now();
            const auto applyAllocations = Iridium::endCpuAllocationFrame();
            if (!applied) throw std::runtime_error(applied.diagnostic);
            apply.add(applyEnd - applyStart, applyAllocations, targetCount);
            historyBytes.push_back(
                static_cast<double>(history.estimatedHistoryBytes()));

            Iridium::beginCpuAllocationFrame();
            const auto undoStart = Clock::now();
            const auto undone = history.undo();
            const auto undoEnd = Clock::now();
            const auto undoAllocations = Iridium::endCpuAllocationFrame();
            if (!undone) throw std::runtime_error(undone.diagnostic);
            undo.add(undoEnd - undoStart, undoAllocations, targetCount);

            Iridium::beginCpuAllocationFrame();
            const auto redoStart = Clock::now();
            const auto redone = history.redo();
            const auto redoEnd = Clock::now();
            const auto redoAllocations = Iridium::endCpuAllocationFrame();
            if (!redone) throw std::runtime_error(redone.diagnostic);
            redo.add(redoEnd - redoStart, redoAllocations, targetCount);
            if (!std::ranges::all_of(values,
                    [](int value) { return value == 1; })) {
                throw std::runtime_error("Benchmark transaction was not exact");
            }
        }
        return {
            { "targetCount", targetCount },
            { "samples", kSamples },
            { "apply", apply.report() },
            { "undo", undo.report() },
            { "redo", redo.report() },
            { "estimatedHistoryBytes", statistics(historyBytes) },
            { "estimatedHistoryBytesPerTarget",
                statistics([&] {
                    std::vector<double> result = historyBytes;
                    for (double& value : result) value /= targetCount;
                    return result;
                }()) },
        };
    }

} // namespace

int main(int argc, char** argv) {
    try {
        json report{
            { "schema", "iridium.editor.transaction-benchmark.v1" },
            { "configuration", "Release" },
            { "allocationScope", "C++ global new/new[] requested bytes" },
            { "workloads", json::array() },
        };
        for (const size_t count : { size_t{1}, size_t{100}, size_t{10'000} }) {
            report["workloads"].push_back(runWorkload(count));
        }
        const std::string bytes = report.dump(2) + "\n";
        if (argc > 1) {
            const std::filesystem::path path(argv[1]);
            std::filesystem::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!output) throw std::runtime_error("Could not write benchmark output");
        }
        std::cout << bytes;
        return 0;
    }
    catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
