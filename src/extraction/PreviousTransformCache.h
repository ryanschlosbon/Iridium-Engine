#pragma once

#include <span>
#include <vector>

#include <glm/glm.hpp>

#include "core/types/AssetGuid.h"
#include "core/types/SceneEntityUuid.h"

namespace Iridium {

    // M9 G4: last frame's world transform for draw packets that have no
    // GPU-scene record (non-GPU-scene owners and their fallbacks), keyed by
    // stable identity (owner entity UUID + cooked primitive GUID), never by
    // ECS index, pointer or packet position. GPU-scene-backed packets read
    // their previous transform from the published tables instead.
    //
    // Each frame: beginFrame(), then resolve() once per packet (returns the
    // previous transform, or the current one for a key not seen last frame),
    // then endFrame(). The storage grows only when the packet count grows;
    // steady frames allocate nothing.
    class PreviousTransformCache {
    public:
        struct Key {
            SceneEntityUuid owner;
            AssetGuid primitiveGuid;
        };

        void beginFrame() noexcept;
        [[nodiscard]] glm::mat4 resolve(const Key& key, const glm::mat4& current);
        void endFrame();
        void clear() noexcept;

        [[nodiscard]] size_t trackedCount() const noexcept { return previous_.size(); }

    private:
        struct Entry {
            Key key;
            glm::mat4 world{ 1.0f };
        };

        std::vector<Entry> previous_;   // sorted by key
        std::vector<Entry> current_;    // this frame, sorted at endFrame
    };

} // namespace Iridium
