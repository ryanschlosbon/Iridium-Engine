#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "renderer/graph/ViewHistory.h"

namespace Iridium::RenderGraph {

    inline constexpr uint32_t InvalidIndex = UINT32_MAX;

    struct ResourceHandle {
        uint32_t index = InvalidIndex;
        uint32_t generation = 0;

        [[nodiscard]] constexpr bool isValid() const noexcept {
            return index != InvalidIndex;
        }

        friend constexpr bool operator==(ResourceHandle, ResourceHandle) = default;
    };

    struct PassHandle {
        uint32_t index = InvalidIndex;
        uint32_t generation = 0;

        [[nodiscard]] constexpr bool isValid() const noexcept {
            return index != InvalidIndex;
        }

        friend constexpr bool operator==(PassHandle, PassHandle) = default;
    };

    // M7R R3b: execution-time identities in a compiled graph. A PassId is the
    // compiled (execution) order index; a GraphResourceId is the logical
    // resource index. Both are resolved once per rebuild and are only valid for
    // the compiled graph they were resolved against.
    struct PassId {
        uint32_t order = InvalidIndex;

        [[nodiscard]] constexpr bool isValid() const noexcept {
            return order != InvalidIndex;
        }

        friend constexpr bool operator==(PassId, PassId) = default;
    };

    struct GraphResourceId {
        uint32_t logical = InvalidIndex;

        [[nodiscard]] constexpr bool isValid() const noexcept {
            return logical != InvalidIndex;
        }

        friend constexpr bool operator==(GraphResourceId, GraphResourceId) = default;
    };

    enum class ResourceType : uint8_t {
        Image,
        Buffer,
    };

    enum class ResourceLifetime : uint8_t {
        Transient,
        Persistent,
        History,
        External,
    };

    enum class QueueClass : uint8_t {
        Graphics,
        Compute,
        Transfer,
    };

    enum class Format : uint8_t {
        Undefined,
        Rgba8Unorm,
        Bgra8Srgb,
        Rgb10A2Unorm,
        Rgba16Float,
        R16Float,
        Rg16Snorm,
        R11G11B10Float,
        R16Uint,
        R32Uint,
        R32Float,
        D32Float,
    };

    enum class Access : uint8_t {
        Undefined,
        ColorAttachment,
        DepthAttachmentWrite,
        DepthAttachmentRead,
        SampledRead,
        StorageRead,
        StorageWrite,
        StorageReadWrite,
        TransferSource,
        TransferDestination,
        VertexRead,
        IndexRead,
        IndirectRead,
        Present,
    };

    enum class LoadOp : uint8_t {
        DontCare,
        Clear,
        Load,
    };

    // R4a: None leaves the attachment untouched by the store (Vulkan 1.3
    // STORE_OP_NONE); it is implied for DepthAttachmentRead. A write declared
    // with None is treated like DontCare: later reads see no contents.
    enum class StoreOp : uint8_t {
        DontCare,
        Store,
        None,
    };

    // R4a: the bit-exact clear value of a LoadOp::Clear attachment usage.
    // Colour channels are stored as raw 32-bit patterns (float, int or uint
    // per the attachment format), so the value the executor hands Vulkan is
    // exactly the declared one. Hashed only when the usage clears.
    struct ClearValue {
        std::array<uint32_t, 4> colorBits{};
        float depth = 1.0f;
        uint32_t stencil = 0;

        [[nodiscard]] static constexpr ClearValue color(float red, float green,
            float blue, float alpha) noexcept {
            ClearValue value{};
            value.colorBits = { std::bit_cast<uint32_t>(red),
                std::bit_cast<uint32_t>(green), std::bit_cast<uint32_t>(blue),
                std::bit_cast<uint32_t>(alpha) };
            return value;
        }
        [[nodiscard]] static constexpr ClearValue colorUint(uint32_t red,
            uint32_t green = 0, uint32_t blue = 0, uint32_t alpha = 0) noexcept {
            ClearValue value{};
            value.colorBits = { red, green, blue, alpha };
            return value;
        }
        [[nodiscard]] static constexpr ClearValue depthStencil(float depth,
            uint32_t stencil = 0) noexcept {
            ClearValue value{};
            value.depth = depth;
            value.stencil = stencil;
            return value;
        }

