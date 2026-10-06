#pragma once

#include "renderer/color/OutputTransformConfig.h"
#include "renderer/rhi/RenderBackendConfig.h"
#include "renderer/rhi/ShadowTypes.h"
#include "renderer/rhi/ReflectionProbeSettings.h"
#include "assets/AssetBrowserModel.h"

#include <array>
#include <optional>
#include <string>

struct EditorOutputSettings {
    Iridium::Color::OutputTransport transport =
        Iridium::Color::OutputTransport::SdrSrgb;
    Iridium::Color::OutputTransport effectiveTransport =
        Iridium::Color::OutputTransport::SdrSrgb;
    std::array<bool, 3> supportedTransports{ true, false, false };
    std::string transportDiagnostic;
    float manualExposureEv = 0.0f;
    float paperWhiteNits = 203.0f;
    float peakNits = 1000.0f;
    // M9.2c: the active mode, and why the last switch failed (if it did).
    Iridium::AntiAliasingMode antiAliasing = Iridium::AntiAliasingMode::None;
    std::string antiAliasingDiagnostic;
    // M9.4: the active bloom settings, and why the last switch failed.
    Iridium::BloomSettings bloom{};
    std::string bloomDiagnostic;
    // M9.8: TAA tuning (applies live).
    Iridium::TemporalAntiAliasingTuning taaTuning{};
    // M9.5: the active exposure mode and auto-exposure settings.
    Iridium::ExposureMode exposureMode = Iridium::ExposureMode::Auto;
    Iridium::AutoExposureSettings autoExposure{};
    std::string exposureDiagnostic;
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
    // Session-only renderer quality override; zero preserves authored policy.
    int layeredInterfaceOverride = 0;

    // We will use this one to test the architecture right now
    bool showDemoWindow = false;
};
