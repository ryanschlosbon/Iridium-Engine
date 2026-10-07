// Shader include and function ownership, read from what glslc actually compiled:
// the per-shader depfiles (which sources each module consumed) and the SPIR-V
// function table (glslang links only called functions). Replaces the #include
// and identifier text checks of SceneColorTests, LightingReferenceTests,
// StandardMaterialShadingTests and Stage3ArchitectureTests group 3 with the ADR
// invariants they protect: one shared BSDF, one clustered-light representation
// and scene-linear AP1 until the single output transform.

#include "ShaderDepfile.h"
#include "SpirvInspector.h"
#include "TestHarness.h"

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

    using Iridium::Test::ShaderDepfile;
    using Iridium::Test::SpirvModule;

    struct CompiledShader {
        std::string name;
        ShaderDepfile depfile;
        SpirvModule module;
    };

    const std::map<std::string, CompiledShader>& shaders() {
        static const std::map<std::string, CompiledShader> all = [] {
            std::map<std::string, CompiledShader> result;
            for (const char* name : Iridium::Test::compiledShaders()) {
                result.emplace(name, CompiledShader{ name,
                    ShaderDepfile::load(Iridium::Test::shaderDepfileDirectory(), name),
                    SpirvModule::load(Iridium::Test::shaderBinary(name)) });
            }
            return result;
        }();
        return all;
    }

    const CompiledShader& shader(const std::string& name) {
        const auto found = shaders().find(name);
        if (found == shaders().end())
            throw std::runtime_error("shader not built: " + name);
        return found->second;
    }

    constexpr std::array DeferredLighting{
        "canonical_reference_lighting_frag.spv",
        "canonical_packed_lighting_frag.spv",
    };
    constexpr std::array ForwardMaterial{
        "complex_material_indexed_frag.spv",
        "complex_opaque_material_indexed_frag.spv",
        "complex_opaque_material_velocity_indexed_frag.spv",   // M9.1
        "layered_ordinary2_material_indexed_frag.spv",
        "layered_deep_material_indexed_frag.spv",
        "layered_deep_residual_material_indexed_frag.spv",
        "weighted_oit_material_indexed_frag.spv",
    };
    constexpr std::array GBufferMaterial{
        "canonical_reference_indexed_frag.spv",
        "canonical_packed_indexed_frag.spv",
    };
    // Lit consumers beyond the main view: probe capture shades with the same
    // clustered lights and BSDF.
    constexpr const char* ProbeCapture = "reflection_probe_capture_frag.spv";

    std::vector<std::string> litConsumers() {
        std::vector<std::string> result(DeferredLighting.begin(), DeferredLighting.end());
        result.insert(result.end(), ForwardMaterial.begin(), ForwardMaterial.end());
        result.emplace_back(ProbeCapture);
        return result;
    }

    // Every module that links `function` compiled `owner`: the function has one
    // definition, in the shared include.
    bool definedOnlyIn(std::string_view function, std::string_view owner) {
        for (const auto& [name, compiled] : shaders()) {
            if (!compiled.module.callsFunction(function)) continue;
            IRIDIUM_CHECK_MSG(compiled.depfile.includes(owner), name << " links "
                << function << " without compiling " << owner);
        }
        return true;
    }

    std::vector<std::string> modulesLinking(std::string_view function) {
        std::vector<std::string> result;
        for (const auto& [name, compiled] : shaders())
            if (compiled.module.callsFunction(function)) result.push_back(name);
        return result;
    }

    bool testSharedBsdfOwnership() {
        for (const std::string& name : litConsumers())
            IRIDIUM_CHECK_MSG(shader(name).depfile.includes("include/material_bsdf.glsl"),
                name);
        for (const char* name : DeferredLighting)
            IRIDIUM_CHECK_MSG(shader(name).module.callsFunction(
                "materialEvaluateCanonicalBrdf"), name);
        for (const char* name : ForwardMaterial)
            IRIDIUM_CHECK_MSG(shader(name).module.callsFunction(
                "materialEvaluateStandardBrdf"), name);
        for (const char* function : { "materialEvaluateStandardBrdf",
                "materialEvaluateCanonicalBrdf", "materialDistributionGgx",
                "materialGeometrySmith", "materialFresnelSchlick" })
            IRIDIUM_CHECK(definedOnlyIn(function, "include/material_bsdf.glsl"));
        // No module carries a private microfacet copy under the pre-M5 names.
        for (const char* legacy : { "DistributionGGX", "FresnelSchlick",
                "GeometrySmith", "GeometrySchlickGGX", "fresnelSchlick" })
            IRIDIUM_CHECK_MSG(modulesLinking(legacy).empty(), legacy << " linked by "
                << modulesLinking(legacy).front());
        return true;
    }

    bool testOneClusteredLightRepresentation() {
        for (const std::string& name : litConsumers()) {
            const CompiledShader& compiled = shader(name);
            // Every lit consumer shades lights with the same direct-light model
            // on the same packed record.
            IRIDIUM_CHECK_MSG(compiled.module.callsFunction("iridiumEvaluateDirectLight"),
                name);
            if (name == ProbeCapture) {
                // Probe faces evaluate the active light list directly (no main-view
                // cluster grid), but through the same access include.
                IRIDIUM_CHECK(compiled.depfile.includes("include/clustered_light_access.glsl"));
                IRIDIUM_CHECK(compiled.depfile.includes("include/direct_lighting.glsl"));
                continue;
            }
            IRIDIUM_CHECK_MSG(compiled.depfile.includes(
                "include/clustered_light_access.glsl"), name);
            IRIDIUM_CHECK_MSG(compiled.depfile.includes("include/direct_lighting.glsl"),
                name);
            IRIDIUM_CHECK_MSG(compiled.depfile.includes("include/lighting_records.glsl"),
                name);
            // Consumers read the cluster product; only the builder sees its
            // writable form.
            IRIDIUM_CHECK_MSG(!compiled.depfile.includes(
                "include/clustered_lighting.glsl"), name);
            IRIDIUM_CHECK_MSG(compiled.module.callsFunction(
                "iridiumEvaluateDirectLightSlot"), name);
            IRIDIUM_CHECK_MSG(compiled.module.callsFunction("iridiumDirectLightSlot"),
                name);
        }
        for (const auto& [name, compiled] : shaders()) {
            if (!compiled.depfile.includes("include/clustered_lighting.glsl")) continue;
            IRIDIUM_CHECK_MSG(name.starts_with("cluster_") &&
                compiled.module.executionModel() ==
                    Iridium::Test::SpirvExecutionModel::GLCompute, name);
        }
        IRIDIUM_CHECK(definedOnlyIn("iridiumEvaluateDirectLightSlot",
            "include/clustered_light_access.glsl"));
        IRIDIUM_CHECK(definedOnlyIn("iridiumEvaluateDirectLight",
            "include/direct_lighting.glsl"));
        IRIDIUM_CHECK(definedOnlyIn("iridiumSpotCone", "include/direct_lighting.glsl"));
        return true;
    }

    bool testSharedShadowAndEnvironmentOwnership() {
        std::vector<std::string> mainView(DeferredLighting.begin(), DeferredLighting.end());
        mainView.insert(mainView.end(), ForwardMaterial.begin(), ForwardMaterial.end());
        for (const std::string& name : mainView) {
            const CompiledShader& compiled = shader(name);
            for (const char* include : { "include/spot_shadow.glsl",
                    "include/point_shadow.glsl", "include/directional_shadow.glsl",
                    "include/environment_ibl.glsl", "include/shadow_filter.glsl" })
                IRIDIUM_CHECK_MSG(compiled.depfile.includes(include), name << ' ' << include);
            for (const char* function : { "iridiumSpotShadowVisibility",
                    "iridiumPointShadowVisibility",
                    "iridiumDirectionalShadowVisibility",
                    // M7.10.6: one receiver contract (normal offset and
                    // bounded receiver-plane correction) for every owner.
                    "iridiumShadowNormalOffsetScale",
                    "iridiumShadowReceiverPlaneGradient",
                    "iridiumPointShadowReference" })
                IRIDIUM_CHECK_MSG(compiled.module.callsFunction(function),
                    name << ' ' << function);
        }
        for (const char* name : DeferredLighting)
            IRIDIUM_CHECK_MSG(shader(name).module.callsFunction(
                "iridiumEvaluateCanonicalIbl"), name);
        for (const char* name : ForwardMaterial)
            IRIDIUM_CHECK_MSG(shader(name).module.callsFunction(
                "iridiumEvaluateStandardIbl"), name);
        IRIDIUM_CHECK(definedOnlyIn("iridiumSpotShadowVisibility",
            "include/spot_shadow.glsl"));
        IRIDIUM_CHECK(definedOnlyIn("iridiumPointShadowVisibility",
            "include/point_shadow.glsl"));
        for (const char* function : { "iridiumShadowNormalOffsetScale",
                "iridiumShadowReceiverPlaneGradient",
                "iridiumShadowReceiverPlaneReference" })
            IRIDIUM_CHECK_MSG(definedOnlyIn(function,
                "include/shadow_filter.glsl"), function);
        return true;
    }

    // StandardMaterialShadingTests (normal/transport) and Stage3 group 3
    // (complex_material_body ownership and the compiled tier permutations).
    bool testMaterialBodyOwnership() {
        for (const char* name : ForwardMaterial) {
            const CompiledShader& compiled = shader(name);
            for (const char* include : { "include/complex_material_body.glsl",
                    "include/material_complex.glsl", "include/material_normal.glsl",
                    "include/transparency_transport.glsl",
                    "include/packed_material.glsl" })
                IRIDIUM_CHECK_MSG(compiled.depfile.includes(include), name << ' ' << include);
            IRIDIUM_CHECK(compiled.module.executionModel() ==
                Iridium::Test::SpirvExecutionModel::Fragment);
        }
        for (const char* name : GBufferMaterial) {
            const CompiledShader& compiled = shader(name);
            IRIDIUM_CHECK_MSG(compiled.depfile.includes("include/canonical_gbuffer_body.glsl"),
                name);
            IRIDIUM_CHECK_MSG(compiled.depfile.includes("include/material_normal.glsl"), name);
            IRIDIUM_CHECK_MSG(!compiled.depfile.includes("include/complex_material_body.glsl"),
                name);
        }
        for (const char* name : DeferredLighting) {
            const CompiledShader& compiled = shader(name);
            IRIDIUM_CHECK_MSG(compiled.depfile.includes("include/canonical_lighting_body.glsl"),
                name);
            IRIDIUM_CHECK_MSG(!compiled.depfile.includes("include/complex_material_body.glsl"),
                name);
        }
        for (const auto& [name, compiled] : shaders()) {
            if (!compiled.depfile.includes("include/complex_material_body.glsl")) continue;
            IRIDIUM_CHECK_MSG(std::ranges::find(ForwardMaterial, name) !=
                ForwardMaterial.end(), name << " compiles the forward material body");
        }

        // One body, compiled into distinct tier permutations: the Ordinary2
        // entry/exit pairing, the bounded deep-interface search, its residual
        // tail, and refraction transport that WeightedOIT/opaque forward omit.
        const auto links = [](const char* name, const char* function) {
            return shader(name).module.callsFunction(function);
        };
        IRIDIUM_CHECK(links("layered_ordinary2_material_indexed_frag.spv",
            "iridiumLayeredOrdinary2PairIsValid"));
        IRIDIUM_CHECK(!links("layered_deep_material_indexed_frag.spv",
            "iridiumLayeredOrdinary2PairIsValid"));
        IRIDIUM_CHECK(links("layered_deep_material_indexed_frag.spv",
            "iridiumLayeredDeepFindPair"));
        IRIDIUM_CHECK(!links("layered_ordinary2_material_indexed_frag.spv",
            "iridiumLayeredDeepFindPair"));
        IRIDIUM_CHECK(links("layered_deep_residual_material_indexed_frag.spv",
            "iridiumLayeredDeepResidualEntryIsValid"));
        IRIDIUM_CHECK(!links("complex_material_indexed_frag.spv",
            "iridiumLayeredDeepFindPair"));
        IRIDIUM_CHECK(links("complex_material_indexed_frag.spv",
            "iridiumProjectTransparencyRay"));
        IRIDIUM_CHECK(!links("weighted_oit_material_indexed_frag.spv",
            "iridiumProjectTransparencyRay"));
        IRIDIUM_CHECK(!links("complex_opaque_material_indexed_frag.spv",
            "iridiumProjectTransparencyRay"));
        // Interface capture is material-aware (alpha mask from the packed record)
        // without compiling the full material body.
        const CompiledShader& capture = shader("layered_interface_capture_frag.spv");
        IRIDIUM_CHECK(capture.depfile.includes("include/packed_material.glsl"));
        IRIDIUM_CHECK(!capture.depfile.includes("include/complex_material_body.glsl"));
        IRIDIUM_CHECK(capture.module.callsFunction("packedMaterialHasTexture"));
        return true;
    }

    // SceneColorTests: material inputs convert to AP1 once, lighting stays in
    // AP1, and only output.frag leaves scene-linear AP1.
    bool testSceneColorBoundary() {
        for (const char* name : GBufferMaterial)
            IRIDIUM_CHECK_MSG(shader(name).module.callsFunction("linearSrgbToAcesCg"), name);
        for (const char* name : ForwardMaterial)
            IRIDIUM_CHECK_MSG(shader(name).module.callsFunction("linearSrgbToAcesCg"), name);
        for (const char* name : DeferredLighting)
            IRIDIUM_CHECK_MSG(!shader(name).module.callsFunction("linearSrgbToAcesCg"), name);
        IRIDIUM_CHECK(definedOnlyIn("linearSrgbToAcesCg", "include/scene_color.glsl"));
        IRIDIUM_CHECK(definedOnlyIn("acesCgToLinearSrgb", "include/scene_color.glsl"));

        const CompiledShader& output = shader("output_frag.spv");
        IRIDIUM_CHECK(output.depfile.primarySource() == "output.frag");
        IRIDIUM_CHECK(output.depfile.includes("include/scene_color.glsl"));
        for (const char* function : { "acesCgToLinearSrgb", "sampleAces2Encoded",
                "decodeSt2084ToNits" }) {
            const auto owners = modulesLinking(function);
            IRIDIUM_CHECK_MSG(owners.size() == 1u && owners.front() == "output_frag.spv",
                function << " linked by " << owners.size() << " modules");
        }
        IRIDIUM_CHECK(modulesLinking("ACESFilm").empty());
        // The UI shader decodes its sRGB vertex colours before composition.
        IRIDIUM_CHECK(shader("imgui_color_managed_frag.spv").module.callsFunction(
            "decodeSrgb"));
        return true;
    }

    bool testDepfilesCoverEveryShader() {
        IRIDIUM_CHECK(!shaders().empty());
        for (const auto& [name, compiled] : shaders()) {
            IRIDIUM_CHECK_MSG(!compiled.depfile.sources().empty(), name);
            IRIDIUM_CHECK_MSG(!compiled.module.functionNames().empty(), name);
        }
        return true;
    }

} // namespace

int main() {
    constexpr Iridium::Test::TestCase tests[] = {
        { "depfiles cover every compiled shader", testDepfilesCoverEveryShader },
        { "shared BSDF ownership", testSharedBsdfOwnership },
        { "one clustered-light representation", testOneClusteredLightRepresentation },
        { "shared shadow and environment ownership",
            testSharedShadowAndEnvironmentOwnership },
        { "material body ownership and permutations", testMaterialBodyOwnership },
        { "scene-colour boundary and single output transform", testSceneColorBoundary },
    };
    return Iridium::Test::runTests(tests);
}
