#include "renderer/graph/RenderGraph.h"

#include <algorithm>
#include <bit>
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
        // Hashed only when set, so graphs without the flag keep their hashes.
        if (desc.buffer.variableSize) {
            const uint8_t variableSize = 1;
            hashValue(hash, variableSize);
        }
    }

    void hashClearValue(uint64_t& hash, const ClearValue& value) noexcept {
        for (const uint32_t bits : value.colorBits) hashValue(hash, bits);
        hashValue(hash, std::bit_cast<uint32_t>(value.depth));
        hashValue(hash, value.stencil);
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

    bool initialVersionReadable(const ResourceDesc& desc,
        HistoryRole role = HistoryRole::None) noexcept {
        // A pair's `current` holds nothing until this frame's writer runs.
        if (role == HistoryRole::Current) return false;
        return desc.imported || desc.initialAccess != Access::Undefined ||
            desc.lifetime == ResourceLifetime::History;
    }

    // The first usage of a logical resource in compiled order. A pass holds at
    // most one usage of a resource whose first use is a write (a second write
    // or a read of its own product would be a self-dependency).
    struct FirstUsage {
        bool seen = false;
        bool write = false;
        LoadOp loadOp = LoadOp::DontCare;
        bool wholeResource = false;
    };

    AliasEligibility classifyAliasing(const CompiledResource& resource,
        const FirstUsage& first, bool excluded) noexcept {
        if (resource.firstUse == InvalidIndex || !first.seen)
            return AliasEligibility::Unused;
        if (resource.desc.imported ||
            resource.desc.lifetime == ResourceLifetime::External)
            return AliasEligibility::Imported;
        if (resource.historyPair != InvalidIndex ||
            resource.desc.lifetime == ResourceLifetime::History)
            return AliasEligibility::History;
        if (resource.desc.lifetime != ResourceLifetime::Transient)
            return AliasEligibility::NotTransient;
        if (resource.exported) return AliasEligibility::Exported;
        if (resource.desc.type != ResourceType::Image) return AliasEligibility::Buffer;
        if (excluded) return AliasEligibility::Excluded;
        const bool discards = first.write && (first.loadOp == LoadOp::Clear ||
            (first.loadOp == LoadOp::DontCare && first.wholeResource));
        return discards ? AliasEligibility::Eligible
                        : AliasEligibility::FirstUseNotDiscard;
    }

} // namespace

const char* aliasEligibilityName(AliasEligibility eligibility) noexcept {
    switch (eligibility) {
    case AliasEligibility::Eligible: return "eligible";
    case AliasEligibility::Unused: return "unused";
    case AliasEligibility::Imported: return "imported";
    case AliasEligibility::History: return "history";
    case AliasEligibility::NotTransient: return "not-transient";
    case AliasEligibility::Exported: return "exported";
    case AliasEligibility::Buffer: return "buffer";
    case AliasEligibility::FirstUseNotDiscard: return "first-use-not-discard";
    case AliasEligibility::Excluded: return "excluded";
    }
    return "invalid";
}

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
    m_historyPairCount = 0;
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

RenderGraphBuilder::HistoryHandles RenderGraphBuilder::createHistory(
    std::string name, const ResourceDesc& desc, HistoryReset reset) {
    ResourceDesc history = desc;
    if (history.imported || history.lifetime == ResourceLifetime::External)
        throw GraphBuildError("History resources cannot be imported");
    if (history.initialAccess != Access::Undefined)
        throw GraphBuildError("History resources start undefined");
    history.lifetime = ResourceLifetime::History;
    validateDescriptor(history);
    requireCapacity(m_logicalResources.size() + 2 <= m_capacity.maxLogicalResources,
        "logical resources");
    requireCapacity(m_resourceVersions.size() + 2 <= m_capacity.maxResourceVersions,
        "resource versions");
    if (name.empty()) {
        throw GraphBuildError("Render graph resource name must not be empty");
    }
    const uint32_t pair = m_historyPairCount++;
    HistoryHandles handles{};
    handles.pair = pair;
    for (const HistoryRole role : { HistoryRole::Previous, HistoryRole::Current }) {
        const uint32_t logicalIndex = static_cast<uint32_t>(m_logicalResources.size());
        m_logicalResources.push_back({ name + (role == HistoryRole::Previous
            ? ".previous" : ".current"), history, pair, role, reset });
        const uint32_t versionIndex = static_cast<uint32_t>(m_resourceVersions.size());
        m_resourceVersions.push_back({ logicalIndex });
        (role == HistoryRole::Previous ? handles.previous : handles.current) =
            { versionIndex, m_generation };
    }
    return handles;
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
    return write(pass, previousVersion, access, loadOp, storeOp, ClearValue{});
}

