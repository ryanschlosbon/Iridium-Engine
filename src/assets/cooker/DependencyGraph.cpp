#include "assets/cooker/DependencyGraph.h"

#include <algorithm>
#include <functional>
#include <map>
#include <queue>
#include <set>

namespace Iridium {

    namespace {

        const std::vector<AssetDependency> kNoDependencies;

        std::vector<AssetGuid> assetEdges(
            const std::vector<AssetDependency>& dependencies) {
            std::vector<AssetGuid> result;
            for (const AssetDependency& dependency : dependencies) {
                if (dependency.assetGuid &&
                    (dependency.type == AssetDependencyType::Asset ||
                     dependency.type == AssetDependencyType::OptionalAsset)) {
                    result.push_back(*dependency.assetGuid);
                }
            }
            std::sort(result.begin(), result.end());
            result.erase(std::unique(result.begin(), result.end()), result.end());
            return result;
        }

        DependencyCycle canonicalCycle(std::vector<AssetGuid> chain) {
            if (chain.size() > 1 && chain.front() == chain.back()) chain.pop_back();
            if (chain.empty()) return {};
            const auto smallest = std::min_element(chain.begin(), chain.end());
            std::rotate(chain.begin(), smallest, chain.end());
            chain.push_back(chain.front());
            return { std::move(chain) };
        }

    } // namespace

    void AssetDependencyGraph::setDependencies(
        AssetGuid asset, std::vector<AssetDependency> dependencies) {
        const auto existing = m_direct.find(asset);
        if (existing != m_direct.end()) {
            for (const AssetGuid dependency :
                assetEdges(existing->second)) {
                const auto reverse =
                    m_reverse.find(dependency);
                if (reverse == m_reverse.end()) {
                    continue;
                }
                std::erase(reverse->second, asset);
                if (reverse->second.empty()) {
                    m_reverse.erase(reverse);
                }
            }
        }

        std::sort(dependencies.begin(), dependencies.end());
        dependencies.erase(std::unique(dependencies.begin(), dependencies.end()),
            dependencies.end());
        m_direct[asset] = std::move(dependencies);

        for (const AssetGuid dependency :
            assetEdges(m_direct.at(asset))) {
            std::vector<AssetGuid>& reverse =
                m_reverse[dependency];
            const auto insertion =
                std::lower_bound(
                    reverse.begin(), reverse.end(),
                    asset);
            if (insertion == reverse.end() ||
                *insertion != asset) {
                reverse.insert(insertion, asset);
            }
        }
    }

    void AssetDependencyGraph::removeAsset(
        AssetGuid asset) {
        const auto existing = m_direct.find(asset);
        if (existing != m_direct.end()) {
            for (const AssetGuid dependency :
                assetEdges(existing->second)) {
                const auto reverse =
                    m_reverse.find(dependency);
                if (reverse == m_reverse.end()) {
                    continue;
                }
                std::erase(reverse->second, asset);
                if (reverse->second.empty()) {
                    m_reverse.erase(reverse);
                }
            }
        }
        m_direct.erase(asset);
    }

    const std::vector<AssetDependency>& AssetDependencyGraph::directDependencies(
        AssetGuid asset) const noexcept {
        const auto found = m_direct.find(asset);
        return found == m_direct.end() ? kNoDependencies : found->second;
    }

    std::vector<AssetGuid> AssetDependencyGraph::reverseDependents(
        AssetGuid asset) const {
        const auto found = m_reverse.find(asset);
        return found == m_reverse.end() ? std::vector<AssetGuid>{} : found->second;
    }

    std::vector<AssetGuid> AssetDependencyGraph::invalidationClosure(
        std::span<const AssetGuid> changedAssets) const {
        std::set<AssetGuid> visited(changedAssets.begin(), changedAssets.end());
        std::queue<AssetGuid> pending;
        for (const AssetGuid guid : visited) pending.push(guid);
        while (!pending.empty()) {
            const AssetGuid current = pending.front();
            pending.pop();
            const auto found = m_reverse.find(current);
            if (found == m_reverse.end()) continue;
            for (const AssetGuid dependent : found->second) {
                if (visited.insert(dependent).second) pending.push(dependent);
            }
        }
        return { visited.begin(), visited.end() };
    }

    std::vector<DependencyCycle> AssetDependencyGraph::cycles() const {
        enum class Visit : uint8_t { Unvisited, Active, Complete };
        std::map<AssetGuid, Visit> state;
        for (const auto& [asset, dependencies] : m_direct) {
            state.try_emplace(asset, Visit::Unvisited);
            for (const AssetGuid dependency : assetEdges(dependencies)) {
                state.try_emplace(dependency, Visit::Unvisited);
            }
        }

        std::vector<AssetGuid> stack;
        std::set<std::vector<AssetGuid>> uniqueCycles;
        std::function<void(AssetGuid)> visit = [&](AssetGuid asset) {
            state[asset] = Visit::Active;
            stack.push_back(asset);
            const auto found = m_direct.find(asset);
            const std::vector<AssetGuid> edges = found == m_direct.end()
                ? std::vector<AssetGuid>{} : assetEdges(found->second);
            for (const AssetGuid dependency : edges) {
                if (state[dependency] == Visit::Unvisited) {
                    visit(dependency);
                } else if (state[dependency] == Visit::Active) {
                    const auto start = std::find(stack.begin(), stack.end(), dependency);
                    std::vector<AssetGuid> chain(start, stack.end());
                    DependencyCycle cycle = canonicalCycle(std::move(chain));
                    uniqueCycles.insert(std::move(cycle.chain));
                }
            }
            stack.pop_back();
            state[asset] = Visit::Complete;
        };

        for (const auto& [asset, visitState] : state) {
            if (visitState == Visit::Unvisited) visit(asset);
        }
        std::vector<DependencyCycle> result;
        for (const auto& cycle : uniqueCycles) result.push_back({ cycle });
        return result;
    }

} // namespace Iridium
