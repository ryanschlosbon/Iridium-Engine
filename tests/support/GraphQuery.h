#pragma once

// Small read-only queries over a compiled render graph, used by topology tests.

#include "renderer/graph/RenderGraph.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Iridium::Test {

    class GraphQuery {
    public:
        explicit GraphQuery(const RenderGraph::CompiledGraph& graph) noexcept
            : graph_(graph) {}

        [[nodiscard]] const RenderGraph::CompiledGraph& graph() const noexcept {
            return graph_;
        }

        // Execution-order index of the named pass.
        [[nodiscard]] std::optional<uint32_t> passOrder(std::string_view name) const noexcept {
            const auto& passes = graph_.passes();
            for (uint32_t index = 0; index < passes.size(); ++index)
                if (passes[index].name == name) return index;
            return std::nullopt;
        }
        [[nodiscard]] bool hasPass(std::string_view name) const noexcept {
            return passOrder(name).has_value();
        }
        [[nodiscard]] const RenderGraph::CompiledPass* pass(std::string_view name) const noexcept {
            const auto order = passOrder(name);
            return order ? &graph_.passes()[*order] : nullptr;
        }

        [[nodiscard]] std::optional<uint32_t> resourceIndex(std::string_view name) const noexcept {
            for (const RenderGraph::CompiledResource& resource : graph_.resources())
                if (resource.name == name) return resource.logicalResourceIndex;
            return std::nullopt;
        }
        [[nodiscard]] const RenderGraph::CompiledResource* resource(std::string_view name) const noexcept {
            for (const RenderGraph::CompiledResource& value : graph_.resources())
                if (value.name == name) return &value;
            return nullptr;
        }
        [[nodiscard]] const RenderGraph::CompiledResource* resourceAt(uint32_t logicalIndex) const noexcept {
            for (const RenderGraph::CompiledResource& value : graph_.resources())
                if (value.logicalResourceIndex == logicalIndex) return &value;
            return nullptr;
        }

        [[nodiscard]] std::span<const RenderGraph::CompiledUsage> usages(
            const RenderGraph::CompiledPass& pass) const noexcept {
            return std::span(graph_.usages()).subspan(pass.firstUsage, pass.usageCount);
        }
        [[nodiscard]] std::span<const RenderGraph::CompiledUsage> usages(
            std::string_view passName) const noexcept {
            const RenderGraph::CompiledPass* value = pass(passName);
            return value ? usages(*value) : std::span<const RenderGraph::CompiledUsage>{};
        }

        // Usages by `passName` of `resourceName` (may be several, e.g. read+write).
        [[nodiscard]] std::vector<RenderGraph::CompiledUsage> usagesOf(
            std::string_view passName, std::string_view resourceName) const {
            std::vector<RenderGraph::CompiledUsage> result;
            const auto index = resourceIndex(resourceName);
            if (!index) return result;
            for (const RenderGraph::CompiledUsage& usage : usages(passName))
                if (usage.logicalResourceIndex == *index) result.push_back(usage);
            return result;
        }

        [[nodiscard]] bool reads(std::string_view passName, std::string_view resourceName,
            RenderGraph::Access access) const {
            for (const auto& usage : usagesOf(passName, resourceName))
                if (!usage.write && usage.access == access) return true;
            return false;
        }
        [[nodiscard]] bool writes(std::string_view passName, std::string_view resourceName,
            RenderGraph::Access access, std::optional<RenderGraph::LoadOp> load = std::nullopt) const {
            for (const auto& usage : usagesOf(passName, resourceName))
                if (usage.write && usage.access == access &&
                    (!load || usage.loadOp == *load)) return true;
            return false;
        }

        // Names of passes that write the resource, in execution order.
        [[nodiscard]] std::vector<std::string_view> writers(std::string_view resourceName) const {
            std::vector<std::string_view> result;
            const auto index = resourceIndex(resourceName);
            if (!index) return result;
            for (const RenderGraph::CompiledPass& value : graph_.passes())
                for (const auto& usage : usages(value))
                    if (usage.logicalResourceIndex == *index && usage.write) {
                        result.push_back(value.name);
                        break;
                    }
            return result;
        }

        // Names of passes that use the resource at all, in execution order.
        [[nodiscard]] std::vector<std::string_view> users(std::string_view resourceName) const {
            std::vector<std::string_view> result;
            const auto index = resourceIndex(resourceName);
            if (!index) return result;
            for (const RenderGraph::CompiledPass& value : graph_.passes())
                for (const auto& usage : usages(value))
                    if (usage.logicalResourceIndex == *index) {
                        result.push_back(value.name);
                        break;
                    }
            return result;
        }

        // True when every listed pass exists and they execute in this order.
        [[nodiscard]] bool ordered(std::initializer_list<std::string_view> names) const noexcept {
            std::optional<uint32_t> previous;
            for (const std::string_view name : names) {
                const auto order = passOrder(name);
                if (!order || (previous && *order <= *previous)) return false;
                previous = order;
            }
            return true;
        }

    private:
        const RenderGraph::CompiledGraph& graph_;
    };

} // namespace Iridium::Test
