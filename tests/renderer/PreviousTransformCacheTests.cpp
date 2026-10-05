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
}

int main() {
    previousIsLastFrame();
    if (failures == 0) std::cout << "PreviousTransformCacheTests passed\n";
    return failures == 0 ? 0 : 1;
}
