#include "ProjectSettingsPanel.h"
#include "editor/EditorUIState.h"
#include "editor/Reflection.h"
#include "renderer/rhi/ReflectionProbeSettings.h"
#include <imgui.h>
#include <array>

ProjectSettingsPanel::ProjectSettingsPanel(bool* isOpenPtr,
    EditorOutputSettings* outputSettingsPtr,
    Iridium::ProjectShadowSettings* shadowSettingsPtr,
    bool* shadowSettingsChangedPtr,
    Iridium::ProjectReflectionProbeSettings* reflectionProbeSettingsPtr,
    bool* reflectionProbeSettingsChangedPtr)
    : isOpen(isOpenPtr), outputSettings(outputSettingsPtr),
      shadowSettings(shadowSettingsPtr),
      shadowSettingsChanged(shadowSettingsChangedPtr),
      reflectionProbeSettings(reflectionProbeSettingsPtr),
      reflectionProbeSettingsChanged(reflectionProbeSettingsChangedPtr) {
}

namespace {
    const char* transportName(Iridium::Color::OutputTransport transport) {
        switch (transport) {
        case Iridium::Color::OutputTransport::SdrSrgb: return "Windows SDR (sRGB)";
        case Iridium::Color::OutputTransport::ScRgb: return "Windows HDR (scRGB)";
        case Iridium::Color::OutputTransport::Hdr10Pq: return "HDR10 (Rec.2100 PQ)";
        }
        return "Unknown";
    }
}