        // Bit-exact: -0.0 and +0.0 differ; NaN payloads compare by bits.
        friend constexpr bool operator==(const ClearValue& left,
            const ClearValue& right) noexcept {
            return left.colorBits == right.colorBits &&
                std::bit_cast<uint32_t>(left.depth) == std::bit_cast<uint32_t>(right.depth) &&
                left.stencil == right.stencil;
        }
    };

    using UsageMask = uint64_t;

    [[nodiscard]] constexpr UsageMask usageBit(Access access) noexcept {
        return access == Access::Undefined
            ? UsageMask{ 0 }
            : UsageMask{ 1 } << static_cast<uint8_t>(access);
    }

    struct Extent3D {
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t depth = 1;

        friend constexpr bool operator==(Extent3D, Extent3D) = default;
    };

    struct ImageDesc {
        Format format = Format::Undefined;
        Extent3D extent{};
        uint16_t mipLevels = 1;
        uint16_t arrayLayers = 1;
        uint8_t samples = 1;

        friend constexpr bool operator==(const ImageDesc&, const ImageDesc&) = default;
    };

    struct BufferDesc {
        uint64_t size = 0;
        uint32_t alignment = 1;
        // Imported buffers only (R3b): `size` is the minimum; barriers cover the
        // bound size, so capacity growth leaves the topology hash unchanged.
        bool variableSize = false;

        friend constexpr bool operator==(const BufferDesc&, const BufferDesc&) = default;
    };

    struct ResourceDesc {
        ResourceType type = ResourceType::Image;
        ResourceLifetime lifetime = ResourceLifetime::Transient;
        ImageDesc image{};
        BufferDesc buffer{};
        Access initialAccess = Access::Undefined;
        bool imported = false;

        friend constexpr bool operator==(const ResourceDesc&, const ResourceDesc&) = default;
    };

    struct GraphCapacity {
        uint32_t maxPasses = 128;
        uint32_t maxLogicalResources = 128;
        uint32_t maxResourceVersions = 256;
        uint32_t maxUsages = 512;
        uint32_t maxDependencies = 512;
    };

    class GraphBuildError final : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    enum class DiagnosticCode : uint8_t {
        InvalidDescriptor,
        ReadBeforeWrite,
        InvalidExport,
        Cycle,
        InvalidUsage,
    };

    struct GraphDiagnostic {
        DiagnosticCode code = DiagnosticCode::InvalidUsage;
        std::string message;
    };

    struct CompiledPass {
        uint32_t sourcePassIndex = InvalidIndex;
        std::string name;
        QueueClass queue = QueueClass::Graphics;
        uint32_t firstUsage = 0;
        uint32_t usageCount = 0;
    };

    struct CompiledUsage {
        uint32_t passOrderIndex = InvalidIndex;
        uint32_t logicalResourceIndex = InvalidIndex;
        Access access = Access::Undefined;
        bool write = false;
        LoadOp loadOp = LoadOp::DontCare;
        StoreOp storeOp = StoreOp::Store;
        // Meaningful only when loadOp == Clear (otherwise the default value).
        // A DepthAttachmentRead usage compiles to Load + None (R4a).
        ClearValue clearValue{};
    };

    // R3b.10: the two logical halves of a History pair.
    enum class HistoryRole : uint8_t {
        None,
        Previous,   // read-only: last frame's `current`
        Current,    // written this frame
    };

    // R4b.3: whether a logical resource's memory may be aliased with other
    // transients (RenderGraphAliasing.h). Only Eligible resources are placed;
    // every other value names the first rule that excludes the resource.
    enum class AliasEligibility : uint8_t {
        Eligible,
        Unused,          // never used by a pass
        Imported,        // External lifetime: memory owned outside the graph
        History,         // a History pair member (persists across frames)
        NotTransient,    // Persistent lifetime
        Exported,        // contents leave the graph
        Buffer,          // buffers stay dedicated (images only for now)
        // The first use neither clears nor is a declared whole-resource
        // DontCare write (RenderGraphBuilder::declareWholeResourceWrite), so
        // the first user could observe the previous occupant's bytes.
        FirstUseNotDiscard,
        // R4b.5: declared by RenderGraphBuilder::excludeFromAliasing (a reader
        // may run in a frame whose first-use writer is skipped).
        Excluded,
    };

