#pragma once

// Build and source provenance recorded at configure time (M7R R2.9). The values
// live in one generated translation unit (cmake/BuildInfo.cpp.in), so a new
// commit or toolchain recompiles only that file.

namespace Iridium::BuildInfo {

    // CMAKE_BUILD_TYPE ("Release", "Debug", ...).
    [[nodiscard]] const char* configuration() noexcept;
    // `git rev-parse HEAD` at configure time, or "unknown".
    [[nodiscard]] const char* sourceCommit() noexcept;
    [[nodiscard]] const char* sourceBranch() noexcept;
    // The worktree had uncommitted changes at configure time.
    [[nodiscard]] bool sourceDirtyAtConfigure() noexcept;
    // "<compiler id> <version>".
    [[nodiscard]] const char* compiler() noexcept;
    // glslc version line, or "unavailable".
    [[nodiscard]] const char* shaderCompiler() noexcept;
    // Vulkan SDK version, or "unavailable".
    [[nodiscard]] const char* vulkanSdk() noexcept;

} // namespace Iridium::BuildInfo
