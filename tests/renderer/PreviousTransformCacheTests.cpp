// M9 G4: last frame's world transform for packets without a GPU-scene record,
// keyed by stable identity.
#include "extraction/PreviousTransformCache.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

using namespace Iridium;

namespace {
    int failures = 0;
#define CHECK(expression) do { if (!(expression)) { \
    std::cerr << "check failed: " #expression " (line " << __LINE__ << ")\n"; \
    ++failures; } } while (false)

    PreviousTransformCache::Key key(const char* owner, const char* primitive) {
        return { *SceneEntityUuid::parse(owner), *AssetGuid::parse(primitive) };
    }

    glm::mat4 at(float x) { return glm::translate(glm::mat4(1.0f), { x, 0.0f, 0.0f }); }

    void previousIsLastFrame() {
        PreviousTransformCache cache;
        const auto a = key("019fb73d-5a60-7000-8000-0000000000a0",
            "019fb73d-5a60-7000-8000-0000000000a1");
        const auto b = key("019fb73d-5a60-7000-8000-0000000000b0",
            "019fb73d-5a60-7000-8000-0000000000a1");   // same primitive, other owner
        // Frame 1: unseen keys report their current transform.
        cache.beginFrame();
        CHECK(cache.resolve(a, at(1.0f)) == at(1.0f));
        CHECK(cache.resolve(b, at(10.0f)) == at(10.0f));
        cache.endFrame();
        CHECK(cache.trackedCount() == 2);
        // Frame 2: both moved; previous is frame 1's, per identity.
        cache.beginFrame();
        CHECK(cache.resolve(b, at(11.0f)) == at(10.0f));   // order-independent
        CHECK(cache.resolve(a, at(2.0f)) == at(1.0f));
        cache.endFrame();
        // Frame 3: a stopped (settles to its own transform), b disappeared.
        cache.beginFrame();
        CHECK(cache.resolve(a, at(2.0f)) == at(2.0f));
        cache.endFrame();
        CHECK(cache.trackedCount() == 1);
        // Frame 4: b reappears with no history.
        cache.beginFrame();
        CHECK(cache.resolve(b, at(20.0f)) == at(20.0f));
        CHECK(cache.resolve(a, at(3.0f)) == at(2.0f));
        // A duplicate key in one frame keeps the first transform.
        (void)cache.resolve(a, at(99.0f));
        cache.endFrame();
        cache.beginFrame();
        CHECK(cache.resolve(a, at(4.0f)) == at(3.0f));
        cache.endFrame();
        cache.clear();
        CHECK(cache.trackedCount() == 0);
    }

    // M7.10.1: a key culled for some frames (touched with its current
    // transform) re-enters with exactly the previous transform it would have
    // had without culling; resolved entries win over touched ones.
    void touchedKeysStayTracked() {
        const auto a = key("019fb73d-5a60-7000-8000-0000000000a0",
            "019fb73d-5a60-7000-8000-0000000000a1");
        const auto b = key("019fb73d-5a60-7000-8000-0000000000b0",
            "019fb73d-5a60-7000-8000-0000000000a1");
        const auto c = key("019fb73d-5a60-7000-8000-0000000000c0",
            "019fb73d-5a60-7000-8000-0000000000a1");
        PreviousTransformCache culled;
        PreviousTransformCache reference;   // never culls
        const auto frame = [&](float t, bool cullA) {
            culled.beginFrame();
            reference.beginFrame();
            const glm::mat4 bCulled = culled.resolve(b, at(100.0f + t));
            CHECK(bCulled == reference.resolve(b, at(100.0f + t)));
            const glm::mat4 aReference = reference.resolve(a, at(t));
            if (cullA) {
                culled.touch(a, at(t));
            }
            else {
                CHECK(culled.resolve(a, at(t)) == aReference);
            }
            culled.endFrame();
            reference.endFrame();
        };
        frame(1.0f, false);
        frame(2.0f, true);    // moving while culled
        frame(3.0f, true);
        frame(4.0f, false);   // re-enters: previous is frame 3's
        frame(5.0f, true);
        frame(6.0f, false);
        CHECK(culled.trackedCount() == reference.trackedCount());

        // Touched before ever resolved: still the previous transform.
        PreviousTransformCache cache;
        cache.beginFrame();
        cache.touch(c, at(7.0f));
        cache.endFrame();
        CHECK(cache.trackedCount() == 1);
        cache.beginFrame();
        CHECK(cache.resolve(c, at(8.0f)) == at(7.0f));
        // Resolved and touched in one frame: the resolved entry wins.
        cache.touch(c, at(50.0f));
        cache.touch(a, at(9.0f));
        cache.touch(a, at(10.0f));   // duplicate touch keeps one entry
        cache.endFrame();
        CHECK(cache.trackedCount() == 2);
        cache.beginFrame();
        CHECK(cache.resolve(c, at(9.0f)) == at(8.0f));
        const glm::mat4 aPrevious = cache.resolve(a, at(11.0f));
        CHECK(aPrevious == at(9.0f) || aPrevious == at(10.0f));
        cache.endFrame();
        // Untouched and unresolved keys age out as before.
        cache.beginFrame();
        cache.endFrame();
        CHECK(cache.trackedCount() == 0);
    }

    // M7.10.1: an owner rejected as a whole keeps one owner entry; every key
    // of that owner re-enters with the owner's transform from its last culled
    // frame, exactly as a never-culled cache reports, and own entries win.
    void touchedOwnersStandInForTheirKeys() {
        const char* ownerId = "019fb73d-5a60-7000-8000-0000000000a0";
        const auto a = key(ownerId, "019fb73d-5a60-7000-8000-0000000000a1");
        const auto b = key(ownerId, "019fb73d-5a60-7000-8000-0000000000a2");
        const auto other = key("019fb73d-5a60-7000-8000-0000000000b0",
            "019fb73d-5a60-7000-8000-0000000000a1");
        PreviousTransformCache culled;
        PreviousTransformCache reference;   // never culls
        const auto frame = [&](float t, bool cullOwner) {
            culled.beginFrame();
            reference.beginFrame();
            const glm::mat4 aReference = reference.resolve(a, at(t));
            const glm::mat4 bReference = reference.resolve(b, at(t));
            CHECK(culled.resolve(other, at(50.0f + t)) ==
                reference.resolve(other, at(50.0f + t)));
            if (cullOwner) culled.touchOwner(a.owner, at(t));
            else {
                CHECK(culled.resolve(a, at(t)) == aReference);
                CHECK(culled.resolve(b, at(t)) == bReference);
            }
            culled.endFrame();
            reference.endFrame();
        };
        frame(1.0f, false);
        frame(2.0f, true);    // moving while rejected as a whole
        frame(3.0f, true);
        frame(4.0f, false);   // re-enters: previous is frame 3's
        frame(5.0f, true);
        frame(6.0f, false);

        // An own entry wins over the owner entry; owner entries age out.
        PreviousTransformCache cache;
        cache.beginFrame();
        (void)cache.resolve(a, at(1.0f));
        cache.endFrame();
        cache.beginFrame();
        CHECK(cache.resolve(a, at(2.0f)) == at(1.0f));
        cache.touchOwner(a.owner, at(30.0f));
        cache.touchOwner(a.owner, at(31.0f));   // duplicate keeps one entry
        cache.endFrame();
        CHECK(cache.trackedCount() == 2);
        cache.beginFrame();
        CHECK(cache.resolve(a, at(3.0f)) == at(2.0f));
        const glm::mat4 bPrevious = cache.resolve(b, at(3.0f));
        CHECK(bPrevious == at(30.0f) || bPrevious == at(31.0f));
        cache.endFrame();
        cache.beginFrame();
        cache.endFrame();
        CHECK(cache.trackedCount() == 0);
        cache.beginFrame();
        cache.touchOwner(a.owner, at(40.0f));
        cache.endFrame();
        cache.clear();
        CHECK(cache.trackedCount() == 0);
    }

    // M7.10.3: the hashed index against an ordered-map reference over
    // thousands of keys, growth, duplicates, touches in either order and
    // aging out.
    void hashedIndexMatchesReference() {
        const auto makeKey = [](uint32_t owner, uint32_t primitive) {
            char ownerText[37], primitiveText[37];
            std::snprintf(ownerText, sizeof(ownerText),
                "019fb73d-5a60-7000-8000-%012x", owner);
            std::snprintf(primitiveText, sizeof(primitiveText),
                "019fb73d-5a61-7000-8000-%012x", primitive);
            return key(ownerText, primitiveText);
        };
        std::mt19937 random(7);
        std::map<std::pair<uint32_t, uint32_t>, glm::mat4> previous;
        PreviousTransformCache cache;
        for (int frame = 0; frame < 12; ++frame) {
            std::map<std::pair<uint32_t, uint32_t>, std::pair<glm::mat4, bool>> current;
            cache.beginFrame();
            const uint32_t count = 500u + static_cast<uint32_t>(frame) * 500u;
            for (uint32_t index = 0; index < count; ++index) {
                const uint32_t owner = random() % 64u;
                const uint32_t primitive = random() % 128u;
                const auto id = std::make_pair(owner, primitive);
                const glm::mat4 world = at(static_cast<float>(random() % 1000u));
                const auto k = makeKey(owner, primitive);
                if (random() % 4u == 0u) {
                    cache.touch(k, world);
                    current.try_emplace(id, world, true);
                }
                else {
                    const auto found = previous.find(id);
                    const glm::mat4 expected = found != previous.end() ? found->second : world;
                    CHECK(cache.resolve(k, world) == expected);
                    auto [entry, inserted] = current.try_emplace(id, world, false);
                    if (!inserted && entry->second.second) entry->second = { world, false };
                }
            }
            cache.endFrame();
            previous.clear();
            for (const auto& [id, value] : current) previous.emplace(id, value.first);
            CHECK(cache.trackedCount() == previous.size());
        }
        // Touch first, then resolve: the resolved entry wins.
        const auto k = makeKey(900u, 900u);
        cache.beginFrame();
        cache.touch(k, at(1.0f));
        (void)cache.resolve(k, at(2.0f));
        cache.endFrame();
        cache.beginFrame();
        CHECK(cache.resolve(k, at(3.0f)) == at(2.0f));
        cache.endFrame();
    }
}

int main() {
    previousIsLastFrame();
    touchedKeysStayTracked();
    touchedOwnersStandInForTheirKeys();
    hashedIndexMatchesReference();
    if (failures == 0) std::cout << "PreviousTransformCacheTests passed\n";
    return failures == 0 ? 0 : 1;
}
