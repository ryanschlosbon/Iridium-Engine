#include "assets/cooker/TextFixtureImporter.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <string>

namespace Iridium {

    namespace {

        constexpr std::string_view kMagic = "IRIDIUM_TEXT\n";

        std::string asString(std::span<const std::byte> bytes) {
            return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }

        void addSettingsError(NormalizedImportSettings& result,
            std::string code, std::string field, std::string message) {
            result.diagnostics.push_back({
                .code = std::move(code),
                .field = std::move(field),
                .message = std::move(message),
            });
        }

    } // namespace

    const ImporterDescriptor& TextFixtureImporter::descriptor() const noexcept {
        static const ImporterDescriptor descriptor{
            .id = "iridium.fixture-text",
            .implementationVersion = 1,
            .currentSettingsSchemaVersion = 2,
            .assetTypes = { "iridium.fixture-text" },
            .extensions = { ".irtest" },
        };
        return descriptor;
    }

    ImportProbeResult TextFixtureImporter::probe(
        const std::filesystem::path& relativePath,
        std::span<const std::byte> sourceBytes) const {
        (void)relativePath;
        return asString(sourceBytes).starts_with(kMagic)
            ? ImportProbeResult::Supported : ImportProbeResult::Unsupported;
    }

    NormalizedImportSettings TextFixtureImporter::normalizeSettings(
        uint32_t sourceSchemaVersion, const nlohmann::json& settings,
        bool strict) const {
        NormalizedImportSettings result;
        result.schemaVersion = descriptor().currentSettingsSchemaVersion;
        if (!settings.is_object()) {
            addSettingsError(result, "FIXTURE_SETTINGS_OBJECT", "settings",
                "Fixture settings must be an object.");
            return result;
        }

        if (sourceSchemaVersion == 1) {
            const auto uppercase = settings.find("uppercase");
            if (uppercase == settings.end() || !uppercase->is_boolean()) {
                addSettingsError(result, "FIXTURE_SETTINGS_V1", "uppercase",
                    "Schema 1 requires a boolean uppercase setting.");
                return result;
            }
            result.values = {
                { "repeat", 1u },
                { "transform", uppercase->get<bool>() ? "uppercase" : "identity" },
            };
        } else if (sourceSchemaVersion == 2) {
            result.values = settings;
        } else {
            addSettingsError(result, "FIXTURE_SETTINGS_SCHEMA", "settings.schemaVersion",
                "Fixture importer cannot migrate this settings schema version.");
            return result;
        }

        const std::set<std::string> known{ "repeat", "transform" };
        for (const auto& [key, ignored] : result.values.items()) {
            (void)ignored;
            if (!known.contains(key)) {
                result.diagnostics.push_back({
                    .severity = strict
                        ? CookDiagnosticSeverity::Error
                        : CookDiagnosticSeverity::Warning,
                    .code = "FIXTURE_SETTINGS_UNKNOWN",
                    .field = key,
                    .message = strict
                        ? "Strict cooking rejects unknown fixture settings."
                        : "Unknown fixture setting is preserved for editor recovery.",
                });
            }
        }
        const auto transform = result.values.find("transform");
        if (transform == result.values.end() || !transform->is_string() ||
            (transform->get<std::string>() != "identity" &&
             transform->get<std::string>() != "uppercase")) {
            addSettingsError(result, "FIXTURE_SETTINGS_TRANSFORM", "transform",
                "Transform must be identity or uppercase.");
        }
        const auto repeat = result.values.find("repeat");
        if (repeat == result.values.end() || !repeat->is_number_unsigned() ||
            repeat->get<uint64_t>() < 1 || repeat->get<uint64_t>() > 16) {
            addSettingsError(result, "FIXTURE_SETTINGS_REPEAT", "repeat",
                "Repeat must be an unsigned integer from 1 through 16.");
        }
        if (result.valid()) {
            CanonicalSettingsResult canonical = canonicalizeSettings(result.values);
            result.canonicalBytes = std::move(canonical.bytes);
            result.diagnostics.insert(result.diagnostics.end(),
                canonical.diagnostics.begin(), canonical.diagnostics.end());
        }
        return result;
    }

