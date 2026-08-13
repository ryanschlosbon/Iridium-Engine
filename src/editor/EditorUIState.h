#pragma once

#include "renderer/color/OutputTransformConfig.h"
#include "renderer/rhi/ShadowTypes.h"
#include "renderer/rhi/ReflectionProbeSettings.h"
#include "assets/AssetBrowserModel.h"

#include <optional>

struct EditorOutputSettings {
    Iridium::Color::OutputTransport transport =
        Iridium::Color::OutputTransport::SdrSrgb;
    float manualExposureEv = 0.0f;
    float paperWhiteNits = 203.0f;
    float peakNits = 1000.0f;
    bool changed = false;
};

struct EditorUIState {
    // We can add as many booleans here as we want in the future!
    bool showProjectSettings = false;
    bool showProfiler = false;
    bool showPhysicsDebugger = false;
    bool showMaterialDiagnostics = false;
    bool showAssetBrowser = true;
    bool showConsole = true;
    std::optional<Iridium::AssetDragPayload>
        selectedAsset;
    EditorOutputSettings outputSettings;
    Iridium::ProjectShadowSettings shadowSettings;
    bool shadowSettingsChanged = false;
    Iridium::ProjectReflectionProbeSettings reflectionProbeSettings;
    bool reflectionProbeSettingsChanged = false;

    // We will use this one to test the architecture right now
    bool showDemoWindow = false;
};
