// M7R R4c.4: the persisted pipeline cache's file format, device-free. A file is
// accepted only for the exact vendor, device, driver version, driver ID and
// pipeline-cache UUID it was written for, with an intact payload (size and
// FNV-64) that starts with a matching VkPipelineCacheHeaderVersionOne; every
// other case is discarded. Saves replace the file atomically. The device
// round trip (create, save, reload warm) is in VulkanPipelineContractTests.

#include "platform/UserCacheDirectory.h"
#include "renderer/vulkan/VulkanPipelineCache.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << "  check failed: " #condition " (" << __FILE__ << ':' \
                << __LINE__ << ")\n"; \
            return false; \
        } \
    } while (false)

namespace {

    using namespace Iridium;
    using Status = PipelineCacheFileStatus;

    VulkanPipelineCacheIdentity identity() {
        VulkanPipelineCacheIdentity id{};
        id.vendorId = 0x10de;
        id.deviceId = 0x2684;
        id.driverVersion = 0x8f2b0000u;
        id.driverId = static_cast<uint32_t>(VK_DRIVER_ID_NVIDIA_PROPRIETARY);
        for (uint32_t i = 0; i < VK_UUID_SIZE; ++i)
            id.pipelineCacheUuid[i] = static_cast<uint8_t>(0xa0u + i);
        return id;
    }

    void put32(std::vector<std::byte>& bytes, size_t offset, uint32_t value) {
        for (int i = 0; i < 4; ++i)
            bytes[offset + i] = static_cast<std::byte>((value >> (8 * i)) & 0xffu);
    }

    // A driver-shaped payload: VkPipelineCacheHeaderVersionOne + opaque data.
    std::vector<std::byte> payload(const VulkanPipelineCacheIdentity& id,
        size_t dataBytes = 200) {
        std::vector<std::byte> bytes(32 + dataBytes);
        put32(bytes, 0, 32u);
        put32(bytes, 4, static_cast<uint32_t>(VK_PIPELINE_CACHE_HEADER_VERSION_ONE));
        put32(bytes, 8, id.vendorId);
        put32(bytes, 12, id.deviceId);
        std::memcpy(bytes.data() + 16, id.pipelineCacheUuid.data(), VK_UUID_SIZE);
        for (size_t i = 0; i < dataBytes; ++i)
            bytes[32 + i] = static_cast<std::byte>((i * 37u + 11u) & 0xffu);
        return bytes;
    }

    Status check(const std::vector<std::byte>& file,
        const VulkanPipelineCacheIdentity& id = identity(),
        uint64_t limit = kPipelineCacheLoadLimitBytes) {
        return checkPipelineCacheFile(file, id, limit).status;
    }

