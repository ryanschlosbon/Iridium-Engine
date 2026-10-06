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
    bool* reflectionProbeSettingsChangedPtr, int* layeredInterfaceOverridePtr)
    : isOpen(isOpenPtr), outputSettings(outputSettingsPtr),
      shadowSettings(shadowSettingsPtr),
      shadowSettingsChanged(shadowSettingsChangedPtr),
      reflectionProbeSettings(reflectionProbeSettingsPtr),
      reflectionProbeSettingsChanged(reflectionProbeSettingsChangedPtr),
      layeredInterfaceOverride(layeredInterfaceOverridePtr) {
}

namespace {
    const char* transportName(Iridium::Color::OutputTransport transport) {
        switch (transport) {
        case Iridium::Color::OutputTransport::SdrSrgb: return "Windows SDR (sRGB)";
        case Iridium::Color::OutputTransport::ScRgb: return "Windows HDR (scRGB)";
        case Iridium::Color::OutputTransport::Hdr10Pq: return "HDR10 (Rec.2100 PQ)";
        case Iridium::Color::OutputTransport::Automatic: return "Auto (prefer scRGB)";
        }
        return "Unknown";
    }

    bool transportSupported(const EditorOutputSettings& settings,
        Iridium::Color::OutputTransport transport) {
        if (transport == Iridium::Color::OutputTransport::Automatic) return true;
        const size_t index = static_cast<size_t>(transport);
        return index < settings.supportedTransports.size() &&
            settings.supportedTransports[index];
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
    ImGui::SetNextWindowSize(ImVec2(980, 720), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Project Settings", isOpen)) {
        search_.Draw("Search settings", -1.0f);
        constexpr const char* categories[]{"Display and HDR", "Lighting and shadows",
            "Reflection probes", "Transparency", "Anti-aliasing", "Post-processing"};
        constexpr const char* searchTerms[]{
            "Display HDR transport SDR scRGB exposure EV paper white nits peak brightness",
            "Lighting shadows directional resolution spot atlas point pool PCF PCSS filter quality penumbra cascade split guard depth padding budget stale",
            "Reflection probes capture budget faces flight realtime interval GGX prefilter samples",
            "Transparency glass layers interfaces quality Ordinary2 Hero4 Cinematic8 bulbs headlight override",
            "Anti-aliasing AA TAA temporal jitter",
            "Post-processing exposure auto-exposure eye adaptation EV metering bloom glow scatter intensity threshold radius tint"};
        ImGui::BeginChild("Settings categories", ImVec2(190, 0), ImGuiChildFlags_Borders);
        for (int index = 0; index < 6; ++index) {
            if (search_.IsActive() && !search_.PassFilter(searchTerms[index])) continue;
            if (ImGui::Selectable(categories[index], selectedCategory_ == index)) {
                selectedCategory_ = index;
                search_.Clear();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("Settings content", ImVec2(0, 0), ImGuiChildFlags_Borders);
        const auto show = [&](int category) {
            return search_.IsActive() ? search_.PassFilter(searchTerms[category]) : selectedCategory_ == category;
        };
        if (search_.IsActive()) ImGui::TextDisabled("Showing matching setting groups. Select a category to clear search.");
        if (!show(0) && !show(1) && !show(2) && !show(3) && !show(4) && !show(5))
            ImGui::TextWrapped("No matching settings.");
        constexpr uint64_t texelsPerMebiTexel = 1024ull * 1024ull;
        if (show(0)) {

        ImGui::Text("Display and HDR");
        ImGui::Separator();

        bool changed = false;
        if (ImGui::BeginCombo("Display transport",
                transportName(outputSettings->transport))) {
            constexpr std::array transports{
                Iridium::Color::OutputTransport::Automatic,
                Iridium::Color::OutputTransport::SdrSrgb,
                Iridium::Color::OutputTransport::ScRgb,
                Iridium::Color::OutputTransport::Hdr10Pq,
            };
            for (const Iridium::Color::OutputTransport transport : transports) {
                const bool supported = transportSupported(*outputSettings, transport);
                ImGui::BeginDisabled(!supported);
                if (ImGui::Selectable(transportName(transport),
                        outputSettings->transport == transport) && supported) {
                    outputSettings->transport = transport;
                    changed = true;
                }
                if (outputSettings->transport == transport) {
                    ImGui::SetItemDefaultFocus();
                }
                ImGui::EndDisabled();
                if (!supported && ImGui::IsItemHovered(
                        ImGuiHoveredFlags_AllowWhenDisabled)) {
                    ImGui::SetTooltip(
                        "This display/desktop does not expose the required Vulkan format and color space.");
                }
            }
            ImGui::EndCombo();
        }
        ImGui::Text("Active transport: %s",
            transportName(outputSettings->effectiveTransport));
        const bool hdrTransport = outputSettings->effectiveTransport !=
            Iridium::Color::OutputTransport::SdrSrgb;
        if (!hdrTransport) {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.25f, 1.0f),
                "Iridium is currently outputting SDR, even if Windows HDR is enabled.");
            ImGui::TextWrapped("Enable HDR in Windows, then choose Auto or scRGB. "
                "Exclusive fullscreen is not required.");
        }
        else {
            ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.5f, 1.0f),
                "HDR output transport is active.");
        }
        if (!outputSettings->transportDiagnostic.empty()) {
            ImGui::TextWrapped("%s",
                outputSettings->transportDiagnostic.c_str());
        }
        ImGui::TextDisabled(
            "Changes rebuild presentation resources at the next frame boundary; scene assets stay resident.");
        ImGui::Spacing();

        changed |= Reflection::DrawField(
            outputSettings->exposureMode == Iridium::ExposureMode::Auto
                ? "Exposure compensation (EV)" : "Scene exposure (EV)",
            outputSettings->manualExposureEv, -16.0f, 16.0f);
        if (outputSettings->exposureMode == Iridium::ExposureMode::Auto)
            ImGui::TextDisabled("Auto-exposure is on (Post-processing): this offsets its result.");
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
        }

