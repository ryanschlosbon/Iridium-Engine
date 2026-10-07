#include "assets/runtime/SourceFileWatcher.h"

#include "core/tasks/TaskSystem.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

    using namespace Iridium;

    #define CHECK(condition) \
        do { \
            if (!(condition)) { \
                std::cerr << "  check failed: " #condition \
                    " (line " << __LINE__ << ")\n"; \
                return false; \
            } \
        } while (false)

    AssetGuid guid(std::string_view value) {
        const auto parsed = AssetGuid::parse(value);
        if (!parsed) {
            throw std::runtime_error(
                "Invalid test GUID.");
        }
        return *parsed;
    }

    class TemporaryDirectory {
    public:
        TemporaryDirectory()
            : path(std::filesystem::temp_directory_path() /
                ("iridium-source-watch-" +
                    createAssetGuidV7().toString())) {
            std::filesystem::create_directories(path);
        }
        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
        std::filesystem::path path;
    };

    void write(
        const std::filesystem::path& path,
        std::string_view text) {
        std::ofstream output(
            path, std::ios::binary |
                std::ios::trunc);
        output << text;
        output.flush();
        if (!output) {
            throw std::runtime_error(
                "Could not write watcher fixture.");
        }
    }

    bool detectsChangesForAllOwners() {
        TemporaryDirectory temporary;
        const auto path =
            temporary.path / "texture.bin";
        write(path, "a");
        const AssetGuid first = guid(
            "019f9bce-85b8-7500-8203-040506070809");
        const AssetGuid second = guid(
            "019f9bce-85b8-7501-8203-040506070809");
        SourceFileWatcher watcher(
            std::chrono::seconds(1), false);
        CHECK(watcher.watch(first, path));
        CHECK(watcher.watch(second, path));
        CHECK(!watcher.watch(second, path));
        watcher.scanNow();
        CHECK(watcher.drainEvents().empty());

        write(path, "changed-size");
        watcher.scanNow();
        const auto events = watcher.drainEvents();
        CHECK(events.size() == 2);
        CHECK(events[0].assetGuid == first);
        CHECK(events[1].assetGuid == second);
        CHECK(events[0].sourcePath ==
            std::filesystem::absolute(path)
                .lexically_normal());
        CHECK(events[0].eventNanoseconds != 0);
        const SourceFileWatcherStats stats =
            watcher.stats();
        CHECK(stats.watchedFiles == 1);
        CHECK(stats.changes == 2);
        CHECK(stats.pendingEvents == 0);
        return true;
    }

    bool detectsDeleteAndRecreate() {
        TemporaryDirectory temporary;
        const auto path =
            temporary.path / "model.gltf";
        write(path, "source");
        const AssetGuid asset = guid(
            "019f9bce-85b8-7510-8203-040506070809");
        SourceFileWatcher watcher(
            std::chrono::seconds(1), false);
        watcher.watch(asset, path);
        CHECK(std::filesystem::remove(path));
        watcher.scanNow();
        CHECK(watcher.drainEvents().size() == 1);
        write(path, "replacement");
        watcher.scanNow();
        CHECK(watcher.drainEvents().size() == 1);
        CHECK(watcher.stats().missingTransitions == 2);
        return true;
    }

    bool unwatchAndValidation() {
        TemporaryDirectory temporary;
        const auto path =
            temporary.path / "asset.bin";
        write(path, "a");
        const AssetGuid asset = guid(
            "019f9bce-85b8-7520-8203-040506070809");
        SourceFileWatcher watcher(
            std::chrono::seconds(1), false);
        watcher.watch(asset, path);
        watcher.unwatchAsset(asset);
        write(path, "larger");
        watcher.scanNow();
        CHECK(watcher.drainEvents().empty());
        CHECK(watcher.stats().watchedFiles == 0);

        bool rejected = false;
        try {
            watcher.watch(AssetGuid{}, {});
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
        return true;
    }

    // M7R R5b.2: automatic scanning is a Periodic on the task system's
    // pinned I/O thread, started by the frame tick; shutdown stops it.
    bool scansPeriodicallyOnTheIoThread() {
        TemporaryDirectory temporary;
        const auto path =
            temporary.path / "periodic.bin";
        write(path, "a");
        const AssetGuid asset = guid(
            "019f9bce-85b8-7530-8203-040506070809");
        Tasks::TaskSystem tasks(Tasks::TaskSystemConfig{
            .workerThreadCount = 2 });
        bool rejected = false;
        try {
            SourceFileWatcher missing(
                std::chrono::milliseconds(1), true, nullptr);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);
        SourceFileWatcher watcher(
            std::chrono::milliseconds(1), true, &tasks);
        CHECK(watcher.watch(asset, path));
        write(path, "changed-size");
        std::vector<SourceFileChangeEvent> events;
        const auto deadline =
            std::chrono::steady_clock::now() +
            std::chrono::seconds(10);
        while (events.empty() &&
            std::chrono::steady_clock::now() < deadline) {
            tasks.tickPeriodic();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(2));
            events = watcher.drainEvents();
        }
        CHECK(events.size() == 1);
        CHECK(events[0].assetGuid == asset);
        CHECK(watcher.stats().scans >= 1);
        watcher.shutdown();
        const uint64_t scans = watcher.stats().scans;
        for (int tick = 0; tick < 10; ++tick) {
            tasks.tickPeriodic();
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(20));
        CHECK(watcher.stats().scans == scans);
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };
    constexpr TestCase tests[]{
        { "multi-owner change detection",
            detectsChangesForAllOwners },
        { "delete and recreate",
            detectsDeleteAndRecreate },
        { "unwatch and validation",
            unwatchAndValidation },
        { "periodic scan on the I/O thread",
            scansPeriodicallyOnTheIoThread },
    };
    size_t failures = 0;
    for (const TestCase& test : tests) {
        try {
            if (test.run()) {
                std::cout << "[PASS] " <<
                    test.name << '\n';
            } else {
                ++failures;
                std::cerr << "[FAIL] " <<
                    test.name << '\n';
            }
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << test.name <<
                ": " << exception.what() << '\n';
        }
    }
    std::cout << (std::size(tests) - failures) <<
        '/' << std::size(tests) <<
        " tests passed\n";
    return failures == 0 ? 0 : 1;
}
