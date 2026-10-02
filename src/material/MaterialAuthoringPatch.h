#pragma once

#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <nlohmann/json.hpp>
#include "material/SourceMaterial.h"

namespace Iridium {
    // Versioned sparse post-import authoring layer. Missing fields inherit the
    // newest source. Store by material GUID, never source index or display name.
    struct MaterialAuthoringField {
        const char* label;
        const char* path;
        float defaultValue;
        float minimum;
        float maximum;
        unsigned components = 1;
    };
    inline constexpr MaterialAuthoringField materialAuthoringFields[]{
        {"Base color / opacity", "/pbrMetallicRoughness/baseColorFactor", 1, 0, 1, 4},
        {"Metallic", "/pbrMetallicRoughness/metallicFactor", 1, 0, 1},
        {"Roughness", "/pbrMetallicRoughness/roughnessFactor", 1, 0, 1},
        {"Emission color", "/emissiveFactor", 0, 0, 1, 3},
        {"Emission strength", "/extensions/KHR_materials_emissive_strength/emissiveStrength", 1, 0, 1000000},
        {"IOR", "/extensions/KHR_materials_ior/ior", 1.5f, 1, 4},
        {"Specular", "/extensions/KHR_materials_specular/specularFactor", 1, 0, 1},
        {"Specular color", "/extensions/KHR_materials_specular/specularColorFactor", 1, 0, 1, 3},
        {"Transmission", "/extensions/KHR_materials_transmission/transmissionFactor", 0, 0, 1},
        {"Volume thickness (m)", "/extensions/KHR_materials_volume/thicknessFactor", 0, 0, 1000},
        {"Attenuation color", "/extensions/KHR_materials_volume/attenuationColor", 1, 0, 1, 3},
        {"Attenuation distance (m)", "/extensions/KHR_materials_volume/attenuationDistance", 1000000, 0.000001f, 1000000},
        {"Clearcoat", "/extensions/KHR_materials_clearcoat/clearcoatFactor", 0, 0, 1},
        {"Clearcoat roughness", "/extensions/KHR_materials_clearcoat/clearcoatRoughnessFactor", 0, 0, 1},
        {"Sheen color", "/extensions/KHR_materials_sheen/sheenColorFactor", 0, 0, 1, 3},
        {"Sheen roughness", "/extensions/KHR_materials_sheen/sheenRoughnessFactor", 0, 0, 1},
        {"Anisotropy", "/extensions/KHR_materials_anisotropy/anisotropyStrength", 0, 0, 1},
        {"Anisotropy rotation (rad)", "/extensions/KHR_materials_anisotropy/anisotropyRotation", 0, -6.283185f, 6.283185f},
        {"Iridescence", "/extensions/KHR_materials_iridescence/iridescenceFactor", 0, 0, 1},
        {"Iridescence IOR", "/extensions/KHR_materials_iridescence/iridescenceIor", 1.3f, 1, 4},
        {"Iridescence minimum thickness (nm)", "/extensions/KHR_materials_iridescence/iridescenceThicknessMinimum", 100, 0, 10000},
        {"Iridescence maximum thickness (nm)", "/extensions/KHR_materials_iridescence/iridescenceThicknessMaximum", 400, 0, 10000},
        {"Dispersion", "/extensions/KHR_materials_dispersion/dispersion", 0, 0, 100},
        {"Diffuse transmission", "/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor", 0, 0, 1},
        {"Diffuse transmission color", "/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor", 1, 0, 1, 3},
        {"Diffuse color / opacity", "/extensions/KHR_materials_pbrSpecularGlossiness/diffuseFactor", 1, 0, 1, 4},
        {"Specular-glossiness color", "/extensions/KHR_materials_pbrSpecularGlossiness/specularFactor", 1, 0, 1, 3},
        {"Glossiness", "/extensions/KHR_materials_pbrSpecularGlossiness/glossinessFactor", 1, 0, 1},
        {"Normal strength", "/normalTexture/scale", 1, -10, 10},
        {"Ambient occlusion strength", "/occlusionTexture/strength", 1, 0, 1},
        {"Alpha cutoff", "/alphaCutoff", .5f, 0, 1},
    };

