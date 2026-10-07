// M9 G4: last frame's world transform for packets without a GPU-scene record,
// keyed by stable identity.
#include "extraction/PreviousTransformCache.h"

#include <iostream>

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
}

int main() {
    previousIsLastFrame();
    touchedKeysStayTracked();
    if (failures == 0) std::cout << "PreviousTransformCacheTests passed\n";
    return failures == 0 ? 0 : 1;
}