    std::filesystem::path scratchDirectory(const char* name) {
        const std::filesystem::path directory = std::filesystem::temp_directory_path() /
            (std::string("iridium-") + name + "-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        return directory;
    }

    void writeFile(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }

    std::vector<std::byte> readFile(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        std::vector<std::byte> bytes(static_cast<size_t>(std::filesystem::file_size(path)));
        input.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        return bytes;
    }

    bool testRoundTrip() {
        const VulkanPipelineCacheIdentity id = identity();
        const std::vector<std::byte> data = payload(id);
        const std::vector<std::byte> file = encodePipelineCacheFile(data, id);
        CHECK(file.size() == kPipelineCacheFileHeaderBytes + data.size());
        CHECK(std::memcmp(file.data(), "IRPCACHE", 8) == 0);
        const PipelineCacheFileCheck result = checkPipelineCacheFile(file, id);
        CHECK(result.status == Status::Accepted);
        CHECK(result.payload.size() == data.size());
        CHECK(std::memcmp(result.payload.data(), data.data(), data.size()) == 0);
        CHECK(pipelineCacheFileName(id) == "10de-2684.ircache");
        CHECK(pipelineCacheHash({}) == 0xcbf29ce484222325ull);
        const std::byte a[] = { std::byte{ 'a' } };
        CHECK(pipelineCacheHash(a) == 0xaf63dc4c8601ec8cull);
        return true;
    }

    bool testIdentityMismatches() {
        const VulkanPipelineCacheIdentity id = identity();
        const std::vector<std::byte> file = encodePipelineCacheFile(payload(id), id);
        VulkanPipelineCacheIdentity other = id;
        other.vendorId = 0x1002;
        CHECK(check(file, other) == Status::VendorMismatch);
        other = id;
        other.deviceId = 0x2704;
        CHECK(check(file, other) == Status::DeviceMismatch);
        other = id;
        other.driverVersion += 1u;
        CHECK(check(file, other) == Status::DriverVersionMismatch);
        other = id;
        other.driverId = static_cast<uint32_t>(VK_DRIVER_ID_MESA_RADV);
        CHECK(check(file, other) == Status::DriverIdMismatch);
        other = id;
        other.pipelineCacheUuid[7] ^= 0x01u;
        CHECK(check(file, other) == Status::UuidMismatch);
        return true;
    }

    bool testCorruption() {
        const VulkanPipelineCacheIdentity id = identity();
        const std::vector<std::byte> good = encodePipelineCacheFile(payload(id), id);

        // Truncated: inside the header, and inside the payload.
        CHECK(check({}) == Status::Truncated);
        CHECK(check({ good.begin(), good.begin() + 40 }) == Status::Truncated);
        CHECK(check({ good.begin(), good.end() - 1 }) == Status::Truncated);
        // Trailing bytes after the declared payload.
        std::vector<std::byte> longer = good;
        longer.push_back(std::byte{ 0 });
        CHECK(check(longer) == Status::SizeMismatch);
        // One flipped payload byte fails the FNV-64.
        std::vector<std::byte> flipped = good;
        flipped[kPipelineCacheFileHeaderBytes + 100] ^= std::byte{ 0x40 };
        CHECK(check(flipped) == Status::HashMismatch);
        // Magic, file version and header size.
        std::vector<std::byte> magic = good;
        magic[0] = std::byte{ 'X' };
        CHECK(check(magic) == Status::BadMagic);
        std::vector<std::byte> version = good;
        put32(version, 8, kPipelineCacheFileVersion + 1u);
        CHECK(check(version) == Status::UnsupportedVersion);
        std::vector<std::byte> headerSize = good;
        put32(headerSize, 12, 72u);
        CHECK(check(headerSize) == Status::UnsupportedVersion);
        return true;
    }

    bool testVulkanHeaderValidation() {
        const VulkanPipelineCacheIdentity id = identity();
        CHECK(validVulkanPipelineCacheHeader(payload(id), id));
        // Each field of VkPipelineCacheHeaderVersionOne, re-hashed so only the
        // Vulkan header check can reject it.
        const auto rejected = [&id](size_t offset, uint32_t value) {
            std::vector<std::byte> data = payload(id);
            put32(data, offset, value);
            return !validVulkanPipelineCacheHeader(data, id) &&
                check(encodePipelineCacheFile(data, id)) == Status::BadVulkanHeader;
        };
        CHECK(rejected(0, 16u));              // header size below the struct
        CHECK(rejected(0, 4096u));            // header size beyond the payload
        CHECK(rejected(4, 2u));               // header version
        CHECK(rejected(8, 0x8086u));          // vendor
        CHECK(rejected(12, 0x1234u));         // device
        std::vector<std::byte> uuid = payload(id);
        uuid[20] ^= std::byte{ 0x01 };
        CHECK(check(encodePipelineCacheFile(uuid, id)) == Status::BadVulkanHeader);
        // A payload too short for the Vulkan header.
        std::vector<std::byte> tiny(16);
        CHECK(check(encodePipelineCacheFile(tiny, id)) == Status::BadVulkanHeader);
        return true;
    }

    bool testOversized() {
        const VulkanPipelineCacheIdentity id = identity();
        // The declared payload size alone rejects a file over the cap.
        std::vector<std::byte> file = encodePipelineCacheFile(payload(id), id);
        std::vector<std::byte> declared = file;
        const uint64_t huge = kPipelineCacheLoadLimitBytes + 1u;
        for (int i = 0; i < 8; ++i)
            declared[48 + i] = static_cast<std::byte>((huge >> (8 * i)) & 0xffu);
        CHECK(check(declared) == Status::Oversized);
        // A file on disk above header + cap is rejected without being read
        // (exercised with a 1 KiB cap instead of 512 MiB).
        const std::filesystem::path directory = scratchDirectory("pipeline-cache-cap");
        const std::filesystem::path path = directory / pipelineCacheFileName(id);
        writeFile(path, encodePipelineCacheFile(payload(id, 2000), id));
        PipelineCacheFileLoad load = loadPipelineCacheFile(path, id, 1024u);
        CHECK(load.found && load.status == Status::Oversized && load.payload.empty());
        load = loadPipelineCacheFile(path, id);
        CHECK(load.found && load.status == Status::Accepted && load.payload.size() == 2032u);
        load = loadPipelineCacheFile(directory / "missing.ircache", id);
        CHECK(!load.found);
        std::filesystem::remove_all(directory);
        return true;
    }

    bool testAtomicSave() {
        const VulkanPipelineCacheIdentity id = identity();
        const std::filesystem::path directory = scratchDirectory("pipeline-cache-save");
        const std::filesystem::path path = directory / "nested" / pipelineCacheFileName(id);
        const std::vector<std::byte> first = encodePipelineCacheFile(payload(id, 100), id);
        const std::vector<std::byte> second = encodePipelineCacheFile(payload(id, 300), id);
        std::string error;
        // Creates the directory, then replaces the existing file.
        CHECK(writeFileAtomically(path, first, error));
        CHECK(readFile(path) == first);
        CHECK(writeFileAtomically(path, second, error));
        CHECK(readFile(path) == second);
        const auto entries = [&path] {
            size_t count = 0;
            for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
                (void)entry;
                ++count;
            }
            return count;
        };
        CHECK(entries() == 1u);   // no temporary file left behind
        CHECK(loadPipelineCacheFile(path, id).status == Status::Accepted);

        // A target that cannot be replaced (a non-empty directory): the save
        // fails, says why, and leaves no temporary file.
        const std::filesystem::path blocked = directory / "blocked.ircache";
        std::filesystem::create_directories(blocked / "child");
        error.clear();
        CHECK(!writeFileAtomically(blocked, first, error));
        CHECK(!error.empty());
        CHECK(std::filesystem::is_directory(blocked / "child"));
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            CHECK(entry.path().extension() != ".tmp");
        std::filesystem::remove_all(directory);
        return true;
    }

