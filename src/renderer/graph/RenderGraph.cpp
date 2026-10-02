#include "renderer/graph/RenderGraph.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace Iridium::RenderGraph {
namespace {

    constexpr uint64_t FnvOffset = 14695981039346656037ull;
    constexpr uint64_t FnvPrime = 1099511628211ull;

    void hashBytes(uint64_t& hash, const void* data, size_t size) noexcept {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= FnvPrime;
        }
    }

    template <typename T>
    void hashValue(uint64_t& hash, const T& value) noexcept {
        hashBytes(hash, &value, sizeof(value));
    }

    void hashString(uint64_t& hash, std::string_view value) noexcept {
        const uint64_t size = value.size();
        hashValue(hash, size);
        hashBytes(hash, value.data(), value.size());
    }

    void hashResourceDesc(uint64_t& hash, const ResourceDesc& desc) noexcept {
        hashValue(hash, desc.type);
        hashValue(hash, desc.lifetime);
        hashValue(hash, desc.image.format);
        hashValue(hash, desc.image.extent.width);
        hashValue(hash, desc.image.extent.height);
        hashValue(hash, desc.image.extent.depth);
        hashValue(hash, desc.image.mipLevels);
        hashValue(hash, desc.image.arrayLayers);
        hashValue(hash, desc.image.samples);
        hashValue(hash, desc.buffer.size);
        hashValue(hash, desc.buffer.alignment);
        hashValue(hash, desc.initialAccess);
        const uint8_t imported = desc.imported ? 1 : 0;
        hashValue(hash, imported);
    }

    bool isPowerOfTwo(uint32_t value) noexcept {
        return value != 0 && (value & (value - 1)) == 0;
    }

    bool isReadAccess(Access access) noexcept {
        switch (access) {
        case Access::DepthAttachmentRead:
        case Access::SampledRead:
        case Access::StorageRead:
        case Access::TransferSource:
        case Access::VertexRead:
        case Access::IndexRead:
        case Access::IndirectRead:
            return true;
        default:
            return false;
        }
    }

    bool isWriteAccess(Access access) noexcept {
        switch (access) {
        case Access::ColorAttachment:
        case Access::DepthAttachmentWrite:
        case Access::StorageWrite:
        case Access::StorageReadWrite:
        case Access::TransferDestination:
            return true;
        default:
            return false;
        }
    }

    bool compatible(const CompiledResource& resource,
        const PhysicalResourceSlot& slot) noexcept {
        if (resource.desc.type != slot.type) {
            return false;
        }
        if (resource.desc.type == ResourceType::Image) {
            return resource.desc.image == slot.image;
        }
        return resource.desc.buffer == slot.buffer;
    }

    bool initialVersionReadable(const ResourceDesc& desc) noexcept {
        return desc.imported || desc.initialAccess != Access::Undefined ||
            desc.lifetime == ResourceLifetime::History;
    }

} // namespace

RenderGraphBuilder::RenderGraphBuilder(GraphCapacity capacity)
    : m_capacity(capacity) {
    if (capacity.maxPasses == 0 || capacity.maxLogicalResources == 0 ||
        capacity.maxResourceVersions == 0 || capacity.maxUsages == 0 ||
        capacity.maxDependencies == 0) {
        throw GraphBuildError("Render graph capacities must be non-zero");
    }
    if (capacity.maxResourceVersions < capacity.maxLogicalResources) {
        throw GraphBuildError(
            "Resource-version capacity must cover logical-resource capacity");
    }

    m_passes.reserve(capacity.maxPasses);
    m_logicalResources.reserve(capacity.maxLogicalResources);
    m_resourceVersions.reserve(capacity.maxResourceVersions);
    m_usages.reserve(capacity.maxUsages);
    m_dependencies.reserve(capacity.maxDependencies);
}

void RenderGraphBuilder::reset() {
    m_passes.clear();
    m_logicalResources.clear();
    m_resourceVersions.clear();
    m_usages.clear();
    m_dependencies.clear();
    ++m_generation;
    if (m_generation == 0) {
        m_generation = 1;
    }
}

