#include "assets/cooker/ImporterRegistry.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace Iridium {

    namespace {

        std::string lowerExtension(const std::filesystem::path& path) {
            std::string extension = path.extension().generic_string();
            std::transform(extension.begin(), extension.end(), extension.begin(),
                [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
            return extension;
        }

        bool supportsExtension(const ImporterDescriptor& descriptor,
            const std::string& extension) {
            return std::any_of(descriptor.extensions.begin(), descriptor.extensions.end(),
                [&extension](std::string candidate) {
                    std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                        [](unsigned char value) {
                            return static_cast<char>(std::tolower(value));
                        });
                    return candidate == extension;
                });
        }

    } // namespace

    void ImporterRegistry::registerImporter(
        std::shared_ptr<const AssetImporter> importer) {
        if (!importer) throw std::invalid_argument("Importer must not be null.");
        const ImporterDescriptor& descriptor = importer->descriptor();
        if (descriptor.id.empty() || descriptor.implementationVersion == 0 ||
            descriptor.currentSettingsSchemaVersion == 0 ||
            descriptor.assetTypes.empty()) {
            throw std::invalid_argument("Importer descriptor is incomplete.");
        }
        const bool duplicate = std::any_of(m_importers.begin(), m_importers.end(),
            [&descriptor](const auto& existing) {
                return existing->descriptor().id == descriptor.id &&
                    existing->descriptor().implementationVersion ==
                        descriptor.implementationVersion;
            });
        if (duplicate) {
            throw std::invalid_argument("Duplicate importer ID and implementation version.");
        }
        m_importers.push_back(std::move(importer));
    }

    std::vector<ImporterDescriptor> ImporterRegistry::descriptors() const {
        std::vector<ImporterDescriptor> result;
        result.reserve(m_importers.size());
        for (const auto& importer : m_importers) {
            result.push_back(importer->descriptor());
        }
        std::sort(result.begin(), result.end(),
            [](const ImporterDescriptor& lhs, const ImporterDescriptor& rhs) {
                if (lhs.id != rhs.id) return lhs.id < rhs.id;
                return lhs.implementationVersion < rhs.implementationVersion;
            });
        return result;
    }

    ImporterSelection ImporterRegistry::selectExplicit(
        std::string_view importerId, uint32_t implementationVersion) const {
        ImporterSelection result;
        const auto found = std::find_if(m_importers.begin(), m_importers.end(),
            [importerId, implementationVersion](const auto& importer) {
                return importer->descriptor().id == importerId &&
                    importer->descriptor().implementationVersion ==
                        implementationVersion;
            });
        if (found == m_importers.end()) {
            result.diagnostics.push_back({
                .code = "IMPORTER_EXPLICIT_UNAVAILABLE",
                .field = "importer",
                .message = "The selected importer ID/version is not registered.",
            });
        } else {
            result.importer = *found;
        }
        return result;
    }

    ImporterSelection ImporterRegistry::selectAutomatic(
        const std::filesystem::path& relativePath,
        std::span<const std::byte> sourceBytes) const {
        ImporterSelection result;
        const std::string extension = lowerExtension(relativePath);
        std::vector<std::shared_ptr<const AssetImporter>> candidates;
        for (const auto& importer : m_importers) {
            if (!supportsExtension(importer->descriptor(), extension)) continue;
            if (importer->probe(relativePath, sourceBytes) ==
                ImportProbeResult::Supported) {
                candidates.push_back(importer);
            }
        }
        std::sort(candidates.begin(), candidates.end(),
            [](const auto& lhs, const auto& rhs) {
                const auto& left = lhs->descriptor();
                const auto& right = rhs->descriptor();
                if (left.id != right.id) return left.id < right.id;
                return left.implementationVersion < right.implementationVersion;
            });
        if (candidates.size() == 1) {
            result.importer = candidates.front();
        } else if (candidates.empty()) {
            result.diagnostics.push_back({
                .code = "IMPORTER_PROBE_NONE",
                .field = relativePath.generic_string(),
                .message = "No registered importer accepted the source.",
            });
        } else {
            std::string names;
            for (const auto& candidate : candidates) {
                if (!names.empty()) names += ", ";
                names += candidate->descriptor().id + "@" +
                    std::to_string(candidate->descriptor().implementationVersion);
            }
            result.diagnostics.push_back({
                .code = "IMPORTER_PROBE_AMBIGUOUS",
                .field = relativePath.generic_string(),
                .message = "Automatic importer selection is ambiguous: " + names,
            });
        }
        return result;
    }

} // namespace Iridium