    [[nodiscard]] const char* aliasEligibilityName(AliasEligibility eligibility) noexcept;

    struct CompiledResource {
        uint32_t logicalResourceIndex = InvalidIndex;
        std::string name;
        ResourceDesc desc{};
        uint32_t firstUse = InvalidIndex;
        uint32_t lastUse = InvalidIndex;
        // Per-frame pool slot; InvalidIndex for imported and History-pair resources.
        uint32_t physicalSlot = InvalidIndex;
        UsageMask usages = 0;
        bool exported = false;
        Access finalAccess = Access::Undefined;
        uint32_t historyPair = InvalidIndex;
        HistoryRole historyRole = HistoryRole::None;
        // R4b.3: computed for every compile, whatever CompileOptions say.
        AliasEligibility aliasEligibility = AliasEligibility::Unused;
    };

    // A History pair: two linked logical resources over two non-reusable
    // physical slots outside the per-frame pool (CompiledGraph::historySlots).
    // Which slot is `current` alternates each frame the writer runs.
    struct CompiledHistoryPair {
        std::string name;
        uint32_t previousLogical = InvalidIndex;
        uint32_t currentLogical = InvalidIndex;
        std::array<uint32_t, 2> slots{ InvalidIndex, InvalidIndex };
        HistoryReset reset = HistoryReset::OnCut;
    };

    struct CompiledTransition {
        uint32_t passOrderIndex = InvalidIndex;
        uint32_t logicalResourceIndex = InvalidIndex;
        Access before = Access::Undefined;
        Access after = Access::Undefined;
    };

    struct PhysicalResourceSlot {
        uint32_t slotIndex = InvalidIndex;
        ResourceType type = ResourceType::Image;
        ImageDesc image{};
        BufferDesc buffer{};
        UsageMask usages = 0;
        uint32_t lastUse = InvalidIndex;
        bool transientReusable = false;
        // R4b.3 (CompileOptions::transientAliasing only): the slot holds exactly
        // one aliasing-eligible transient image, whose memory the aliasing
        // planner places in an alias heap instead of a dedicated allocation.
        bool aliased = false;
        std::vector<uint32_t> logicalResources;
    };

    struct HistoryResourceRecord {
        uint32_t logicalResourceIndex = InvalidIndex;
    };

    class CompiledGraph {
    public:
        [[nodiscard]] uint64_t topologyHash() const noexcept { return m_topologyHash; }
        [[nodiscard]] const std::vector<CompiledPass>& passes() const noexcept {
            return m_passes;
        }
        [[nodiscard]] const std::vector<CompiledResource>& resources() const noexcept {
            return m_resources;
        }
        [[nodiscard]] const std::vector<CompiledUsage>& usages() const noexcept {
            return m_usages;
        }
        [[nodiscard]] const std::vector<CompiledTransition>& transitions() const noexcept {
            return m_transitions;
        }
        [[nodiscard]] const std::vector<PhysicalResourceSlot>& physicalSlots() const noexcept {
            return m_physicalSlots;
        }
        [[nodiscard]] const std::vector<HistoryResourceRecord>& historyResources() const noexcept {
            return m_historyResources;
        }
        [[nodiscard]] const std::vector<CompiledHistoryPair>& historyPairs() const noexcept {
            return m_historyPairs;
        }
        // Two per pair (pair p owns slots 2p and 2p+1); never in physicalSlots().
        [[nodiscard]] const std::vector<PhysicalResourceSlot>& historySlots() const noexcept {
            return m_historySlots;
        }

    private:
        friend struct CompileResult;
        friend class RenderGraphBuilder;
        friend struct CompilerAccess;

        uint64_t m_topologyHash = 0;
        std::vector<CompiledPass> m_passes;
        std::vector<CompiledUsage> m_usages;
        std::vector<CompiledResource> m_resources;
        std::vector<CompiledTransition> m_transitions;
        std::vector<PhysicalResourceSlot> m_physicalSlots;
        std::vector<HistoryResourceRecord> m_historyResources;
        std::vector<CompiledHistoryPair> m_historyPairs;
        std::vector<PhysicalResourceSlot> m_historySlots;
    };