        if (show(1)) {
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
        shadowChanged |= Reflection::DrawField(
            "Directional coverage (meters)",
            shadowSettings->directionalMaximumDistanceMeters,
            1.0f, 100000.0f);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Project/profile maximum. The active camera far plane "
                "may shorten coverage but cannot dilute cascades beyond this distance.");
        }
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
        ImGui::SeparatorText("Directional Receiver Bias");
        shadowChanged |= Reflection::DrawField("Depth bias (shadow texels)",
            shadowSettings->directionalReceiverDepthBiasTexels, 0.0f, 8.0f);
        shadowChanged |= Reflection::DrawField(
            "Receiver-plane clamp (shadow texels)",
            shadowSettings->directionalReceiverPlaneClampTexels, 0.0f, 8.0f);
        shadowChanged |= Reflection::DrawField("Normal offset (shadow texels)",
            shadowSettings->directionalNormalOffsetTexels, 0.0f, 4.0f);
        ImGui::TextWrapped("Receiver-plane compensation follows the local surface "
            "slope across filter taps. The geometric-normal offset is bounded by "
            "the active cascade's world texel size.");
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
        }

        if (show(2)) {
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
        if (show(3)) {
            ImGui::SeparatorText("Transparency");
            constexpr const char* budgets[]{"Use material / primitive policy", "2 interfaces (Ordinary2)",
                "4 interfaces (Hero4)", "8 interfaces (Cinematic8)"};
            int choice = *layeredInterfaceOverride == 2 ? 1 : *layeredInterfaceOverride == 4 ? 2 :
                *layeredInterfaceOverride == 8 ? 3 : 0;
            if (ImGui::Combo("Layered Glass interface budget", &choice, budgets, 4))
                *layeredInterfaceOverride = choice == 0 ? 0 : 1 << choice;
            ImGui::TextWrapped("Session-only live override for the scene and asset viewer. "
                "It does not change saved material or primitive policies. Choose Use material / primitive policy to restore them.");
            ImGui::TextWrapped("Interfaces are glass surfaces, not transparent objects: a closed shell usually uses "
                "two interfaces. This control only affects surfaces already resolved as Layered Glass; it does not "
                "change Thin Glass, Sorted Surface, opaque bulbs, or invalid-volume fallbacks.");
            ImGui::TextWrapped("Higher tiers increase GPU and memory cost. Atlas area caps remain enforced: "
                "Ordinary2 uses up to 1/4 of the view area, Hero4 1/2, Cinematic8 the full area. "
                "Overflow uses a bounded fallback, not unlimited accurate layers.");
            if (*layeredInterfaceOverride != 0) {
                ImGui::TextColored(ImVec4(1, .85f, .2f, 1), "Project-session override active: authored Layered Glass quality is temporarily replaced.");
            }
            ImGui::TextWrapped("Missing headlight bulbs are not proof of a layer-budget problem. "
                "Check the resolved transparency class and isolate the bulb geometry before changing its material.");
        }
        if (show(4)) {
            ImGui::SeparatorText("Anti-aliasing");
            constexpr const char* modes[]{"Off", "Temporal (TAA)"};
            int choice = outputSettings->antiAliasing == Iridium::AntiAliasingMode::Taa ? 1 : 0;
            if (ImGui::Combo("Anti-aliasing", &choice, modes, 2)) {
                outputSettings->antiAliasing = choice == 1
                    ? Iridium::AntiAliasingMode::Taa : Iridium::AntiAliasingMode::None;
                outputSettings->changed = true;
            }
            ImGui::TextWrapped("Temporal anti-aliasing jitters the camera by a sub-pixel amount each frame "
                "and accumulates the result. Changing the mode rebuilds the frame graph at the next frame "
                "boundary; accumulated history starts over.");
            if (!outputSettings->antiAliasingDiagnostic.empty())
                ImGui::TextColored(ImVec4(1, .4f, .3f, 1), "%s", outputSettings->antiAliasingDiagnostic.c_str());

            // M9.8: motion behaviour presets and the full tuning (live).
            Iridium::TemporalAntiAliasingTuning& taa = outputSettings->taaTuning;
            ImGui::BeginDisabled(outputSettings->antiAliasing != Iridium::AntiAliasingMode::Taa);
            struct Preset { const char* name; float minimum; float motionPixels; };
            constexpr Preset presets[]{
                { "Sharp (crisp motion, more crawl)", 0.70f, 2.0f },
                { "Balanced", 0.85f, 6.0f },
                { "Stable (calm motion, softer)", 0.92f, 16.0f },
            };
            int preset = 3;
            for (int index = 0; index < 3; ++index) {
                if (taa.minimumHistoryWeight == presets[index].minimum &&
                    taa.motionPixelsForMinimum == presets[index].motionPixels)
                    preset = index;
            }
            const char* presetName = preset < 3 ? presets[preset].name : "Custom";
            if (ImGui::BeginCombo("Motion preset", presetName)) {
                for (int index = 0; index < 3; ++index) {
                    if (ImGui::Selectable(presets[index].name, preset == index)) {
                        taa.minimumHistoryWeight = presets[index].minimum;
                        taa.motionPixelsForMinimum = presets[index].motionPixels;
                        outputSettings->changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("How much each new (jittered) frame replaces the accumulated image while "
                    "the camera or objects move. Sharp follows motion closely but fine edges crawl; Stable keeps "
                    "more history, so edges stay calm and motion is slightly softer.");
            if (ImGui::TreeNode("Advanced TAA tuning")) {
                const auto slider = [&](const char* label, float& value, float minimum, float maximum,
                        const char* format, const char* help) {
                    if (ImGui::SliderFloat(label, &value, minimum, maximum, format,
                            ImGuiSliderFlags_AlwaysClamp))
                        outputSettings->changed = true;
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", help);
                };
                slider("History weight in motion", taa.minimumHistoryWeight, 0.5f, 0.98f, "%.2f",
                    "The share of the accumulated image kept per frame at full motion. Higher = calmer edges "
                    "in motion but softer, with more lag.");
                if (taa.minimumHistoryWeight > taa.maximumHistoryWeight)
                    taa.maximumHistoryWeight = taa.minimumHistoryWeight;
                slider("Motion for full response", taa.motionPixelsForMinimum, 0.5f, 64.0f, "%.1f px",
                    "How many pixels per frame of motion it takes to reach the motion history weight. Larger "
                    "values keep slow movement calm.");
                slider("History weight (still)", taa.stillHistoryWeight, 0.8f, 0.99f, "%.3f",
                    "History kept per frame where nothing moves. Higher = smoother and more stable stills, "
                    "slower to settle after a change.");
                slider("Still clip width", taa.staticVarianceGamma, 1.0f, 4.0f, "%.2f",
                    "How far still pixels' history may differ from the current frame before it is clipped. "
                    "Wider keeps thin details steady; narrower reacts faster to lighting changes.");
                slider("Reconstruction sharpness", taa.reconstructionSharpness, 2.0f, 10.0f, "%.1f",
                    "Sharpness of the filter that combines each frame's jittered samples. Higher = crisper "
                    "texture, slightly more shimmer.");
                if (ImGui::Button("Reset TAA tuning")) {
                    taa = Iridium::TemporalAntiAliasingTuning{};
                    outputSettings->changed = true;
                }
                ImGui::TreePop();
            }
            ImGui::EndDisabled();
        }
        if (show(5)) {
            const auto tip = [](const char* text) {
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", text);
            };
            const auto slider = [&](const char* label, float& value, float minimum, float maximum,
                    const char* format, const char* help) {
                if (ImGui::SliderFloat(label, &value, minimum, maximum, format,
                        ImGuiSliderFlags_AlwaysClamp))
                    outputSettings->changed = true;
                tip(help);
            };

            ImGui::SeparatorText("Exposure");
            bool autoExposure = outputSettings->exposureMode == Iridium::ExposureMode::Auto;
            if (ImGui::Checkbox("Auto-exposure", &autoExposure)) {
                outputSettings->exposureMode = autoExposure
                    ? Iridium::ExposureMode::Auto : Iridium::ExposureMode::Manual;
                outputSettings->changed = true;
            }
            tip("Meters the scene each frame and adapts like an eye. Off: the fixed exposure from "
                "Display and HDR > Scene exposure (EV). On, that value is exposure compensation.");
            Iridium::AutoExposureSettings& ae = outputSettings->autoExposure;
            ImGui::BeginDisabled(!autoExposure);
            int curve = ae.adaptation == Iridium::ExposureAdaptation::Linear ? 1 : 0;
            constexpr const char* curves[]{ "Exponential (smooth)", "Linear" };
            if (ImGui::Combo("Adaptation curve", &curve, curves, 2)) {
                ae.adaptation = curve == 1 ? Iridium::ExposureAdaptation::Linear
                                           : Iridium::ExposureAdaptation::Exponential;
                outputSettings->changed = true;
            }
            tip("Exponential closes a share of the remaining gap each second: big changes start fast "
                "and ease in. Linear moves at a fixed EV per second and stops abruptly at the target.");
            const bool linear = ae.adaptation == Iridium::ExposureAdaptation::Linear;
            slider("Speed up (to brighter)", ae.speedUpEvPerSecond, 0.05f, 10.0f,
                linear ? "%.2f EV/s" : "%.2f /s",
                "How fast the eye adjusts when the scene gets brighter. Exponential: a rate per "
                "second (1 = about 63% of the gap in one second). Linear: EV per second.");
            slider("Speed down (to darker)", ae.speedDownEvPerSecond, 0.05f, 10.0f,
                linear ? "%.2f EV/s" : "%.2f /s",
                "How fast the eye adjusts when the scene gets darker. Real eyes adapt to darkness "
                "more slowly than to brightness.");
            ImGui::BeginDisabled(linear);
            slider("Max speed", ae.maximumEvPerSecond, 0.0f, 10.0f,
                ae.maximumEvPerSecond > 0.0f ? "%.2f EV/s" : "unlimited",
                "Exponential only: caps how many EV per second the exposure may move (0: no cap). "
                "Use it to soften very large jumps, such as stepping from a dark room into daylight.");
            ImGui::EndDisabled();
            slider("Min EV100", ae.minimumEv100, -10.0f, 20.0f, "%.1f",
                "The darkest exposure auto-exposure may choose (lower EV100 = brighter image in "
                "dark scenes). Raise it to keep night scenes dark.");
            slider("Max EV100", ae.maximumEv100, -10.0f, 20.0f, "%.1f",
                "The brightest-scene exposure auto-exposure may choose. Lower it so bright skies "
                "can stay bright instead of being pulled down to grey.");
            if (ae.maximumEv100 < ae.minimumEv100) ae.maximumEv100 = ae.minimumEv100;
            slider("Low percentile", ae.lowPercentile, 0.0f, 0.99f, "%.2f",
                "Metering ignores this darkest share of the pixels (deep shadows, black sky).");
            slider("High percentile", ae.highPercentile, 0.01f, 1.0f, "%.2f",
                "Metering ignores the brightest share above this point (the sun, light sources, "
                "specular highlights), so they do not darken the whole image.");
            if (ae.highPercentile <= ae.lowPercentile) ae.highPercentile = ae.lowPercentile + 0.01f;
            slider("Centre weighting", ae.centreWeight, 0.0f, 1.0f, "%.2f",
                "0 meters the whole frame equally; 1 weights the centre of the view up to 16x, like "
                "a camera's centre-weighted metering.");
            if (ImGui::TreeNode("Histogram range")) {
                slider("Histogram min EV100", ae.histogramMinEv100, -16.0f, 10.0f, "%.1f",
                    "The darkest luminance the metering histogram can see.");
                slider("Histogram max EV100", ae.histogramMaxEv100, 0.0f, 26.0f, "%.1f",
                    "The brightest luminance the metering histogram can see.");
                if (ae.histogramMaxEv100 <= ae.histogramMinEv100)
                    ae.histogramMaxEv100 = ae.histogramMinEv100 + 1.0f;
                ImGui::TreePop();
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Reset exposure defaults")) {
                ae = Iridium::AutoExposureSettings{};
                outputSettings->changed = true;
            }
            if (!outputSettings->exposureDiagnostic.empty())
                ImGui::TextColored(ImVec4(1, .4f, .3f, 1), "%s", outputSettings->exposureDiagnostic.c_str());

            ImGui::SeparatorText("Bloom");
            Iridium::BloomSettings& bloom = outputSettings->bloom;
            if (ImGui::Checkbox("Bloom", &bloom.enabled)) outputSettings->changed = true;
            tip("Scatters bright light into a glow, as a camera lens does. Turning it on or off "
                "rebuilds the frame graph at the next frame boundary; every other setting applies "
                "immediately.");
            ImGui::BeginDisabled(!bloom.enabled);
            slider("Intensity", bloom.intensity, 0.0f, 0.25f, "%.3f",
                "Without a threshold: the share of all light scattered into the glow, taken from the "
                "image (energy-conserving). With a threshold: how strongly the light above it is added.");
            slider("Radius", bloom.radius, 0.05f, 1.0f, "%.2f",
                "How far the glow spreads. Low values give a tight core around bright sources, as a "
                "good lens does; 1 spreads the light evenly across every blur size (a wide haze).");
            slider("Threshold", bloom.threshold, 0.0f, 16.0f,
                bloom.threshold > 0.0f ? "%.2f" : "off",
                "Only light brighter than this blooms, measured after exposure (1 is roughly diffuse "
                "white), so the same value works with auto-exposure. 0 lets all light bloom.");
            ImGui::BeginDisabled(bloom.threshold <= 0.0f);
            slider("Threshold knee", bloom.knee, 0.0f, 4.0f, "%.2f",
                "Softens the threshold: light within this distance of it blooms partially, so bright "
                "areas fade into the glow instead of switching on abruptly.");
            ImGui::EndDisabled();
            if (ImGui::ColorEdit3("Tint", bloom.tint.data(), ImGuiColorEditFlags_Float))
                outputSettings->changed = true;
            tip("Colours the scattered light (linear). White keeps the source colours.");
            int levels = static_cast<int>(bloom.levels);
            if (ImGui::SliderInt("Levels", &levels, 1, 8, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                bloom.levels = static_cast<uint32_t>(levels);
                outputSettings->changed = true;
            }
            tip("How many blur sizes the glow is built from (each doubles the size). More levels reach "
                "farther. Changing it rebuilds the frame graph.");
            int karis = static_cast<int>(bloom.karis);
            constexpr const char* karisModes[]{ "Auto (off with TAA)", "Off", "On" };
            if (ImGui::Combo("Anti-firefly filter", &karis, karisModes, 3)) {
                bloom.karis = static_cast<Iridium::BloomKarisMode>(karis);
                outputSettings->changed = true;
            }
            tip("Suppresses flickering glow from tiny, very bright points; it also removes some of "
                "their energy. Auto turns it off when TAA already stabilises them.");
            ImGui::EndDisabled();
            if (ImGui::Button("Reset bloom defaults")) {
                const bool enabled = bloom.enabled;
                bloom = Iridium::BloomSettings{};
                bloom.enabled = enabled;
                outputSettings->changed = true;
            }
            if (!outputSettings->bloomDiagnostic.empty())
                ImGui::TextColored(ImVec4(1, .4f, .3f, 1), "%s", outputSettings->bloomDiagnostic.c_str());
        }
        ImGui::EndChild();
    }

    // 3. CLEANUP
    ImGui::End();
}