void ProjectSettingsPanel::OnImGuiRender(Registry& registry, Iridium::AssetManager* assetManager) {
    // 1. THE GATEKEEPER
    // If the boolean is false (because the Menu Bar hasn't toggled it), we exit instantly.
    // The window draws nothing and takes up zero CPU time.
    if (!*isOpen) return;

    // 2. THE WINDOW
    // By passing 'isOpen' (which is already a pointer) as the second argument, 
    // ImGui automatically gives the window an "X" close button in the top right.
    // If the user clicks that "X", ImGui automatically sets *isOpen to false!
    if (ImGui::Begin("Project Settings", isOpen)) {

        ImGui::Text("Display and HDR");
        ImGui::Separator();

        ImGui::Text("Active transport: %s", transportName(outputSettings->transport));
        const bool hdrTransport = outputSettings->transport !=
            Iridium::Color::OutputTransport::SdrSrgb;
        if (!hdrTransport) {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                "Iridium is currently outputting SDR, even if Windows HDR is enabled.");
            ImGui::TextWrapped("Exclusive fullscreen is not required. Select the scRGB "
                "or HDR10 startup transport to activate HDR luminance calibration.");
            ImGui::TextWrapped("For the Windows editor, start Iridium with "
                "--output-transport scrgb. HDR10 is also available for direct PQ "
                "transport testing.");
        }
        else {
            ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.5f, 1.0f),
                "HDR output transport is active.");
        }
        ImGui::TextDisabled("Transport changes still require an application restart.");
        ImGui::Spacing();

        bool changed = Reflection::DrawField("Scene exposure (EV)",
            outputSettings->manualExposureEv, -16.0f, 16.0f);
        changed |= Reflection::DrawField("UI / paper white (nits)",
            outputSettings->paperWhiteNits, 80.0f, 1000.0f);
        if (outputSettings->peakNits < outputSettings->paperWhiteNits) {
            outputSettings->peakNits = outputSettings->paperWhiteNits;
            changed = true;
        }
        changed |= Reflection::DrawField("Display peak (nits)",
            outputSettings->peakNits, outputSettings->paperWhiteNits, 10000.0f);
        ImGui::TextWrapped("Paper white controls editor/UI brightness. Peak limits "
            "scene highlights and updates HDR10 display metadata when available.%s",
            hdrTransport ? "" : " These values are retained but do not alter SDR output.");

        ImGui::TextDisabled(
            "Double-click or Ctrl-click a numeric control to type a value.");
        if (ImGui::Button("Reset output defaults")) {
            outputSettings->manualExposureEv = 0.0f;
            outputSettings->paperWhiteNits = 203.0f;
            outputSettings->peakNits = 1000.0f;
            changed = true;
        }
        outputSettings->changed |= changed;

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Lighting and shadows");
        ImGui::Text("Directional resolution: %u x %u",
            shadowSettings->directionalResolution,
            shadowSettings->directionalResolution);
        ImGui::TextDisabled(
            "Resolution is allocated at startup; use --shadow-directional-resolution.");
        ImGui::Text("Spot atlas: %u x %u",
            shadowSettings->spotAtlasResolution,
            shadowSettings->spotAtlasResolution);
        ImGui::TextDisabled(
            "Allocated at startup; use --shadow-spot-atlas-resolution.");
        ImGui::Text("Point cube pools: 256x%u, 512x%u, 1024x%u",
            shadowSettings->pointPool256Capacity,
            shadowSettings->pointPool512Capacity,
            shadowSettings->pointPool1024Capacity);
        ImGui::TextDisabled(
            "Point resolution is selected per light; pool capacities allocate at startup.");
        ImGui::TextWrapped("High-end defaults reserve 4096 directional maps and an "
            "8192 spot atlas. Ultra spot lights receive 4096 tiles; Ultra point "
            "lights receive 1024 cube faces. Lower per-light qualities reduce cost.");
        constexpr const char* filterModes[]{
            "Fixed 5x5 PCF", "Contact-hardening PCSS" };
        int filterMode = static_cast<int>(shadowSettings->filterMode);
        bool shadowChanged = ImGui::Combo("Shadow filter", &filterMode,
            filterModes, static_cast<int>(std::size(filterModes)));
        shadowSettings->filterMode = static_cast<Iridium::ShadowFilterMode>(
            filterMode);
        constexpr const char* qualityProfiles[]{
            "Low", "Medium", "High", "Ultra", "Cinematic" };
        int qualityProfile = static_cast<int>(shadowSettings->qualityProfile);
        shadowChanged |= ImGui::Combo("Shadow quality ceiling",
            &qualityProfile, qualityProfiles,
            static_cast<int>(std::size(qualityProfiles)));
        shadowSettings->qualityProfile =
            static_cast<Iridium::ShadowQualityProfile>(qualityProfile);
        shadowChanged |= Reflection::DrawField(
            "Directional source diameter (degrees)",
            shadowSettings->directionalSourceAngularDiameterDegrees,
            0.0f, 5.0f);
        shadowChanged |= Reflection::DrawField("Maximum penumbra (texels)",
            shadowSettings->maximumPenumbraTexels, 1.0f, 128.0f);
        const Iridium::ShadowFilterProfile activeFilter =
            Iridium::shadowFilterProfile(shadowSettings->qualityProfile);
        ImGui::TextDisabled("Profile: %u blocker / %u filter samples",
            activeFilter.blockerSearchSamples, activeFilter.filterSamples);
        int maximumLights = static_cast<int>(
            shadowSettings->maximumDirectionalLights);
        shadowChanged |= Reflection::DrawField(
            "Shadowed directional lights", maximumLights, 1,
            static_cast<int>(Iridium::kDirectionalShadowLightCapacity));
        shadowSettings->maximumDirectionalLights =
            static_cast<uint32_t>(maximumLights);
        int cascadeUpdates = static_cast<int>(
            shadowSettings->maximumCascadeUpdatesPerLight);
        shadowChanged |= Reflection::DrawField(
            "Cascade updates / light / frame", cascadeUpdates, 1,
            static_cast<int>(Iridium::kDirectionalShadowCascadeCount));
        shadowSettings->maximumCascadeUpdatesPerLight =
            static_cast<uint32_t>(cascadeUpdates);
        shadowChanged |= Reflection::DrawField("Cascade split blend",
            shadowSettings->directionalSplitLambda, 0.0f, 1.0f);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Higher values place more cascade resolution near the "
                "camera; lower values distribute it more evenly over view distance.");
        }
        shadowChanged |= Reflection::DrawField("Cascade guard band",
            shadowSettings->directionalGuardBandFraction, 0.0f, 0.25f);
        shadowChanged |= Reflection::DrawField("Depth padding (meters)",
            shadowSettings->directionalDepthPaddingMeters, 0.0f, 1000.0f);
        constexpr uint64_t texelsPerMebiTexel = 1024ull * 1024ull;
        int spotBudgetMebiTexels = static_cast<int>(
            shadowSettings->maximumSpotRenderedTexelsPerFrame /
            texelsPerMebiTexel);
        shadowChanged |= Reflection::DrawField(
            "Spot update budget (MiTexels / frame)",
            spotBudgetMebiTexels, 1, 64);
        shadowSettings->maximumSpotRenderedTexelsPerFrame =
            static_cast<uint64_t>(spotBudgetMebiTexels) *
            texelsPerMebiTexel;
        int compatibleStaleFrames = static_cast<int>(
            shadowSettings->maximumCompatibleSpotStaleFrames);
        shadowChanged |= Reflection::DrawField(
            "Compatible stale spot frames", compatibleStaleFrames, 0, 8);
        shadowSettings->maximumCompatibleSpotStaleFrames =
            static_cast<uint32_t>(compatibleStaleFrames);
        int pointBudgetMebiTexels = static_cast<int>(
            shadowSettings->maximumPointRenderedTexelsPerFrame /
            texelsPerMebiTexel);
        shadowChanged |= Reflection::DrawField(
            "Point update budget (MiTexels / frame)",
            pointBudgetMebiTexels, 1, 128);
        shadowSettings->maximumPointRenderedTexelsPerFrame =
            static_cast<uint64_t>(pointBudgetMebiTexels) *
            texelsPerMebiTexel;
        int compatiblePointStaleFrames = static_cast<int>(
            shadowSettings->maximumCompatiblePointStaleFrames);
        shadowChanged |= Reflection::DrawField(
            "Compatible stale point frames", compatiblePointStaleFrames, 0, 8);
        shadowSettings->maximumCompatiblePointStaleFrames =
            static_cast<uint32_t>(compatiblePointStaleFrames);
        ImGui::TextWrapped("Per-light Casts shadows, Shadow quality, and Priority "
            "remain editable on each Light component in the Inspector.");
        if (ImGui::Button("Reset shadow defaults")) {
            const uint32_t activeResolution =
                shadowSettings->directionalResolution;
            const uint32_t activeSpotResolution =
                shadowSettings->spotAtlasResolution;
            const std::array activePointCapacities{
                shadowSettings->pointPool256Capacity,
                shadowSettings->pointPool512Capacity,
                shadowSettings->pointPool1024Capacity };
            *shadowSettings = Iridium::ProjectShadowSettings{};
            shadowSettings->directionalResolution = activeResolution;
            shadowSettings->spotAtlasResolution = activeSpotResolution;
            shadowSettings->pointPool256Capacity = activePointCapacities[0];
            shadowSettings->pointPool512Capacity = activePointCapacities[1];
            shadowSettings->pointPool1024Capacity = activePointCapacities[2];
            shadowChanged = true;
        }
        *shadowSettingsChanged |= shadowChanged;

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Text("Reflection probes");
        int captureBudgetMebiTexels = static_cast<int>(
            reflectionProbeSettings->maximumRenderedTexelsPerFrame /
            texelsPerMebiTexel);
        bool probeChanged = Reflection::DrawField(
            "Capture update budget (MiTexels / frame)",
            captureBudgetMebiTexels, 1, 128);
        reflectionProbeSettings->maximumRenderedTexelsPerFrame =
            static_cast<uint64_t>(captureBudgetMebiTexels) *
            texelsPerMebiTexel;
        int facesPerProbe = static_cast<int>(
            reflectionProbeSettings->maximumFacesPerProbePerFrame);
        probeChanged |= Reflection::DrawField(
            "Faces / probe / frame", facesPerProbe, 1, 6);
        reflectionProbeSettings->maximumFacesPerProbePerFrame =
            static_cast<uint32_t>(facesPerProbe);
        int capturesInFlight = static_cast<int>(
            reflectionProbeSettings->maximumCapturesInFlight);
        probeChanged |= Reflection::DrawField(
            "Captures in flight", capturesInFlight, 1, 4);
        reflectionProbeSettings->maximumCapturesInFlight =
            static_cast<uint32_t>(capturesInFlight);
        int realtimeInterval = static_cast<int>(
            reflectionProbeSettings->minimumRealtimeFramesBetweenCaptures);
        probeChanged |= Reflection::DrawField(
            "Realtime minimum frame interval", realtimeInterval, 1, 600);
        reflectionProbeSettings->minimumRealtimeFramesBetweenCaptures =
            static_cast<uint32_t>(realtimeInterval);
        int prefilterSamples = static_cast<int>(
            reflectionProbeSettings->prefilterSampleCount);
        probeChanged |= Reflection::DrawField(
            "GGX prefilter samples", prefilterSamples, 64, 1024);
        reflectionProbeSettings->prefilterSampleCount =
            static_cast<uint32_t>(prefilterSamples);
        ImGui::TextWrapped("Capture resolution and clip range remain per-probe. "
            "Realtime capture is intentionally cadence-limited; On demand is "
            "the recommended default for stable high-fidelity scenes.");
        if (ImGui::Button("Reset reflection probe defaults")) {
            *reflectionProbeSettings =
                Iridium::ProjectReflectionProbeSettings{};
            probeChanged = true;
        }
        *reflectionProbeSettingsChanged |= probeChanged;
    }

    // 3. CLEANUP
    ImGui::End();
}