ResourceHandle RenderGraphBuilder::createResource(std::string name,
    const ResourceDesc& desc) {
    validateDescriptor(desc);
    requireCapacity(m_logicalResources.size() < m_capacity.maxLogicalResources,
        "logical resources");
    requireCapacity(m_resourceVersions.size() < m_capacity.maxResourceVersions,
        "resource versions");
    if (name.empty()) {
        throw GraphBuildError("Render graph resource name must not be empty");
    }

    const uint32_t logicalIndex = static_cast<uint32_t>(m_logicalResources.size());
    m_logicalResources.push_back({ std::move(name), desc });
    const uint32_t versionIndex = static_cast<uint32_t>(m_resourceVersions.size());
    m_resourceVersions.push_back({ logicalIndex });
    return { versionIndex, m_generation };
}

PassHandle RenderGraphBuilder::addPass(std::string name, QueueClass queue) {
    requireCapacity(m_passes.size() < m_capacity.maxPasses, "passes");
    if (name.empty()) {
        throw GraphBuildError("Render graph pass name must not be empty");
    }
    const uint32_t index = static_cast<uint32_t>(m_passes.size());
    m_passes.push_back({ std::move(name), queue });
    return { index, m_generation };
}

void RenderGraphBuilder::read(PassHandle pass, ResourceHandle resource,
    Access access) {
    validate(pass);
    validate(resource);
    if (!isReadAccess(access)) {
        throw GraphBuildError("Read declaration requires a read access");
    }
    requireCapacity(m_usages.size() < m_capacity.maxUsages, "usages");
    m_usages.push_back({ pass.index, resource.index, access, false });
}

ResourceHandle RenderGraphBuilder::write(PassHandle pass,
    ResourceHandle previousVersion, Access access, LoadOp loadOp, StoreOp storeOp) {
    validate(pass);
    validate(previousVersion);
    if (!isWriteAccess(access)) {
        throw GraphBuildError("Write declaration requires a write access");
    }
    const bool attachmentAccess = access == Access::ColorAttachment ||
        access == Access::DepthAttachmentWrite;
    if (!attachmentAccess && loadOp != LoadOp::DontCare) {
        throw GraphBuildError(
            "Only attachment writes accept clear/load operations");
    }
    if (!attachmentAccess && storeOp != StoreOp::Store) {
        throw GraphBuildError(
            "Non-attachment writes must preserve their declared result");
    }
    requireCapacity(m_resourceVersions.size() < m_capacity.maxResourceVersions,
        "resource versions");
    requireCapacity(m_usages.size() < m_capacity.maxUsages, "usages");

    const ResourceVersionRecord& previous =
        m_resourceVersions[previousVersion.index];
    const uint32_t versionIndex = static_cast<uint32_t>(m_resourceVersions.size());
    ResourceVersionRecord version{};
    version.logicalResourceIndex = previous.logicalResourceIndex;
    version.producerPassIndex = pass.index;
    version.previousVersionIndex = previousVersion.index;
    version.preservePrevious = loadOp == LoadOp::Load;
    m_resourceVersions.push_back(version);
    m_usages.push_back({ pass.index, versionIndex, access, true, loadOp, storeOp });
    return { versionIndex, m_generation };
}

void RenderGraphBuilder::addDependency(PassHandle before, PassHandle after) {
    validate(before);
    validate(after);
    requireCapacity(m_dependencies.size() < m_capacity.maxDependencies,
        "dependencies");
    m_dependencies.push_back({ before.index, after.index });
}

void RenderGraphBuilder::exportResource(ResourceHandle resource,
    Access finalAccess) {
    validate(resource);
    if (finalAccess == Access::Undefined) {
        throw GraphBuildError("Exported resource requires a final access");
    }
    ResourceVersionRecord& version = m_resourceVersions[resource.index];
    version.exported = true;
    version.finalAccess = finalAccess;
}

void RenderGraphBuilder::validate(PassHandle pass) const {
    if (pass.generation != m_generation || pass.index >= m_passes.size()) {
        throw GraphBuildError("Stale or invalid render graph pass handle");
    }
}

void RenderGraphBuilder::validate(ResourceHandle resource) const {
    if (resource.generation != m_generation ||
        resource.index >= m_resourceVersions.size()) {
        throw GraphBuildError("Stale or invalid render graph resource handle");
    }
}

