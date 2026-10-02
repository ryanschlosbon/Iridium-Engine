#include "assets/model/ModelLodGenerator.h"
#include "utils/Sha256.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>

namespace Iridium {

    namespace {

        void checkCancellation(std::stop_token token) {
            if (token.stop_requested())
                throw std::runtime_error("Model LOD generation was cancelled.");
        }

        struct Edge {
            uint32_t first = 0;
            uint32_t second = 0;
            float lengthSquared = 0.0f;
            bool boundary = false;
        };

        float squaredDistance(const std::array<float, 3>& lhs,
            const std::array<float, 3>& rhs) noexcept {
            float result = 0.0f;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                const float delta = lhs[axis] - rhs[axis];
                result += delta * delta;
            }
            return result;
        }

        float vectorLengthSquared(const std::array<float, 3>& value) noexcept {
            return value[0] * value[0] + value[1] * value[1] +
                value[2] * value[2];
        }

        float directionCosine(const std::array<float, 3>& lhs,
            const std::array<float, 3>& rhs) noexcept {
            double lhsLength = 0.0;
            double rhsLength = 0.0;
            double dot = 0.0;
            for (uint32_t axis = 0; axis < 3u; ++axis) {
                lhsLength += static_cast<double>(lhs[axis]) * lhs[axis];
                rhsLength += static_cast<double>(rhs[axis]) * rhs[axis];
                dot += static_cast<double>(lhs[axis]) * rhs[axis];
            }
            if (!(lhsLength > 1.0e-12) || !(rhsLength > 1.0e-12))
                return lhsLength <= 1.0e-12 && rhsLength <= 1.0e-12
                    ? 1.0f : -1.0f;
            return static_cast<float>(dot / std::sqrt(lhsLength * rhsLength));
        }

        float conservativeDistance(const std::array<float, 3>& lhs,
            const std::array<float, 3>& rhs) noexcept {
            double squared = 0.0;
            for (uint32_t axis = 0; axis < 3u; ++axis) {
                const double delta = static_cast<double>(lhs[axis]) - rhs[axis];
                squared += delta * delta;
            }
            const double exact = std::sqrt(squared);
            const float rounded = static_cast<float>(exact);
            return static_cast<double>(rounded) < exact
                ? std::nextafter(rounded,
                    std::numeric_limits<float>::infinity()) : rounded;
        }

        bool compatibleAttributes(const CookedModelVertex& lhs,
            const CookedModelVertex& rhs,
            const ModelLodGenerationSettings& settings) noexcept {
            if (directionCosine(lhs.normal, rhs.normal) <
                    settings.minimumNormalCosine)
                return false;
            const std::array<float, 3> lhsTangent{
                lhs.tangent[0], lhs.tangent[1], lhs.tangent[2] };
            const std::array<float, 3> rhsTangent{
                rhs.tangent[0], rhs.tangent[1], rhs.tangent[2] };
            if (directionCosine(lhsTangent, rhsTangent) <
                    settings.minimumNormalCosine ||
                std::signbit(lhs.tangent[3]) != std::signbit(rhs.tangent[3]))
                return false;
            const auto texCoordCompatible = [&](const auto& a, const auto& b) {
                const float x = a[0] - b[0];
                const float y = a[1] - b[1];
                return x * x + y * y <= settings.maximumTexCoordDelta *
                    settings.maximumTexCoordDelta;
            };
            if (!texCoordCompatible(lhs.texCoord0, rhs.texCoord0) ||
                !texCoordCompatible(lhs.texCoord1, rhs.texCoord1))
                return false;
            for (uint32_t channel = 0; channel < 4; ++channel) {
                if (std::abs(lhs.color[channel] - rhs.color[channel]) >
                        settings.maximumColorDelta)
                    return false;
            }
            return true;
        }

