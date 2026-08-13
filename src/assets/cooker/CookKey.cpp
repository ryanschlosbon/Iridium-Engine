#include "assets/cooker/CookKey.h"

#include "utils/Sha256.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Iridium {

    namespace {

        template <typename Integer>
        void appendInteger(std::vector<std::byte>& bytes, Integer value) {
            for (size_t index = 0; index < sizeof(Integer); ++index) {
                bytes.push_back(static_cast<std::byte>(
                    static_cast<uint64_t>(value) >> (index * 8)));
            }
        }

        void appendBytes(std::vector<std::byte>& bytes,
            std::span<const std::byte> value) {
            appendInteger<uint64_t>(bytes, value.size());
            bytes.insert(bytes.end(), value.begin(), value.end());
        }

        void appendString(std::vector<std::byte>& bytes, std::string_view value) {
            appendBytes(bytes, std::as_bytes(std::span(value.data(), value.size())));
        }

        void appendGuid(std::vector<std::byte>& bytes, const AssetGuid& guid) {
            appendBytes(bytes, std::as_bytes(std::span(guid.bytes())));
        }

    } // namespace

    std::string calculateCookKey(const CookKeyInput& input) {
        std::vector<std::byte> bytes;
        appendString(bytes, "IridiumCookKey/v1");
        appendInteger<uint32_t>(bytes, input.target.artifactContainerVersion);
        appendGuid(bytes, input.assetGuid);
        appendString(bytes, input.target.platform);
        appendString(bytes, input.target.profile);
        appendString(bytes, input.target.qualityPolicy);
        appendString(bytes, input.importerId);
        appendInteger<uint32_t>(bytes, input.importerImplementationVersion);
        appendInteger<uint32_t>(bytes, input.settingsSchemaVersion);
        appendBytes(bytes, input.canonicalSettings);
        appendString(bytes, input.sourceContentHash);
        appendInteger<uint32_t>(bytes, input.target.materialSchemaVersion);
        appendString(bytes, input.cookerFeatureVersion);

        std::vector<AssetDependency> dependencies(
            input.dependencies.begin(), input.dependencies.end());
        std::sort(dependencies.begin(), dependencies.end());
        appendInteger<uint64_t>(bytes, dependencies.size());
        for (const AssetDependency& dependency : dependencies) {
            appendInteger<uint8_t>(bytes, static_cast<uint8_t>(dependency.type));
            appendInteger<uint8_t>(bytes, dependency.assetGuid ? 1 : 0);
            if (dependency.assetGuid) appendGuid(bytes, *dependency.assetGuid);
            appendString(bytes, dependency.location);
            appendString(bytes, dependency.contentHash);
            appendString(bytes, dependency.artifactHash);
        }
        return sha256(bytes);
    }

} // namespace Iridium