void RenderGraphBuilder::validateDescriptor(const ResourceDesc& desc) const {
    if (desc.type == ResourceType::Image) {
        if (desc.image.format == Format::Undefined ||
            desc.image.extent.width == 0 || desc.image.extent.height == 0 ||
            desc.image.extent.depth == 0 || desc.image.mipLevels == 0 ||
            desc.image.arrayLayers == 0 || desc.image.samples == 0) {
            throw GraphBuildError("Invalid render graph image descriptor");
        }
    }
    else if (desc.buffer.size == 0 || !isPowerOfTwo(desc.buffer.alignment)) {
        throw GraphBuildError("Invalid render graph buffer descriptor");
    }

    if (desc.imported && desc.lifetime != ResourceLifetime::External) {
        throw GraphBuildError("Imported resource must have external lifetime");
    }
    if (desc.lifetime == ResourceLifetime::External && !desc.imported) {
        throw GraphBuildError("External resource must be imported");
    }
    if (desc.imported && desc.initialAccess == Access::Undefined) {
        throw GraphBuildError("Imported resource requires an initial access");
    }
    if (desc.lifetime == ResourceLifetime::Transient &&
        desc.initialAccess != Access::Undefined) {
        throw GraphBuildError("Transient resource cannot declare an initial access");
    }
}

void RenderGraphBuilder::requireCapacity(bool condition,
    std::string_view what) const {
    if (!condition) {
        throw GraphBuildError("Render graph capacity exceeded for " +
            std::string(what));
    }
}

