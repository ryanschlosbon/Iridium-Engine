#pragma once

#include "assets/cooker/CookTypes.h"

#include <map>
#include <span>
#include <vector>

namespace Iridium {

    struct DependencyCycle {
        std::vector<AssetGuid> chain;

        auto operator<=>(const DependencyCycle&) const = default;
    };

    class AssetDependencyGraph {
    public:
        void setDependencies(AssetGuid asset,
            std::vector<AssetDependency> dependencies);
        void removeAsset(AssetGuid asset);
        [[nodiscard]] const std::vector<AssetDependency>& directDependencies(
            AssetGuid asset) const noexcept;
        [[nodiscard]] std::vector<AssetGuid> reverseDependents(
            AssetGuid asset) const;
        [[nodiscard]] std::vector<AssetGuid> invalidationClosure(
            std::span<const AssetGuid> changedAssets) const;
        [[nodiscard]] std::vector<DependencyCycle> cycles() const;

    private:
        std::map<AssetGuid, std::vector<AssetDependency>> m_direct;
        std::map<AssetGuid, std::vector<AssetGuid>> m_reverse;
    };

} // namespace Iridium
