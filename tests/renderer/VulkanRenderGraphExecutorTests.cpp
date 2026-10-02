#include "renderer/vulkan/VulkanProductionRenderGraph.h"
#include "renderer/vulkan/VulkanRenderGraphExecutor.h"
#include "renderer/lighting/ClusteredLighting.h"
#include "renderer/rhi/ShadowTypes.h"
#include "renderer/transparency/TransparencyPyramidResidency.h"
#include "renderer/transparency/WeightedOit.h"

#include <cstdint>
#include <array>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <tuple>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    class FakeResourceFactory final : public VulkanGraphResourceFactory {
    public:
        VulkanGraphPhysicalResource create(
            const RenderGraph::PhysicalResourceSlot& slot) override {
            if (createCount == failureAtCreateCount) {
                throw std::runtime_error("injected allocation failure");
            }

            ++createCount;
            const uintptr_t base = createCount * 8 + 1;
            VulkanGraphPhysicalResource result{};
            result.type = slot.type;
            if (slot.type == RenderGraph::ResourceType::Image) {
                result.image.image = reinterpret_cast<VkImage>(base);
                result.image.memory = reinterpret_cast<VkDeviceMemory>(base + 1);
                result.image.view = reinterpret_cast<VkImageView>(base + 2);
                result.image.allocation.requestedBytes = 4096;
                result.image.allocation.committedBytes = 8192;
            }
            else {
                result.buffer.buffer = reinterpret_cast<VkBuffer>(base);
                result.buffer.memory = reinterpret_cast<VkDeviceMemory>(base + 1);
                result.buffer.allocation.requestedBytes = 4096;
                result.buffer.allocation.committedBytes = 8192;
            }
            return result;
        }

        void destroy(VulkanGraphPhysicalResource& resource) noexcept override {
            if (resource.isValid()) {
                ++destroyCount;
            }
            resource = {};
        }

        size_t createCount = 0;
        size_t destroyCount = 0;
        size_t failureAtCreateCount = std::numeric_limits<size_t>::max();
    };

    bool testOptionalOcclusionDepthPyramid() {
        const auto makeGraph = [](VkExtent2D extent, bool enabled) {
            return buildVulkanProductionRenderGraph(extent,
                VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, false,
                GBufferLayout::CanonicalReference, {}, 4096, 8192, false,
                {}, { .depthPyramid = enabled });
        };
        const auto disabled = makeGraph({127, 73}, false);
        CHECK(std::ranges::none_of(disabled.resources(), [](const auto& resource) {
            return resource.name == "depth.occlusion-pyramid";
        }));
        const auto enabled = makeGraph({127, 73}, true);
        CHECK(enabled.resources().size() == disabled.resources().size());
        CHECK(enabled.passes().size() == disabled.passes().size() + 2);
        CHECK(std::ranges::none_of(enabled.resources(), [](const auto& resource) {
            return resource.name == "depth.occlusion-pyramid";
        }));
        const auto pass = [&](std::string_view name) {
            return std::ranges::find_if(enabled.passes(), [&](const auto& value) {
                return value.name == name;
            });
        };
        CHECK(pass("depth.occlusion-pyramid.build") <
            pass("depth.occlusion-pyramid.validation-readback-hook"));
        CHECK(pass("depth.occlusion-pyramid.validation-readback-hook") <
            pass("transparent.sorted.forward"));
        bool rejected = false;
        try { (void)makeGraph({65536, 1}, true); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        return true;
    }

    bool testAccessAndFormatMappings() {
        const VulkanGraphAccessInfo color = getVulkanGraphAccessInfo(
            RenderGraph::Access::ColorAttachment,
            RenderGraph::ResourceType::Image);
        CHECK(color.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        CHECK((color.stages & VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT) != 0);
        CHECK((color.access & VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT) != 0);

        const VulkanGraphAccessInfo sampled = getVulkanGraphAccessInfo(
            RenderGraph::Access::SampledRead, RenderGraph::ResourceType::Image);
        CHECK(sampled.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CHECK((sampled.access & VK_ACCESS_SHADER_READ_BIT) != 0);

        const VulkanGraphAccessInfo storage = getVulkanGraphAccessInfo(
            RenderGraph::Access::StorageWrite, RenderGraph::ResourceType::Image);
        CHECK(storage.layout == VK_IMAGE_LAYOUT_GENERAL);
        CHECK((storage.access & VK_ACCESS_SHADER_WRITE_BIT) != 0);

        const VulkanGraphAccessInfo present = getVulkanGraphAccessInfo(
            RenderGraph::Access::Present, RenderGraph::ResourceType::Image);
        CHECK(present.layout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

        const VulkanGraphAccessInfo buffer = getVulkanGraphAccessInfo(
            RenderGraph::Access::StorageRead, RenderGraph::ResourceType::Buffer);
        CHECK(buffer.layout == VK_IMAGE_LAYOUT_UNDEFINED);
        const VulkanGraphAccessInfo readWrite = getVulkanGraphAccessInfo(
            RenderGraph::Access::StorageReadWrite,
            RenderGraph::ResourceType::Buffer);
        CHECK((readWrite.access & VK_ACCESS_SHADER_READ_BIT) != 0);
        CHECK((readWrite.access & VK_ACCESS_SHADER_WRITE_BIT) != 0);

        constexpr RenderGraph::Format formats[] = {
            RenderGraph::Format::Rgba8Unorm,
            RenderGraph::Format::Bgra8Srgb,
            RenderGraph::Format::Rgb10A2Unorm,
            RenderGraph::Format::Rgba16Float,
            RenderGraph::Format::R16Float,
            RenderGraph::Format::R32Uint,
            RenderGraph::Format::R32Float,
            RenderGraph::Format::D32Float,
        };
        for (const RenderGraph::Format format : formats) {
            CHECK(toGraphFormat(toVkFormat(format)) == format);
        }
        return true;
    }

    bool testProductionTopologyContract() {
        const RenderGraph::CompiledGraph graph = buildVulkanProductionRenderGraph(
            { 3840, 2160 }, VK_FORMAT_B8G8R8A8_SRGB);
        CHECK(graph.passes().size() == 19);
        CHECK(graph.resources().size() == 26);
        CHECK(!graph.transitions().empty());
        CHECK(graph.passes().front().name == "shadow.directional");
        CHECK(graph.passes().back().name == "ui-present");
        CHECK(graph.passes()[1].name == "shadow.spot");
        CHECK(graph.passes()[2].name == "shadow.point");
        CHECK(graph.passes()[3].name == "gbuffer");
        CHECK(graph.passes()[4].name == "lighting.cluster.clear");
        CHECK(graph.passes()[8].name == "lighting.cluster.finalize");
        CHECK(graph.passes()[9].name == "lighting.cluster.readback");
        CHECK(graph.passes()[11].name == "forward-opaque");
        CHECK(graph.passes()[12].name == "transparent.refraction-pyramids");
        CHECK(graph.passes()[12].queue == RenderGraph::QueueClass::Compute);
        CHECK(graph.passes()[13].name == "transparent.sorted.forward");
        CHECK(graph.passes()[14].name ==
            "transparent.compatibility.forward");
        bool sortedReadsDepth = false;
        bool sortedLoadsSceneColor = false;
        const RenderGraph::CompiledPass& sortedPass = graph.passes()[13];
        for (uint32_t usageIndex = sortedPass.firstUsage;
            usageIndex < sortedPass.firstUsage + sortedPass.usageCount;
            ++usageIndex) {
            const RenderGraph::CompiledUsage& usage =
                graph.usages()[usageIndex];
            const std::string& resourceName =
                graph.resources()[usage.logicalResourceIndex].name;
            if (resourceName == "depth.opaque") {
                CHECK(!usage.write);
                CHECK(usage.access ==
                    RenderGraph::Access::DepthAttachmentRead);
                sortedReadsDepth = true;
            }
            if (resourceName == "scene.color") {
                CHECK(usage.write);
                CHECK(usage.access ==
                    RenderGraph::Access::ColorAttachment);
                CHECK(usage.loadOp == RenderGraph::LoadOp::Load);
                sortedLoadsSceneColor = true;
            }
        }
        CHECK(sortedReadsDepth);
        CHECK(sortedLoadsSceneColor);
        for (size_t passIndex = 4; passIndex <= 8; ++passIndex) {
            CHECK(graph.passes()[passIndex].queue ==
                RenderGraph::QueueClass::Compute);
        }

        const uint64_t clusterCount = 120ull * 68ull *
            kClusterDepthSlices;
        const auto findResource = [&](std::string_view name) {
            return std::find_if(graph.resources().begin(), graph.resources().end(),
                [name](const RenderGraph::CompiledResource& resource) {
                    return resource.name == name;
                });
        };
        const auto headers = findResource(kClusterHeaderResourceName);
        const auto indices = findResource(kClusterIndexResourceName);
        const auto shadow = findResource("shadow.directional");
        const auto spotShadow = findResource("shadow.spot");
        CHECK(shadow != graph.resources().end());
        CHECK(shadow->desc.lifetime ==
            RenderGraph::ResourceLifetime::External);
        CHECK(shadow->desc.imported);
        CHECK(shadow->desc.image.arrayLayers ==
            kDirectionalShadowLayerCount);
        CHECK(shadow->desc.image.extent.width == 4096);
        CHECK(shadow->desc.image.extent.height == 4096);
        CHECK(shadow->physicalSlot == RenderGraph::InvalidIndex);
        CHECK(spotShadow != graph.resources().end());
        CHECK(spotShadow->desc.lifetime ==
            RenderGraph::ResourceLifetime::External);
        CHECK(spotShadow->desc.imported);
        CHECK(spotShadow->desc.image.extent.width == 8192);
        CHECK(spotShadow->desc.image.extent.height == 8192);
        CHECK(spotShadow->physicalSlot == RenderGraph::InvalidIndex);
        for (const auto [name, resolution, capacity] : std::array{
                std::tuple{ "shadow.point.256", 256u,
                    kPointShadowPool256Capacity },
                std::tuple{ "shadow.point.512", 512u,
                    kPointShadowPool512Capacity },
                std::tuple{ "shadow.point.1024", 1024u,
                    kPointShadowPool1024Capacity } }) {
            const auto pointShadow = findResource(name);
            CHECK(pointShadow != graph.resources().end());
            CHECK(pointShadow->desc.lifetime ==
                RenderGraph::ResourceLifetime::External);
            CHECK(pointShadow->desc.imported);
            CHECK(pointShadow->desc.image.extent.width == resolution);
            CHECK(pointShadow->desc.image.arrayLayers == capacity * 6u);
            CHECK(pointShadow->physicalSlot == RenderGraph::InvalidIndex);
        }
        CHECK(headers != graph.resources().end());
        CHECK(headers->desc.type == RenderGraph::ResourceType::Buffer);
        CHECK(headers->desc.buffer.size == clusterCount *
            sizeof(ClusterLightHeader));
        CHECK(indices != graph.resources().end());
        CHECK(indices->desc.buffer.size ==
            static_cast<uint64_t>(kMaximumClusterLightReferences) *
                sizeof(uint32_t));
        CHECK((headers->usages & RenderGraph::usageBit(
            RenderGraph::Access::StorageRead)) != 0);
        CHECK((headers->usages & RenderGraph::usageBit(
            RenderGraph::Access::StorageReadWrite)) != 0);
        ClusterGridConfig coarseConfig{};
        coarseConfig.tileWidth = coarseConfig.tileHeight = 32;
        coarseConfig.depthSlices = 32;
        const RenderGraph::CompiledGraph coarse = buildVulkanProductionRenderGraph(
            { 3840, 2160 }, VK_FORMAT_B8G8R8A8_SRGB,
            VK_FORMAT_B8G8R8A8_SRGB, false,
            GBufferLayout::CanonicalReference, coarseConfig);
        const auto coarseHeaders = std::find_if(coarse.resources().begin(),
            coarse.resources().end(), [](const RenderGraph::CompiledResource& resource) {
                return resource.name == kClusterHeaderResourceName;
            });
        CHECK(coarseHeaders != coarse.resources().end());
        CHECK(coarseHeaders->desc.buffer.size ==
            120ull * 68ull * 32ull * sizeof(ClusterLightHeader));
        CHECK(coarse.topologyHash() != graph.topologyHash());

        const RenderGraph::CompiledResource& swapchain = graph.resources().back();
        CHECK(swapchain.desc.lifetime == RenderGraph::ResourceLifetime::External);
        CHECK(swapchain.desc.imported);
        CHECK(swapchain.physicalSlot == RenderGraph::InvalidIndex);
        CHECK(swapchain.exported);
        CHECK(swapchain.finalAccess == RenderGraph::Access::Present);
        const auto colorPyramid = findResource(
            "scene.refraction-color-pyramid");
        const auto depthPyramid = findResource(
            "depth.refraction-nearest-pyramid");
        CHECK(findResource("depth.glass") == graph.resources().end());
        CHECK(std::ranges::none_of(graph.passes(),
            [](const RenderGraph::CompiledPass& pass) {
                return pass.name == "transparent.background.depth" ||
                    pass.name == "transparent.background.forward" ||
                    pass.name == "transparent.foreground.depth" ||
                    pass.name == "transparent.foreground.forward";
            }));
        CHECK(colorPyramid != graph.resources().end());
        CHECK(depthPyramid != graph.resources().end());
        CHECK(colorPyramid->desc.image.format ==
            RenderGraph::Format::Rgba16Float);
        CHECK(depthPyramid->desc.image.format ==
            RenderGraph::Format::R32Float);
        CHECK(colorPyramid->desc.image.mipLevels == 12u);
        CHECK(depthPyramid->desc.image.mipLevels == 12u);
        CHECK((colorPyramid->usages & RenderGraph::usageBit(
            RenderGraph::Access::StorageReadWrite)) != 0);
        CHECK((colorPyramid->usages & RenderGraph::usageBit(
            RenderGraph::Access::SampledRead)) != 0);
        CHECK((depthPyramid->usages & RenderGraph::usageBit(
            RenderGraph::Access::StorageReadWrite)) != 0);
        CHECK((depthPyramid->usages & RenderGraph::usageBit(
            RenderGraph::Access::SampledRead)) != 0);
        const auto output = std::find_if(graph.resources().begin(),
            graph.resources().end(), [](const RenderGraph::CompiledResource& resource) {
                return resource.name == "output.display";
            });
        CHECK(output != graph.resources().end());
        CHECK(output->desc.image.format == RenderGraph::Format::Bgra8Srgb);
        CHECK((output->usages & RenderGraph::usageBit(
            RenderGraph::Access::TransferSource)) != 0);

        // M7R R2: the retired two-bucket topology no longer exists in any
        // configuration; classified transparency keeps one compatibility pass.
        CHECK(std::ranges::none_of(graph.resources(),
            [](const RenderGraph::CompiledResource& resource) {
                return resource.name == "depth.glass";
            }));
        CHECK(std::ranges::none_of(graph.passes(),
            [](const RenderGraph::CompiledPass& pass) {
                return pass.name.starts_with("transparent.background.") ||
                    pass.name.starts_with("transparent.foreground.");
            }));
        CHECK(std::ranges::count_if(graph.passes(),
            [](const RenderGraph::CompiledPass& pass) {
                return pass.name == "transparent.compatibility.forward";
            }) == 1);
        return true;
    }

    bool testHdr10TopologyContract() {
        const RenderGraph::CompiledGraph graph = buildVulkanProductionRenderGraph(
            { 3840, 2160 }, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_FORMAT_R16G16B16A16_SFLOAT, true);
        CHECK(graph.passes().size() == 20);
        CHECK(graph.resources().size() == 27);
        CHECK(graph.passes()[18].name == "ui-compose");
        CHECK(graph.passes().back().name == "hdr10-encode-present");
        const auto composition = std::find_if(graph.resources().begin(),
            graph.resources().end(), [](const RenderGraph::CompiledResource& resource) {
                return resource.name == "output.ui-composition";
            });
        CHECK(composition != graph.resources().end());
        CHECK(composition->desc.image.format == RenderGraph::Format::Rgba16Float);
        CHECK((composition->usages & RenderGraph::usageBit(
            RenderGraph::Access::ColorAttachment)) != 0);
        CHECK((composition->usages & RenderGraph::usageBit(
            RenderGraph::Access::SampledRead)) != 0);
        CHECK(graph.resources().back().desc.image.format ==
            RenderGraph::Format::Rgb10A2Unorm);
        return true;
    }

    bool testSceneAndPresentationExtentSeparation() {
        constexpr VkExtent2D sceneExtent{ 1728, 972 };
        constexpr VkExtent2D presentationExtent{ 3840, 2160 };
        const auto findResource = [](const RenderGraph::CompiledGraph& graph,
                                     std::string_view name) {
            return std::find_if(graph.resources().begin(), graph.resources().end(),
                [name](const RenderGraph::CompiledResource& resource) {
                    return resource.name == name;
                });
        };
        const auto hasExtent = [](const RenderGraph::CompiledResource& resource,
                                  VkExtent2D extent) {
            return resource.desc.image.extent.width == extent.width &&
                resource.desc.image.extent.height == extent.height;
        };

        const RenderGraph::CompiledGraph sdr = buildVulkanProductionRenderGraph(
            sceneExtent, presentationExtent, VK_FORMAT_B8G8R8A8_SRGB);
        for (const std::string_view name : {
                 "gbuffer.albedo", "gbuffer.normal", "gbuffer.emissive",
                 "gbuffer.f0-roughness", "gbuffer.material-flags",
                 "depth.opaque", "scene.color",
                 "scene.refraction-color-pyramid",
                 "depth.refraction-nearest-pyramid",
                 "output.display" }) {
            const auto resource = findResource(sdr, name);
            CHECK(resource != sdr.resources().end());
            CHECK(hasExtent(*resource, sceneExtent));
        }
        const auto sdrSwapchain = findResource(sdr, "swapchain");
        CHECK(findResource(sdr, "depth.glass") == sdr.resources().end());
        CHECK(sdrSwapchain != sdr.resources().end());
        CHECK(hasExtent(*sdrSwapchain, presentationExtent));

        const RenderGraph::CompiledGraph hdr = buildVulkanProductionRenderGraph(
            sceneExtent, presentationExtent,
            VK_FORMAT_A2B10G10R10_UNORM_PACK32,
            VK_FORMAT_R16G16B16A16_SFLOAT, true);
        const auto hdrOutput = findResource(hdr, "output.display");
        const auto hdrComposition = findResource(hdr, "output.ui-composition");
        const auto hdrSwapchain = findResource(hdr, "swapchain");
        CHECK(hdrOutput != hdr.resources().end());
        CHECK(hdrComposition != hdr.resources().end());
        CHECK(hdrSwapchain != hdr.resources().end());
        CHECK(hasExtent(*hdrOutput, sceneExtent));
        CHECK(hasExtent(*hdrComposition, presentationExtent));
        CHECK(hasExtent(*hdrSwapchain, presentationExtent));
        return true;
    }

    bool testConditionalTransparencyPyramidTopology() {
        const RenderGraph::CompiledGraph enabled =
            buildVulkanProductionRenderGraph({ 1920, 1080 },
                VK_FORMAT_B8G8R8A8_SRGB);
        const RenderGraph::CompiledGraph disabled =
            buildVulkanProductionRenderGraph({ 1920, 1080 },
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_FORMAT_B8G8R8A8_SRGB, false,
                GBufferLayout::CanonicalReference, {}, 4096, 8192, false);
        const auto hasResource = [](const RenderGraph::CompiledGraph& graph,
                std::string_view name) {
            return std::any_of(graph.resources().begin(), graph.resources().end(),
                [name](const RenderGraph::CompiledResource& resource) {
                    return resource.name == name;
                });
        };
        const auto hasPass = [](const RenderGraph::CompiledGraph& graph,
                std::string_view name) {
            return std::any_of(graph.passes().begin(), graph.passes().end(),
                [name](const RenderGraph::CompiledPass& pass) {
                    return pass.name == name;
                });
        };
        CHECK(hasPass(enabled, "transparent.refraction-pyramids"));
        CHECK(hasResource(enabled, "scene.refraction-color-pyramid"));
        CHECK(hasResource(enabled, "depth.refraction-nearest-pyramid"));
        CHECK(!hasPass(disabled, "transparent.refraction-pyramids"));
        CHECK(!hasResource(disabled, "scene.refraction-color-pyramid"));
        CHECK(!hasResource(disabled, "depth.refraction-nearest-pyramid"));
        CHECK(!hasPass(enabled, "transparent.background.copy"));
        CHECK(!hasPass(enabled, "transparent.foreground.copy"));
        CHECK(enabled.topologyHash() != disabled.topologyHash());
        CHECK(enabled.resources().size() == disabled.resources().size() + 2u);
        CHECK(enabled.passes().size() == disabled.passes().size() + 1u);
        return true;
    }

    bool testConditionalOrdinary2Topology() {
        constexpr VkExtent2D sceneExtent{ 1920, 1080 };
        constexpr VkExtent2D atlasExtent{ 960, 528 };
        const RenderGraph::CompiledGraph disabled =
            buildVulkanProductionRenderGraph(sceneExtent,
                VK_FORMAT_B8G8R8A8_SRGB);
        const RenderGraph::CompiledGraph enabled =
            buildVulkanProductionRenderGraph(sceneExtent,
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_FORMAT_B8G8R8A8_SRGB, false,
                GBufferLayout::CanonicalReference, {}, 4096, 8192, true,
                VulkanOrdinary2GraphConfig{ atlasExtent });
        const auto findResource = [](const RenderGraph::CompiledGraph& graph,
                std::string_view name) {
            return std::find_if(graph.resources().begin(), graph.resources().end(),
                [name](const RenderGraph::CompiledResource& resource) {
                    return resource.name == name;
                });
        };
        const auto findPass = [](const RenderGraph::CompiledGraph& graph,
                std::string_view name) {
            return std::find_if(graph.passes().begin(), graph.passes().end(),
                [name](const RenderGraph::CompiledPass& pass) {
                    return pass.name == name;
                });
        };
        for (const std::string_view name : {
                 "depth.layered.entry", "identity.layered.entry",
                 "depth.layered.exit", "identity.layered.exit" }) {
            CHECK(findResource(disabled, name) == disabled.resources().end());
            const auto resource = findResource(enabled, name);
            CHECK(resource != enabled.resources().end());
            CHECK(resource->desc.image.extent.width == atlasExtent.width);
            CHECK(resource->desc.image.extent.height == atlasExtent.height);
            CHECK((resource->usages & RenderGraph::usageBit(
                RenderGraph::Access::SampledRead)) != 0u);
            CHECK((resource->usages & RenderGraph::usageBit(
                RenderGraph::Access::TransferSource)) != 0u);
            if (name.starts_with("depth.")) {
                CHECK(resource->desc.image.format ==
                    RenderGraph::Format::D32Float);
                CHECK((resource->usages & RenderGraph::usageBit(
                    RenderGraph::Access::DepthAttachmentWrite)) != 0u);
            }
            else {
                CHECK(resource->desc.image.format ==
                    RenderGraph::Format::R32Uint);
                CHECK((resource->usages & RenderGraph::usageBit(
                    RenderGraph::Access::ColorAttachment)) != 0u);
            }
        }
        const auto localColor = findResource(enabled,
            "scene.layered.local-color");
        CHECK(findResource(disabled, "scene.layered.local-color") ==
            disabled.resources().end());
        CHECK(localColor != enabled.resources().end());
        CHECK(localColor->desc.image.extent.width == atlasExtent.width);
        CHECK(localColor->desc.image.extent.height == atlasExtent.height);
        CHECK(localColor->desc.image.format ==
            RenderGraph::Format::Rgba16Float);
        CHECK((localColor->usages & RenderGraph::usageBit(
            RenderGraph::Access::ColorAttachment)) != 0u);
        CHECK((localColor->usages & RenderGraph::usageBit(
            RenderGraph::Access::SampledRead)) != 0u);
        CHECK((localColor->usages & RenderGraph::usageBit(
            RenderGraph::Access::TransferSource)) != 0u);
        for (const std::string_view name : {
                 "transparent.layered.entry.capture",
                 "transparent.layered.exit.capture",
                 "transparent.layered.validation-readback-hook",
                 "transparent.layered.local-compose",
                 "transparent.layered.compose-hook" }) {
            CHECK(findPass(disabled, name) == disabled.passes().end());
            CHECK(findPass(enabled, name) != enabled.passes().end());
        }
        CHECK(enabled.resources().size() == disabled.resources().size() + 5u);
        CHECK(enabled.passes().size() == disabled.passes().size() + 5u);
        CHECK(enabled.topologyHash() != disabled.topologyHash());

        const auto rejects = [](VulkanOrdinary2GraphConfig config,
                bool pyramids = true) {
            try {
                (void)buildVulkanProductionRenderGraph({ 1920, 1080 },
                    VK_FORMAT_B8G8R8A8_SRGB,
                    VK_FORMAT_B8G8R8A8_SRGB, false,
                    GBufferLayout::CanonicalReference, {}, 4096, 8192,
                    pyramids, config);
                return false;
            }
            catch (const std::invalid_argument&) {
                return true;
            }
        };
        CHECK(rejects(VulkanOrdinary2GraphConfig{ { 960, 544 } }));
        CHECK(rejects(VulkanOrdinary2GraphConfig{ { 959, 528 } }));
        CHECK(rejects(VulkanOrdinary2GraphConfig{ { 960, 528 } }, false));
        CHECK(rejects(VulkanOrdinary2GraphConfig{ { 960, 0 } }));
        return true;
    }

    bool testConditionalDeepLayeredTopology() {
        constexpr VkExtent2D sceneExtent{ 1920, 1080 };
        constexpr VkExtent2D heroExtent{ 1920, 528 };
        constexpr VkExtent2D cinematicExtent{ 1920, 1072 };
        const RenderGraph::CompiledGraph disabled =
            buildVulkanProductionRenderGraph(sceneExtent,
                VK_FORMAT_B8G8R8A8_SRGB);
        const RenderGraph::CompiledGraph hero =
            buildVulkanProductionRenderGraph(sceneExtent,
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_FORMAT_B8G8R8A8_SRGB, false,
                GBufferLayout::CanonicalReference, {}, 4096, 8192, true,
                VulkanLayeredGraphConfig{ {}, heroExtent, {} });
        const RenderGraph::CompiledGraph cinematic =
            buildVulkanProductionRenderGraph(sceneExtent,
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_FORMAT_B8G8R8A8_SRGB, false,
                GBufferLayout::CanonicalReference, {}, 4096, 8192, true,
                VulkanLayeredGraphConfig{ {}, {}, cinematicExtent });
        const RenderGraph::CompiledGraph combined =
            buildVulkanProductionRenderGraph(sceneExtent,
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_FORMAT_B8G8R8A8_SRGB, false,
                GBufferLayout::CanonicalReference, {}, 4096, 8192, true,
                VulkanLayeredGraphConfig{
                    {}, heroExtent, cinematicExtent });

        const auto findResource = [](const RenderGraph::CompiledGraph& graph,
                std::string_view name) {
            return std::find_if(graph.resources().begin(), graph.resources().end(),
                [name](const RenderGraph::CompiledResource& resource) {
                    return resource.name == name;
                });
        };
        const auto hasPass = [](const RenderGraph::CompiledGraph& graph,
                std::string_view name) {
            return std::any_of(graph.passes().begin(), graph.passes().end(),
                [name](const RenderGraph::CompiledPass& pass) {
                    return pass.name == name;
                });
        };
        const auto validateTier = [&](const RenderGraph::CompiledGraph& graph,
                std::string_view tierName, VkExtent2D extent,
                uint32_t interfaceCount) {
            for (uint32_t interfaceIndex = 0u;
                interfaceIndex < interfaceCount; ++interfaceIndex) {
                const std::string suffix = std::string(tierName) +
                    ".interface." + std::to_string(interfaceIndex);
                const auto depth = findResource(graph,
                    "depth.layered." + suffix);
                const auto identity = findResource(graph,
                    "identity.layered." + suffix);
                CHECK(depth != graph.resources().end());
                CHECK(identity != graph.resources().end());
                CHECK(depth->desc.image.extent.width == extent.width);
                CHECK(depth->desc.image.extent.height == extent.height);
                CHECK(depth->desc.image.format ==
                    RenderGraph::Format::D32Float);
                CHECK(identity->desc.image.format ==
                    RenderGraph::Format::R32Uint);
                CHECK((depth->usages & RenderGraph::usageBit(
                    RenderGraph::Access::DepthAttachmentWrite)) != 0u);
                CHECK((depth->usages & RenderGraph::usageBit(
                    RenderGraph::Access::SampledRead)) != 0u);
                CHECK((depth->usages & RenderGraph::usageBit(
                    RenderGraph::Access::TransferSource)) != 0u);
                CHECK((identity->usages & RenderGraph::usageBit(
                    RenderGraph::Access::ColorAttachment)) != 0u);
                CHECK((identity->usages & RenderGraph::usageBit(
                    RenderGraph::Access::SampledRead)) != 0u);
                CHECK((identity->usages & RenderGraph::usageBit(
                    RenderGraph::Access::TransferSource)) != 0u);
                CHECK(hasPass(graph, "transparent.layered." + suffix +
                    ".capture"));
                const auto termination = findResource(graph,
                    "termination.layered." + suffix);
                const bool reduces = deepLayeredTerminationInterface(
                    interfaceIndex, interfaceCount);
                CHECK((termination != graph.resources().end()) == reduces);
                CHECK(hasPass(graph, "transparent.layered." + suffix +
                    ".terminate-tiles") == reduces);
                if (reduces) {
                    CHECK(termination->desc.image.extent.width ==
                        (extent.width + 15u) / 16u);
                    CHECK(termination->desc.image.extent.height ==
                        (extent.height + 15u) / 16u);
                    CHECK(termination->desc.image.format ==
                        RenderGraph::Format::R32Uint);
                    CHECK((termination->usages & RenderGraph::usageBit(
                        RenderGraph::Access::StorageWrite)) != 0u);
                    CHECK((termination->usages & RenderGraph::usageBit(
                        RenderGraph::Access::SampledRead)) != 0u);
                }
            }
            const std::string localName = "scene.layered." +
                std::string(tierName) + ".local-color";
            const auto local = findResource(graph, localName);
            CHECK(local != graph.resources().end());
            CHECK(local->desc.image.extent.width == extent.width);
            CHECK(local->desc.image.extent.height == extent.height);
            CHECK(local->desc.image.format ==
                RenderGraph::Format::Rgba16Float);
            CHECK((local->usages & RenderGraph::usageBit(
                RenderGraph::Access::ColorAttachment)) != 0u);
            CHECK((local->usages & RenderGraph::usageBit(
                RenderGraph::Access::SampledRead)) != 0u);
            CHECK((local->usages & RenderGraph::usageBit(
                RenderGraph::Access::TransferSource)) != 0u);
            CHECK(hasPass(graph, "transparent.layered." +
                std::string(tierName) + ".local-compose"));
            CHECK(hasPass(graph, "transparent.layered." +
                std::string(tierName) + ".validation-readback-hook"));
            return true;
        };

        CHECK(validateTier(hero, "hero4", heroExtent, 4u));
        CHECK(validateTier(cinematic, "cinematic8", cinematicExtent, 8u));
        CHECK(validateTier(combined, "hero4", heroExtent, 4u));
        CHECK(validateTier(combined, "cinematic8", cinematicExtent, 8u));
        CHECK(hasPass(hero,
            "transparent.layered.hero4.compose-hook"));
        CHECK(hasPass(cinematic,
            "transparent.layered.cinematic8.compose-hook"));
        CHECK(hasPass(combined,
            "transparent.layered.deep.compose-hook"));
        CHECK(!hasPass(combined,
            "transparent.layered.hero4.compose-hook"));
        CHECK(!hasPass(combined,
            "transparent.layered.cinematic8.compose-hook"));
        CHECK(findResource(disabled,
            "depth.layered.hero4.interface.0") ==
            disabled.resources().end());
        CHECK(findResource(hero,
            "depth.layered.cinematic8.interface.0") ==
            hero.resources().end());
        CHECK(findResource(hero,
            "depth.layered.hero4.interface.4") == hero.resources().end());
        CHECK(findResource(cinematic,
            "depth.layered.cinematic8.interface.8") ==
            cinematic.resources().end());
        CHECK(hero.resources().size() == disabled.resources().size() + 10u);
        CHECK(hero.passes().size() == disabled.passes().size() + 8u);
        CHECK(cinematic.resources().size() ==
            disabled.resources().size() + 20u);
        CHECK(cinematic.passes().size() == disabled.passes().size() + 14u);
        CHECK(combined.resources().size() ==
            disabled.resources().size() + 30u);
        CHECK(combined.passes().size() ==
            disabled.passes().size() + 21u);
        CHECK(hero.topologyHash() != disabled.topologyHash());
        CHECK(cinematic.topologyHash() != hero.topologyHash());
        CHECK(combined.topologyHash() != cinematic.topologyHash());

        constexpr VulkanLayeredGraphConfig config{
            { 960, 528 }, heroExtent, cinematicExtent };
        CHECK(config.anyEnabled());
        CHECK(config.enabled(TransparencyQuality::Ordinary2));
        CHECK(config.enabled(TransparencyQuality::Hero4));
        CHECK(config.enabled(TransparencyQuality::Cinematic8));
        CHECK(config.atlasExtent(TransparencyQuality::Hero4).height == 528u);
        CHECK(layeredTierLogicalStorageBytes({ 3840, 528 },
            TransparencyQuality::Ordinary2) == 48'660'480u);
        CHECK(layeredTierLogicalStorageBytes({ 3840, 1072 },
            TransparencyQuality::Hero4) == 164'723'520u);
        CHECK(layeredTierLogicalStorageBytes({ 3840, 2160 },
            TransparencyQuality::Cinematic8) == 597'585'600u);
        CHECK(layeredTierLogicalStorageBytes({},
            TransparencyQuality::Hero4) == 0u);

        const auto rejects = [](VulkanLayeredGraphConfig candidate,
                bool pyramids = true) {
            try {
                (void)buildVulkanProductionRenderGraph({ 1920, 1080 },
                    VK_FORMAT_B8G8R8A8_SRGB,
                    VK_FORMAT_B8G8R8A8_SRGB, false,
                    GBufferLayout::CanonicalReference, {}, 4096, 8192,
                    pyramids, candidate);
                return false;
            }
            catch (const std::invalid_argument&) {
                return true;
            }
        };
        CHECK(rejects(VulkanLayeredGraphConfig{
            {}, { 1920, 544 }, {} }));
        CHECK(rejects(VulkanLayeredGraphConfig{
            {}, { 1919, 528 }, {} }));
        CHECK(rejects(VulkanLayeredGraphConfig{
            {}, { 1920, 0 }, {} }));
        CHECK(rejects(VulkanLayeredGraphConfig{
            {}, {}, { 1920, 1088 } }));
        CHECK(rejects(VulkanLayeredGraphConfig{
            {}, heroExtent, {} }, false));
        return true;
    }

    bool testConditionalWeightedOitTopology() {
        constexpr VkExtent2D sceneExtent{ 1920, 1080 };
        const RenderGraph::CompiledGraph disabled =
            buildVulkanProductionRenderGraph(sceneExtent,
                VK_FORMAT_B8G8R8A8_SRGB);
        const RenderGraph::CompiledGraph enabled =
            buildVulkanProductionRenderGraph(sceneExtent,
                VK_FORMAT_B8G8R8A8_SRGB,
                VK_FORMAT_B8G8R8A8_SRGB, false,
                GBufferLayout::CanonicalReference, {}, 4096, 8192, true,
                VulkanLayeredGraphConfig{ {}, {}, {}, true });

        const auto findResource = [](const RenderGraph::CompiledGraph& graph,
                std::string_view name) {
            return std::find_if(graph.resources().begin(), graph.resources().end(),
                [name](const RenderGraph::CompiledResource& resource) {
                    return resource.name == name;
                });
        };
        const auto findPass = [](const RenderGraph::CompiledGraph& graph,
                std::string_view name) {
            return std::find_if(graph.passes().begin(), graph.passes().end(),
                [name](const RenderGraph::CompiledPass& pass) {
                    return pass.name == name;
                });
        };

        CHECK(findResource(disabled, "transparency.oit.accumulation") ==
            disabled.resources().end());
        CHECK(findResource(disabled, "transparency.oit.revealage") ==
            disabled.resources().end());
        CHECK(findPass(disabled, "transparent.oit.accumulate") ==
            disabled.passes().end());
        CHECK(findPass(disabled, "transparent.oit.resolve") ==
            disabled.passes().end());

        const auto accumulation = findResource(enabled,
            "transparency.oit.accumulation");
        const auto revealage = findResource(enabled,
            "transparency.oit.revealage");
        CHECK(accumulation != enabled.resources().end());
        CHECK(revealage != enabled.resources().end());
        CHECK(accumulation->desc.image.extent.width == sceneExtent.width);
        CHECK(accumulation->desc.image.extent.height == sceneExtent.height);
        CHECK(revealage->desc.image.extent.width == sceneExtent.width);
        CHECK(revealage->desc.image.extent.height == sceneExtent.height);
        CHECK(accumulation->desc.image.format ==
            RenderGraph::Format::Rgba16Float);
        CHECK(revealage->desc.image.format == RenderGraph::Format::R16Float);
        for (const RenderGraph::CompiledResource* resource :
                { &*accumulation, &*revealage }) {
            CHECK((resource->usages & RenderGraph::usageBit(
                RenderGraph::Access::ColorAttachment)) != 0u);
            CHECK((resource->usages & RenderGraph::usageBit(
                RenderGraph::Access::SampledRead)) != 0u);
        }

        const auto accumulatePass = findPass(enabled,
            "transparent.oit.accumulate");
        const auto resolvePass = findPass(enabled,
            "transparent.oit.resolve");
        CHECK(accumulatePass != enabled.passes().end());
        CHECK(resolvePass != enabled.passes().end());
        CHECK(accumulatePass < resolvePass);
        const auto bloom = findPass(enabled, "bloom-hook");
        CHECK(bloom != enabled.passes().end());
        CHECK(resolvePass < bloom);

        bool readsOpaqueDepth = false;
        for (uint32_t i = accumulatePass->firstUsage;
            i < accumulatePass->firstUsage + accumulatePass->usageCount; ++i) {
            const RenderGraph::CompiledUsage& usage = enabled.usages()[i];
            if (usage.access == RenderGraph::Access::DepthAttachmentRead) {
                readsOpaqueDepth = true;
                CHECK(!usage.write);
            }
        }
        CHECK(readsOpaqueDepth);
        CHECK(enabled.resources().size() == disabled.resources().size() + 2u);
        CHECK(enabled.passes().size() == disabled.passes().size() + 2u);
        CHECK(enabled.topologyHash() != disabled.topologyHash());
        CHECK(weightedOitLogicalStorageBytes(sceneExtent.width,
            sceneExtent.height) == 20'736'000u);
        CHECK(weightedOitLogicalStorageBytes(3840u, 2160u) == 82'944'000u);
        return true;
    }

    bool testTransparencyPyramidResidencyPolicy() {
        TransparencyPyramidResidency residency(3);
        CHECK(!residency.enabled());
        CHECK(!residency.changePending());

        residency.observe(true);
        CHECK(residency.changePending());
        CHECK(residency.requestedEnabled());
        CHECK(residency.requiresFallback(true));
        residency.publishRequested();
        CHECK(residency.enabled());
        CHECK(!residency.requiresFallback(true));

        residency.observe(false);
        residency.observe(false);
        CHECK(residency.enabled());
        CHECK(!residency.changePending());
        CHECK(residency.inactiveFrames() == 2u);
        residency.observe(true);
        CHECK(residency.inactiveFrames() == 0u);

        residency.observe(false);
        residency.observe(false);
        residency.observe(false);
        CHECK(residency.enabled());
        CHECK(residency.changePending());
        CHECK(!residency.requestedEnabled());
        residency.publishRequested();
        CHECK(!residency.enabled());

        residency.observe(true);
        residency.rejectRequested();
        CHECK(!residency.enabled());
        CHECK(!residency.changePending());
        return true;
    }

    bool testFenceScopedRetirementAndCleanup() {
        const RenderGraph::CompiledGraph graph = buildVulkanProductionRenderGraph(
            { 1920, 1080 }, VK_FORMAT_B8G8R8A8_SRGB);
        const size_t slotCount = graph.physicalSlots().size();
        FakeResourceFactory factory;
        VulkanGraphResourcePool pool;
        pool.init(factory, 2);
        pool.rebuild(graph);
        CHECK(factory.createCount == slotCount * 2);
        CHECK(pool.activeResourceCount(0) == slotCount);
        CHECK(pool.activeResourceCount(1) == slotCount);
        CHECK(pool.retiredResourceCount(0) == 0);
        CHECK(pool.requestedBytes() == slotCount * 2 * 4096);
        CHECK(pool.committedBytes() == slotCount * 2 * 8192);

        pool.rebuild(graph);
        CHECK(pool.retiredResourceCount(0) == slotCount);
        CHECK(pool.retiredResourceCount(1) == slotCount);
        pool.onFrameFenceCompleted(0);
        CHECK(factory.destroyCount == slotCount);
        CHECK(pool.retiredResourceCount(0) == 0);
        CHECK(pool.retiredResourceCount(1) == slotCount);

        pool.cleanupAfterDeviceIdle();
        CHECK(factory.destroyCount == slotCount * 4);
        CHECK(pool.frameCount() == 0);
        return true;
    }

    bool testAllocationFailurePreservesActiveSet() {
        const RenderGraph::CompiledGraph graph = buildVulkanProductionRenderGraph(
            { 1280, 720 }, VK_FORMAT_B8G8R8A8_SRGB);
        const size_t slotCount = graph.physicalSlots().size();
        FakeResourceFactory factory;
        VulkanGraphResourcePool pool;
        pool.init(factory, 2);
        pool.rebuild(graph);
        const size_t firstBuildCount = factory.createCount;
        factory.failureAtCreateCount = firstBuildCount + 3;

        bool threw = false;
        try {
            pool.rebuild(graph);
        }
        catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(factory.destroyCount == 3);
        CHECK(pool.activeResourceCount(0) == slotCount);
        CHECK(pool.activeResourceCount(1) == slotCount);
        CHECK(pool.retiredResourceCount(0) == 0);
        CHECK(pool.retiredResourceCount(1) == 0);

        pool.cleanupAfterDeviceIdle();
        CHECK(factory.destroyCount == 3 + slotCount * 2);
        return true;
    }

    bool testResizeAndBounds() {
        const RenderGraph::CompiledGraph small = buildVulkanProductionRenderGraph(
            { 1280, 720 }, VK_FORMAT_B8G8R8A8_SRGB);
        const RenderGraph::CompiledGraph large = buildVulkanProductionRenderGraph(
            { 3840, 2160 }, VK_FORMAT_B8G8R8A8_SRGB);
        CHECK(small.topologyHash() != large.topologyHash());
        CHECK(small.physicalSlots().front().image.extent.width == 1280);
        CHECK(large.physicalSlots().front().image.extent.width == 3840);

        FakeResourceFactory factory;
        VulkanGraphResourcePool pool;
        pool.init(factory, 2);
        pool.rebuild(small);
        pool.rebuild(large);
        CHECK(pool.activeResourceCount(0) == large.physicalSlots().size());

        bool threw = false;
        try {
            pool.onFrameFenceCompleted(2);
        }
        catch (const std::out_of_range&) {
            threw = true;
        }
        CHECK(threw);
        pool.cleanupAfterDeviceIdle();
        return true;
    }

    bool testExecutorCacheBarriersAndStats() {
        RenderGraph::CompiledGraph graph = buildVulkanProductionRenderGraph(
            { 1920, 1080 }, VK_FORMAT_B8G8R8A8_SRGB);
        const size_t transitionCount = graph.transitions().size();
        FakeResourceFactory factory;
        VulkanRenderGraphExecutor executor;
        executor.init(factory, 2);
        executor.rebuild(std::move(graph));

        const VulkanGraphStats stats = executor.stats();
        CHECK(stats.enabled);
        CHECK(stats.passCount == 19);
        CHECK(stats.logicalResourceCount == 26);
        CHECK(stats.physicalSlotCount == 19);
        CHECK(stats.barrierCount == transitionCount);
        CHECK(stats.frameCount == 2);
        CHECK(stats.rebuildCount == 1);
        CHECK(stats.cacheMissCount == 0);
        CHECK(executor.barriers().size() == transitionCount);
        bool foundDepthSampledRead = false;
        for (const VulkanGraphBarrierIntent& barrier : executor.barriers()) {
            if (barrier.after.layout ==
                VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL) {
                CHECK((barrier.after.access & VK_ACCESS_SHADER_READ_BIT) != 0);
                foundDepthSampledRead = true;
            }
        }
        CHECK(foundDepthSampledRead);
        CHECK(executor.validateFrame(0));
        CHECK(executor.validateFrame(1));
        CHECK(!executor.validateFrame(2));
        CHECK(executor.stats().cacheMissCount == 0);

        constexpr std::string_view passNames[] = {
            "shadow.directional",
            "shadow.spot",
            "shadow.point",
            "gbuffer",
            "lighting.cluster.clear",
            "lighting.cluster.count",
            "lighting.cluster.scan",
            "lighting.cluster.fill",
            "lighting.cluster.finalize",
            "lighting.cluster.readback",
            "lighting",
            "forward-opaque",
            "transparent.refraction-pyramids",
            "transparent.sorted.forward",
            "transparent.compatibility.forward",
            "bloom-hook",
            "output-transform",
            "final-capture-hook",
            "ui-present",
        };
        executor.beginFrameExecution(0);
        for (const std::string_view passName : passNames) {
            executor.skipPass(passName);
        }
        executor.finishFrameExecution();

        bool orderRejected = false;
        executor.beginFrameExecution(1);
        try {
            executor.skipPass("lighting");
        }
        catch (const std::logic_error&) {
            orderRejected = true;
        }
        CHECK(orderRejected);
        for (const std::string_view passName : passNames) {
            executor.skipPass(passName);
        }
        executor.finishFrameExecution();

        executor.cleanupAfterDeviceIdle();
        CHECK(factory.destroyCount == stats.physicalSlotCount * stats.frameCount);
        CHECK(!executor.stats().enabled);
        CHECK(executor.stats().rebuildCount == 1);
        return true;
    }

    bool testNamedBufferAccess() {
        RenderGraph::RenderGraphBuilder builder;
        RenderGraph::ResourceDesc desc{};
        desc.type = RenderGraph::ResourceType::Buffer;
        desc.buffer.size = 256;
        desc.buffer.alignment = 16;
        auto buffer = builder.createResource("lighting.cluster.headers", desc);
        const auto pass = builder.addPass("cluster-build",
            RenderGraph::QueueClass::Compute);
        buffer = builder.write(pass, buffer, RenderGraph::Access::StorageWrite);
        const auto compiled = builder.compile();
        CHECK(compiled.succeeded());
        FakeResourceFactory factory;
        VulkanRenderGraphExecutor executor;
        executor.init(factory, 2);
        executor.rebuild(*compiled.graph);
        CHECK(executor.bufferResource(0, "lighting.cluster.headers").isValid());
        CHECK(executor.bufferResource(1, "lighting.cluster.headers").isValid());
        bool rejectedImage = false;
        try {
            (void)executor.imageResource(0, "lighting.cluster.headers");
        }
        catch (const std::out_of_range&) {
            rejectedImage = true;
        }
        CHECK(rejectedImage);
        executor.cleanupAfterDeviceIdle();
        return true;
    }

    // M7R R2.3: telemetry/qualification-only graph work is declared only when
    // its consumer is active; defaults reproduce the production graph.
    bool testOptionalTelemetryAndQualificationReadbacks() {
        const auto build = [](VulkanProductionGraphFeatures features) {
            return buildVulkanProductionRenderGraph({127, 73},
                VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, false,
                GBufferLayout::CanonicalReference, {}, 4096, 8192, true, {},
                features);
        };
        const auto hasPass = [](const RenderGraph::CompiledGraph& graph,
            std::string_view name) {
            return std::ranges::any_of(graph.passes(),
                [name](const auto& pass) { return pass.name == name; });
        };
        const auto hasResource = [](const RenderGraph::CompiledGraph& graph,
            std::string_view name) {
            return std::ranges::any_of(graph.resources(),
                [name](const auto& resource) { return resource.name == name; });
        };
        const auto defaults = build({});
        CHECK(hasPass(defaults, "lighting.cluster.readback"));
        CHECK(hasResource(defaults, "lighting.cluster.diagnostics-readback"));
        const auto noTelemetry = build({ .clusterTelemetryReadback = false });
        CHECK(!hasPass(noTelemetry, "lighting.cluster.readback"));
        CHECK(!hasResource(noTelemetry, "lighting.cluster.diagnostics-readback"));
        CHECK(noTelemetry.passes().size() + 1 == defaults.passes().size());

        const auto depthUsages = [](const RenderGraph::CompiledGraph& graph) {
            const auto depth = std::ranges::find_if(graph.resources(),
                [](const auto& resource) { return resource.name == "depth.opaque"; });
            return depth == graph.resources().end() ? 0u : depth->usages;
        };
        const auto transferSource = RenderGraph::usageBit(
            RenderGraph::Access::TransferSource);
        const auto vsmOracle = build({ .virtualShadowWorkingSetBytes = 8'192 });
        const auto vsmNoOracle = build({ .virtualShadowWorkingSetBytes = 8'192,
            .virtualShadowDepthSnapshot = false });
        CHECK(hasPass(vsmNoOracle, "shadow.virtual.request-readback"));
        CHECK((depthUsages(vsmOracle) & transferSource) != 0);
        CHECK((depthUsages(vsmNoOracle) & transferSource) == 0);
        return true;
    }

    bool testExternalBufferBindingAndUploadTopology() {
        const auto normal = buildVulkanProductionRenderGraph({127, 73}, VK_FORMAT_B8G8R8A8_SRGB);
        const auto upload = buildVulkanProductionRenderGraph({127, 73}, VK_FORMAT_B8G8R8A8_SRGB,
            VK_FORMAT_B8G8R8A8_SRGB, false, GBufferLayout::CanonicalReference, {},
            4096, 8192, true, {}, { .virtualShadowWorkingSetBytes = 8'192 });
        CHECK(upload.passes().size() == normal.passes().size() + 3);
        CHECK(upload.resources().size() == normal.resources().size() + 1);
        CHECK(upload.physicalSlots().size() == normal.physicalSlots().size());
        CHECK(upload.passes().front().name == "shadow.virtual.clip-upload");
        const auto passIndex = [&](std::string_view name) {
            for (uint32_t i = 0; i < upload.passes().size(); ++i)
                if (upload.passes()[i].name == name) return i;
            return RenderGraph::InvalidIndex;
        };
        const auto hasUsage = [&](std::string_view pass, std::string_view resource, RenderGraph::Access access) {
            for (const auto& usage : upload.usages())
                if (usage.passOrderIndex == passIndex(pass) &&
                    upload.resources()[usage.logicalResourceIndex].name == resource && usage.access == access)
                    return true;
            return false;
        };
        CHECK(passIndex("forward-opaque") < passIndex("shadow.virtual.depth-mark"));
        CHECK(passIndex("shadow.virtual.depth-mark") < passIndex("shadow.virtual.request-readback"));
        CHECK(hasUsage("shadow.virtual.depth-mark", "depth.opaque", RenderGraph::Access::SampledRead));
        CHECK(hasUsage("shadow.virtual.depth-mark", "shadow.virtual.working-set", RenderGraph::Access::StorageReadWrite));
        CHECK(hasUsage("shadow.virtual.request-readback", "depth.opaque", RenderGraph::Access::TransferSource));
        CHECK(hasUsage("shadow.virtual.request-readback", "shadow.virtual.working-set", RenderGraph::Access::TransferSource));
        RenderGraph::RenderGraphBuilder builder;
        RenderGraph::ResourceDesc desc{};
        desc.type = RenderGraph::ResourceType::Buffer;
        desc.lifetime = RenderGraph::ResourceLifetime::External; desc.imported = true;
        desc.buffer.size = 256; desc.initialAccess = RenderGraph::Access::TransferDestination;
        auto buffer = builder.createResource("external", desc);
        buffer = builder.write(builder.addPass("upload"), buffer, RenderGraph::Access::TransferDestination);
        const auto compiled = builder.compile(); CHECK(compiled.succeeded());
        FakeResourceFactory factory;
        VulkanRenderGraphExecutor executor; executor.init(factory, 2); executor.rebuild(*compiled.graph);
        const auto handle = reinterpret_cast<VkBuffer>(uintptr_t{123});
        const auto other = reinterpret_cast<VkBuffer>(uintptr_t{456});
        const auto rejects = [&](uint32_t slot, std::string_view name, VkBuffer value, uint64_t bytes) {
            try { executor.bindExternalBuffer(slot, name, value, bytes); }
            catch (const std::invalid_argument&) { return true; }
            return false;
        };
        CHECK(rejects(0, "external", VK_NULL_HANDLE, 256));
        CHECK(rejects(2, "external", handle, 256));
        CHECK(rejects(0, "unknown", handle, 256));
        CHECK(rejects(0, "external", handle, 255));
        executor.bindExternalBuffer(0, "external", handle, 256);
        CHECK(executor.validateFrame(0)); CHECK(!executor.validateFrame(1));
        CHECK(rejects(1, "external", handle, 256));
        executor.bindExternalBuffer(1, "external", other, 256);
        CHECK(executor.validateFrame(1));
        executor.beginFrameExecution(0);
        CHECK(rejects(0, "external", handle, 256));
        executor.skipPass("upload"); executor.finishFrameExecution();
        CHECK(rejects(0, "external", handle, 256));
        executor.onFrameFenceCompleted(0);
        executor.bindExternalBuffer(0, "external", handle, 256);
        executor.cleanupAfterDeviceIdle();
        CHECK(factory.createCount == 0 && factory.destroyCount == 0);
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        std::string_view name;
        bool (*function)();
    };
    constexpr TestCase tests[] = {
        { "external buffer binding and clip upload topology", testExternalBufferBindingAndUploadTopology },
        { "access and format mappings", testAccessAndFormatMappings },
        { "optional occlusion depth pyramid", testOptionalOcclusionDepthPyramid },
        { "optional telemetry and qualification readbacks", testOptionalTelemetryAndQualificationReadbacks },
        { "production topology contract", testProductionTopologyContract },
        { "HDR10 topology contract", testHdr10TopologyContract },
        { "scene and presentation extent separation",
            testSceneAndPresentationExtentSeparation },
        { "conditional transparency pyramid topology",
            testConditionalTransparencyPyramidTopology },
        { "conditional Ordinary2 topology",
            testConditionalOrdinary2Topology },
        { "conditional deep layered topology",
            testConditionalDeepLayeredTopology },
        { "conditional WeightedOIT topology",
            testConditionalWeightedOitTopology },
        { "transparency pyramid residency policy",
            testTransparencyPyramidResidencyPolicy },
        { "fence-scoped retirement and cleanup", testFenceScopedRetirementAndCleanup },
        { "allocation failure preserves active set", testAllocationFailurePreservesActiveSet },
        { "resize and bounds", testResizeAndBounds },
        { "executor cache barriers and stats", testExecutorCacheBarriersAndStats },
        { "named buffer access", testNamedBufferAccess },
    };

    size_t passed = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.function()) {
                ++passed;
                std::cout << "[pass] " << test.name << '\n';
            }
            else {
                std::cout << "[fail] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            std::cout << "[fail] " << test.name << ": " << exception.what() << '\n';
        }
    }
    std::cout << passed << '/' << std::size(tests) << " tests passed\n";
    return passed == std::size(tests) ? 0 : 1;
}
