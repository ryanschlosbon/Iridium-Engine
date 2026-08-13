#include "editor/EditorSceneCommandService.h"
#include "editor/EditorSceneDocumentService.h"
#include "editor/EditorSceneHierarchy.h"
#include "editor/EditorSelectionState.h"
#include "editor/EditorTransactionService.h"
#include "profiling/CpuAllocationProfile.h"
#include "scene/Components.h"
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
#include <string_view>
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
        std::vector<double> nanosecondsPerEntity;
        std::vector<double> allocationCount;
        std::vector<double> allocationBytes;

        void add(Clock::duration duration,
            Iridium::CpuAllocationFrameSample allocations,
            size_t entityCount) {
            const double elapsed = static_cast<double>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    duration).count());
            nanoseconds.push_back(elapsed);
            nanosecondsPerEntity.push_back(
                elapsed / static_cast<double>(entityCount));
            allocationCount.push_back(
                static_cast<double>(allocations.allocationCount));
            allocationBytes.push_back(
                static_cast<double>(allocations.requestedBytes));
        }

        [[nodiscard]] json report() const {
            return {
                { "nanoseconds", statistics(nanoseconds) },
                { "nanosecondsPerEntity", statistics(nanosecondsPerEntity) },
                { "allocationCount", statistics(allocationCount) },
                { "allocationBytes", statistics(allocationBytes) },
            };
        }
    };

    [[nodiscard]] std::vector<Entity> populateHierarchy(
        Registry& registry, size_t entityCount, bool depthChain) {
        std::vector<Entity> entities;
        entities.reserve(entityCount);
        for (size_t index = 0; index < entityCount; ++index) {
            const Entity entity = registry.createEntity();
            RelationshipComponent& relationship =
                registry.addComponent<RelationshipComponent>(entity);
            relationship.parent = index == 0
                ? NULL_ENTITY
                : (depthChain ? entities.back() : entities.front());
            relationship.siblingOrder = depthChain
                ? 0 : static_cast<int>(index);
            entities.push_back(entity);
        }
        return entities;
    }

    [[nodiscard]] json runHierarchyWorkload(
        size_t entityCount, bool depthChain, bool collect) {
        Registry registry;
        const std::vector<Entity> entities = populateHierarchy(
            registry, entityCount, depthChain);
        Measurements measurements;
        std::vector<Entity> subtree;
        for (uint32_t sample = 0; sample < kSamples; ++sample) {
            Iridium::beginCpuAllocationFrame();
            const auto start = Clock::now();
            const Iridium::EditorHierarchyResult result = collect
                ? Iridium::collectEditorSceneSubtree(
                    registry, entities.front(), subtree)
                : Iridium::rebuildEditorSceneHierarchy(registry);
            const auto end = Clock::now();
            const auto allocations = Iridium::endCpuAllocationFrame();
            if (!result) throw std::runtime_error(result.diagnostic);
            if (collect && subtree.size() != entityCount) {
                throw std::runtime_error("Hierarchy traversal missed entities");
            }
            measurements.add(end - start, allocations, entityCount);
        }
        return {
            { "name", collect ? "collect_subtree" : "rebuild_hierarchy" },
            { "topology", depthChain ? "depth_chain" : "breadth_children" },
            { "entityCount", entityCount },
            { "samples", kSamples },
            { "measurement", measurements.report() },
        };
    }

    [[nodiscard]] std::vector<Entity> populateStructuralScene(
        Iridium::SceneWorld& world, size_t entityCount) {
        std::vector<Entity> entities;
        entities.reserve(entityCount);
        for (size_t index = 0; index < entityCount; ++index) {
            const Entity entity = world.createEntity();
            world.registry().addComponent<NameComponent>(entity).name =
                "Benchmark Entity";
            world.registry().addComponent<TransformComponent>(entity);
            RelationshipComponent& relationship =
                world.registry().addComponent<RelationshipComponent>(entity);
            relationship.parent = index == 0 ? NULL_ENTITY : entities.back();
            relationship.siblingOrder = 0;
            entities.push_back(entity);
        }
        const Iridium::EditorHierarchyResult hierarchy =
            Iridium::rebuildEditorSceneHierarchy(world.registry());
        if (!hierarchy) throw std::runtime_error(hierarchy.diagnostic);
        return entities;
    }

    [[nodiscard]] json runStructuralWorkload(
        size_t entityCount, bool duplicate) {
        Measurements execute;
        Measurements undo;
        Measurements redo;
        std::vector<double> historyBytes;
        for (uint32_t sample = 0; sample < kSamples; ++sample) {
            Iridium::SceneWorld world;
            Iridium::EditorSceneDocumentService document(world);
            Iridium::EditorTransactionService history(document);
            Iridium::EditorSelectionState selection;
            Iridium::EditorSceneCommandService commands(
                document, history, selection);
            const std::vector<Entity> entities = populateStructuralScene(
                world, entityCount);

            Iridium::beginCpuAllocationFrame();
            const auto executeStart = Clock::now();
            const bool executed = duplicate
                ? commands.duplicateEntity(entities.front()) != NULL_ENTITY
                : commands.deleteEntity(entities.front());
            const auto executeEnd = Clock::now();
            const auto executeAllocations = Iridium::endCpuAllocationFrame();
            if (!executed) throw std::runtime_error(commands.diagnostic());
            execute.add(executeEnd - executeStart,
                executeAllocations, entityCount);
            historyBytes.push_back(static_cast<double>(
                history.estimatedHistoryBytes()));

            Iridium::beginCpuAllocationFrame();
            const auto undoStart = Clock::now();
            const auto undone = history.undo();
            const auto undoEnd = Clock::now();
            const auto undoAllocations = Iridium::endCpuAllocationFrame();
            if (!undone) throw std::runtime_error(undone.diagnostic);
            undo.add(undoEnd - undoStart, undoAllocations, entityCount);

            Iridium::beginCpuAllocationFrame();
            const auto redoStart = Clock::now();
            const auto redone = history.redo();
            const auto redoEnd = Clock::now();
            const auto redoAllocations = Iridium::endCpuAllocationFrame();
            if (!redone) throw std::runtime_error(redone.diagnostic);
            redo.add(redoEnd - redoStart, redoAllocations, entityCount);
            const size_t expected = duplicate ? entityCount * 2 : 0;
            if (world.registry().aliveCount() != expected) {
                throw std::runtime_error("Structural command result was not exact");
            }
        }
        return {
            { "name", duplicate ? "duplicate_subtree" : "delete_subtree" },
            { "entityCount", entityCount },
            { "samples", kSamples },
            { "execute", execute.report() },
            { "undo", undo.report() },
            { "redo", redo.report() },
            { "estimatedHistoryBytes", statistics(historyBytes) },
        };
    }

} // namespace

int main(int argc, char** argv) {
    try {
        json report{
            { "schema", "iridium.editor.structural-benchmark.v1" },
            { "configuration", "Release" },
            { "allocationScope", "C++ global new/new[] requested bytes" },
            { "workloads", json::array() },
        };
        for (const size_t count : { size_t{100}, size_t{10'000}, size_t{100'000} }) {
            report["workloads"].push_back(
                runHierarchyWorkload(count, false, false));
            report["workloads"].push_back(
                runHierarchyWorkload(count, true, true));
        }
        for (const size_t count : { size_t{1}, size_t{100}, size_t{10'000} }) {
            report["workloads"].push_back(
                runStructuralWorkload(count, false));
            report["workloads"].push_back(
                runStructuralWorkload(count, true));
        }

        const std::string bytes = report.dump(2) + "\n";
        if (argc > 1) {
            const std::filesystem::path path(argv[1]);
            if (!path.parent_path().empty()) {
                std::filesystem::create_directories(path.parent_path());
            }
            std::ofstream output(path, std::ios::binary);
            output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (!output) throw std::runtime_error(
                "Could not write structural benchmark output");
        }
        std::cout << bytes;
        return 0;
    }
    catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
