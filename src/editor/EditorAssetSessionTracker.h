#pragma once

#include "editor/EditorAssetDocumentService.h"
#include <map>
#include <vector>

namespace Iridium {
    // GUID identifies the asset; sessionSerial identifies this opening of it.
    // A close/reopen may happen between UI frames, without an observed empty list.
    class EditorAssetSessionTracker {
    public:
        [[nodiscard]] bool matches(const EditorAssetDocument& document) const {
            const auto found = sessions_.find(document.assetGuid);
            return found != sessions_.end() && found->second == document.sessionSerial;
        }
        [[nodiscard]] std::vector<AssetGuid> synchronize(const EditorAssetDocumentService& documents) {
            std::vector<AssetGuid> retired;
            for (auto it = sessions_.begin(); it != sessions_.end();) {
                const auto* document = documents.find(it->first);
                if (!document || document->sessionSerial != it->second) {
                    retired.push_back(it->first);
                    it = sessions_.erase(it);
                } else ++it;
            }
            for (const auto& document : documents.documents())
                sessions_.try_emplace(document.assetGuid, document.sessionSerial);
            return retired;
        }
    private:
        std::map<AssetGuid, uint64_t> sessions_;
    };
}
