#include "assets/cooker/LocalDerivedDataCache.h"
#include "utils/Sha256.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#include <vector>

namespace {

    using namespace Iridium;
    using Clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    std::vector<std::byte> bytes(std::string_view text) {
        return {
            reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size()),
        };
    }

    AssetGuid guid() {
        std::array<uint8_t, 10> random{};
        return AssetGuid::fromUuidV7Fields(1'800'000'000'000ull, random);
    }

    CookedArtifactBlob artifact(std::string cookKey) {
        return serializeCookedArtifact({
            .assetGuid = guid(),
            .artifactType = "iridium.benchmark",
            .target = {
                .platform = "windows-x64",
                .profile = "release",
                .qualityPolicy = "reference",
            },
            .cookKey = std::move(cookKey),
            .sections = {
                { 1, 1, 16, std::vector<std::byte>(4096, std::byte{ 0x5a }) },
            },
        });
    }

    double milliseconds(Clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    }

    double percentile(std::vector<double> samples, double fraction) {
        std::sort(samples.begin(), samples.end());
        return samples[static_cast<size_t>((samples.size() - 1) * fraction)];
    }

    struct TemporaryDirectory {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("iridium-ddc-benchmark-" + createAssetGuidV7().toString());
        TemporaryDirectory() { std::filesystem::create_directories(path); }
        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    };

} // namespace

int main() {
    TemporaryDirectory temporary;
    LocalDerivedDataCache cache(temporary.path);
    const std::string hitKey = sha256(bytes("resident-ddc-hit"));
    const CookedArtifactBlob hitArtifact = artifact(hitKey);
    const auto publish = cache.storeAtomic(hitKey, hitArtifact);
    if (hasCookErrors(publish)) return 2;

    std::vector<double> lookupSamples;
    lookupSamples.reserve(1000);
    for (size_t index = 0; index < 1000; ++index) {
        const auto begin = Clock::now();
        const DdcProbeResult probe = cache.probe(hitKey);
        lookupSamples.push_back(milliseconds(Clock::now() - begin));
        if (probe.status != DdcLookupStatus::Hit) return 3;
    }

    const std::string coalescedKey = sha256(bytes("scheduling-key"));
    const CookedArtifactBlob coalescedArtifact = artifact(coalescedKey);
    std::atomic<bool> releaseBuilder{ false };
    auto first = cache.request(coalescedKey, {},
        [&releaseBuilder, &coalescedArtifact](std::stop_token) {
            while (!releaseBuilder.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return coalescedArtifact;
        });
    std::vector<double> schedulingSamples;
    schedulingSamples.reserve(10'000);
    for (size_t index = 0; index < 10'000; ++index) {
        const auto begin = Clock::now();
        (void)cache.request(coalescedKey, {},
            [](std::stop_token) { return CookedArtifactBlob{}; });
        schedulingSamples.push_back(milliseconds(Clock::now() - begin));
    }
    releaseBuilder.store(true, std::memory_order_release);
    if (first.get().status != DdcRequestStatus::Built) return 4;

    const double lookupP95 = percentile(lookupSamples, 0.95);
    const double schedulingP99 = percentile(schedulingSamples, 0.99);
    const bool passed = lookupP95 <= 1.0 && schedulingP99 <= 0.10;
    std::cout
        << "{\n"
        << "  \"lookupSamples\": " << lookupSamples.size() << ",\n"
        << "  \"lookupHeaderValidationP95Ms\": " << lookupP95 << ",\n"
        << "  \"lookupGateMs\": 1.0,\n"
        << "  \"schedulingSamples\": " << schedulingSamples.size() << ",\n"
        << "  \"coalescedSchedulingP99Ms\": " << schedulingP99 << ",\n"
        << "  \"schedulingGateMs\": 0.10,\n"
        << "  \"passed\": " << (passed ? "true" : "false") << "\n"
        << "}\n";
    return passed ? 0 : 1;
}
