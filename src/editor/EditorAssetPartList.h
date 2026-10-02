#pragma once

#include "renderer/rhi/Mesh.h"
#include <algorithm>
#include <map>
#include <vector>

namespace Iridium {
    struct EditorAssetPartRow {
        AssetGuid guid;
        bool material = false;
        bool transparent = false;
        uint32_t sourceMesh = 0;
        uint32_t sourcePrimitive = 0;
        uint32_t runtimePieces = 0;
        uint64_t triangles = 0;
        glm::vec3 minimum{}, maximum{};
        std::vector<AssetGuid> materials;
    };

    // A source primitive may have multiple disconnected runtime pieces. Group
    // only its UI entry, never its geometry, and report all of those pieces.
    inline std::vector<EditorAssetPartRow> makeEditorAssetPartRows(const ModelAsset& model) {
        std::vector<EditorAssetPartRow> rows;
        std::map<std::pair<bool, AssetGuid>, size_t> indices;
        for (const auto& part : model.subMeshes) {
            for (const bool material : {true, false}) {
                const auto guid = material ? part.materialGuid : part.sourcePrimitiveGuid;
                const auto [entry, inserted] = indices.emplace(std::pair{material, guid}, rows.size());
                if (inserted) rows.push_back({
                    .guid = guid, .material = material,
                    .sourceMesh = part.sourceMesh, .sourcePrimitive = part.sourcePrimitive,
                    .minimum = part.boundsMin, .maximum = part.boundsMax});
                auto& row = rows[entry->second];
                if (!material && !part.materialGuid.isNil() &&
                    std::find(row.materials.begin(), row.materials.end(), part.materialGuid) == row.materials.end())
                    row.materials.push_back(part.materialGuid);
                const bool transparent = part.transparency.resolvedClass != TransparencyClass::None &&
                    part.transparency.resolvedClass != TransparencyClass::AlphaClip;
                row.transparent |= transparent;
                ++row.runtimePieces;
                row.triangles += part.indexCount / 3;
                row.minimum = glm::min(row.minimum, part.boundsMin);
                row.maximum = glm::max(row.maximum, part.boundsMax);
            }
        }
        return rows;
    }
}