struct CompilerAccess {
    static CompileResult compile(const RenderGraphBuilder& builder) {
        CompileResult result;
        const uint32_t passCount = static_cast<uint32_t>(builder.m_passes.size());
        const uint32_t logicalCount =
            static_cast<uint32_t>(builder.m_logicalResources.size());
        const uint32_t versionCount =
            static_cast<uint32_t>(builder.m_resourceVersions.size());

        std::vector<std::vector<uint32_t>> edges(passCount);
        std::vector<uint32_t> indegrees(passCount, 0);
        const auto addEdge = [&](uint32_t before, uint32_t after) {
            if (before >= passCount || after >= passCount) {
                return;
            }
            auto& destinations = edges[before];
            if (std::find(destinations.begin(), destinations.end(), after) ==
                destinations.end()) {
                destinations.push_back(after);
                ++indegrees[after];
            }
        };

        for (const auto& dependency : builder.m_dependencies) {
            addEdge(dependency.beforePassIndex, dependency.afterPassIndex);
        }

        std::vector<std::vector<uint32_t>> versionUsers(versionCount);
        std::vector<StoreOp> versionStores(versionCount, StoreOp::DontCare);
        for (const auto& usage : builder.m_usages) {
            if (usage.write) {
                versionStores[usage.resourceVersionIndex] = usage.storeOp;
            }
        }
        for (const auto& usage : builder.m_usages) {
            versionUsers[usage.resourceVersionIndex].push_back(usage.passIndex);
            if (!usage.write) {
                const auto& version =
                    builder.m_resourceVersions[usage.resourceVersionIndex];
                if (version.producerPassIndex != InvalidIndex) {
                    addEdge(version.producerPassIndex, usage.passIndex);
                    if (versionStores[usage.resourceVersionIndex] != StoreOp::Store) {
                        result.diagnostics.push_back({ DiagnosticCode::InvalidUsage,
                            "Resource version is read after its producer discarded contents" });
                    }
                }
                else {
                    const ResourceDesc& desc = builder.m_logicalResources[
                        version.logicalResourceIndex].desc;
                    if (!initialVersionReadable(desc)) {
                        result.diagnostics.push_back({ DiagnosticCode::ReadBeforeWrite,
                            "Resource '" + builder.m_logicalResources[
                                version.logicalResourceIndex].name +
                            "' is read before it has a producer or valid initial state" });
                    }
                }
            }
        }

        for (uint32_t versionIndex = 0; versionIndex < versionCount; ++versionIndex) {
            const auto& version = builder.m_resourceVersions[versionIndex];
            if (version.producerPassIndex == InvalidIndex ||
                version.previousVersionIndex == InvalidIndex) {
                continue;
            }

            const auto& previous =
                builder.m_resourceVersions[version.previousVersionIndex];
            if (previous.producerPassIndex != InvalidIndex) {
                addEdge(previous.producerPassIndex, version.producerPassIndex);
            }
            for (uint32_t user : versionUsers[version.previousVersionIndex]) {
                if (user != version.producerPassIndex) {
                    addEdge(user, version.producerPassIndex);
                }
            }

            if (version.preservePrevious &&
                previous.producerPassIndex == InvalidIndex) {
                const ResourceDesc& desc = builder.m_logicalResources[
                    previous.logicalResourceIndex].desc;
                if (!initialVersionReadable(desc)) {
                    result.diagnostics.push_back({ DiagnosticCode::ReadBeforeWrite,
                        "Load operation for resource '" + builder.m_logicalResources[
                            previous.logicalResourceIndex].name +
                        "' has no valid previous contents" });
                }
            }
            else if (version.preservePrevious &&
                versionStores[version.previousVersionIndex] != StoreOp::Store) {
                result.diagnostics.push_back({ DiagnosticCode::InvalidUsage,
                    "Load operation depends on a discarded resource version" });
            }
        }

        std::vector<uint32_t> passOrder;
        passOrder.reserve(passCount);
        std::vector<bool> emitted(passCount, false);
        for (uint32_t outputIndex = 0; outputIndex < passCount; ++outputIndex) {
            uint32_t selected = InvalidIndex;
            for (uint32_t candidate = 0; candidate < passCount; ++candidate) {
                if (!emitted[candidate] && indegrees[candidate] == 0) {
                    selected = candidate;
                    break;
                }
            }
            if (selected == InvalidIndex) {
                result.diagnostics.push_back({ DiagnosticCode::Cycle,
                    "Render graph contains a dependency cycle" });
                break;
            }
            emitted[selected] = true;
            passOrder.push_back(selected);
            for (uint32_t destination : edges[selected]) {
                --indegrees[destination];
            }
        }

        std::vector<uint32_t> exportedVersions(logicalCount, InvalidIndex);
        std::vector<uint32_t> latestVersions(logicalCount, InvalidIndex);
        for (uint32_t versionIndex = 0; versionIndex < versionCount; ++versionIndex) {
            latestVersions[builder.m_resourceVersions[versionIndex].logicalResourceIndex] =
                versionIndex;
        }
        for (uint32_t versionIndex = 0; versionIndex < versionCount; ++versionIndex) {
            const auto& version = builder.m_resourceVersions[versionIndex];
            if (!version.exported) {
                continue;
            }
            uint32_t& prior = exportedVersions[version.logicalResourceIndex];
            if (prior != InvalidIndex) {
                result.diagnostics.push_back({ DiagnosticCode::InvalidExport,
                    "Logical resource '" + builder.m_logicalResources[
                        version.logicalResourceIndex].name +
                    "' exports more than one version" });
            }
            prior = versionIndex;
            if (latestVersions[version.logicalResourceIndex] != versionIndex) {
                result.diagnostics.push_back({ DiagnosticCode::InvalidExport,
                    "Logical resource exports a stale version instead of its latest version" });
            }
            if (version.producerPassIndex == InvalidIndex &&
                !initialVersionReadable(builder.m_logicalResources[
                    version.logicalResourceIndex].desc)) {
                result.diagnostics.push_back({ DiagnosticCode::InvalidExport,
                    "Exported resource has no producer or valid initial state" });
            }
        }

        if (!result.diagnostics.empty() || passOrder.size() != passCount) {
            return result;
        }

        CompiledGraph graph;
        graph.m_passes.reserve(passCount);
        std::vector<uint32_t> passOrderIndices(passCount, InvalidIndex);
        for (uint32_t orderIndex = 0; orderIndex < passCount; ++orderIndex) {
            const uint32_t sourceIndex = passOrder[orderIndex];
            passOrderIndices[sourceIndex] = orderIndex;
            const auto& pass = builder.m_passes[sourceIndex];
            graph.m_passes.push_back({ sourceIndex, pass.name, pass.queue });
        }

        graph.m_resources.reserve(logicalCount);
        for (uint32_t logicalIndex = 0; logicalIndex < logicalCount; ++logicalIndex) {
            const auto& source = builder.m_logicalResources[logicalIndex];
            CompiledResource resource{};
            resource.logicalResourceIndex = logicalIndex;
            resource.name = source.name;
            resource.desc = source.desc;
            if (exportedVersions[logicalIndex] != InvalidIndex) {
                const auto& version =
                    builder.m_resourceVersions[exportedVersions[logicalIndex]];
                resource.exported = true;
                resource.finalAccess = version.finalAccess;
            }
            graph.m_resources.push_back(std::move(resource));

            if (source.desc.lifetime == ResourceLifetime::History) {
                graph.m_historyResources.push_back({ logicalIndex });
            }
        }

        for (const auto& usage : builder.m_usages) {
            const uint32_t logicalIndex = builder.m_resourceVersions[
                usage.resourceVersionIndex].logicalResourceIndex;
            CompiledResource& resource = graph.m_resources[logicalIndex];
            const uint32_t orderIndex = passOrderIndices[usage.passIndex];
            resource.firstUse = std::min(resource.firstUse, orderIndex);
            resource.lastUse = resource.lastUse == InvalidIndex
                ? orderIndex
                : std::max(resource.lastUse, orderIndex);
            resource.usages |= usageBit(usage.access);
        }

        graph.m_usages.reserve(builder.m_usages.size());
        for (uint32_t orderIndex = 0; orderIndex < passCount; ++orderIndex) {
            CompiledPass& pass = graph.m_passes[orderIndex];
            pass.firstUsage = static_cast<uint32_t>(graph.m_usages.size());
            for (const auto& usage : builder.m_usages) {
                if (usage.passIndex != pass.sourcePassIndex) {
                    continue;
                }
                const uint32_t logicalIndex = builder.m_resourceVersions[
                    usage.resourceVersionIndex].logicalResourceIndex;
                graph.m_usages.push_back({ orderIndex, logicalIndex, usage.access,
                    usage.write, usage.loadOp, usage.storeOp });
                ++pass.usageCount;
            }
        }

        std::vector<Access> currentAccess(logicalCount, Access::Undefined);
        for (uint32_t logicalIndex = 0; logicalIndex < logicalCount; ++logicalIndex) {
            currentAccess[logicalIndex] =
                builder.m_logicalResources[logicalIndex].desc.initialAccess;
        }
        for (uint32_t orderIndex = 0; orderIndex < passCount; ++orderIndex) {
            const uint32_t sourcePass = passOrder[orderIndex];
            for (const auto& usage : builder.m_usages) {
                if (usage.passIndex != sourcePass) {
                    continue;
                }
                const uint32_t logicalIndex = builder.m_resourceVersions[
                    usage.resourceVersionIndex].logicalResourceIndex;
                if (currentAccess[logicalIndex] != usage.access) {
                    graph.m_transitions.push_back({ orderIndex, logicalIndex,
                        currentAccess[logicalIndex], usage.access });
                    currentAccess[logicalIndex] = usage.access;
                }
            }
        }
        for (CompiledResource& resource : graph.m_resources) {
            if (resource.exported && currentAccess[resource.logicalResourceIndex] !=
                resource.finalAccess) {
                graph.m_transitions.push_back({ passCount,
                    resource.logicalResourceIndex,
                    currentAccess[resource.logicalResourceIndex],
                    resource.finalAccess });
            }
        }

        std::vector<uint32_t> reusableResources;
        reusableResources.reserve(logicalCount);
        for (CompiledResource& resource : graph.m_resources) {
            if (resource.firstUse == InvalidIndex ||
                resource.desc.lifetime == ResourceLifetime::External) {
                continue;
            }
            if (resource.desc.lifetime == ResourceLifetime::Transient &&
                !resource.exported) {
                reusableResources.push_back(resource.logicalResourceIndex);
                continue;
            }

            PhysicalResourceSlot slot{};
            slot.slotIndex = static_cast<uint32_t>(graph.m_physicalSlots.size());
            slot.type = resource.desc.type;
            slot.image = resource.desc.image;
            slot.buffer = resource.desc.buffer;
            slot.usages = resource.usages;
            slot.lastUse = resource.lastUse;
            slot.transientReusable = false;
            slot.logicalResources.push_back(resource.logicalResourceIndex);
            resource.physicalSlot = slot.slotIndex;
            graph.m_physicalSlots.push_back(std::move(slot));
        }

        std::stable_sort(reusableResources.begin(), reusableResources.end(),
            [&](uint32_t left, uint32_t right) {
                const CompiledResource& lhs = graph.m_resources[left];
                const CompiledResource& rhs = graph.m_resources[right];
                return lhs.firstUse != rhs.firstUse
                    ? lhs.firstUse < rhs.firstUse
                    : left < right;
            });

        for (uint32_t logicalIndex : reusableResources) {
            CompiledResource& resource = graph.m_resources[logicalIndex];
            PhysicalResourceSlot* selected = nullptr;
            for (PhysicalResourceSlot& slot : graph.m_physicalSlots) {
                if (slot.transientReusable && slot.lastUse < resource.firstUse &&
                    compatible(resource, slot)) {
                    selected = &slot;
                    break;
                }
            }
            if (selected == nullptr) {
                PhysicalResourceSlot slot{};
                slot.slotIndex = static_cast<uint32_t>(graph.m_physicalSlots.size());
                slot.type = resource.desc.type;
                slot.image = resource.desc.image;
                slot.buffer = resource.desc.buffer;
                slot.transientReusable = true;
                graph.m_physicalSlots.push_back(std::move(slot));
                selected = &graph.m_physicalSlots.back();
            }
            selected->usages |= resource.usages;
            selected->lastUse = resource.lastUse;
            selected->logicalResources.push_back(logicalIndex);
            resource.physicalSlot = selected->slotIndex;
        }

        uint64_t hash = FnvOffset;
        for (const auto& resource : builder.m_logicalResources) {
            hashString(hash, resource.name);
            hashResourceDesc(hash, resource.desc);
        }
        for (const auto& pass : builder.m_passes) {
            hashString(hash, pass.name);
            hashValue(hash, pass.queue);
        }
        for (const auto& version : builder.m_resourceVersions) {
            hashValue(hash, version.logicalResourceIndex);
            hashValue(hash, version.producerPassIndex);
            hashValue(hash, version.previousVersionIndex);
            const uint8_t preserve = version.preservePrevious ? 1 : 0;
            const uint8_t exported = version.exported ? 1 : 0;
            hashValue(hash, preserve);
            hashValue(hash, exported);
            hashValue(hash, version.finalAccess);
        }
        for (const auto& usage : builder.m_usages) {
            hashValue(hash, usage.passIndex);
            hashValue(hash, usage.resourceVersionIndex);
            hashValue(hash, usage.access);
            const uint8_t write = usage.write ? 1 : 0;
            hashValue(hash, write);
            hashValue(hash, usage.loadOp);
            hashValue(hash, usage.storeOp);
        }
        for (const auto& dependency : builder.m_dependencies) {
            hashValue(hash, dependency.beforePassIndex);
            hashValue(hash, dependency.afterPassIndex);
        }
        graph.m_topologyHash = hash == 0 ? 1 : hash;
        result.graph = std::move(graph);
        return result;
    }
};

