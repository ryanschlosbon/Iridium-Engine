#pragma once

#include "assets/model/ModelProduct.h"
#include <nlohmann/json.hpp>
#include <map>
#include <cmath>
#include <stdexcept>

namespace Iridium {
    inline TransparencyPolicyV1 previewAuthoredPolicy(const nlohmann::json& settings,
        AssetGuid guid, TransparencyPolicyV1 inherited) {
        const auto policies = settings.value("transparency_policies", nlohmann::json::object());
        const auto found = policies.find(guid.toString());
        if (found == policies.end()) return inherited;
        const auto name = found->value("class", std::string("auto"));
        if (name == "auto") inherited.requestedClass = TransparencyClass::Auto;
        else if (name == "alpha_clip") inherited.requestedClass = TransparencyClass::AlphaClip;
        else if (name == "sorted_surface") inherited.requestedClass = TransparencyClass::SortedSurface;
        else if (name == "thin_glass") inherited.requestedClass = TransparencyClass::ThinGlass;
        else if (name == "layered_glass") inherited.requestedClass = TransparencyClass::LayeredGlass;
        else if (name == "weighted_oit") inherited.requestedClass = TransparencyClass::WeightedOit;
        else throw std::runtime_error("Invalid preview transparency class");
        const auto quality = found->value("quality", std::string("ordinary2"));
        if (quality == "ordinary2") inherited.quality = TransparencyQuality::Ordinary2;
        else if (quality == "hero4") inherited.quality = TransparencyQuality::Hero4;
        else if (quality == "cinematic8") inherited.quality = TransparencyQuality::Cinematic8;
        else throw std::runtime_error("Invalid preview transparency quality");
        inherited.priority = found->value("priority", 0);
        inherited.thinSheetThicknessMeters = found->value("thin_sheet_thickness_m", 0.0f);
        if (!std::isfinite(inherited.thinSheetThicknessMeters) || inherited.thinSheetThicknessMeters < 0 ||
            inherited.thinSheetThicknessMeters > 1.0e6f)
            throw std::runtime_error("Invalid preview thin-sheet thickness");
        return inherited;
    }

    // Route-stable policy edits reuse existing geometry and topology proof. Never
    // invent closed-volume validity or skip disconnected-surface recooking.
    inline std::map<AssetGuid, CompiledTransparencyPolicy> applyPreviewPolicySettings(
        CookedModelProductData& product, const std::map<AssetGuid, SourceMaterial>& sources,
        const nlohmann::json& settings) {
        std::map<AssetGuid, TransparencyPolicyV1> materialPolicies;
        for (const auto& material : product.materials) {
            const auto source = sources.find(material.materialGuid);
            if (source != sources.end()) materialPolicies.emplace(material.materialGuid,
                previewAuthoredPolicy(settings, material.materialGuid, source->second.transparencyPolicy));
        }
        std::map<AssetGuid, CompiledTransparencyPolicy> result;
        for (auto& primitive : product.manifest.primitives) {
            const auto material = materialPolicies.find(primitive.materialGuid);
            if (material == materialPolicies.end()) {
                result.emplace(primitive.primitiveGuid, primitive.transparency);
                continue;
            }
            const auto policy = previewAuthoredPolicy(settings, primitive.sourcePrimitiveGuid, material->second);
            if (policy.requestedClass != primitive.transparency.requestedClass)
                throw std::runtime_error("Changing transparency class requires Apply and reimport; last valid preview retained.");
            primitive.transparency.quality = policy.quality;
            primitive.transparency.priority = policy.priority;
            primitive.transparency.thinSheetThicknessMeters = policy.thinSheetThicknessMeters;
            result.emplace(primitive.primitiveGuid, primitive.transparency);
        }
        return result;
    }
}