    inline void validateMaterialAuthoringPatch(const nlohmann::json& patch) {
        if (!patch.is_object() || patch.value("schema_version", 0) != 1 ||
            !patch.contains("values") || !patch.at("values").is_object())
            throw std::runtime_error("Material edits require schema_version 1 and an object values map");
        for (const auto& [path, value] : patch.at("values").items()) {
            if (path == "/doubleSided" && value.is_boolean()) continue;
            if (path == "/alphaMode" && value.is_string() &&
                (value == "OPAQUE" || value == "MASK" || value == "BLEND")) continue;
            const MaterialAuthoringField* field = nullptr;
            for (const auto& candidate : materialAuthoringFields)
                if (path == candidate.path) field = &candidate;
            if (!field) throw std::runtime_error("Unsupported material edit: " + path);
            const auto valid = [field](const nlohmann::json& number) {
                if (!number.is_number()) return false;
                const double v = number.get<double>();
                return std::isfinite(v) && v >= field->minimum && v <= field->maximum;
            };
            if (field->components == 1) {
                if (!valid(value)) throw std::runtime_error("Invalid material value: " + path);
            } else {
                if (!value.is_array() || value.size() != field->components)
                    throw std::runtime_error("Invalid material vector: " + path);
                for (const auto& v : value)
                    if (!valid(v)) throw std::runtime_error("Invalid material vector value: " + path);
            }
        }
    }

    inline void applyMaterialAuthoringPatch(nlohmann::json& material,
        const nlohmann::json& patch) {
        validateMaterialAuthoringPatch(patch);
        for (const auto& [path, value] : patch.at("values").items()) {
            // Scale alone cannot create a valid texture use without an index.
            if ((path == "/normalTexture/scale" && !material.contains("normalTexture")) ||
                (path == "/occlusionTexture/strength" && !material.contains("occlusionTexture")))
                throw std::runtime_error("Material edit requires an existing texture: " + path);
            material[nlohmann::json::json_pointer(path)] = value;
        }
    }

    inline nlohmann::json materialAuthoringSourceValues(const SourceMaterial& material) {
        nlohmann::json values = nlohmann::json::object();
        const auto& pbr = material.metallicRoughness;
        values["/pbrMetallicRoughness/baseColorFactor"] = {
            pbr.baseColorFactor.value.r, pbr.baseColorFactor.value.g,
            pbr.baseColorFactor.value.b, pbr.baseColorFactor.value.a};
        values["/pbrMetallicRoughness/metallicFactor"] = pbr.metallicFactor.value;
        values["/pbrMetallicRoughness/roughnessFactor"] = pbr.roughnessFactor.value;
        values["/emissiveFactor"] = {material.emissiveFactor.value.r,
            material.emissiveFactor.value.g, material.emissiveFactor.value.b};
        values["/doubleSided"] = material.doubleSided.value;
        values["/alphaMode"] = material.alphaMode.value == SourceAlphaMode::Blend ? "BLEND" :
            material.alphaMode.value == SourceAlphaMode::Mask ? "MASK" : "OPAQUE";
        values["/alphaCutoff"] = material.alphaCutoff.value;
        if (findSourceTexture(material, SourceTextureSemantic::Normal))
            values["/normalTexture/scale"] = material.normalScale.value;
        if (findSourceTexture(material, SourceTextureSemantic::Occlusion))
            values["/occlusionTexture/strength"] = material.occlusionStrength.value;
        for (const auto& extension : material.extensions)
            for (const auto& property : extension.properties)
                values["/extensions/" + extension.name + "/" + property.name] =
                    property.canonicalValue == "infinity" ? nlohmann::json("infinity") :
                        nlohmann::json::parse(property.canonicalValue);
        return values;
    }

