#include "qualification/harness/HarnessDetail.h"

#include "renderer/rhi/Mesh.h"

namespace Iridium {

    namespace {
        constexpr std::string_view Aces2TransformId =
            "urn:ampas:aces:transformId:v2.0:Output.Academy.Rec709-D65_100nit_in_Rec709-D65_sRGB-Piecewise.a2.v1";
        constexpr std::string_view Aces2HdrTransformId =
            "urn:ampas:aces:transformId:v2.0:Output.Academy.P3-D65_1000nit_in_Rec2100-D65_ST2084.a2.v1";
    }

    Ordinary2FallbackModelStats ordinary2FallbackModelStats(
        const ModelAsset& model) noexcept {
        Ordinary2FallbackModelStats result{};
        for (const SubMesh& subMesh : model.subMeshes) {
            if (subMesh.materialIndex < 0 ||
                static_cast<size_t>(subMesh.materialIndex) >=
                    model.materials.size() ||
                model.materials[static_cast<size_t>(
                    subMesh.materialIndex)].renderQueue !=
                    RenderQueue::Transparent) {
                continue;
            }
            ++result.transparentSubmeshes;
            if (subMesh.transparency.requestedClass ==
                    TransparencyClass::Auto ||
                subMesh.transparency.requestedClass ==
                    TransparencyClass::LayeredGlass) {
                ++result.requestedLayeredCandidates;
            }
            if (subMesh.transparency.resolvedClass ==
                    TransparencyClass::ThinGlass) {
                ++result.fallbackThinGlassSubmeshes;
            }
            if ((subMesh.transparency.flags &
                    CompiledTransparencyFallbackApplied) != 0u) {
                ++result.fallbackFlaggedSubmeshes;
            }
            if ((subMesh.transparency.flags &
                    CompiledTransparencyTopologyRequired) != 0u) {
                ++result.topologyRequiredSubmeshes;
            }
            if (subMesh.transparency.resolvedClass ==
                    TransparencyClass::LayeredGlass) {
                ++result.layeredGlassSubmeshes;
            }
        }
        return result;
    }

    std::string_view outputOperatorName(OutputTransformOperator value) {
        switch (value) {
        case OutputTransformOperator::Aces2: return "aces2";
        case OutputTransformOperator::AcesFittedLegacy:
            return "aces_fitted_legacy";
        case OutputTransformOperator::IdentityClampDiagnostic:
            return "identity_clamp_diagnostic";
        }
        return "unknown";
    }

    std::string_view gamutMappingName(OutputTransformOperator value) {
        switch (value) {
        case OutputTransformOperator::Aces2:
            return "aces2_jmh_chroma_and_gamut_compression_lut128_tetrahedral";
        case OutputTransformOperator::AcesFittedLegacy:
            return "ap1_to_rec709_matrix_then_clip_negative";
        case OutputTransformOperator::IdentityClampDiagnostic:
            return "ap1_to_rec709_matrix_then_clamp_diagnostic";
        }
        return "unknown";
    }

    std::string_view transformId(OutputTransformOperator value,
        Color::OutputTransport transport) {
        return value == OutputTransformOperator::Aces2
            ? (transport == Color::OutputTransport::SdrSrgb
                ? Aces2TransformId : Aces2HdrTransformId)
            : (value == OutputTransformOperator::AcesFittedLegacy
                ? "legacy_fitted_compatibility" : "identity_clamp_diagnostic");
    }

} // namespace Iridium