        void accumulateStatistics(ModelLodGenerationStatistics& target,
            const ModelLodGenerationStatistics& source) noexcept {
#define IRIDIUM_ACCUMULATE_LOD_STAT(field) target.field += source.field
            IRIDIUM_ACCUMULATE_LOD_STAT(canonicalPrimitiveCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(canonicalTriangleCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(skippedNonOpaquePrimitiveCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(skippedNonTrianglePrimitiveCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(skippedExistingChainPrimitiveCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(attemptedPrimitiveCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(attemptedTriangleCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(belowMinimumPrimitiveCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(belowMinimumTriangleCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(invalidExtentPrimitiveCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(invalidExtentTriangleCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(degenerateSourcePrimitiveCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(degenerateSourceTriangleCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(invalidSourceTopologyPrimitiveCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(invalidSourceTopologyTriangleCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(splitBoundaryFanVertexCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(duplicatedBoundaryFanVertexCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(noAcceptedLevelPrimitiveCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(noAcceptedLevelTriangleCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(generatedChainCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(generatedBaseTriangleCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(generatedLevelCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(edgeConsiderationCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(boundaryRejectedEdgeCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(attributeRejectedEdgeCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(budgetRejectedEdgeCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(acceptedMergeCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(orientationRejectedEdgeCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(correspondenceRejectedEdgeCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(topologyRejectedEdgeCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(orientationRejectedLevelCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(noReductionLevelCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(belowMinimumReductionLevelCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(correspondenceRejectedLevelCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(topologyRejectedLevelCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(reducedTopologyInvalidLevelCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(topologySignatureMismatchLevelCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(topologyComponentMismatchLevelCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(topologyEulerMismatchLevelCount);
            IRIDIUM_ACCUMULATE_LOD_STAT(topologyBoundaryMismatchLevelCount);
#undef IRIDIUM_ACCUMULATE_LOD_STAT
            for (size_t budget = 0; budget < ModelLodBudgetCount; ++budget) {
                for (size_t outcome = 0;
                        outcome < ModelLodLevelOutcomeCount; ++outcome) {
                    target.levelOutcomeCounts[budget][outcome] +=
                        source.levelOutcomeCounts[budget][outcome];
                    target.levelOutcomeTriangleCounts[budget][outcome] +=
                        source.levelOutcomeTriangleCounts[budget][outcome];
                }
            }
            for (size_t outcome = 0;
                    outcome < ModelLodLevelOutcomeCount; ++outcome) {
                target.noAcceptedTerminalOutcomePrimitiveCounts[outcome] +=
                    source.noAcceptedTerminalOutcomePrimitiveCounts[outcome];
                target.noAcceptedTerminalOutcomeTriangleCounts[outcome] +=
                    source.noAcceptedTerminalOutcomeTriangleCounts[outcome];
            }
            for (size_t index = 0;
                    index < ModelLodTopologyFailureCount; ++index) {
                target.sourceTopologyFailurePrimitiveCounts[index] +=
                    source.sourceTopologyFailurePrimitiveCounts[index];
                target.sourceTopologyFailureTriangleCounts[index] +=
                    source.sourceTopologyFailureTriangleCounts[index];
                target.reducedTopologyFailureLevelCounts[index] +=
                    source.reducedTopologyFailureLevelCounts[index];
            }
        }

        std::array<uint32_t, 3> stableTriangleKey(
            uint32_t a, uint32_t b, uint32_t c) noexcept {
            return (std::min)({ std::array{ a, b, c },
                std::array{ b, c, a }, std::array{ c, a, b } });
        }

        std::array<float, 3> triangleNormal(
            const CookedModelVertex& a, const CookedModelVertex& b,
            const CookedModelVertex& c) noexcept {
            const std::array<float, 3> ab{
                b.position[0] - a.position[0],
                b.position[1] - a.position[1],
                b.position[2] - a.position[2] };
            const std::array<float, 3> ac{
                c.position[0] - a.position[0],
                c.position[1] - a.position[1],
                c.position[2] - a.position[2] };
            return {
                ab[1] * ac[2] - ab[2] * ac[1],
                ab[2] * ac[0] - ab[0] * ac[2],
                ab[0] * ac[1] - ab[1] * ac[0],
            };
        }

        uint32_t componentRoot(std::vector<uint32_t>& parents,
            uint32_t value) {
            while (parents[value] != value) {
                parents[value] = parents[parents[value]];
                value = parents[value];
            }
            return value;
        }

        void joinComponents(std::vector<uint32_t>& parents,
            uint32_t a, uint32_t b) {
            a = componentRoot(parents, a);
            b = componentRoot(parents, b);
            if (a != b) parents[(std::max)(a, b)] = (std::min)(a, b);
        }

        struct BoundaryFanSplitResult {
            std::vector<CookedModelVertex> vertices;
            std::vector<uint32_t> indices;
            uint64_t splitVertexCount = 0;
            uint64_t duplicatedVertexCount = 0;
        };

        BoundaryFanSplitResult splitBoundaryFans(
            std::span<const CookedModelVertex> vertices,
            std::span<const uint32_t> indices,
            std::stop_token stopToken) {
            BoundaryFanSplitResult result{
                .vertices = { vertices.begin(), vertices.end() },
                .indices = { indices.begin(), indices.end() },
            };
            const size_t triangleCount = indices.size() / 3u;
            std::vector<std::vector<uint32_t>> incident(vertices.size());
            for (uint32_t triangle = 0; triangle < triangleCount; ++triangle) {
                if (triangle % 1024u == 0u) checkCancellation(stopToken);
                for (uint32_t corner = 0; corner < 3u; ++corner)
                    incident[indices[triangle * 3u + corner]].push_back(triangle);
            }
            for (uint32_t vertex = 0; vertex < incident.size(); ++vertex) {
                checkCancellation(stopToken);
                const std::vector<uint32_t>& triangles = incident[vertex];
                if (triangles.size() < 2u) continue;
                std::vector<uint32_t> parents(triangles.size());
                std::iota(parents.begin(), parents.end(), 0u);
                std::map<uint32_t, uint32_t> firstTriangleByNeighbor;
                for (uint32_t local = 0; local < triangles.size(); ++local) {
                    const uint32_t triangle = triangles[local];
                    const size_t base = static_cast<size_t>(triangle) * 3u;
                    uint32_t corner = 0;
                    while (corner < 3u && indices[base + corner] != vertex)
                        ++corner;
                    if (corner == 3u) continue;
                    for (uint32_t offset : { 1u, 2u }) {
                        const uint32_t neighbor =
                            indices[base + (corner + offset) % 3u];
                        const auto [found, inserted] =
                            firstTriangleByNeighbor.emplace(neighbor, local);
                        if (!inserted)
                            joinComponents(parents, local, found->second);
                    }
                }
                std::map<uint32_t, std::vector<uint32_t>> fans;
                for (uint32_t local = 0; local < triangles.size(); ++local)
                    fans[componentRoot(parents, local)].push_back(triangles[local]);
                if (fans.size() < 2u) continue;
                ++result.splitVertexCount;
                bool firstFan = true;
                for (const auto& [fan, fanTriangles] : fans) {
                    (void)fan;
                    if (firstFan) {
                        firstFan = false;
                        continue;
                    }
                    if (result.vertices.size() >=
                            std::numeric_limits<uint32_t>::max())
                        throw std::length_error(
                            "Boundary-fan normalization exceeds vertex limits.");
                    const uint32_t duplicate =
                        static_cast<uint32_t>(result.vertices.size());
                    result.vertices.push_back(vertices[vertex]);
                    ++result.duplicatedVertexCount;
                    for (uint32_t triangle : fanTriangles) {
                        const size_t base = static_cast<size_t>(triangle) * 3u;
                        for (uint32_t corner = 0; corner < 3u; ++corner) {
                            if (result.indices[base + corner] == vertex) {
                                result.indices[base + corner] = duplicate;
                                break;
                            }
                        }
                    }
                }
            }
            return result;
        }

        struct TopologyComponent {
            int64_t eulerCharacteristic = 0;
            size_t boundaryLoops = 0;

            bool operator==(const TopologyComponent&) const = default;
        };

        using TopologySignature = std::map<uint32_t, TopologyComponent>;

        std::optional<TopologySignature> topologySignature(
            std::span<const uint32_t> indices,
            std::span<const uint32_t> sourceComponents,
            std::stop_token stopToken,
            ModelLodTopologyFailure* failure = nullptr) {
            const auto reject = [failure](ModelLodTopologyFailure reason) {
                if (failure) *failure = reason;
                return std::optional<TopologySignature>{};
            };
            struct EdgeUse {
                uint32_t count = 0;
                int32_t orientation = 0;
            };
            const size_t vertexCount = sourceComponents.size();
            std::map<std::pair<uint32_t, uint32_t>, EdgeUse> edges;
            std::vector<std::vector<std::pair<uint32_t, uint32_t>>>
                links(vertexCount);
            std::vector<uint32_t> components(vertexCount);
            std::iota(components.begin(), components.end(), 0u);
            std::vector<uint32_t> boundaries = components;
            std::vector<uint32_t> boundaryDegree(vertexCount, 0u);
            std::set<uint32_t> usedVertices;
            TopologySignature signature;
            for (size_t item = 0; item < indices.size(); item += 3u) {
                if (item % 3072u == 0u) checkCancellation(stopToken);
                const std::array<uint32_t, 3> triangle{
                    indices[item], indices[item + 1u], indices[item + 2u] };
                if (triangle[0] == triangle[1] || triangle[1] == triangle[2] ||
                    triangle[2] == triangle[0] ||
                    sourceComponents[triangle[0]] != sourceComponents[triangle[1]] ||
                    sourceComponents[triangle[0]] != sourceComponents[triangle[2]])
                    return reject(ModelLodTopologyFailure::MixedComponentTriangle);
                ++signature[sourceComponents[triangle[0]]].eulerCharacteristic;
                for (uint32_t corner = 0; corner < 3u; ++corner) {
                    const uint32_t a = triangle[corner];
                    const uint32_t b = triangle[(corner + 1u) % 3u];
                    const uint32_t c = triangle[(corner + 2u) % 3u];
                    usedVertices.insert(a);
                    joinComponents(components, a, b);
                    links[a].emplace_back(b, c);
                    const auto endpoints = (std::minmax)(a, b);
                    EdgeUse& use = edges[{ endpoints.first, endpoints.second }];
                    ++use.count;
                    use.orientation += a < b ? 1 : -1;
                }
            }
            for (const auto& [edge, use] : edges) {
                if (use.count > 2u)
                    return reject(ModelLodTopologyFailure::NonManifoldEdge);
                if (use.count == 2u && use.orientation != 0)
                    return reject(
                        ModelLodTopologyFailure::InconsistentEdgeOrientation);
                --signature[sourceComponents[edge.first]].eulerCharacteristic;
                if (use.count == 1u) {
                    ++boundaryDegree[edge.first];
                    ++boundaryDegree[edge.second];
                    joinComponents(boundaries, edge.first, edge.second);
                }
            }
            std::map<uint32_t, std::set<uint32_t>> componentRoots;
            std::map<uint32_t, std::set<uint32_t>> boundaryRoots;
            for (uint32_t vertex : usedVertices) {
                if (vertex % 1024u == 0u) checkCancellation(stopToken);
                const uint32_t source = sourceComponents[vertex];
                ++signature[source].eulerCharacteristic;
                componentRoots[source].insert(componentRoot(components, vertex));
                if (boundaryDegree[vertex] != 0u) {
                    if (boundaryDegree[vertex] != 2u)
                        return reject(ModelLodTopologyFailure::BoundaryDegree);
                    boundaryRoots[source].insert(componentRoot(boundaries, vertex));
                }

                // The vertex link must be one cycle (interior) or one path
                // (boundary), not disconnected fans touching at a bow-tie.
                std::map<uint32_t, uint32_t> degree;
                std::map<uint32_t, std::vector<uint32_t>> adjacency;
                for (const auto& [a, b] : links[vertex]) {
                    ++degree[a];
                    ++degree[b];
                    adjacency[a].push_back(b);
                    adjacency[b].push_back(a);
                }
                size_t endpoints = 0;
                for (const auto& [neighbor, count] : degree) {
                    (void)neighbor;
                    if (count == 1u) ++endpoints;
                    else if (count != 2u)
                        return reject(ModelLodTopologyFailure::VertexLinkDegree);
                }
                if (endpoints != (boundaryDegree[vertex] == 0u ? 0u : 2u))
                    return reject(ModelLodTopologyFailure::VertexLinkDegree);
                std::vector<uint32_t> pending{ degree.begin()->first };
                std::set<uint32_t> visited;
                while (!pending.empty()) {
                    const uint32_t neighbor = pending.back();
                    pending.pop_back();
                    if (!visited.insert(neighbor).second) continue;
                    for (uint32_t adjacent : adjacency[neighbor])
                        pending.push_back(adjacent);
                }
                if (visited.size() != degree.size())
                    return reject(
                        ModelLodTopologyFailure::VertexLinkDisconnected);
            }
            for (auto& [component, value] : signature) {
                if (componentRoots[component].size() != 1u)
                    return reject(ModelLodTopologyFailure::DisconnectedComponent);
                value.boundaryLoops = boundaryRoots[component].size();
            }
            return signature;
        }

    } // namespace

    std::vector<GeneratedModelLod> generateModelLods(
        std::span<const CookedModelVertex> vertices,
        std::span<const uint32_t> localTriangleIndices,
        const ModelLodGenerationSettings& settings,
        std::stop_token stopToken,
        ModelLodGenerationStatistics* statistics) {
        ModelLodGenerationStatistics localStatistics;
        localStatistics.canonicalPrimitiveCount = 1u;
        localStatistics.canonicalTriangleCount =
            localTriangleIndices.size() / 3u;
        localStatistics.attemptedPrimitiveCount = 1u;
        localStatistics.attemptedTriangleCount =
            localTriangleIndices.size() / 3u;
        const auto publishStatistics = [&]() {
            if (statistics) *statistics = localStatistics;
        };
        checkCancellation(stopToken);
        if (vertices.size() > std::numeric_limits<uint32_t>::max() ||
            localTriangleIndices.size() % 3u != 0u ||
            std::ranges::any_of(localTriangleIndices,
                [&](uint32_t index) { return index >= vertices.size(); })) {
            throw std::invalid_argument(
                "LOD generation requires complete in-range local triangles.");
        }
        if (!std::isfinite(settings.minimumNormalCosine) ||
            settings.minimumNormalCosine < -1.0f ||
            settings.minimumNormalCosine > 1.0f ||
            !std::isfinite(settings.maximumTexCoordDelta) ||
            settings.maximumTexCoordDelta < 0.0f ||
            !std::isfinite(settings.maximumColorDelta) ||
            settings.maximumColorDelta < 0.0f)
            throw std::invalid_argument("LOD generation settings are invalid.");
        float previousBudget = 0.0f;
        for (float relativeError : settings.maximumRelativeErrors) {
            if (!std::isfinite(relativeError) || relativeError <= previousBudget)
                throw std::invalid_argument(
                    "LOD error budgets must be finite, positive, and increasing.");
            previousBudget = relativeError;
        }

        std::array<float, 3> boundsMin{
            (std::numeric_limits<float>::max)(),
            (std::numeric_limits<float>::max)(),
            (std::numeric_limits<float>::max)() };
        std::array<float, 3> boundsMax{
            (std::numeric_limits<float>::lowest)(),
            (std::numeric_limits<float>::lowest)(),
            (std::numeric_limits<float>::lowest)() };
        for (const CookedModelVertex& vertex : vertices) {
            const auto finite = [](const auto& values) {
                return std::ranges::all_of(values,
                    [](float value) { return std::isfinite(value); });
            };
            if (!finite(vertex.position) || !finite(vertex.normal) ||
                !finite(vertex.tangent) || !finite(vertex.texCoord0) ||
                !finite(vertex.texCoord1) || !finite(vertex.color))
                throw std::invalid_argument(
                    "LOD generation vertex attributes must be finite.");
            for (uint32_t axis = 0; axis < 3; ++axis) {
                if (!std::isfinite(vertex.position[axis]))
                    throw std::invalid_argument(
                        "LOD generation positions must be finite.");
                boundsMin[axis] = (std::min)(boundsMin[axis],
                    vertex.position[axis]);
                boundsMax[axis] = (std::max)(boundsMax[axis],
                    vertex.position[axis]);
            }
        }
        if (vertices.empty() || localTriangleIndices.size() / 3u <
                settings.minimumSourceTriangles) {
            ++localStatistics.belowMinimumPrimitiveCount;
            localStatistics.belowMinimumTriangleCount =
                localTriangleIndices.size() / 3u;
            publishStatistics();
            return {};
        }
        const float diagonal = std::sqrt(squaredDistance(boundsMin, boundsMax));
        if (!(diagonal > 0.0f) || !std::isfinite(diagonal)) {
            ++localStatistics.invalidExtentPrimitiveCount;
            localStatistics.invalidExtentTriangleCount =
                localTriangleIndices.size() / 3u;
            publishStatistics();
            return {};
        }

        std::vector<uint32_t> sourceComponents(vertices.size());
        std::iota(sourceComponents.begin(), sourceComponents.end(), 0u);
        for (size_t item = 0; item < localTriangleIndices.size(); item += 3u) {
            const auto normal = triangleNormal(
                vertices[localTriangleIndices[item]],
                vertices[localTriangleIndices[item + 1u]],
                vertices[localTriangleIndices[item + 2u]]);
            if (!(vectorLengthSquared(normal) > 0.0f) ||
                !std::isfinite(vectorLengthSquared(normal)))
            {
                ++localStatistics.degenerateSourcePrimitiveCount;
                localStatistics.degenerateSourceTriangleCount =
                    localTriangleIndices.size() / 3u;
                publishStatistics();
                return {};
            }
            joinComponents(sourceComponents, localTriangleIndices[item],
                localTriangleIndices[item + 1u]);
            joinComponents(sourceComponents, localTriangleIndices[item],
                localTriangleIndices[item + 2u]);
        }
        if (settings.splitNonManifoldBoundaryFans) {
            BoundaryFanSplitResult split = splitBoundaryFans(
                vertices, localTriangleIndices, stopToken);
            if (split.duplicatedVertexCount != 0u) {
                ModelLodGenerationSettings normalizedSettings = settings;
                normalizedSettings.splitNonManifoldBoundaryFans = false;
                ModelLodGenerationStatistics normalizedStatistics;
                std::vector<GeneratedModelLod> normalized = generateModelLods(
                    split.vertices, split.indices, normalizedSettings, stopToken,
                    &normalizedStatistics);
                normalizedStatistics.splitBoundaryFanVertexCount +=
                    split.splitVertexCount;
                normalizedStatistics.duplicatedBoundaryFanVertexCount +=
                    split.duplicatedVertexCount;
                if (statistics) *statistics = normalizedStatistics;
                return normalized;
            }
        }
        for (uint32_t vertex = 0; vertex < sourceComponents.size(); ++vertex)
            sourceComponents[vertex] = componentRoot(sourceComponents, vertex);
        ModelLodTopologyFailure sourceTopologyFailure =
            ModelLodTopologyFailure::MixedComponentTriangle;
        const auto sourceTopology = topologySignature(localTriangleIndices,
            sourceComponents, stopToken, &sourceTopologyFailure);
        if (!sourceTopology) {
            ++localStatistics.invalidSourceTopologyPrimitiveCount;
            localStatistics.invalidSourceTopologyTriangleCount =
                localTriangleIndices.size() / 3u;
            const size_t failureIndex =
                static_cast<size_t>(sourceTopologyFailure);
            ++localStatistics.sourceTopologyFailurePrimitiveCounts[failureIndex];
            localStatistics.sourceTopologyFailureTriangleCounts[failureIndex] =
                localTriangleIndices.size() / 3u;
            publishStatistics();
            return {};
        }

        std::map<std::pair<uint32_t, uint32_t>, uint32_t> edgeUse;
        for (size_t item = 0; item < localTriangleIndices.size(); item += 3u) {
            for (uint32_t edge = 0; edge < 3; ++edge) {
                uint32_t a = localTriangleIndices[item + edge];
                uint32_t b = localTriangleIndices[item + (edge + 1u) % 3u];
                if (a > b) std::swap(a, b);
                if (a != b) ++edgeUse[{ a, b }];
            }
        }
        std::vector<bool> boundaryVertex(vertices.size(), false);
        std::vector<Edge> edges;
        edges.reserve(edgeUse.size());
        for (const auto& [key, count] : edgeUse) {
            // The generator does not attempt to repair a nonmanifold source.
            if (count > 2u) {
                ++localStatistics.invalidSourceTopologyPrimitiveCount;
                localStatistics.invalidSourceTopologyTriangleCount =
                    localTriangleIndices.size() / 3u;
                publishStatistics();
                return {};
            }
            const bool boundary = count == 1u;
            boundaryVertex[key.first] = boundaryVertex[key.first] || boundary;
            boundaryVertex[key.second] = boundaryVertex[key.second] || boundary;
            edges.push_back({
                .first = key.first,
                .second = key.second,
                .lengthSquared = squaredDistance(
                    vertices[key.first].position,
                    vertices[key.second].position),
                .boundary = boundary,
            });
        }
        std::ranges::sort(edges, [](const Edge& lhs, const Edge& rhs) {
            if (lhs.lengthSquared != rhs.lengthSquared)
                return lhs.lengthSquared < rhs.lengthSquared;
            if (lhs.first != rhs.first) return lhs.first < rhs.first;
            return lhs.second < rhs.second;
        });

        std::vector<GeneratedModelLod> result;
        size_t previousIndexCount = localTriangleIndices.size();
        float previousError = -1.0f;
        std::array<ModelLodLevelOutcome, ModelLodBudgetCount> levelOutcomes{};
        levelOutcomes.fill(ModelLodLevelOutcome::NotAttempted);
        const auto recordLevelOutcome = [&](size_t budgetIndex,
            ModelLodLevelOutcome outcome) {
            levelOutcomes[budgetIndex] = outcome;
            const size_t outcomeIndex = static_cast<size_t>(outcome);
            ++localStatistics.levelOutcomeCounts[budgetIndex][outcomeIndex];
            localStatistics.levelOutcomeTriangleCounts[budgetIndex][outcomeIndex] +=
                localTriangleIndices.size() / 3u;
        };
        for (size_t budgetIndex = 0;
                budgetIndex < settings.maximumRelativeErrors.size(); ++budgetIndex) {
            checkCancellation(stopToken);
            const float relativeError =
                settings.maximumRelativeErrors[budgetIndex];
            const float budget = relativeError * diagonal;
            std::vector<uint32_t> parent(vertices.size());
            std::iota(parent.begin(), parent.end(), 0u);
            std::vector<float> groupError(vertices.size(), 0.0f);
            std::vector<bool> groupBoundary = boundaryVertex;
            std::vector<bool> groupBoundaryCollapsed(vertices.size(), false);
            std::vector<std::vector<uint32_t>> groupTriangles;
            std::vector<std::vector<uint32_t>> groupVertices;
            const bool transactionalMergeValidation =
                settings.preventOrientationChangingMerges ||
                settings.preventTopologyChangingMerges;
            if (transactionalMergeValidation) {
                groupTriangles.resize(vertices.size());
                groupVertices.resize(vertices.size());
                for (uint32_t vertex = 0; vertex < vertices.size(); ++vertex)
                    groupVertices[vertex].push_back(vertex);
                for (uint32_t triangle = 0;
                        triangle < localTriangleIndices.size() / 3u; ++triangle) {
                    for (uint32_t corner = 0; corner < 3u; ++corner)
                        groupTriangles[localTriangleIndices[triangle * 3u + corner]]
                            .push_back(triangle);
                }
            }
            const auto find = [&parent](uint32_t value) {
                uint32_t root = value;
                while (parent[root] != root) root = parent[root];
                while (parent[value] != value) {
                    const uint32_t next = parent[value];
                    parent[value] = root;
                    value = next;
                }
                return root;
            };
            for (const Edge& edge : edges) {
                checkCancellation(stopToken);
                ++localStatistics.edgeConsiderationCount;
                uint32_t a = find(edge.first);
                uint32_t b = find(edge.second);
                if (a == b) continue;
                const bool aBoundary = groupBoundary[a];
                const bool bBoundary = groupBoundary[b];
                if ((aBoundary || bBoundary) &&
                    (!settings.allowBoundaryEdgeCollapse || !edge.boundary ||
                        !aBoundary || !bBoundary ||
                        groupBoundaryCollapsed[a] || groupBoundaryCollapsed[b]))
                {
                    ++localStatistics.boundaryRejectedEdgeCount;
                    continue;
                }
                if (!compatibleAttributes(vertices[a], vertices[b], settings)) {
                    ++localStatistics.attributeRejectedEdgeCount;
                    continue;
                }
                uint32_t root = (std::min)(a, b);
                uint32_t other = (std::max)(a, b);
                const float separation = std::sqrt(squaredDistance(
                    vertices[root].position, vertices[other].position));
                const float error = (std::max)(groupError[root],
                    groupError[other] + separation);
                if (!std::isfinite(error) || error > budget) {
                    ++localStatistics.budgetRejectedEdgeCount;
                    continue;
                }
                std::vector<uint32_t> mergedTriangles;
                std::vector<uint32_t> mergedVertices;
                if (transactionalMergeValidation) {
                    mergedTriangles.reserve(groupTriangles[root].size() +
                        groupTriangles[other].size());
                    std::ranges::set_union(groupTriangles[root],
                        groupTriangles[other],
                        std::back_inserter(mergedTriangles));
                    mergedVertices.reserve(groupVertices[root].size() +
                        groupVertices[other].size());
                    std::ranges::set_union(groupVertices[root],
                        groupVertices[other],
                        std::back_inserter(mergedVertices));
                    if (settings.preventTopologyChangingMerges) {
                        std::set<uint32_t> rootLink;
                        std::set<uint32_t> otherLink;
                        std::set<uint32_t> edgeLink;
                        for (uint32_t triangle : mergedTriangles) {
                            const size_t item =
                                static_cast<size_t>(triangle) * 3u;
                            const std::array<uint32_t, 3> mapped{
                                find(localTriangleIndices[item]),
                                find(localTriangleIndices[item + 1u]),
                                find(localTriangleIndices[item + 2u]) };
                            std::array<uint32_t, 3> uniqueMapped = mapped;
                            std::ranges::sort(uniqueMapped);
                            const auto uniqueEnd = std::unique(
                                uniqueMapped.begin(), uniqueMapped.end());
                            if (uniqueEnd - uniqueMapped.begin() != 3)
                                continue;
                            const bool hasRoot = std::ranges::find(
                                mapped, root) != mapped.end();
                            const bool hasOther = std::ranges::find(
                                mapped, other) != mapped.end();
                            for (uint32_t value : mapped) {
                                if (hasRoot && value != root)
                                    rootLink.insert(value);
                                if (hasOther && value != other)
                                    otherLink.insert(value);
                                if (hasRoot && hasOther && value != root &&
                                    value != other)
                                    edgeLink.insert(value);
                            }
                        }
                        std::set<uint32_t> commonLink;
                        std::ranges::set_intersection(rootLink, otherLink,
                            std::inserter(commonLink, commonLink.end()));
                        if (edgeLink.empty() || edgeLink.size() > 2u ||
                            commonLink != edgeLink) {
                            ++localStatistics.topologyRejectedEdgeCount;
                            continue;
                        }
                    }
                    bool correspondenceSafe = std::ranges::all_of(
                        mergedVertices, [&](uint32_t original) {
                            return compatibleAttributes(vertices[original],
                                vertices[root], settings);
                        });
                    bool orientationSafe = true;
                    std::set<uint32_t> retainedVertices;
                    std::set<std::pair<uint32_t, uint32_t>> retainedEdges;
                    std::vector<uint32_t> collapsedVertices;
                    std::vector<std::pair<uint32_t, uint32_t>> collapsedEdges;
                    for (uint32_t triangle : mergedTriangles) {
                        const size_t item = static_cast<size_t>(triangle) * 3u;
                        const std::array<uint32_t, 3> source{
                            localTriangleIndices[item],
                            localTriangleIndices[item + 1u],
                            localTriangleIndices[item + 2u] };
                        std::array<uint32_t, 3> mapped{
                            find(source[0]), find(source[1]), find(source[2]) };
                        for (uint32_t& value : mapped) {
                            if (value == other) value = root;
                        }
                        std::array<uint32_t, 3> uniqueMapped = mapped;
                        std::ranges::sort(uniqueMapped);
                        const auto uniqueEnd = std::unique(
                            uniqueMapped.begin(), uniqueMapped.end());
                        const size_t uniqueCount =
                            static_cast<size_t>(uniqueEnd - uniqueMapped.begin());
                        if (uniqueCount == 1u) {
                            collapsedVertices.push_back(uniqueMapped[0]);
                            continue;
                        }
                        if (uniqueCount == 2u) {
                            collapsedEdges.emplace_back(
                                uniqueMapped[0], uniqueMapped[1]);
                            continue;
                        }
                        const auto sourceNormal = triangleNormal(vertices[source[0]],
                            vertices[source[1]], vertices[source[2]]);
                        const auto mappedNormal = triangleNormal(vertices[mapped[0]],
                            vertices[mapped[1]], vertices[mapped[2]]);
                        if (!(vectorLengthSquared(mappedNormal) > 0.0f) ||
                            !std::isfinite(vectorLengthSquared(mappedNormal)) ||
                            directionCosine(sourceNormal, mappedNormal) < 0.5f) {
                            orientationSafe = false;
                            break;
                        }
                        for (uint32_t corner = 0; corner < 3u; ++corner) {
                            retainedVertices.insert(mapped[corner]);
                            const auto endpoints = (std::minmax)(mapped[corner],
                                mapped[(corner + 1u) % 3u]);
                            retainedEdges.emplace(
                                endpoints.first, endpoints.second);
                        }
                    }
                    if (!orientationSafe) {
                        ++localStatistics.orientationRejectedEdgeCount;
                        continue;
                    }
                    if (correspondenceSafe) {
                        correspondenceSafe = std::ranges::all_of(
                            collapsedVertices, [&](uint32_t value) {
                                return retainedVertices.contains(value);
                            }) && std::ranges::all_of(collapsedEdges,
                            [&](const auto& value) {
                                return retainedEdges.contains(value);
                            });
                    }
                    if (!correspondenceSafe) {
                        ++localStatistics.correspondenceRejectedEdgeCount;
                        continue;
                    }
                }
                parent[other] = root;
                groupError[root] = error;
                groupBoundary[root] = groupBoundary[root] ||
                    groupBoundary[other];
                groupBoundaryCollapsed[root] = groupBoundaryCollapsed[root] ||
                    groupBoundaryCollapsed[other] || (aBoundary && bBoundary);
                if (transactionalMergeValidation) {
                    groupTriangles[root] = std::move(mergedTriangles);
                    groupTriangles[other].clear();
                    groupVertices[root] = std::move(mergedVertices);
                    groupVertices[other].clear();
                }
                ++localStatistics.acceptedMergeCount;
            }

            std::vector<std::array<uint32_t, 3>> triangles;
            triangles.reserve(localTriangleIndices.size() / 3u);
            std::set<std::array<uint32_t, 3>> unique;
            bool orientationValid = true;
            for (size_t item = 0;
                    item < localTriangleIndices.size(); item += 3u) {
                const std::array<uint32_t, 3> source{
                    localTriangleIndices[item],
                    localTriangleIndices[item + 1u],
                    localTriangleIndices[item + 2u] };
                const std::array<uint32_t, 3> mapped{
                    find(source[0]), find(source[1]), find(source[2]) };
                if (mapped[0] == mapped[1] || mapped[1] == mapped[2] ||
                    mapped[2] == mapped[0])
                    continue;
                const auto sourceNormal = triangleNormal(vertices[source[0]],
                    vertices[source[1]], vertices[source[2]]);
                const auto mappedNormal = triangleNormal(vertices[mapped[0]],
                    vertices[mapped[1]], vertices[mapped[2]]);
                if (!(vectorLengthSquared(sourceNormal) > 0.0f) ||
                    !(vectorLengthSquared(mappedNormal) > 0.0f) ||
                    !std::isfinite(vectorLengthSquared(sourceNormal)) ||
                    !std::isfinite(vectorLengthSquared(mappedNormal)) ||
                    directionCosine(sourceNormal, mappedNormal) < 0.5f) {
                    orientationValid = false;
                    break;
                }
                if (unique.insert(stableTriangleKey(mapped[0], mapped[1],
                        mapped[2])).second)
                    triangles.push_back(mapped);
            }
            const size_t newIndexCount = triangles.size() * 3u;
            if (!orientationValid) {
                ++localStatistics.orientationRejectedLevelCount;
                recordLevelOutcome(budgetIndex,
                    ModelLodLevelOutcome::OrientationRejected);
                continue;
            }
            if (newIndexCount >= previousIndexCount || newIndexCount < 3u) {
                ++localStatistics.noReductionLevelCount;
                recordLevelOutcome(budgetIndex,
                    ModelLodLevelOutcome::NoReduction);
                continue;
            }
            if (newIndexCount * 10u > previousIndexCount * 9u) {
                ++localStatistics.belowMinimumReductionLevelCount;
                recordLevelOutcome(budgetIndex,
                    ModelLodLevelOutcome::BelowMinimumReduction);
                continue;
            }

            // Prove that the image of every source triangle remains in the
            // reduced surface. A collapsed triangle must map to a surviving
            // edge or vertex; otherwise a small island or dangling feature
            // could disappear while the reported displacement stayed small.
            std::set<uint32_t> retainedVertices;
            std::set<std::pair<uint32_t, uint32_t>> retainedEdges;
            for (const auto& triangle : triangles) {
                for (uint32_t corner = 0; corner < 3u; ++corner) {
                    retainedVertices.insert(triangle[corner]);
                    const auto endpoints = (std::minmax)(triangle[corner],
                        triangle[(corner + 1u) % 3u]);
                    retainedEdges.emplace(endpoints.first, endpoints.second);
                }
            }
            bool correspondenceValid = true;
            float achievedError = 0.0f;
            for (size_t item = 0; item < localTriangleIndices.size(); item += 3u) {
                std::array<uint32_t, 3> mapped{};
                for (uint32_t corner = 0; corner < 3u; ++corner) {
                    const uint32_t original = localTriangleIndices[item + corner];
                    const uint32_t root = find(original);
                    mapped[corner] = root;
                    if (!retainedVertices.contains(root) ||
                        !compatibleAttributes(vertices[original], vertices[root],
                            settings)) {
                        correspondenceValid = false;
                        break;
                    }
                    achievedError = (std::max)(achievedError,
                        conservativeDistance(vertices[original].position,
                            vertices[root].position));
                }
                if (!correspondenceValid) break;
                std::ranges::sort(mapped);
                const auto uniqueEnd = std::unique(mapped.begin(), mapped.end());
                if (uniqueEnd - mapped.begin() == 2 &&
                    !retainedEdges.contains({ mapped[0], mapped[1] })) {
                    correspondenceValid = false;
                    break;
                }
            }
            if (!correspondenceValid || !std::isfinite(achievedError) ||
                achievedError > budget) {
                ++localStatistics.correspondenceRejectedLevelCount;
                recordLevelOutcome(budgetIndex,
                    ModelLodLevelOutcome::CorrespondenceRejected);
                continue;
            }

            std::vector<uint32_t> reducedIndices;
            reducedIndices.reserve(newIndexCount);
            for (const auto& triangle : triangles)
                reducedIndices.insert(reducedIndices.end(),
                    triangle.begin(), triangle.end());
            ModelLodTopologyFailure reducedTopologyFailure =
                ModelLodTopologyFailure::MixedComponentTriangle;
            const auto reducedTopology = topologySignature(reducedIndices,
                sourceComponents, stopToken, &reducedTopologyFailure);
            if (!reducedTopology) {
                ++localStatistics.topologyRejectedLevelCount;
                ++localStatistics.reducedTopologyInvalidLevelCount;
                ++localStatistics.reducedTopologyFailureLevelCounts[
                    static_cast<size_t>(reducedTopologyFailure)];
                recordLevelOutcome(budgetIndex,
                    ModelLodLevelOutcome::ReducedTopologyInvalid);
                continue;
            }
            if (*reducedTopology != *sourceTopology) {
                ++localStatistics.topologyRejectedLevelCount;
                ++localStatistics.topologySignatureMismatchLevelCount;
                bool componentMismatch =
                    reducedTopology->size() != sourceTopology->size();
                bool eulerMismatch = false;
                bool boundaryMismatch = false;
                for (const auto& [component, sourceValue] : *sourceTopology) {
                    const auto reduced = reducedTopology->find(component);
                    if (reduced == reducedTopology->end()) {
                        componentMismatch = true;
                        continue;
                    }
                    eulerMismatch = eulerMismatch ||
                        reduced->second.eulerCharacteristic !=
                            sourceValue.eulerCharacteristic;
                    boundaryMismatch = boundaryMismatch ||
                        reduced->second.boundaryLoops !=
                            sourceValue.boundaryLoops;
                }
                if (componentMismatch)
                    ++localStatistics.topologyComponentMismatchLevelCount;
                else if (eulerMismatch)
                    ++localStatistics.topologyEulerMismatchLevelCount;
                else if (boundaryMismatch)
                    ++localStatistics.topologyBoundaryMismatchLevelCount;
                recordLevelOutcome(budgetIndex,
                    ModelLodLevelOutcome::TopologySignatureMismatch);
                continue;
            }

            GeneratedModelLod lod;
            std::map<uint32_t, uint32_t> compact;
            lod.indices.reserve(newIndexCount);
            for (const auto& triangle : triangles) {
                for (uint32_t root : triangle) {
                    const auto [found, inserted] = compact.emplace(root,
                        static_cast<uint32_t>(compact.size()));
                    if (inserted) {
                        lod.vertices.push_back(vertices[root]);
                    }
                    lod.indices.push_back(found->second);
                }
            }
            lod.geometricError = (std::max)(achievedError, previousError);
            previousError = lod.geometricError;
            previousIndexCount = lod.indices.size();
            result.push_back(std::move(lod));
            ++localStatistics.generatedLevelCount;
            recordLevelOutcome(budgetIndex, ModelLodLevelOutcome::Accepted);
        }
        if (result.empty()) {
            ++localStatistics.noAcceptedLevelPrimitiveCount;
            localStatistics.noAcceptedLevelTriangleCount =
                localTriangleIndices.size() / 3u;
            const size_t terminalOutcome =
                static_cast<size_t>(levelOutcomes.back());
            ++localStatistics.noAcceptedTerminalOutcomePrimitiveCounts[
                terminalOutcome];
            localStatistics.noAcceptedTerminalOutcomeTriangleCounts[
                terminalOutcome] += localTriangleIndices.size() / 3u;
        }
        else {
            ++localStatistics.generatedChainCount;
            localStatistics.generatedBaseTriangleCount =
                localTriangleIndices.size() / 3u;
        }
        publishStatistics();
        return result;
    }

    size_t appendGeneratedModelLods(CookedModelProductData& product,
        const ModelLodGenerationSettings& settings, std::stop_token stopToken,
        ModelLodGenerationStatistics* statistics) {
        ModelLodGenerationStatistics aggregate;
        checkCancellation(stopToken);
        const size_t baseCount = product.manifest.primitives.size();
        size_t generatedChains = 0;
        for (size_t baseIndex = 0; baseIndex < baseCount; ++baseIndex) {
            checkCancellation(stopToken);
            const CookedModelPrimitive base = product.manifest.primitives[baseIndex];
            ++aggregate.canonicalPrimitiveCount;
            aggregate.canonicalTriangleCount += base.indexCount / 3u;
            if (base.coverage != ModelCoverage::Opaque) {
                ++aggregate.skippedNonOpaquePrimitiveCount;
                continue;
            }
            if (base.topology != ModelPrimitiveTopology::Triangles) {
                ++aggregate.skippedNonTrianglePrimitiveCount;
                continue;
            }
            if (base.lodSection != kNoModelSection) {
                ++aggregate.skippedExistingChainPrimitiveCount;
                continue;
            }
            if (base.firstVertex > product.vertices.size() ||
                base.vertexCount > product.vertices.size() - base.firstVertex ||
                base.firstIndex > product.indices.size() ||
                base.indexCount > product.indices.size() - base.firstIndex)
                throw std::invalid_argument("Source LOD primitive ranges are invalid.");
            std::vector<uint32_t> localIndices;
            localIndices.reserve(static_cast<size_t>(base.indexCount));
            for (size_t item = static_cast<size_t>(base.firstIndex);
                    item < base.firstIndex + base.indexCount; ++item) {
                const uint64_t index = product.indices[item];
                if (index < base.firstVertex ||
                    index - base.firstVertex >= base.vertexCount)
                    throw std::invalid_argument("Source LOD index is out of range.");
                localIndices.push_back(static_cast<uint32_t>(index - base.firstVertex));
            }
            ModelLodGenerationStatistics primitiveStatistics;
            const auto levels = generateModelLods(
                std::span(product.vertices).subspan(
                    static_cast<size_t>(base.firstVertex),
                    static_cast<size_t>(base.vertexCount)),
                localIndices, settings, stopToken, &primitiveStatistics);
            // Canonical totals were already counted for every base primitive.
            primitiveStatistics.canonicalPrimitiveCount = 0u;
            primitiveStatistics.canonicalTriangleCount = 0u;
            accumulateStatistics(aggregate, primitiveStatistics);
            if (levels.empty()) continue;
            if (baseIndex > std::numeric_limits<uint32_t>::max())
                throw std::length_error("LOD base primitive exceeds schema limits.");
            CookedModelLodChain chain{
                .basePrimitiveIndex = static_cast<uint32_t>(baseIndex),
                .levels = {{ static_cast<uint32_t>(baseIndex), 0.0f }},
            };
            product.manifest.primitives[baseIndex].lodSection = kCookedModelLodSection;
            for (size_t levelIndex = 0; levelIndex < levels.size(); ++levelIndex) {
                checkCancellation(stopToken);
                const GeneratedModelLod& level = levels[levelIndex];
                if (product.vertices.size() > std::numeric_limits<uint32_t>::max() ||
                    level.vertices.size() > std::numeric_limits<uint32_t>::max() -
                        product.vertices.size() ||
                    product.manifest.primitives.size() >=
                        std::numeric_limits<uint32_t>::max())
                    throw std::length_error("Generated LOD exceeds model stream limits.");
                const std::string identity = "iridium.model.lod.v1/" +
                    base.primitiveGuid.toString() + "/" +
                    std::to_string(levelIndex + 1u);
                const std::string digest = sha256(std::as_bytes(std::span(identity)));
                AssetGuid::Bytes guidBytes = base.primitiveGuid.bytes();
                const auto nibble = [](char value) -> uint8_t {
                    return static_cast<uint8_t>(value <= '9' ? value - '0' :
                        value - 'a' + 10);
                };
                for (size_t byte = 6; byte < guidBytes.size(); ++byte) {
                    const size_t hex = (byte - 6u) * 2u;
                    guidBytes[byte] = static_cast<uint8_t>(
                        (nibble(digest[hex]) << 4u) | nibble(digest[hex + 1u]));
                }
                guidBytes[6] = static_cast<uint8_t>((guidBytes[6] & 0x0fu) | 0x70u);
                guidBytes[8] = static_cast<uint8_t>((guidBytes[8] & 0x3fu) | 0x80u);
                CookedModelPrimitive child = base;
                child.primitiveGuid = AssetGuid(guidBytes);
                child.sourceKey += "/lod/" + std::to_string(levelIndex + 1u);
                child.firstVertex = product.vertices.size();
                child.vertexCount = level.vertices.size();
                child.firstIndex = product.indices.size();
                child.indexCount = level.indices.size();
                child.indexFormat = level.vertices.size() <= 65536u
                    ? ModelIndexFormat::UInt16 : ModelIndexFormat::UInt32;
                child.rtFirstPosition = 0;
                child.rtPositionCount = 0;
                child.rtFirstIndex = 0;
                child.rtIndexCount = 0;
                child.rtFlags = 0;
                child.lodSection = kCookedModelLodSection;
                child.meshletSection = kNoModelSection;
                product.vertices.insert(product.vertices.end(),
                    level.vertices.begin(), level.vertices.end());
                for (uint32_t index : level.indices)
                    product.indices.push_back(static_cast<uint32_t>(
                        child.firstVertex + index));
                const uint32_t primitiveIndex =
                    static_cast<uint32_t>(product.manifest.primitives.size());
                product.manifest.primitives.push_back(std::move(child));
                chain.levels.push_back({ primitiveIndex, level.geometricError });
            }
            product.lodChains.push_back(std::move(chain));
            ++generatedChains;
        }
        std::ranges::sort(product.lodChains, {},
            &CookedModelLodChain::basePrimitiveIndex);
        product.manifest.vertexCount = product.vertices.size();
        product.manifest.indexCount = product.indices.size();
        if (statistics) *statistics = aggregate;
        return generatedChains;
    }

} // namespace Iridium