    // Apply the same authoring schema to an already ingested source snapshot.
    // Preserve texture identities, samplers, unsupported extensions and provenance;
    // a preview must not reconstruct these from display values or packed GPU data.
    inline SourceMaterial withMaterialAuthoringPatch(SourceMaterial source,
        const nlohmann::json& patch) {
        validateMaterialAuthoringPatch(patch);
        const auto& values = patch.at("values");
        nlohmann::json material = nlohmann::json::object();
        for (const auto& [path, value] : values.items()) {
            if (path == "/normalTexture/scale" || path == "/occlusionTexture/strength") continue;
            material[nlohmann::json::json_pointer(path)] = value;
        }
        const auto document = importGltfSourceMaterialsJson(nlohmann::json{
            {"asset", {{"version", "2.0"}}}, {"materials", {material}}}.dump());
        if (document.hasErrors() || document.materials().empty())
            throw std::runtime_error("Material preview source is invalid");
        const auto& edited = document.materials().front();
        if (values.contains("/pbrMetallicRoughness/baseColorFactor")) source.metallicRoughness.baseColorFactor = edited.metallicRoughness.baseColorFactor;
        if (values.contains("/pbrMetallicRoughness/metallicFactor")) source.metallicRoughness.metallicFactor = edited.metallicRoughness.metallicFactor;
        if (values.contains("/pbrMetallicRoughness/roughnessFactor")) source.metallicRoughness.roughnessFactor = edited.metallicRoughness.roughnessFactor;
        if (values.contains("/emissiveFactor")) source.emissiveFactor = edited.emissiveFactor;
        if (values.contains("/extensions/KHR_materials_emissive_strength/emissiveStrength")) source.emissiveStrength = edited.emissiveStrength;
        if (values.contains("/alphaMode")) source.alphaMode = edited.alphaMode;
        if (values.contains("/alphaCutoff")) source.alphaCutoff = edited.alphaCutoff;
        if (values.contains("/doubleSided")) source.doubleSided = edited.doubleSided;
        for (const auto& [path, value] : values.items()) {
            if (path.starts_with("/extensions/")) {
                const auto slash = path.find('/', 12);
                const auto name = path.substr(12, slash - 12);
                const auto propertyName = path.substr(slash + 1);
                auto extension = std::find_if(source.extensions.begin(), source.extensions.end(),
                    [&](const auto& e) { return e.name == name; });
                if (extension == source.extensions.end()) {
                    const auto* added = findSourceExtension(edited, name);
                    if (!added) throw std::runtime_error("Preview extension was not ingested: " + name);
                    source.extensions.push_back(*added);
                    continue;
                }
                auto property = std::find_if(extension->properties.begin(), extension->properties.end(),
                    [&](const auto& p) { return p.name == propertyName; });
                SourceExtensionProperty replacement{propertyName, value.dump(), SourceValueOrigin::Authored};
                if (property == extension->properties.end()) extension->properties.push_back(replacement);
                else *property = replacement;
                auto canonical = nlohmann::json::parse(extension->canonicalValues);
                canonical[propertyName] = value;
                extension->canonicalValues = canonical.dump();
            }
        }
        for (const auto& [path, semantic] : {
            std::pair{"/normalTexture/scale", SourceTextureSemantic::Normal},
            std::pair{"/occlusionTexture/strength", SourceTextureSemantic::Occlusion}}) {
            if (!values.contains(path)) continue;
            if (!findSourceTexture(source, semantic))
                throw std::runtime_error("Material edit requires an existing texture: " + std::string(path));
            const float value = values.at(path).get<float>();
            if (semantic == SourceTextureSemantic::Normal) source.normalScale = {value, SourceValueOrigin::Authored};
            else source.occlusionStrength = {value, SourceValueOrigin::Authored};
            for (auto& texture : source.textures)
                if (texture.semantic == semantic) texture.scalar = {value, SourceValueOrigin::Authored};
        }
        return source;
    }
}