ResourceHandle RenderGraphBuilder::write(PassHandle pass,
    ResourceHandle previousVersion, Access access, LoadOp loadOp, StoreOp storeOp,
    const ClearValue& clearValue) {
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
    if (m_logicalResources[previous.logicalResourceIndex].historyRole ==
        HistoryRole::Previous) {
        throw GraphBuildError("History 'previous' resources are read-only");
    }
    const uint32_t versionIndex = static_cast<uint32_t>(m_resourceVersions.size());
    ResourceVersionRecord version{};
    version.logicalResourceIndex = previous.logicalResourceIndex;
    version.producerPassIndex = pass.index;
    version.previousVersionIndex = previousVersion.index;
    version.preservePrevious = loadOp == LoadOp::Load;
    m_resourceVersions.push_back(version);
    // Only a clearing usage carries its value, so every other usage compiles
    // and hashes identically whatever value was passed.
    m_usages.push_back({ pass.index, versionIndex, access, true, loadOp, storeOp,
        loadOp == LoadOp::Clear ? clearValue : ClearValue{} });
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

void RenderGraphBuilder::declareWholeResourceWrite(ResourceHandle writtenVersion) {
    validate(writtenVersion);
    ResourceVersionRecord& version = m_resourceVersions[writtenVersion.index];
    if (version.producerPassIndex == InvalidIndex) {
        throw GraphBuildError(
            "Whole-resource write declaration requires a written version");
    }
    if (version.preservePrevious) {
        throw GraphBuildError(
            "A whole-resource write cannot load its previous contents");
    }
    version.wholeResourceWrite = true;
}

void RenderGraphBuilder::excludeFromAliasing(ResourceHandle resource) {
    validate(resource);
    m_logicalResources[m_resourceVersions[resource.index].logicalResourceIndex]
        .aliasingExcluded = true;
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

    if (desc.buffer.variableSize &&
        (desc.type != ResourceType::Buffer || !desc.imported)) {
        throw GraphBuildError("Only imported buffers can have a variable size");
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
    static CompileResult compile(const RenderGraphBuilder& builder,
        const CompileOptions& options) {
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
                    if (!initialVersionReadable(desc, builder.m_logicalResources[
                            version.logicalResourceIndex].historyRole)) {
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
                if (!initialVersionReadable(desc, builder.m_logicalResources[
                        previous.logicalResourceIndex].historyRole)) {
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
            resource.historyPair = source.historyPair;
            resource.historyRole = source.historyRole;
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
        std::vector<FirstUsage> firstUsages(logicalCount);
        for (uint32_t orderIndex = 0; orderIndex < passCount; ++orderIndex) {
            CompiledPass& pass = graph.m_passes[orderIndex];
            pass.firstUsage = static_cast<uint32_t>(graph.m_usages.size());
            for (const auto& usage : builder.m_usages) {
                if (usage.passIndex != pass.sourcePassIndex) {
                    continue;
                }
                const auto& usageVersion =
                    builder.m_resourceVersions[usage.resourceVersionIndex];
                const uint32_t logicalIndex = usageVersion.logicalResourceIndex;
                FirstUsage& first = firstUsages[logicalIndex];
                if (!first.seen) {
                    first = { true, usage.write, usage.loadOp,
                        usage.write && usageVersion.wholeResourceWrite };
                }
                // R4a: a read-only depth attachment preserves its contents
                // (LOAD) and is never stored (NONE).
                const bool readOnlyDepth = !usage.write &&
                    usage.access == Access::DepthAttachmentRead;
                graph.m_usages.push_back({ orderIndex, logicalIndex, usage.access,
                    usage.write, readOnlyDepth ? LoadOp::Load : usage.loadOp,
                    readOnlyDepth ? StoreOp::None : usage.storeOp, usage.clearValue });
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

        for (CompiledResource& resource : graph.m_resources) {
            resource.aliasEligibility = classifyAliasing(resource,
                firstUsages[resource.logicalResourceIndex],
                builder.m_logicalResources[resource.logicalResourceIndex].aliasingExcluded);
        }

        std::vector<uint32_t> reusableResources;
        reusableResources.reserve(logicalCount);
        for (CompiledResource& resource : graph.m_resources) {
            // History pairs live outside the per-frame pool (below).
            if (resource.firstUse == InvalidIndex ||
                resource.desc.lifetime == ResourceLifetime::External ||
                resource.historyPair != InvalidIndex) {
                continue;
            }
            // R4b.3: with transient aliasing, an eligible image keeps its own
            // slot; the aliasing planner shares its memory instead.
            const bool aliased = options.transientAliasing &&
                resource.aliasEligibility == AliasEligibility::Eligible;
            if (!aliased && resource.desc.lifetime == ResourceLifetime::Transient &&
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
            slot.aliased = aliased;
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

        // History pairs: two non-reusable slots each, outside the per-frame
        // pool; pair p owns history slots 2p and 2p+1.
        graph.m_historyPairs.resize(builder.m_historyPairCount);
        for (const CompiledResource& resource : graph.m_resources) {
            if (resource.historyPair == InvalidIndex) continue;
            CompiledHistoryPair& pair = graph.m_historyPairs[resource.historyPair];
            pair.reset = builder.m_logicalResources[resource.logicalResourceIndex].historyReset;
            if (resource.historyRole == HistoryRole::Previous) {
                pair.previousLogical = resource.logicalResourceIndex;
                constexpr std::string_view Suffix = ".previous";
                pair.name = resource.name.substr(0, resource.name.size() - Suffix.size());
            }
            else {
                pair.currentLogical = resource.logicalResourceIndex;
            }
        }
        graph.m_historySlots.reserve(graph.m_historyPairs.size() * 2);
        for (uint32_t pairIndex = 0; pairIndex < graph.m_historyPairs.size(); ++pairIndex) {
            CompiledHistoryPair& pair = graph.m_historyPairs[pairIndex];
            const CompiledResource& previous = graph.m_resources[pair.previousLogical];
            const CompiledResource& current = graph.m_resources[pair.currentLogical];
            for (uint32_t half = 0; half < 2; ++half) {
                PhysicalResourceSlot slot{};
                slot.slotIndex = pairIndex * 2 + half;
                slot.type = current.desc.type;
                slot.image = current.desc.image;
                slot.buffer = current.desc.buffer;
                // Each slot is written as `current` and read as `previous`.
                slot.usages = previous.usages | current.usages;
                slot.lastUse = previous.lastUse == InvalidIndex ? current.lastUse
                    : current.lastUse == InvalidIndex ? previous.lastUse
                    : std::max(previous.lastUse, current.lastUse);
                slot.transientReusable = false;
                slot.logicalResources = { pair.previousLogical, pair.currentLogical };
                pair.slots[half] = slot.slotIndex;
                graph.m_historySlots.push_back(std::move(slot));
            }
        }

        uint64_t hash = FnvOffset;
        for (const auto& resource : builder.m_logicalResources) {
            hashString(hash, resource.name);
            hashResourceDesc(hash, resource.desc);
            // Only pair members hash pair data, so other graphs keep their hashes.
            if (resource.historyPair != InvalidIndex) {
                hashValue(hash, resource.historyPair);
                hashValue(hash, resource.historyRole);
                // M9 G2: hashed only when not the default policy.
                if (resource.historyReset != HistoryReset::OnCut)
                    hashValue(hash, resource.historyReset);
            }
            // R4b.5: hashed only when declared.
            if (resource.aliasingExcluded) {
                const uint8_t excluded = 0xE5;
                hashValue(hash, excluded);
            }
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
            // R4b.3: hashed only when declared, so other graphs keep their hashes.
            if (version.wholeResourceWrite) {
                const uint8_t wholeResource = 0x57;
                hashValue(hash, wholeResource);
            }
        }
        for (const auto& usage : builder.m_usages) {
            hashValue(hash, usage.passIndex);
            hashValue(hash, usage.resourceVersionIndex);
            hashValue(hash, usage.access);
            const uint8_t write = usage.write ? 1 : 0;
            hashValue(hash, write);
            hashValue(hash, usage.loadOp);
            hashValue(hash, usage.storeOp);
            // R4a: clear values are topology (a changed clear rebuilds the
            // plan), hashed only for clearing usages.
            if (usage.loadOp == LoadOp::Clear) hashClearValue(hash, usage.clearValue);
        }
        for (const auto& dependency : builder.m_dependencies) {
            hashValue(hash, dependency.beforePassIndex);
            hashValue(hash, dependency.afterPassIndex);
        }
        // R4b.3: aliased slots are a different physical layout; hashed only
        // when on, so the default compile keeps its hash.
        if (options.transientAliasing) {
            const uint8_t transientAliasing = 0xA1;
            hashValue(hash, transientAliasing);
        }
        graph.m_topologyHash = hash == 0 ? 1 : hash;
        result.graph = std::move(graph);
        return result;
    }
};

CompileResult RenderGraphBuilder::compile() const {
    return CompilerAccess::compile(*this, CompileOptions{});
}

CompileResult RenderGraphBuilder::compile(const CompileOptions& options) const {
    return CompilerAccess::compile(*this, options);
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
    m_pairOfLogical.assign(graph.resources().size(), InvalidIndex);
    for (const HistoryResourceRecord& history : graph.historyResources()) {
        m_isHistory[history.logicalResourceIndex] = 1;
    }
    m_pairCount = static_cast<uint32_t>(graph.historyPairs().size());
    m_activeSet = 0;
    m_pairs.assign(static_cast<size_t>(m_pairCount) * HistoryViewSetCount, PairState{});
    for (uint32_t pairIndex = 0; pairIndex < m_pairCount; ++pairIndex) {
        const CompiledHistoryPair& pair = graph.historyPairs()[pairIndex];
        m_pairOfLogical[pair.previousLogical] = pairIndex;
        m_pairOfLogical[pair.currentLogical] = pairIndex;
        const ResourceDesc& desc = graph.resources()[pair.currentLogical].desc;
        PairState state{};
        state.reset = pair.reset;
        if (desc.type == ResourceType::Image) {
            state.extent = desc.image.extent;
            state.format = desc.image.format;
        }
        else {
            state.extent = { static_cast<uint32_t>(desc.buffer.size),
                static_cast<uint32_t>(desc.buffer.size >> 32), 1 };
        }
        for (uint32_t set = 0; set < HistoryViewSetCount; ++set)
            m_pairs[static_cast<size_t>(set) * m_pairCount + pairIndex] = state;
    }
}

void HistoryValidityTracker::invalidateAll() noexcept {
    std::fill(m_validity.begin(), m_validity.end(), uint8_t{ 0 });
    for (PairState& pair : m_pairs) {
        pair.valid = false;
        pair.written = false;
        pair.writtenLastTurn = false;
    }
}

void HistoryValidityTracker::setValid(uint32_t logicalResourceIndex, bool valid) {
    if (logicalResourceIndex >= m_validity.size() ||
        m_isHistory[logicalResourceIndex] == 0) {
        throw GraphBuildError("History validity update targets a non-history resource");
    }
    const uint32_t pair = m_pairOfLogical[logicalResourceIndex];
    if (pair != InvalidIndex) {
        active(pair).valid = valid;
        if (!valid) active(pair).writtenLastTurn = false;
        return;
    }
    m_validity[logicalResourceIndex] = valid ? 1 : 0;
}

bool HistoryValidityTracker::isValid(uint32_t logicalResourceIndex) const noexcept {
    if (logicalResourceIndex >= m_validity.size() ||
        m_isHistory[logicalResourceIndex] == 0) {
        return false;
    }
    const uint32_t pair = m_pairOfLogical[logicalResourceIndex];
    return pair != InvalidIndex ? active(pair).valid
                                : m_validity[logicalResourceIndex] != 0;
}

void HistoryValidityTracker::beginFrame(const ViewHistoryContext& view) {
    if (view.historySet >= HistoryViewSetCount) {
        throw GraphBuildError("History view set is out of range");
    }
    m_activeSet = view.historySet;
    for (uint32_t index = 0; index < m_pairCount; ++index) {
        PairState& pair = active(index);
        const HistoryValidityKey key{ view.identity,
            pair.reset == HistoryReset::OnCut ? view.resetRevision : 0u,
            pair.extent, pair.format, m_topologyHash };
        // Identity 0 is "no view": nothing is valid under it.
        pair.valid = view.identity != 0 && pair.keyed && pair.key == key &&
            pair.writtenLastTurn;
        pair.key = key;
        pair.keyed = true;
        pair.written = false;
    }
}

void HistoryValidityTracker::markWritten(uint32_t pair) {
    if (pair >= m_pairCount) {
        throw GraphBuildError("History write targets an unknown pair");
    }
    active(pair).written = true;
}

void HistoryValidityTracker::endFrame() noexcept {
    for (uint32_t index = 0; index < m_pairCount; ++index) {
        PairState& pair = active(index);
        pair.writtenLastTurn = pair.written;
        pair.written = false;
    }
}

bool HistoryValidityTracker::pairValid(uint32_t pair) const noexcept {
    return pair < m_pairCount && active(pair).valid;
}

const HistoryValidityKey& HistoryValidityTracker::pairKey(uint32_t pair) const {
    if (pair >= m_pairCount) {
        throw GraphBuildError("History key requested for an unknown pair");
    }
    return active(pair).key;
}

} // namespace Iridium::RenderGraph
