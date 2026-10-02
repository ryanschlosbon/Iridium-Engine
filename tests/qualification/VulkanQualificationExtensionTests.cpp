// Unit tests for the Vulkan qualification extension (M7R R2.7/R2.8): readback
// analyzers, the indirect oracle comparisons, extension hook declarations, and
// backend-factory attachment. No device is created.
#include "qualification/vulkan/VulkanIndirectOracle.h"
#include "qualification/vulkan/VulkanQualificationExtension.h"
#include "qualification/vulkan/VulkanQualificationInstall.h"
#include "qualification/vulkan/VulkanReadbackAnalysis.h"
#include "renderer/rhi/RenderBackendFactory.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    constexpr uint16_t HalfOne = 0x3C00u;
    constexpr uint16_t HalfHalf = 0x3800u;
    constexpr uint16_t HalfQuarter = 0x3400u;

    template<typename T>
    void put(std::vector<std::byte>& bytes, size_t offset, T value) {
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    bool testOrdinary2Analysis() {
        // Two-pixel atlas: a valid entry/exit pair and an entry-only pixel
        // with an out-of-range work index.
        constexpr uint32_t pixels = 2u;
        constexpr size_t image = pixels * sizeof(uint32_t);
        std::vector<std::byte> bytes(image * 4u + pixels * 8u);
        put<uint32_t>(bytes, 0u, 1u);                         // entry id p0
        put<uint32_t>(bytes, 4u, 2u);                         // entry id p1
        put<float>(bytes, image, 0.25f);                      // entry depth p0
        put<float>(bytes, image + 4u, 0.5f);                  // entry depth p1
        put<uint32_t>(bytes, image * 2u, 0x80000001u);        // exit id p0
        put<float>(bytes, image * 3u, 0.5f);                  // exit depth p0
        const std::array<uint16_t, 4> local{ HalfOne, HalfHalf, HalfQuarter,
            HalfHalf };
        std::memcpy(bytes.data() + image * 4u, local.data(), 8u);
        const Ordinary2CaptureValidationResult result =
            analyzeOrdinary2CaptureReadback({ 9u, 2u, 1u, 1u, 1u }, bytes);
        CHECK(result.validationId == 9u);
        CHECK(result.inspectedPixelCount == 2u);
        CHECK(result.entryPixelCount == 2u);
        CHECK(result.exitPixelCount == 1u);
        CHECK(result.pairedPixelCount == 1u);
        CHECK(result.entryOnlyPixelCount == 1u);
        CHECK(result.invalidWorkIndexPixelCount == 1u);
        CHECK(result.invalidOrientationPixelCount == 0u);
        CHECK(result.nonIncreasingDepthPixelCount == 0u);
        CHECK(result.minimumPairedDepthDelta == 0.25f);
        CHECK(result.localColorPixelCount == 1u);
        CHECK(result.localColorInvalidPixelCount == 0u);
        CHECK(result.maximumLocalAlpha == 0.5f);
        bool rejected = false;
        try {
            (void)analyzeOrdinary2CaptureReadback({ 0u, 2u, 1u, 1u, 1u },
                std::span(bytes).first(8u));
        }
        catch (const std::runtime_error&) { rejected = true; }
        CHECK(rejected);
        return true;
    }

    bool testDeepLayeredAnalysis() {
        // One pixel, two interfaces: Entry(work 1) then Exit(work 1).
        constexpr uint32_t interfaces = 2u;
        constexpr size_t image = sizeof(uint32_t);
        const size_t local = image * interfaces * 2u;
        const size_t tiles = local + 8u;
        std::vector<std::byte> bytes(tiles + sizeof(uint32_t) * interfaces);
        put<uint32_t>(bytes, 0u, 1u);
        put<float>(bytes, image, 0.2f);
        put<uint32_t>(bytes, image * 2u, 0x80000001u);
        put<float>(bytes, image * 3u, 0.4f);
        const std::array<uint16_t, 4> color{ HalfQuarter, HalfQuarter,
            HalfQuarter, HalfHalf };
        std::memcpy(bytes.data() + local, color.data(), 8u);
        const DeepLayeredCaptureValidationResult result =
            analyzeDeepLayeredCaptureReadback({ .validationId = 3u,
                .quality = TransparencyQuality::Hero4, .width = 1u, .height = 1u,
                .interfaceCount = interfaces, .expectedDrawCount = 2u,
                .sceneResolveDrawCount = 5u, .workItemCount = 1u }, bytes);
        CHECK(result.validationId == 3u);
        CHECK(result.sceneResolveDrawCount == 5u);
        CHECK(result.interfacePixelCounts[0] == 1u);
        CHECK(result.interfacePixelCounts[1] == 1u);
        CHECK(result.pairedPixelCount == 1u);
        CHECK(result.unmatchedExitPixelCount == 0u);
        CHECK(result.unclosedEntryPixelCount == 0u);
        CHECK(result.maximumObservedInterfaceCount == 2u);
        CHECK(result.localColorPixelCount == 1u);
        CHECK(result.localColorInvalidPixelCount == 0u);
        CHECK(result.terminatedOccupiedTileCount == 0u);
        CHECK(result.minimumDepthDelta > 0.19f && result.minimumDepthDelta < 0.21f);

        // An exit with no entry is unmatched and invalidates the local color.
        put<uint32_t>(bytes, 0u, 0u);
        const DeepLayeredCaptureValidationResult broken =
            analyzeDeepLayeredCaptureReadback({ .width = 1u, .height = 1u,
                .interfaceCount = interfaces, .workItemCount = 1u }, bytes);
        CHECK(broken.unmatchedExitPixelCount == 1u);
        CHECK(broken.interfaceGapPixelCount == 1u);
        CHECK(broken.pairedPixelCount == 0u);
        CHECK(broken.localColorInvalidPixelCount == 1u);
        return true;
    }

    bool testDepthPyramidAnalysis() {
        constexpr uint32_t width = 4u, height = 3u;
        std::vector<float> source(width * height);
        for (size_t index = 0; index < source.size(); ++index)
            source[index] = 0.05f * static_cast<float>(index + 1u);
        DepthPyramidReference reference;
        reference.build({ width, height }, DeviceDepthConvention::ForwardZeroToOne,
            source);
        std::vector<float> readback = source;
        for (uint32_t mip = 0; mip < reference.mipCount(); ++mip) {
            const auto values = reference.mip(mip);
            readback.insert(readback.end(), values.begin(), values.end());
        }
        const auto bytes = std::as_bytes(std::span(readback));
        const DepthPyramidCaptureValidationResult passed =
            analyzeDepthPyramidCaptureReadback({ 4u, width, height,
                reference.mipCount() }, bytes);
        CHECK(passed.passed());
        CHECK(passed.sourceTexelCount == width * height);
        CHECK(passed.pyramidTexelCount == readback.size() - source.size());

        readback[source.size() + 1u] += 0.5f;
        const DepthPyramidCaptureValidationResult failed =
            analyzeDepthPyramidCaptureReadback({ 4u, width, height,
                reference.mipCount() }, std::as_bytes(std::span(readback)));
        CHECK(!failed.passed());
        CHECK(failed.mismatchTexelCount == 1u);
        CHECK(failed.firstMismatchMip == 0u);
        CHECK(failed.firstMismatchTexel == 1u);
        CHECK(failed.maximumAbsoluteError > 0.49f);
        return true;
    }

    bool testFrameCaptureConversion() {
        const std::array<uint16_t, 4> half{ HalfOne, HalfHalf, HalfQuarter, 0u };
        const FrameCapture linear = convertFrameCaptureReadback({ .captureId = 7u,
            .width = 1u, .height = 1u,
            .pixelFormat = FrameCapturePixelFormat::Rgba32Float,
            .point = FrameCapturePoint::SceneLinear, .halfFloatSource = true },
            std::as_bytes(std::span(half)));
        CHECK(linear.captureId == 7u);
        CHECK(linear.rowPitchBytes == 16u);
        CHECK(linear.colorDomain == FrameCaptureColorDomain::SceneLinearAcesCg);
        std::array<float, 4> rgba{};
        std::memcpy(rgba.data(), linear.pixels.data(), sizeof(rgba));
        CHECK(rgba[0] == 1.0f && rgba[1] == 0.5f && rgba[2] == 0.25f);

        const std::array<uint8_t, 4> sdr{ 1u, 2u, 3u, 4u };
        const FrameCapture final = convertFrameCaptureReadback({ .width = 1u,
            .height = 1u, .pixelFormat = FrameCapturePixelFormat::Bgra8Srgb,
            .point = FrameCapturePoint::FinalSdr },
            std::as_bytes(std::span(sdr)));
        CHECK(final.colorDomain == FrameCaptureColorDomain::DisplayEncodedSdr);
        CHECK(final.pixels.size() == 4u);
        CHECK(std::to_integer<uint8_t>(final.pixels[3]) == 4u);
        return true;
    }

    bool testShadowOracleComparison() {
        VulkanIndirectOracle oracle;
        CHECK(!oracle.enabled(VulkanIndirectOracleView::SpotShadow));
        oracle.configure({ .shadowIndirect = true });
        CHECK(oracle.enabled(VulkanIndirectOracleView::SpotShadow));
        CHECK(oracle.enabled(VulkanIndirectOracleView::ReflectionProbe));
        CHECK(!oracle.enabled(VulkanIndirectOracleView::OpaqueLod));

        // Geometry 0 has 30 indices (base); the oracle expects a reduced LOD.
        std::array<GpuScenePrimitiveRecord, 2> primitives{};
        for (GpuScenePrimitiveRecord& primitive : primitives)
            primitive.binding.y = 0u;
        std::array<GpuSceneGeometryRecord, 1> geometries{};
        geometries[0].draw.y = 30u;
        const GpuSceneIndexedIndirectCommand a{ 12u, 1u, 0u, 0, 0u };
        const GpuSceneIndexedIndirectCommand b{ 30u, 1u, 0u, 0, 1u };
        const auto view = VulkanIndirectOracleView::SpotShadow;
        // Nothing begun: no validation.
        const auto none = oracle.verifyShadowWork(view, 0u, {});
        CHECK(!none.validated);

        oracle.beginShadowWork(view, 1u, 2u);
        oracle.expectShadowCommand(view, 1u, 0u, a);
        oracle.expectShadowCommand(view, 1u, 1u, b);
        oracle.expectShadowCommand(view, 1u, 9u, b); // out of range: ignored
        const std::array<uint32_t, 2> counts{ 1u, 1u };
        const std::array<uint32_t, 2> capacities{ 2u, 2u };
        const std::array<uint32_t, 2> offsets{ 0u, 2u };
        std::array<GpuSceneIndexedIndirectCommand, 4> device{ a, {}, b, {} };
        const VulkanIndirectReadback readback{ counts.data(), device.data(),
            capacities, offsets, primitives, geometries };
        const auto match = oracle.verifyShadowWork(view, 1u, readback);
        CHECK(match.validated);
        CHECK(match.oracleCommands == 2u);
        CHECK(match.mismatchedBins == 0u);
        CHECK(match.mismatchedCommandRegions == 0u);
        CHECK(match.oracleTriangles == 14u);
        CHECK(match.oracleReducedCommands == 1u);
        // Verification consumes the batch.
        CHECK(!oracle.verifyShadowWork(view, 1u, readback).validated);

        oracle.beginShadowWork(view, 1u, 2u);
        oracle.expectShadowCommand(view, 1u, 0u, b);
        device[0] = a;
        const auto mismatch = oracle.verifyShadowWork(view, 1u, readback);
        CHECK(mismatch.validated);
        CHECK(mismatch.mismatchedBins == 1u);          // region 1 expected none
        CHECK(mismatch.mismatchedCommandRegions == 2u);
        return true;
    }

    bool testOpaqueOracleComparison() {
        VulkanIndirectOracle oracle;
        oracle.configure({ .gpuLod = true, .depthOcclusion = true });
        oracle.beginOpaqueWork(0u, 3u, 3u, true, true);
        const GpuSceneIndexedIndirectCommand p0{ 9u, 1u, 0u, 0, 0u };
        const GpuSceneIndexedIndirectCommand p2{ 6u, 1u, 3u, 0, 2u };
        oracle.expectOpaqueLod(0u, 0u, { .command = p0, .baseTriangles = 4u,
            .oracleTriangles = 3u, .reduced = true, .historyValid = false });
        oracle.expectOpaqueLod(0u, 2u, { .command = p2, .baseTriangles = 2u,
            .oracleTriangles = 2u, .historyValid = true, .historyChanged = true });
        oracle.expectOpaqueVisibleCandidate(0u, 0u);
        oracle.expectOpaqueVisibleCandidate(0u, 2u);
        oracle.expectOpaqueOcclusionQuery(0u, 2u);
        oracle.expectOpaqueProjectionRejected(0u);

        const std::array<uint32_t, 2> counts{ 1u, 0u };
        const std::array<uint32_t, 2> capacities{ 2u, 1u };
        const std::array<GpuSceneIndexedIndirectCommand, 3> commands{ p0, {}, {} };
        oracle.verifyOpaqueLodCommands(0u, counts.data(), capacities,
            commands.data());

        DepthPyramidDeviceResult result{};
        result.abiVersion = DepthPyramidAbiVersion;
        result.sampledTexels = 1u;
        result.tested = 1u;
        result.occluded = 1u;
        result.farthestOccluderDepth = 0.5f;
        const auto queries = oracle.verifyOcclusionQueries(0u, &result);
        CHECK(queries.active);
        CHECK(queries.queryCount == 1u);
        CHECK(queries.projectionRejected == 1u);
        CHECK(queries.tested == 1u);
        CHECK(queries.wouldReject == 1u);
        CHECK(queries.invalidResults == 0u);
        // Candidate 2 was CPU-occluded: a fused rejection is safe. Candidate 0
        // was CPU-visible and never queried: rejecting it is unsafe.
        CHECK(!oracle.unsafeGpuSceneOcclusion(0u, 2u));
        CHECK(oracle.unsafeGpuSceneOcclusion(0u, 0u));
        CHECK(!oracle.unsafeGpuSceneOcclusion(0u, 1u));
        CHECK(oracle.opaqueCandidateCpuVisible(0u, 2u));
        CHECK(!oracle.opaqueCandidateCpuVisible(0u, 1u));
        // The fused rejection removed primitive 2's command.
        CHECK(oracle.rejectOpaqueLodPrimitive(0u, 2u));
        CHECK(!oracle.rejectOpaqueLodPrimitive(0u, 7u));
        const VulkanOpaqueLodVerdict lod = oracle.finishOpaqueLod(0u);
        CHECK(lod.active);
        CHECK(lod.mismatchedCommands == 0u);
        CHECK(lod.deviceTriangles == 3u);
        CHECK(lod.baseTriangles == 6u);
        CHECK(lod.oracleTriangles == 5u);
        CHECK(lod.oracleReducedCommands == 1u);
        CHECK(lod.historyValid == 1u && lod.historyReset == 1u &&
            lod.historyChanged == 1u);

        // A missing expected command is a mismatch.
        oracle.beginOpaqueWork(1u, 1u, 0u, true, false);
        oracle.expectOpaqueLod(1u, 0u, { .command = p0 });
        const std::array<uint32_t, 1> none{ 0u };
        const std::array<uint32_t, 1> one{ 1u };
        oracle.verifyOpaqueLodCommands(1u, none.data(), one, commands.data());
        CHECK(oracle.finishOpaqueLod(1u).mismatchedCommands == 1u);
        return true;
    }

    bool testExtensionHooksAndRequests() {
        VulkanQualificationExtension extension;
        CHECK(extension.api() == RenderBackendApi::Vulkan);
        RenderBackendConfig config{};
        extension.configure(config);
        VulkanGraphHooks hooks = extension.graphHooks();
        CHECK(hooks.depthPyramidValidation && hooks.layeredValidation);
        CHECK(!hooks.virtualShadowDepthSnapshot);
        config.virtualShadowDepthQualificationOracle = true;
        extension.configure(config);
        CHECK(extension.graphHooks().virtualShadowDepthSnapshot);
        CHECK(extension.indirectOracle()->enabled(
            VulkanIndirectOracleView::VirtualShadowDepth));

        // Validation hooks run only for a pending request with work to read.
        VulkanHookContext context{ .point = VulkanHookPoint::Ordinary2Validation,
            .payload = VulkanOrdinary2HookPayload{ { 8u, 8u }, 1u, 1u } };
        CHECK(!extension.wantsHook(context));
        const VulkanFrameRecording closed{};
        bool rejected = false;
        try { extension.requestOrdinary2CaptureValidation(1u, closed); }
        catch (const std::logic_error&) { rejected = true; }
        CHECK(rejected);
        const VulkanFrameRecording open{ true,
            reinterpret_cast<VkCommandBuffer>(uintptr_t{ 1 }), 0u };
        extension.requestOrdinary2CaptureValidation(1u, open);
        CHECK(extension.wantsHook(context));
        context.payload = VulkanOrdinary2HookPayload{ { 8u, 8u }, 0u, 1u };
        CHECK(!extension.wantsHook(context));
        rejected = false;
        try { extension.requestOrdinary2CaptureValidation(2u, open); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);

        extension.requestDeepLayeredCaptureValidation(4u,
            TransparencyQuality::Cinematic8, open);
        VulkanHookContext deep{ .point = VulkanHookPoint::DeepLayeredValidation,
            .payload = VulkanDeepLayeredHookPayload{
                TransparencyQuality::Hero4, 4u, 3u, 1u } };
        CHECK(!extension.wantsHook(deep));
        deep.payload = VulkanDeepLayeredHookPayload{
            TransparencyQuality::Cinematic8, 8u, 3u, 1u };
        CHECK(extension.wantsHook(deep));

        // Depth-pyramid requests need the pyramid (absent before init).
        rejected = false;
        try { extension.requestDepthPyramidCaptureValidation(5u, open); }
        catch (const std::logic_error&) { rejected = true; }
        CHECK(rejected);

        // Deferred capture arms exactly one point.
        CHECK(!extension.wantsHook({ .point = VulkanHookPoint::SceneColorComplete }));
        extension.armFrameCapture(6u, FrameCapturePoint::FinalSdr);
        CHECK(!extension.wantsHook({ .point = VulkanHookPoint::SceneColorComplete }));
        CHECK(extension.wantsHook({ .point = VulkanHookPoint::FinalCaptureHook }));
        rejected = false;
        try { extension.armFrameCapture(7u, FrameCapturePoint::SceneLinear); }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);
        CHECK(extension.collectFrameCaptures(false, false).empty());
        rejected = false;
        try { (void)extension.collectFrameCaptures(true, false); }
        catch (const std::logic_error&) { rejected = true; }
        CHECK(rejected);
        return true;
    }

    class ForeignExtension final : public IRenderBackendExtension {
    public:
        [[nodiscard]] RenderBackendApi api() const noexcept override {
            return RenderBackendApi::DirectX12;
        }
    };

    bool testFactoryAttachment() {
        ForeignExtension foreign;
        IRenderBackendExtension* const foreignList[]{ &foreign };
        bool rejected = false;
        try {
            (void)createRenderBackend(RenderBackendCreateInfo{
                .api = RenderBackendApi::Vulkan, .extensions = foreignList });
        }
        catch (const std::invalid_argument&) { rejected = true; }
        CHECK(rejected);

        VulkanQualificationExtension extension;
        IRenderBackendExtension* const list[]{ &extension };
        CHECK(createRenderBackend(RenderBackendCreateInfo{
            .api = RenderBackendApi::Vulkan, .extensions = list }) != nullptr);
        // Without registration the legacy overload attaches nothing; the
        // installer registers the qualification extension explicitly.
        std::unique_ptr<IRenderBackend> legacy =
            createRenderBackend(RenderBackendApi::Vulkan);
        CHECK(legacy != nullptr);
        CHECK(legacy->collectFrameCaptures(true).empty());
        installQualificationBackendExtensions();
        legacy = createRenderBackend(RenderBackendApi::Vulkan);
        CHECK(legacy != nullptr);
        rejected = false;
        try { legacy->requestOrdinary2CaptureValidation(1u); }
        catch (const std::logic_error& error) {
            // Reached the extension: the frame-state check, not "requires".
            rejected = std::string_view(error.what()).find("during a frame") !=
                std::string_view::npos;
        }
        CHECK(rejected);
        setDefaultRenderBackendExtensionFactory(nullptr);
        return true;
    }

    struct TestCase {
        const char* name;
        bool (*run)();
    };

} // namespace

int main() {
    constexpr TestCase tests[] = {
        { "Ordinary2 readback analysis", testOrdinary2Analysis },
        { "Deep layered readback analysis", testDeepLayeredAnalysis },
        { "Depth-pyramid readback analysis", testDepthPyramidAnalysis },
        { "Frame capture conversion", testFrameCaptureConversion },
        { "Shadow indirect oracle comparison", testShadowOracleComparison },
        { "Opaque LOD/occlusion oracle", testOpaqueOracleComparison },
        { "Extension hooks and requests", testExtensionHooksAndRequests },
        { "Factory attachment", testFactoryAttachment },
    };

    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " << test.name << '\n';
            }
            else {
                ++failures;
                std::cerr << "[FAIL] " << test.name << '\n';
            }
        }
        catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << exception.what() << '\n';
        }
    }

    constexpr size_t testCount = sizeof(tests) / sizeof(tests[0]);
    std::cout << testCount - failures << '/' << testCount << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