    bool testUserCacheDirectory() {
        const std::filesystem::path directory = userCacheDirectory("PipelineCache");
        CHECK(!directory.empty());
        CHECK(directory.is_absolute());
        CHECK(directory.filename() == "PipelineCache");
        CHECK(directory.parent_path().filename() == "Iridium");
#if defined(_WIN32)
        if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && *local)
            CHECK(directory.parent_path().parent_path() == std::filesystem::path(local));
#endif
        return true;
    }

} // namespace

int main() {
    struct TestCase {
        const char* name;
        bool (*run)();
    };
    constexpr TestCase tests[] = {
        { "encode/check round trip, file name and FNV-64", testRoundTrip },
        { "vendor, device, driver version, driver ID or UUID mismatch discards",
            testIdentityMismatches },
        { "truncated, trailing, bad-hash, bad-magic and bad-version files discard",
            testCorruption },
        { "VkPipelineCacheHeaderVersionOne is validated", testVulkanHeaderValidation },
        { "payloads over the 512 MiB cap discard without a read", testOversized },
        { "saves replace the file atomically", testAtomicSave },
        { "user cache directory", testUserCacheDirectory },
    };
    size_t failures = 0;
    for (const TestCase& test : tests) {
        bool passed = false;
        try {
            passed = test.run();
        }
        catch (const std::exception& exception) {
            std::cerr << "  exception: " << exception.what() << '\n';
        }
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << test.name << '\n';
        if (!passed) ++failures;
    }
    constexpr size_t count = sizeof(tests) / sizeof(tests[0]);
    std::cout << count - failures << '/' << count << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
