#include "editor/panels/windows/MaterialDiagnosticsPanel.h"

#include "assets/AssetManager.h"
#include "assets/MaterialProvenance.h"
#include "material/MaterialDiagnosticEvaluation.h"
#include "renderer/rhi/Mesh.h"
#include "scene/components/MeshComponent.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>

namespace {
    using Json = nlohmann::json;

    std::string lower(std::string_view value) {
        std::string result(value);
        std::transform(result.begin(), result.end(), result.begin(),
            [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
        return result;
    }

    bool matches(const Iridium::MaterialProvenance& material,
        std::string_view query) {
        if (query.empty()) return true;
        const std::string needle = lower(query);
        std::string haystack = material.sourceName + " " + material.sourceAsset + " " +
            material.compiledWorkflow + " " + material.compiledClosure + " " +
            std::to_string(material.sourceMaterialIndex);
        for (const std::string& warning : material.warnings) haystack += " " + warning;
        return lower(haystack).find(needle) != std::string::npos;
    }

    std::string materialLabel(const Iridium::MaterialProvenance& material) {
        return std::to_string(material.sourceMaterialIndex) + ": " +
            (material.sourceName.empty() ? "unnamed" : material.sourceName);
    }

    std::string jsonText(const Json& value) {
        if (value.is_string()) return value.get<std::string>();
        if (value.is_null()) return "none";
        return value.dump();
    }

    void property(const char* label, const Json& object, const char* key,
        const char* originKey = nullptr) {
        if (!object.contains(key)) return;
        const std::string value = jsonText(object.at(key));
        ImGui::Text("%s:", label);
        ImGui::SameLine();
        ImGui::TextUnformatted(value.c_str());
        if (originKey && object.contains(originKey)) {
            const std::string origin = jsonText(object.at(originKey));
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", origin.c_str());
        }
    }

    const Iridium::MaterialDiagnosticTexturePreview* previewFor(
        const Iridium::MaterialProvenance& material, std::string_view semantic) {
        const auto found = std::find_if(material.diagnosticTexturePreviews.begin(),
            material.diagnosticTexturePreviews.end(), [&](const auto& candidate) {
                return candidate.semantic == semantic;
            });
        return found == material.diagnosticTexturePreviews.end() ? nullptr : &*found;
    }

    void textureTable(const Json& textures, const Iridium::MaterialProvenance& material,
        Iridium::AssetManager* assetManager, const float selectedUv[2]) {
        if (!textures.is_array() || textures.empty()) {
            ImGui::TextDisabled("No source textures.");
            return;
        }
        if (!ImGui::BeginTable("diagnostic-textures", 8,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_SizingStretchProp)) return;
        for (const char* heading : { "Preview", "Semantic", "Image", "Channels",
            "Transfer", "UV", "Sampler", "Evaluated UV" })
            ImGui::TableSetupColumn(heading);
        ImGui::TableHeadersRow();
        for (const Json& texture : textures) {
            const std::string semantic = texture.value("semantic", "unknown");
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const auto* preview = previewFor(material, semantic);
            void* image = preview && assetManager
                ? assetManager->getMaterialTexturePreview(preview->texture) : nullptr;
            if (image) ImGui::Image(image, ImVec2(56.0f, 56.0f));
            else ImGui::TextDisabled("unavailable");
            if (preview && preview->engineFallback) ImGui::TextDisabled("fallback");
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(semantic.c_str());
            ImGui::TableSetColumnIndex(2);
            const std::string imageName = texture.value("image", "none");
            ImGui::TextWrapped("%s", imageName.c_str());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(texture.value("channels", "-").c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(texture.value("transfer", "-").c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("set %u", texture.value("uv", 0u));
            const auto offset = texture.value("offset", std::vector<float>{ 0.0f, 0.0f });
            const auto scale = texture.value("scale", std::vector<float>{ 1.0f, 1.0f });
            ImGui::TextDisabled("o %.3g,%.3g s %.3g,%.3g r %.3g",
                offset.at(0), offset.at(1), scale.at(0), scale.at(1),
                texture.value("rotation", 0.0f));
            ImGui::TableSetColumnIndex(6);
            const std::string sampler = "mag " + jsonText(texture.value("mag_filter", Json{})) +
                ", min " + jsonText(texture.value("min_filter", Json{})) + ", wrap " +
                std::to_string(texture.value("wrap_s", 10497)) + "/" +
                std::to_string(texture.value("wrap_t", 10497));
            ImGui::TextWrapped("%s", sampler.c_str());
            ImGui::TableSetColumnIndex(7);
            const float rotation = texture.value("rotation", 0.0f);
            const glm::vec2 evaluated = Iridium::evaluateMaterialTextureUv(
                { selectedUv[0], selectedUv[1] }, { offset.at(0), offset.at(1) },
                { scale.at(0), scale.at(1) }, rotation);
            ImGui::Text("%.5g, %.5g", evaluated.x, evaluated.y);
        }
        ImGui::EndTable();
    }

    void scalarClosureValues(const Json& object) {
        property("Base color", object, "base_color");
        property("Metallic", object, "metallic");
        property("Perceptual roughness", object, "roughness");
        property("IOR", object, "ior");
        property("Specular factor", object, "specular_factor");
        property("Specular color", object, "specular_color");
        property("Emissive", object, "emissive");
        property("Emissive strength", object, "emissive_strength");
        property("Normal scale", object, "normal_scale");
        property("AO strength", object, "occlusion_strength");
        property("Alpha mode", object, "alpha_mode");
        property("Alpha cutoff", object, "alpha_cutoff");
        property("Double sided", object, "double_sided");
    }

    std::string closureReason(const Json& compiled) {
        const std::string closure = compiled.value("closure", "invalid");
        if (closure == "standard-deferred")
            return "Equivalent single-lobe surface; canonical deferred closure is lossless.";
        if (closure == "unlit-forward")
            return "Unlit source material; routed forward without lighting evaluation.";
        std::string reason = "Requires the complex forward closure";
        if (compiled.contains("complex_lobes") && !compiled.at("complex_lobes").empty()) {
            reason += ": ";
            bool first = true;
            for (const Json& lobe : compiled.at("complex_lobes")) {
                if (!first) reason += ", ";
                reason += lobe.value("type", "unknown");
                first = false;
            }
        }
        return reason + ".";
    }
}

MaterialDiagnosticsPanel::MaterialDiagnosticsPanel(bool* isOpen,
    Entity* selectedEntity) : isOpen_(isOpen), selectedEntity_(selectedEntity) {}

void MaterialDiagnosticsPanel::OnImGuiRender(Registry& registry,
    Iridium::AssetManager* assetManager) {
    if (!isOpen_ || !*isOpen_) return;
    if (!ImGui::Begin("Material Diagnostics", isOpen_)) { ImGui::End(); return; }
    if (!selectedEntity_ || *selectedEntity_ == NULL_ENTITY) {
        ImGui::TextUnformatted("Select an entity with a mesh to inspect its materials.");
        ImGui::End(); return;
    }
    auto* meshPool = registry.getPool<MeshComponent>();
    if (!meshPool || !meshPool->has(*selectedEntity_)) {
        ImGui::TextUnformatted("The selected entity has no mesh component.");
        ImGui::End(); return;
    }
    const auto& model = meshPool->get(*selectedEntity_).model;
    const auto materials = model && assetManager
        ? assetManager->getMaterialProvenance(*model)
        : std::span<const Iridium::MaterialProvenance>{};
    if (materials.empty()) {
        ImGui::TextUnformatted("No material diagnostics are available for this model.");
        ImGui::End(); return;
    }

    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputTextWithHint("##material-search", "Search name, index, closure, warning...",
        search_.data(), search_.size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear")) search_.fill('\0');

    selectedMaterial_ = std::min(selectedMaterial_, materials.size() - 1);
    if (!matches(materials[selectedMaterial_], search_.data())) {
        const auto found = std::find_if(materials.begin(), materials.end(),
            [&](const auto& candidate) { return matches(candidate, search_.data()); });
        if (found != materials.end()) selectedMaterial_ =
            static_cast<size_t>(std::distance(materials.begin(), found));
    }
    const std::string selectedLabel = materialLabel(materials[selectedMaterial_]);
    if (ImGui::BeginCombo("Material", selectedLabel.c_str())) {
        for (size_t index = 0; index < materials.size(); ++index) {
            if (!matches(materials[index], search_.data())) continue;
            const std::string label = materialLabel(materials[index]);
            if (ImGui::Selectable(label.c_str(), index == selectedMaterial_))
                selectedMaterial_ = index;
        }
        ImGui::EndCombo();
    }

    const Iridium::MaterialProvenance& material = materials[selectedMaterial_];
    ImGui::TextWrapped("Source: %s", material.sourceAsset.c_str());
    ImGui::Text("Workflow: %s    Closure: %s", material.compiledWorkflow.c_str(),
        material.compiledClosure.c_str());
    if (!material.diagnosticSnapshotSha256.empty())
        ImGui::TextDisabled("Snapshot SHA-256: %s", material.diagnosticSnapshotSha256.c_str());

    if (material.diagnosticSnapshotJson.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1.0f),
            "Compiled/packed snapshot unavailable.");
        ImGui::TextWrapped("Load a material-bearing model to inspect the complete source-to-GPU contract.");
        for (const std::string& warning : material.warnings) ImGui::BulletText("%s", warning.c_str());
        ImGui::End(); return;
    }

    if (cachedSnapshotHash_ != material.diagnosticSnapshotSha256) {
        try {
            cachedSnapshot_ = Json::parse(material.diagnosticSnapshotJson);
            cachedSnapshotHash_ = material.diagnosticSnapshotSha256;
        }
        catch (const std::exception& exception) {
            cachedSnapshot_.clear();
            cachedSnapshotHash_.clear();
            ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1),
                "Invalid snapshot: %s", exception.what());
            ImGui::End(); return;
        }
    }
    const Json& document = cachedSnapshot_;
    const Json& source = document.at("source");
    const Json& compiled = document.at("compiled");
    const Json& instance = document.at("instance");
    const Json& packed = document.at("packed");
    const std::string reason = closureReason(compiled);
    ImGui::TextWrapped("Classification: %s", reason.c_str());

    if (ImGui::CollapsingHeader("Source material", ImGuiTreeNodeFlags_DefaultOpen)) {
        property("Base color", source, "base_color", "base_color_origin");
        property("Metallic", source, "metallic", "metallic_origin");
        property("Roughness", source, "roughness", "roughness_origin");
        property("Emissive", source, "emissive", "emissive_origin");
        property("Emissive strength", source, "emissive_strength", "emissive_strength_origin");
        property("Normal scale", source, "normal_scale", "normal_scale_origin");
        property("AO strength", source, "occlusion_strength", "occlusion_strength_origin");
        property("Alpha mode", source, "alpha_mode", "alpha_mode_origin");
        property("Alpha cutoff", source, "alpha_cutoff", "alpha_cutoff_origin");
        property("Double sided", source, "double_sided", "double_sided_origin");
    }

    if (ImGui::CollapsingHeader("Texture interpretation", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat2("Selected UV", selectedUv_, 0.005f, -16.0f, 16.0f,
            "%.5f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SameLine(); ImGui::TextDisabled("double-click or Ctrl-click for text input");
        textureTable(source.at("textures"), material, assetManager, selectedUv_);
    }

    if (ImGui::CollapsingHeader("Compiled closure", ImGuiTreeNodeFlags_DefaultOpen)) {
        property("Schema", compiled, "schema_version");
        property("Content hash", compiled, "hash");
        property("Workflow", compiled, "workflow");
        property("Closure", compiled, "closure");
        property("Feature flags", compiled, "feature_flags");
        scalarClosureValues(compiled);
        ImGui::TextWrapped("Reason: %s", reason.c_str());
        if (compiled.contains("complex_lobes") && !compiled.at("complex_lobes").empty()) {
            ImGui::TextUnformatted("Complex lobes:");
            for (const Json& lobe : compiled.at("complex_lobes"))
                ImGui::BulletText("%s (%s)", lobe.value("type", "unknown").c_str(),
                    lobe.value("source_extension", "source").c_str());
        }
        ImGui::Text("Texture operations: %zu",
            compiled.value("texture_operations", Json::array()).size());
    }

    if (ImGui::CollapsingHeader("Material instance", ImGuiTreeNodeFlags_DefaultOpen)) {
        property("Revision", instance, "revision");
        property("Override mask", instance, "override_mask");
        scalarClosureValues(instance);
        const Json overrides = instance.value("overrides", Json::array());
        ImGui::Text("Overrides: %s", overrides.empty() ? "none" : overrides.dump().c_str());
        if (ImGui::TreeNode("Texture bindings")) {
            for (const Json& binding : instance.value("texture_bindings", Json::array()))
                ImGui::BulletText("%s: texture %u:%u, sampler %u:%u",
                    binding.value("semantic", "unknown").c_str(),
                    binding.value("texture_index", 0u), binding.value("texture_generation", 0u),
                    binding.value("sampler_index", 0u), binding.value("sampler_generation", 0u));
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("Packed GPU material", ImGuiTreeNodeFlags_DefaultOpen)) {
        property("Schema", packed, "schema_version");
        property("Bytes", packed, "byte_size");
        property("Packed SHA-256", packed, "sha256");
        property("Closure ID", packed, "closure");
        property("Workflow ID", packed, "workflow");
        property("Feature flags", packed, "feature_flags");
        property("Texture mask", packed, "texture_mask");
        property("Complex lobe count", packed, "complex_lobe_count");
        property("Base color", packed, "base_color");
        property("Metallic / roughness / IOR / specular", packed,
            "metallic_roughness_ior_specular");
        property("Specular color / normal scale", packed, "specular_color_normal_scale");
        property("Emissive / strength", packed, "emissive_strength");
        if (ImGui::TreeNode("Texture indices and packed half bits")) {
            property("Texture indices", packed, "texture_indices");
            property("Half-bit records", packed, "texture_half_bits");
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("Diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
        const Json diagnostics = document.value("diagnostics", Json::array());
        if (diagnostics.empty() && material.warnings.empty()) ImGui::TextDisabled("No diagnostics.");
        for (const Json& diagnostic : diagnostics) {
            const bool error = diagnostic.value("severity", "warning") == "error";
            ImGui::TextColored(error ? ImVec4(1, .3f, .3f, 1) : ImVec4(1, .75f, .25f, 1),
                "%s %s", diagnostic.value("stage", "compile").c_str(),
                diagnostic.value("code", "diagnostic").c_str());
            ImGui::SameLine(); ImGui::TextWrapped("%s",
                diagnostic.value("message", "").c_str());
        }
        for (const std::string& warning : material.warnings) ImGui::BulletText("%s", warning.c_str());
    }

    if (ImGui::CollapsingHeader("Runtime routing")) {
        ImGui::Text("Material / pipeline handles: %u / %u",
            material.gpuBinding.material.id, material.gpuBinding.pipeline.id);
        const char* queueName = "deferred opaque";
        if (material.runtime.renderQueue == Iridium::RenderQueue::ForwardOpaque) {
            queueName = "forward opaque";
        }
        else if (material.runtime.renderQueue == Iridium::RenderQueue::Transparent) {
            queueName = "forward transparent";
        }
        ImGui::Text("Queue: %s", queueName);
        ImGui::Text("Shader: %u", static_cast<unsigned>(material.runtime.pipelineState.shaderProgram));
    }
    ImGui::Separator();
    ImGui::TextDisabled("Material/closure ID debug views describe deferred opaque surfaces.");
    ImGui::TextDisabled("Forward closures provide matching diagnostic views without entering the GBuffer.");
    ImGui::End();
}
