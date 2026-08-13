#pragma once

#include "assets/AssetCatalog.h"
#include "assets/AssetDiscovery.h"

#include <filesystem>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

namespace Iridium {

    struct AssetContentMutationResult {
        std::filesystem::path path;
        std::filesystem::path previousPath;
        std::string diagnostic;

        [[nodiscard]] bool succeeded() const noexcept {
            return diagnostic.empty();
        }
    };

    // Physical project-content mutations. Every path is resolved beneath a
    // registered asset root; source assets keep their metadata sidecars and GUIDs.
    class AssetContentOperations {
    public:
        explicit AssetContentOperations(
            std::span<const AssetRoot> roots);

        [[nodiscard]] AssetContentMutationResult importAsset(
            std::string_view rootId,
            const std::filesystem::path& destinationDirectory,
            const std::filesystem::path& source,
            std::stop_token stopToken = {}) const;
        [[nodiscard]] AssetContentMutationResult createFolder(
            std::string_view rootId,
            const std::filesystem::path& parentDirectory,
            std::string_view name) const;
        [[nodiscard]] AssetContentMutationResult renameFolder(
            std::string_view rootId,
            const std::filesystem::path& directory,
            std::string_view name) const;
        [[nodiscard]] AssetContentMutationResult deleteFolder(
            std::string_view rootId,
            const std::filesystem::path& directory) const;
        [[nodiscard]] AssetContentMutationResult moveAsset(
            const AssetCatalogRecord& rootRecord,
            const std::filesystem::path& destinationDirectory) const;
        [[nodiscard]] AssetContentMutationResult renameAsset(
            const AssetCatalogRecord& rootRecord,
            std::string_view name) const;
        [[nodiscard]] AssetContentMutationResult deleteAsset(
            const AssetCatalogRecord& rootRecord) const;

    private:
        [[nodiscard]] const AssetRoot* root(
            std::string_view id) const noexcept;

        std::vector<AssetRoot> roots_;
    };

} // namespace Iridium
