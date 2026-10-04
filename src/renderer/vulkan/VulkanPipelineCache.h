#pragma once

// M7R R4c.4: a persisted VkPipelineCache. The backend owns one per device and
// passes its handle to every vkCreate*Pipelines call and to ImGui; feature
// owners reach it through VulkanFeatureContext::pipelineCache.
//
// File: <directory>/<vendor>-<device>.ircache (lower-case hex IDs), an Iridium
// header followed by the vkGetPipelineCacheData payload. Little-endian, 64 bytes:
//
//   0  char[8]  magic "IRPCACHE"
//   8  u32      file version (kPipelineCacheFileVersion)
//  12  u32      header size (64)
//  16  u32      vendorID          20 u32 deviceID
//  24  u32      driverVersion     28 u32 driverID (VkDriverId)
//  32  u8[16]   pipelineCacheUUID
//  48  u64      payload size
//  56  u64      FNV-1a 64 of the payload
//
// The payload must also start with a VkPipelineCacheHeaderVersionOne naming the
// same vendor, device and UUID. Any mismatch or corruption discards the file
// (logged once) and the cache starts empty; a payload above the 512 MiB load
// cap is discarded without being read. The cache is saved at backend cleanup,
// after device idle and before the device is destroyed, by writing a temporary
// file and renaming it over the old one, so a crash never leaves a torn file.

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Iridium {

    inline constexpr uint32_t kPipelineCacheFileVersion = 1u;
    inline constexpr size_t kPipelineCacheFileHeaderBytes = 64u;
    inline constexpr uint64_t kPipelineCacheLoadLimitBytes = 512ull << 20;

    // The device and driver a cache file belongs to.
    struct VulkanPipelineCacheIdentity {
        uint32_t vendorId = 0;
        uint32_t deviceId = 0;
        uint32_t driverVersion = 0;
        uint32_t driverId = 0;
        std::array<uint8_t, VK_UUID_SIZE> pipelineCacheUuid{};

        [[nodiscard]] static VulkanPipelineCacheIdentity of(
            VkPhysicalDevice physicalDevice);
    };

    enum class PipelineCacheFileStatus : uint8_t {
        Accepted,
        Truncated,              // shorter than the header or its payload size
        BadMagic,
        UnsupportedVersion,     // file version or header size
        VendorMismatch,
        DeviceMismatch,
        DriverVersionMismatch,
        DriverIdMismatch,
        UuidMismatch,
        Oversized,              // payload above the load cap
        SizeMismatch,           // trailing bytes after the payload
        HashMismatch,
        BadVulkanHeader,        // VkPipelineCacheHeaderVersionOne disagrees
    };
    [[nodiscard]] std::string_view pipelineCacheFileStatusName(
        PipelineCacheFileStatus status) noexcept;

    struct PipelineCacheFileCheck {
        PipelineCacheFileStatus status = PipelineCacheFileStatus::Truncated;
        // The payload inside the checked image (Accepted only).
        std::span<const std::byte> payload;
    };

    [[nodiscard]] uint64_t pipelineCacheHash(std::span<const std::byte> bytes) noexcept;
    // "<vendor>-<device>.ircache" in lower-case hex, e.g. "10de-2684.ircache".
    [[nodiscard]] std::string pipelineCacheFileName(
        const VulkanPipelineCacheIdentity& identity);
    // Header of the Vulkan payload against the identity (vendor, device, UUID).
    [[nodiscard]] bool validVulkanPipelineCacheHeader(
        std::span<const std::byte> payload,
        const VulkanPipelineCacheIdentity& identity) noexcept;
    [[nodiscard]] PipelineCacheFileCheck checkPipelineCacheFile(
        std::span<const std::byte> file, const VulkanPipelineCacheIdentity& identity,
        uint64_t payloadLimit = kPipelineCacheLoadLimitBytes) noexcept;
    [[nodiscard]] std::vector<std::byte> encodePipelineCacheFile(
        std::span<const std::byte> payload, const VulkanPipelineCacheIdentity& identity);

    // Reads and checks a cache file without a device. A file larger than the
    // header plus `payloadLimit` is rejected as Oversized without being read.
    struct PipelineCacheFileLoad {
        bool found = false;
        PipelineCacheFileStatus status = PipelineCacheFileStatus::Truncated;
        std::vector<std::byte> payload;   // Accepted only
    };
    [[nodiscard]] PipelineCacheFileLoad loadPipelineCacheFile(
        const std::filesystem::path& path, const VulkanPipelineCacheIdentity& identity,
        uint64_t payloadLimit = kPipelineCacheLoadLimitBytes);
    // Writes `<path>.<pid>.tmp`, then renames it over `path` (creating the
    // directory). On failure the temporary file is removed, `path` is left as
    // it was, and `error` says why.
    [[nodiscard]] bool writeFileAtomically(const std::filesystem::path& path,
        std::span<const std::byte> bytes, std::string& error);

    enum class PipelineCacheState : uint8_t {
        Off,        // no directory: VK_NULL_HANDLE, as before R4c.4
        Cold,       // no file: started empty
        Warm,       // a valid file was loaded
        Discarded,  // a file was rejected (see discardReason): started empty
    };
    [[nodiscard]] std::string_view pipelineCacheStateName(PipelineCacheState state) noexcept;

    struct VulkanPipelineCacheStats {
        PipelineCacheState state = PipelineCacheState::Off;
        PipelineCacheFileStatus discardReason = PipelineCacheFileStatus::Accepted;
        // Payload bytes handed to vkCreatePipelineCache.
        uint64_t loadedBytes = 0;
        // File bytes written by save() (0 when nothing was written).
        uint64_t savedBytes = 0;
        bool saveSkippedUnchanged = false;
        std::filesystem::path file;
    };

    class VulkanPipelineCache final {
    public:
        VulkanPipelineCache() = default;
        VulkanPipelineCache(const VulkanPipelineCache&) = delete;
        VulkanPipelineCache& operator=(const VulkanPipelineCache&) = delete;
        ~VulkanPipelineCache() { destroy(); }

        // Loads <directory>/<vendor>-<device>.ircache when it is valid and
        // creates the cache. An empty directory leaves the cache off.
        void init(VkDevice device, const VulkanPipelineCacheIdentity& identity,
            const std::filesystem::path& directory);
        // Saves the cache when its contents changed since load. Call after
        // device idle (no pipeline creation in flight). False when the save
        // was attempted and failed.
        bool save();
        void destroy() noexcept;

        [[nodiscard]] VkPipelineCache handle() const noexcept { return cache_; }
        [[nodiscard]] const VulkanPipelineCacheStats& stats() const noexcept { return stats_; }

    private:
        VkDevice device_ = VK_NULL_HANDLE;
        VkPipelineCache cache_ = VK_NULL_HANDLE;
        VulkanPipelineCacheIdentity identity_{};
        uint64_t loadedHash_ = 0;
        VulkanPipelineCacheStats stats_{};
    };

} // namespace Iridium
