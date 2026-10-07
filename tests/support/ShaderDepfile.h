#pragma once

// Reads the Make-style dependency files glslc writes beside each compiled shader
// (<build>/shader-deps/<name>.spv.d, see cmake/IridiumShaders.cmake). They record
// exactly which source files the compiler consumed, so include ownership is
// checked against what was compiled, not against #include text.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium::Test {

    class ShaderDepfile {
    public:
        // `spvName` is the compiled file name, e.g. "output_frag.spv".
        [[nodiscard]] static ShaderDepfile load(const std::filesystem::path& depfileDirectory,
            std::string_view spvName);
        [[nodiscard]] static ShaderDepfile parse(std::string_view text,
            std::string label = {});

        [[nodiscard]] const std::string& label() const noexcept { return label_; }
        // Every prerequisite, relative to assets/shaders/ (e.g. "output.frag",
        // "include/scene_color.glsl"); the primary source comes first.
        [[nodiscard]] const std::vector<std::string>& sources() const noexcept {
            return sources_;
        }
        [[nodiscard]] const std::string& primarySource() const noexcept;
        [[nodiscard]] bool includes(std::string_view relativePath) const noexcept;

    private:
        std::string label_;
        std::vector<std::string> sources_;
    };

} // namespace Iridium::Test
