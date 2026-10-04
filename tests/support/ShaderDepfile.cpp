#include "ShaderDepfile.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace Iridium::Test {
namespace {

    // Maps an absolute or relative prerequisite to its path below assets/shaders.
    std::string shaderRelative(std::string path) {
        std::ranges::replace(path, '\\', '/');
        constexpr std::string_view Marker = "assets/shaders/";
        const size_t marker = path.rfind(Marker);
        if (marker != std::string::npos) return path.substr(marker + Marker.size());
        return path;
    }

} // namespace

ShaderDepfile ShaderDepfile::load(const std::filesystem::path& depfileDirectory,
    std::string_view spvName) {
    const std::filesystem::path path = depfileDirectory / (std::string(spvName) + ".d");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("missing shader depfile " + path.string());
    std::ostringstream text;
    text << input.rdbuf();
    return parse(text.str(), std::string(spvName));
}

ShaderDepfile ShaderDepfile::parse(std::string_view text, std::string label) {
    ShaderDepfile result;
    result.label_ = std::move(label);
    // Split into whitespace-separated tokens, honouring "\ " escapes and
    // backslash-newline continuations. The first token ending in ':' (the rule
    // target) is skipped.
    std::vector<std::string> tokens;
    std::string current;
    const auto flush = [&] {
        if (!current.empty()) tokens.push_back(std::move(current));
        current.clear();
    };
    for (size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '\\' && index + 1 < text.size()) {
            const char next = text[index + 1];
            if (next == ' ' || next == '#') { current.push_back(next); ++index; continue; }
            if (next == '\n' || next == '\r') {
                flush();
                ++index;
                if (next == '\r' && index + 1 < text.size() && text[index + 1] == '\n') ++index;
                continue;
            }
        }
        if (character == ' ' || character == '\t' || character == '\n' ||
            character == '\r') {
            flush();
            continue;
        }
        current.push_back(character);
    }
    flush();

    bool targetSeen = false;
    for (std::string& token : tokens) {
        if (!targetSeen) {
            // "C:/path/x.spv:" (the target may itself contain a drive colon).
            if (token.size() > 1 && token.back() == ':') { targetSeen = true; continue; }
            if (token == ":") { targetSeen = true; continue; }
            continue;
        }
        result.sources_.push_back(shaderRelative(std::move(token)));
    }
    if (!targetSeen || result.sources_.empty())
        throw std::runtime_error("malformed shader depfile " + result.label_);
    return result;
}

const std::string& ShaderDepfile::primarySource() const noexcept {
    return sources_.front();
}

bool ShaderDepfile::includes(std::string_view relativePath) const noexcept {
    return std::ranges::find(sources_, relativePath) != sources_.end();
}

} // namespace Iridium::Test
