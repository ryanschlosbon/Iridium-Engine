#pragma once

#include "core/cli/CliOptionRegistry.h"

#include <string_view>

namespace Iridium {

    struct ApplicationConfig;

    // Per-module registration of the IridiumEngine command line. Each function
    // registers its owner's flags (and post-parse checks) against one
    // ApplicationConfig; the callbacks write into `config`, which must outlive
    // every registry.parse() call.
    namespace AppCli {

        inline constexpr std::string_view kRuntimeOwner = "runtime";
        inline constexpr std::string_view kEditorOwner = "editor";
        inline constexpr std::string_view kRendererOwner = "renderer";
        inline constexpr std::string_view kQualificationOwner = "qualification";

        // Post-parse checks span owners, and only the first failing one is
        // reported, so their relative order is part of the CLI contract. These
        // values freeze the order of the single pre-registry parser (6b000ad).
        enum ValidatorOrder : int {
            kValidateCapturePairing = 10,       // qualification
            kValidateCaptureSignal = 20,        // qualification
            kValidateHdrOperator = 30,          // renderer
            kValidateNitsOrdering = 40,         // renderer
            kValidateFinalSdrCapture = 50,      // qualification
            kValidateWireframeDebugView = 60,   // editor
            kValidateLightGenerators = 70,      // qualification
        };

        void registerRuntimeOptions(Cli::CliOptionRegistry& registry,
            ApplicationConfig& config);
        void registerEditorOptions(Cli::CliOptionRegistry& registry,
            ApplicationConfig& config);
        void registerRendererOptions(Cli::CliOptionRegistry& registry,
            ApplicationConfig& config);

        // Every module above, in usage order: runtime, editor, renderer. The
        // qualification library registers its own flags after these
        // (registerQualificationOptions, qualification/QualificationOptions.h);
        // main.cpp adds them only in IRIDIUM_QUALIFICATION=ON builds.
        void registerApplicationOptions(Cli::CliOptionRegistry& registry,
            ApplicationConfig& config);

    } // namespace AppCli

} // namespace Iridium