    struct CompileOptions {
        // R4b.3: give every aliasing-eligible transient image its own physical
        // slot (PhysicalResourceSlot::aliased) so the aliasing planner can share
        // memory between them; exact-descriptor slot reuse then applies only to
        // the remaining transients. Off (the default) compiles exactly as
        // before. Part of the topology hash only when set.
        bool transientAliasing = false;
    };

    struct CompileResult {
        std::optional<CompiledGraph> graph;
        std::vector<GraphDiagnostic> diagnostics;

        [[nodiscard]] bool succeeded() const noexcept {
            return graph.has_value() && diagnostics.empty();
        }
    };

    class RenderGraphBuilder {
    public:
        explicit RenderGraphBuilder(GraphCapacity capacity = {});

        void reset();

        [[nodiscard]] ResourceHandle createResource(std::string name,
            const ResourceDesc& desc);
        // R3b.10 History lifetime: "<name>.previous" (read-only, last frame's
        // contents) and "<name>.current" (must be written before it is read).
        // `desc` must not be imported; its lifetime becomes History. `reset`
        // is the pair's cut policy (M9 G2).
        struct HistoryHandles {
            ResourceHandle previous;
            ResourceHandle current;
            uint32_t pair = InvalidIndex;
        };
        [[nodiscard]] HistoryHandles createHistory(std::string name,
            const ResourceDesc& desc, HistoryReset reset = HistoryReset::OnCut);
        [[nodiscard]] PassHandle addPass(std::string name,
            QueueClass queue = QueueClass::Graphics);

        void read(PassHandle pass, ResourceHandle resource, Access access);
        [[nodiscard]] ResourceHandle write(PassHandle pass,
            ResourceHandle previousVersion, Access access,
            LoadOp loadOp = LoadOp::DontCare,
            StoreOp storeOp = StoreOp::Store);
        // R4a: an attachment write with its clear value. The value is kept
        // only for LoadOp::Clear (otherwise the default) and is part of the
        // topology hash only then.
        [[nodiscard]] ResourceHandle write(PassHandle pass,
            ResourceHandle previousVersion, Access access, LoadOp loadOp,
            StoreOp storeOp, const ClearValue& clearValue);
        void addDependency(PassHandle before, PassHandle after);
        void exportResource(ResourceHandle resource, Access finalAccess);
        // R4b.3: asserts that the write producing `writtenVersion` overwrites
        // every texel of every subresource before anything (including the
        // writing pass itself) reads it, so the previous contents never matter.
        // A transient image whose first use is such a write may be aliased.
        // The write must not load (LoadOp::Load). Hashed only when declared.
        void declareWholeResourceWrite(ResourceHandle writtenVersion);
        // R4b.5: keeps a transient image out of aliasing whatever its first
        // use, for resources a pass may read in frames whose first-use writer
        // is skipped (the executor rejects reading an aliased image before
        // its writer ran). Any version of the resource names it. Hashed only
        // when declared.
        void excludeFromAliasing(ResourceHandle resource);

        [[nodiscard]] CompileResult compile() const;
        [[nodiscard]] CompileResult compile(const CompileOptions& options) const;
        [[nodiscard]] uint32_t generation() const noexcept { return m_generation; }

    private:
        struct PassRecord {
            std::string name;
            QueueClass queue = QueueClass::Graphics;
        };

        struct LogicalResourceRecord {
            std::string name;
            ResourceDesc desc{};
            uint32_t historyPair = InvalidIndex;
            HistoryRole historyRole = HistoryRole::None;
            HistoryReset historyReset = HistoryReset::OnCut;
            bool aliasingExcluded = false;
        };

        struct ResourceVersionRecord {
            uint32_t logicalResourceIndex = InvalidIndex;
            uint32_t producerPassIndex = InvalidIndex;
            uint32_t previousVersionIndex = InvalidIndex;
            bool preservePrevious = false;
            bool exported = false;
            Access finalAccess = Access::Undefined;
            bool wholeResourceWrite = false;
        };

        struct UsageRecord {
            uint32_t passIndex = InvalidIndex;
            uint32_t resourceVersionIndex = InvalidIndex;
            Access access = Access::Undefined;
            bool write = false;
            LoadOp loadOp = LoadOp::DontCare;
            StoreOp storeOp = StoreOp::Store;
            ClearValue clearValue{};
        };

