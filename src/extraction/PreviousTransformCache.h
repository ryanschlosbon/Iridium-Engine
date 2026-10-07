#pragma once

#include <cstdint>
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
    // then endFrame(). A key resolved twice in one frame keeps the first
    // transform. The storage grows only when the packet count grows; steady
    // frames allocate nothing.
    //
    // M7.10.1: touch() records the current transform of a packet that was
    // culled this frame (it needs no previous transform), so the key stays
    // tracked and a packet that re-enters view next frame resolves exactly
    // the previous transform it would have resolved had it never been
    // culled. A key both resolved and touched in one frame keeps the resolved
    // entry, in either order.
    //
    // touchOwner() does the same for a whole owner whose transparent work was
    // rejected as one model: every packet of an owner carries the owner's
    // world transform, so one owner entry stands in for all of its keys. A
    // key with no entry of its own falls back to its owner's entry. This keeps
    // culled work O(owners) rather than O(packets) per frame.
    //
    // M7.10.3: each frame's entries are dense and indexed by an
    // open-addressing table (linear probing) instead of being sorted, so a
    // frame costs O(packets) rather than O(packets log packets).
    class PreviousTransformCache {
    public:
        struct Key {
            SceneEntityUuid owner;
            AssetGuid primitiveGuid;
        };

        void beginFrame() noexcept;
        [[nodiscard]] glm::mat4 resolve(const Key& key, const glm::mat4& current);
        void touch(const Key& key, const glm::mat4& current);
        void touchOwner(SceneEntityUuid owner, const glm::mat4& current);
        void endFrame();
        void clear() noexcept;

        [[nodiscard]] size_t trackedCount() const noexcept {
            return previous_.entries.size() + previousOwners_.size();
        }

    private:
        struct Entry {
            Key key;
            glm::mat4 world{ 1.0f };
            bool touched = false;
        };
        // One frame's entries plus their open-addressed index (a slot holds
        // entry index + 1; 0 is empty). The table is a power of two, at most
        // half full.
        struct Frame {
            std::vector<Entry> entries;
            std::vector<uint32_t> table;

            void reset(size_t expected);
            [[nodiscard]] const Entry* find(const Key& key) const noexcept;
            // The slot for key: its entry if present, else the empty slot where
            // it belongs (the caller fills it).
            [[nodiscard]] uint32_t& slot(const Key& key);
        };

        struct OwnerEntry {
            SceneEntityUuid owner;
            glm::mat4 world{ 1.0f };
        };

        Frame previous_;
        Frame current_;
        std::vector<OwnerEntry> previousOwners_;   // sorted by owner
        std::vector<OwnerEntry> touchedOwners_;    // this frame's rejected owners
    };

} // namespace Iridium
