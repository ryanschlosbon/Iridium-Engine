#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

    enum class StoreOp : uint8_t {
        DontCare,
        Store,
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
    };

    struct CompiledResource {
        uint32_t logicalResourceIndex = InvalidIndex;
        std::string name;
        ResourceDesc desc{};
        uint32_t firstUse = InvalidIndex;
        uint32_t lastUse = InvalidIndex;
        uint32_t physicalSlot = InvalidIndex;
        UsageMask usages = 0;
        bool exported = false;
        Access finalAccess = Access::Undefined;
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
        [[nodiscard]] PassHandle addPass(std::string name,
            QueueClass queue = QueueClass::Graphics);

        void read(PassHandle pass, ResourceHandle resource, Access access);
        [[nodiscard]] ResourceHandle write(PassHandle pass,
            ResourceHandle previousVersion, Access access,
            LoadOp loadOp = LoadOp::DontCare,
            StoreOp storeOp = StoreOp::Store);
        void addDependency(PassHandle before, PassHandle after);
        void exportResource(ResourceHandle resource, Access finalAccess);

        [[nodiscard]] CompileResult compile() const;
        [[nodiscard]] uint32_t generation() const noexcept { return m_generation; }

    private:
        struct PassRecord {
            std::string name;
            QueueClass queue = QueueClass::Graphics;
        };

        struct LogicalResourceRecord {
            std::string name;
            ResourceDesc desc{};
        };

        struct ResourceVersionRecord {
            uint32_t logicalResourceIndex = InvalidIndex;
            uint32_t producerPassIndex = InvalidIndex;
            uint32_t previousVersionIndex = InvalidIndex;
            bool preservePrevious = false;
            bool exported = false;
            Access finalAccess = Access::Undefined;
        };

        struct UsageRecord {
            uint32_t passIndex = InvalidIndex;
            uint32_t resourceVersionIndex = InvalidIndex;
            Access access = Access::Undefined;
            bool write = false;
            LoadOp loadOp = LoadOp::DontCare;
            StoreOp storeOp = StoreOp::Store;
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

    class HistoryValidityTracker {
    public:
        void resetForGraph(const CompiledGraph& graph);
        void invalidateAll() noexcept;
        void setValid(uint32_t logicalResourceIndex, bool valid);
        [[nodiscard]] bool isValid(uint32_t logicalResourceIndex) const noexcept;
        [[nodiscard]] uint64_t topologyHash() const noexcept { return m_topologyHash; }

    private:
        uint64_t m_topologyHash = 0;
        std::vector<uint8_t> m_validity;
        std::vector<uint8_t> m_isHistory;
    };

} // namespace Iridium::RenderGraph