        struct DependencyRecord {
            uint32_t beforePassIndex = InvalidIndex;
            uint32_t afterPassIndex = InvalidIndex;
        };

        friend struct CompilerAccess;

        void validate(PassHandle pass) const;
        void validate(ResourceHandle resource) const;
        void validateDescriptor(const ResourceDesc& desc) const;
        void requireCapacity(bool condition, std::string_view what) const;

        GraphCapacity m_capacity{};
        uint32_t m_generation = 1;
        std::vector<PassRecord> m_passes;
        std::vector<LogicalResourceRecord> m_logicalResources;
        std::vector<ResourceVersionRecord> m_resourceVersions;
        std::vector<UsageRecord> m_usages;
        std::vector<DependencyRecord> m_dependencies;
        uint32_t m_historyPairCount = 0;
    };

    class CompiledGraphCache {
    public:
        static constexpr size_t Capacity = 8;

        void store(CompiledGraph graph);
        [[nodiscard]] const CompiledGraph* find(uint64_t topologyHash) const noexcept;
        void clear() noexcept;
        [[nodiscard]] size_t size() const noexcept { return m_size; }

    private:
        std::array<std::optional<CompiledGraph>, Capacity> m_entries{};
        size_t m_size = 0;
        size_t m_nextReplacement = 0;
    };

    // A pair's previous contents are valid only under the key they were
    // written with. Buffers use extent {low32(size), high32(size), 1}.
    struct HistoryValidityKey {
        uint64_t identity = 0;
        uint64_t resetRevision = 0;
        Extent3D extent{};
        Format format = Format::Undefined;
        uint64_t topologyHash = 0;

        friend constexpr bool operator==(const HistoryValidityKey&,
            const HistoryValidityKey&) = default;
    };

    class HistoryValidityTracker {
    public:
        // Also resets every History pair: nothing is valid after a rebuild.
        void resetForGraph(const CompiledGraph& graph);
        void invalidateAll() noexcept;
        void setValid(uint32_t logicalResourceIndex, bool valid);
        // For a pair member this is the pair's validity (pairValid).
        [[nodiscard]] bool isValid(uint32_t logicalResourceIndex) const noexcept;
        [[nodiscard]] uint64_t topologyHash() const noexcept { return m_topologyHash; }

        // Pair lifetime (R3b.10; per view set since M9 G2). beginFrame selects
        // the view's history set. A pair's previous contents in that set are
        // valid iff its writer ran on the set's previous turn (the last frame
        // that rendered this view, not necessarily the last frame) under an
        // identical key; a key change invalidates. OnCut pairs key on the
        // view's resetRevision, SurviveCut pairs do not. Other sets are left
        // untouched, so alternating views keep their history.
        void beginFrame(const ViewHistoryContext& view);
        void markWritten(uint32_t pair);
        void endFrame() noexcept;
        [[nodiscard]] bool pairValid(uint32_t pair) const noexcept;
        [[nodiscard]] size_t pairCount() const noexcept { return m_pairCount; }
        [[nodiscard]] uint32_t activeSet() const noexcept { return m_activeSet; }
        [[nodiscard]] const HistoryValidityKey& pairKey(uint32_t pair) const;

    private:
        struct PairState {
            Extent3D extent{};
            Format format = Format::Undefined;
            HistoryReset reset = HistoryReset::OnCut;
            HistoryValidityKey key{};
            bool keyed = false;
            bool valid = false;
            bool written = false;
            bool writtenLastTurn = false;
        };
        [[nodiscard]] PairState& active(uint32_t pair) noexcept {
            return m_pairs[static_cast<size_t>(m_activeSet) * m_pairCount + pair];
        }
        [[nodiscard]] const PairState& active(uint32_t pair) const noexcept {
            return m_pairs[static_cast<size_t>(m_activeSet) * m_pairCount + pair];
        }

        uint64_t m_topologyHash = 0;
        std::vector<uint8_t> m_validity;
        std::vector<uint8_t> m_isHistory;
        std::vector<uint32_t> m_pairOfLogical;
        // HistoryViewSetCount sets of m_pairCount states, set-major.
        std::vector<PairState> m_pairs;
        uint32_t m_pairCount = 0;
        uint32_t m_activeSet = 0;
    };

} // namespace Iridium::RenderGraph