    ParsedSourceAsset TextFixtureImporter::parse(
        const ImportSource& input,
        const NormalizedImportSettings& settings) const {
        ParsedSourceAsset result;
        if (input.stopToken.stop_requested()) {
            result.diagnostics.push_back({
                .code = "FIXTURE_IMPORT_CANCELLED",
                .message = "Asset import cancelled.",
            });
            return result;
        }
        if (!settings.valid()) {
            result.diagnostics.push_back({
                .code = "FIXTURE_SETTINGS_NOT_NORMALIZED",
                .message = "Fixture source cannot parse with invalid settings.",
            });
            return result;
        }
        const std::string source = asString(input.bytes);
        if (!source.starts_with(kMagic)) {
            result.diagnostics.push_back({
                .code = "FIXTURE_SOURCE_MAGIC",
                .message = "Fixture source magic is missing.",
            });
            return result;
        }

        size_t cursor = kMagic.size();
        while (cursor < source.size()) {
            if (input.stopToken.stop_requested()) {
                result.diagnostics.push_back({
                    .code = "FIXTURE_IMPORT_CANCELLED",
                    .message = "Asset import cancelled.",
                });
                return result;
            }
            const size_t lineEnd = source.find('\n', cursor);
            const size_t end = lineEnd == std::string::npos ? source.size() : lineEnd;
            const std::string_view line(source.data() + cursor, end - cursor);
            cursor = lineEnd == std::string::npos ? source.size() : lineEnd + 1;
            if (line.empty()) break;
            constexpr std::string_view sourcePrefix = "dep-source:";
            constexpr std::string_view optionalPrefix = "dep-optional:";
            if (line.starts_with(sourcePrefix)) {
                const std::string location(line.substr(sourcePrefix.size()));
                if (location.empty() ||
                    std::filesystem::path(location).is_absolute() ||
                    location.find("..") != std::string::npos) {
                    result.diagnostics.push_back({
                        .code = "FIXTURE_DEPENDENCY_PATH",
                        .field = location,
                        .message = "Source dependency must be normalized and relative.",
                    });
                } else {
                    result.dependencies.push_back({
                        .type = AssetDependencyType::SourceFile,
                        .location = location,
                    });
                }
            } else if (line.starts_with(optionalPrefix)) {
                const auto guid = AssetGuid::parse(line.substr(optionalPrefix.size()));
                if (!guid) {
                    result.diagnostics.push_back({
                        .code = "FIXTURE_DEPENDENCY_GUID",
                        .field = std::string(line),
                        .message = "Optional dependency GUID is invalid.",
                    });
                } else {
                    result.dependencies.push_back({
                        .type = AssetDependencyType::OptionalAsset,
                        .assetGuid = *guid,
                    });
                }
            } else {
                result.diagnostics.push_back({
                    .code = "FIXTURE_DIRECTIVE_UNKNOWN",
                    .field = std::string(line),
                    .message = "Fixture source contains an unknown directive.",
                });
            }
        }
        const std::string_view payload(source.data() + cursor, source.size() - cursor);
        result.documentBytes.assign(
            reinterpret_cast<const std::byte*>(payload.data()),
            reinterpret_cast<const std::byte*>(payload.data() + payload.size()));
        std::sort(result.dependencies.begin(), result.dependencies.end());
        return result;
    }

    CookProduct TextFixtureImporter::cook(
        const ParsedSourceAsset& source,
        const NormalizedImportSettings& settings,
        const CookTarget& target,
        const AssetCookContext& context,
        std::stop_token stopToken) const {
        (void)target;
        (void)context;
        CookProduct result{
            .artifactType = "iridium.fixture-text",
            .artifactSchemaVersion = 1,
        };
        if (stopToken.stop_requested()) {
            result.diagnostics.push_back({
                .code = "FIXTURE_COOK_CANCELLED",
                .message = "Fixture cook was cancelled.",
            });
            return result;
        }
        if (hasCookErrors(source.diagnostics) || !settings.valid()) {
            result.diagnostics.push_back({
                .code = "FIXTURE_COOK_INPUT",
                .message = "Fixture cook input is invalid.",
            });
            return result;
        }
        std::string payload = asString(source.documentBytes);
        if (settings.values.at("transform") == "uppercase") {
            std::transform(payload.begin(), payload.end(), payload.begin(),
                [](unsigned char value) { return static_cast<char>(std::toupper(value)); });
        }
        const uint32_t repeat = settings.values.at("repeat").get<uint32_t>();
        std::string cooked;
        cooked.reserve(payload.size() * repeat);
        for (uint32_t index = 0; index < repeat; ++index) {
            if (stopToken.stop_requested()) {
                result.diagnostics.push_back({
                    .code =
                        "FIXTURE_COOK_CANCELLED",
                    .message =
                        "Fixture cook was cancelled.",
                });
                return result;
            }
            cooked += payload;
        }
        result.sections.push_back({
            .id = 1,
            .schemaVersion = 1,
            .alignment = 16,
            .bytes = std::vector<std::byte>(
                reinterpret_cast<const std::byte*>(cooked.data()),
                reinterpret_cast<const std::byte*>(cooked.data() + cooked.size())),
        });
        return result;
    }

} // namespace Iridium