CompileResult RenderGraphBuilder::compile() const {
    return CompilerAccess::compile(*this);
}

void CompiledGraphCache::store(CompiledGraph graph) {
    for (auto& entry : m_entries) {
        if (entry && entry->topologyHash() == graph.topologyHash()) {
            entry = std::move(graph);
            return;
        }
    }

    m_entries[m_nextReplacement] = std::move(graph);
    m_nextReplacement = (m_nextReplacement + 1) % Capacity;
    m_size = std::min(m_size + 1, Capacity);
}

const CompiledGraph* CompiledGraphCache::find(uint64_t topologyHash) const noexcept {
    for (const auto& entry : m_entries) {
        if (entry && entry->topologyHash() == topologyHash) {
            return &*entry;
        }
    }
    return nullptr;
}

void CompiledGraphCache::clear() noexcept {
    for (auto& entry : m_entries) {
        entry.reset();
    }
    m_size = 0;
    m_nextReplacement = 0;
}

void HistoryValidityTracker::resetForGraph(const CompiledGraph& graph) {
    m_topologyHash = graph.topologyHash();
    m_validity.assign(graph.resources().size(), 0);
    m_isHistory.assign(graph.resources().size(), 0);
    for (const HistoryResourceRecord& history : graph.historyResources()) {
        m_isHistory[history.logicalResourceIndex] = 1;
    }
}

void HistoryValidityTracker::invalidateAll() noexcept {
    std::fill(m_validity.begin(), m_validity.end(), uint8_t{ 0 });
}

void HistoryValidityTracker::setValid(uint32_t logicalResourceIndex, bool valid) {
    if (logicalResourceIndex >= m_validity.size() ||
        m_isHistory[logicalResourceIndex] == 0) {
        throw GraphBuildError("History validity update targets a non-history resource");
    }
    m_validity[logicalResourceIndex] = valid ? 1 : 0;
}

bool HistoryValidityTracker::isValid(uint32_t logicalResourceIndex) const noexcept {
    return logicalResourceIndex < m_validity.size() &&
        m_isHistory[logicalResourceIndex] != 0 &&
        m_validity[logicalResourceIndex] != 0;
}

} // namespace Iridium::RenderGraph
