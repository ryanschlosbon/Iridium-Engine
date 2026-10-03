#include "VulkanPipelineCache.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace Iridium {

    namespace {

        constexpr std::array<char, 8> kMagic{ 'I', 'R', 'P', 'C', 'A', 'C', 'H', 'E' };
        constexpr size_t kVulkanHeaderBytes = 16u + VK_UUID_SIZE;

        void put32(std::byte* out, uint32_t value) noexcept {
            for (int i = 0; i < 4; ++i)
                out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xffu);
        }
        void put64(std::byte* out, uint64_t value) noexcept {
            for (int i = 0; i < 8; ++i)
                out[i] = static_cast<std::byte>((value >> (8 * i)) & 0xffu);
        }
        uint32_t get32(const std::byte* in) noexcept {
            uint32_t value = 0;
            for (int i = 0; i < 4; ++i)
                value |= static_cast<uint32_t>(in[i]) << (8 * i);
            return value;
        }
        uint64_t get64(const std::byte* in) noexcept {
            uint64_t value = 0;
            for (int i = 0; i < 8; ++i)
                value |= static_cast<uint64_t>(in[i]) << (8 * i);
            return value;
        }

        unsigned long processId() noexcept {
#if defined(_WIN32)
            return static_cast<unsigned long>(_getpid());
#else
            return static_cast<unsigned long>(getpid());
#endif
        }

    } // namespace

    VulkanPipelineCacheIdentity VulkanPipelineCacheIdentity::of(
        VkPhysicalDevice physicalDevice) {
        VkPhysicalDeviceDriverProperties driver{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 properties{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties.pNext = &driver;
        vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
        VulkanPipelineCacheIdentity identity{};
        identity.vendorId = properties.properties.vendorID;
        identity.deviceId = properties.properties.deviceID;
        identity.driverVersion = properties.properties.driverVersion;
        identity.driverId = static_cast<uint32_t>(driver.driverID);
        std::memcpy(identity.pipelineCacheUuid.data(),
            properties.properties.pipelineCacheUUID, VK_UUID_SIZE);
        return identity;
    }

    std::string_view pipelineCacheFileStatusName(PipelineCacheFileStatus status) noexcept {
        switch (status) {
        case PipelineCacheFileStatus::Accepted: return "accepted";
        case PipelineCacheFileStatus::Truncated: return "truncated";
        case PipelineCacheFileStatus::BadMagic: return "bad-magic";
        case PipelineCacheFileStatus::UnsupportedVersion: return "unsupported-version";
        case PipelineCacheFileStatus::VendorMismatch: return "vendor-mismatch";
        case PipelineCacheFileStatus::DeviceMismatch: return "device-mismatch";
        case PipelineCacheFileStatus::DriverVersionMismatch: return "driver-version-mismatch";
        case PipelineCacheFileStatus::DriverIdMismatch: return "driver-id-mismatch";
        case PipelineCacheFileStatus::UuidMismatch: return "uuid-mismatch";
        case PipelineCacheFileStatus::Oversized: return "oversized";
        case PipelineCacheFileStatus::SizeMismatch: return "size-mismatch";
        case PipelineCacheFileStatus::HashMismatch: return "hash-mismatch";
        case PipelineCacheFileStatus::BadVulkanHeader: return "bad-vulkan-header";
        }
        return "unknown";
    }

    std::string_view pipelineCacheStateName(PipelineCacheState state) noexcept {
        switch (state) {
        case PipelineCacheState::Off: return "off";
        case PipelineCacheState::Cold: return "cold";
        case PipelineCacheState::Warm: return "warm";
        case PipelineCacheState::Discarded: return "discarded";
        }
        return "unknown";
    }

    uint64_t pipelineCacheHash(std::span<const std::byte> bytes) noexcept {
        uint64_t hash = 0xcbf29ce484222325ull;
        for (const std::byte value : bytes) {
            hash ^= static_cast<uint64_t>(value);
            hash *= 0x100000001b3ull;
        }
        return hash;
    }

    std::string pipelineCacheFileName(const VulkanPipelineCacheIdentity& identity) {
        char name[48]{};
        std::snprintf(name, sizeof(name), "%04x-%04x.ircache",
            identity.vendorId, identity.deviceId);
        return name;
    }

    bool validVulkanPipelineCacheHeader(std::span<const std::byte> payload,
        const VulkanPipelineCacheIdentity& identity) noexcept {
        if (payload.size() < kVulkanHeaderBytes) return false;
        const uint32_t headerSize = get32(payload.data());
        const uint32_t headerVersion = get32(payload.data() + 4);
        return headerSize >= kVulkanHeaderBytes && headerSize <= payload.size() &&
            headerVersion == static_cast<uint32_t>(VK_PIPELINE_CACHE_HEADER_VERSION_ONE) &&
            get32(payload.data() + 8) == identity.vendorId &&
            get32(payload.data() + 12) == identity.deviceId &&
            std::memcmp(payload.data() + 16, identity.pipelineCacheUuid.data(),
                VK_UUID_SIZE) == 0;
    }

    PipelineCacheFileCheck checkPipelineCacheFile(std::span<const std::byte> file,
        const VulkanPipelineCacheIdentity& identity, uint64_t payloadLimit) noexcept {
        using Status = PipelineCacheFileStatus;
        if (file.size() < kPipelineCacheFileHeaderBytes) return { Status::Truncated, {} };
        const std::byte* header = file.data();
        if (std::memcmp(header, kMagic.data(), kMagic.size()) != 0)
            return { Status::BadMagic, {} };
        if (get32(header + 8) != kPipelineCacheFileVersion ||
            get32(header + 12) != kPipelineCacheFileHeaderBytes)
            return { Status::UnsupportedVersion, {} };
        if (get32(header + 16) != identity.vendorId) return { Status::VendorMismatch, {} };
        if (get32(header + 20) != identity.deviceId) return { Status::DeviceMismatch, {} };
        if (get32(header + 24) != identity.driverVersion)
            return { Status::DriverVersionMismatch, {} };
        if (get32(header + 28) != identity.driverId) return { Status::DriverIdMismatch, {} };
        if (std::memcmp(header + 32, identity.pipelineCacheUuid.data(), VK_UUID_SIZE) != 0)
            return { Status::UuidMismatch, {} };
        const uint64_t payloadSize = get64(header + 48);
        if (payloadSize > payloadLimit) return { Status::Oversized, {} };
        const uint64_t available = file.size() - kPipelineCacheFileHeaderBytes;
        if (available < payloadSize) return { Status::Truncated, {} };
        if (available > payloadSize) return { Status::SizeMismatch, {} };
        const std::span<const std::byte> payload = file.subspan(
            kPipelineCacheFileHeaderBytes, static_cast<size_t>(payloadSize));
        if (pipelineCacheHash(payload) != get64(header + 56))
            return { Status::HashMismatch, {} };
        if (!validVulkanPipelineCacheHeader(payload, identity))
            return { Status::BadVulkanHeader, {} };
        return { Status::Accepted, payload };
    }

    std::vector<std::byte> encodePipelineCacheFile(std::span<const std::byte> payload,
        const VulkanPipelineCacheIdentity& identity) {
        std::vector<std::byte> file(kPipelineCacheFileHeaderBytes + payload.size());
        std::byte* header = file.data();
        std::memcpy(header, kMagic.data(), kMagic.size());
        put32(header + 8, kPipelineCacheFileVersion);
        put32(header + 12, static_cast<uint32_t>(kPipelineCacheFileHeaderBytes));
        put32(header + 16, identity.vendorId);
        put32(header + 20, identity.deviceId);
        put32(header + 24, identity.driverVersion);
        put32(header + 28, identity.driverId);
        std::memcpy(header + 32, identity.pipelineCacheUuid.data(), VK_UUID_SIZE);
        put64(header + 48, payload.size());
        put64(header + 56, pipelineCacheHash(payload));
        if (!payload.empty())
            std::memcpy(header + kPipelineCacheFileHeaderBytes, payload.data(),
                payload.size());
        return file;
    }

    PipelineCacheFileLoad loadPipelineCacheFile(const std::filesystem::path& path,
        const VulkanPipelineCacheIdentity& identity, uint64_t payloadLimit) {
        PipelineCacheFileLoad result{};
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) return result;
        result.found = true;
        const uintmax_t size = std::filesystem::file_size(path, error);
        if (error) return result;
        if (size > kPipelineCacheFileHeaderBytes + payloadLimit) {
            result.status = PipelineCacheFileStatus::Oversized;
            return result;
        }
        std::vector<std::byte> file(static_cast<size_t>(size));
        std::ifstream input(path, std::ios::binary);
        if (!input.read(reinterpret_cast<char*>(file.data()),
                static_cast<std::streamsize>(file.size()))) {
            result.status = PipelineCacheFileStatus::Truncated;
            return result;
        }
        const PipelineCacheFileCheck check =
            checkPipelineCacheFile(file, identity, payloadLimit);
        result.status = check.status;
        if (check.status == PipelineCacheFileStatus::Accepted)
            result.payload.assign(check.payload.begin(), check.payload.end());
        return result;
    }

    bool writeFileAtomically(const std::filesystem::path& path,
        std::span<const std::byte> bytes, std::string& error) {
        std::error_code code;
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path(), code);
            if (code) {
                error = "cannot create " + path.parent_path().string() + ": " +
                    code.message();
                return false;
            }
        }
        std::filesystem::path temporary = path;
        temporary += "." + std::to_string(processId()) + ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (output) {
                output.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
                output.flush();
            }
            if (!output) {
                output.close();
                std::filesystem::remove(temporary, code);
                error = "cannot write " + temporary.string();
                return false;
            }
        }
        // Replaces an existing file (MoveFileExW with MOVEFILE_REPLACE_EXISTING
        // on Windows, rename(2) elsewhere).
        std::filesystem::rename(temporary, path, code);
        if (code) {
            error = "cannot rename " + temporary.string() + " to " + path.string() +
                ": " + code.message();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
        return true;
    }

    void VulkanPipelineCache::init(VkDevice device,
        const VulkanPipelineCacheIdentity& identity,
        const std::filesystem::path& directory) {
        destroy();
        stats_ = {};
        device_ = device;
        identity_ = identity;
        if (directory.empty()) return;
        stats_.file = directory / pipelineCacheFileName(identity);

        PipelineCacheFileLoad load = loadPipelineCacheFile(stats_.file, identity);
        VkPipelineCacheCreateInfo info{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        if (load.found && load.status == PipelineCacheFileStatus::Accepted) {
            info.initialDataSize = load.payload.size();
            info.pInitialData = load.payload.data();
            if (vkCreatePipelineCache(device_, &info, nullptr, &cache_) == VK_SUCCESS) {
                stats_.state = PipelineCacheState::Warm;
                stats_.loadedBytes = load.payload.size();
                loadedHash_ = pipelineCacheHash(load.payload);
                std::cout << "Pipeline cache: loaded " << stats_.loadedBytes
                    << " bytes from " << stats_.file.string() << '\n';
                return;
            }
            // The driver rejected data that passed every check; start empty.
            load.status = PipelineCacheFileStatus::BadVulkanHeader;
            cache_ = VK_NULL_HANDLE;
        }
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        if (vkCreatePipelineCache(device_, &info, nullptr, &cache_) != VK_SUCCESS) {
            cache_ = VK_NULL_HANDLE;
            stats_.state = PipelineCacheState::Off;
            std::cout << "Pipeline cache: vkCreatePipelineCache failed; running without one\n";
            return;
        }
        if (load.found) {
            stats_.state = PipelineCacheState::Discarded;
            stats_.discardReason = load.status;
            std::cout << "Pipeline cache: discarded " << stats_.file.string() << " ("
                << pipelineCacheFileStatusName(load.status) << "); starting empty\n";
        }
        else {
            stats_.state = PipelineCacheState::Cold;
            std::cout << "Pipeline cache: no file at " << stats_.file.string()
                << "; starting empty\n";
        }
    }

    bool VulkanPipelineCache::save() {
        if (cache_ == VK_NULL_HANDLE || stats_.file.empty()) return true;
        stats_.savedBytes = 0;
        stats_.saveSkippedUnchanged = false;
        size_t size = 0;
        if (vkGetPipelineCacheData(device_, cache_, &size, nullptr) != VK_SUCCESS) {
            std::cout << "Pipeline cache: vkGetPipelineCacheData failed; not saved\n";
            return false;
        }
        if (size > kPipelineCacheLoadLimitBytes) {
            std::cout << "Pipeline cache: " << size
                << " bytes exceed the load cap; not saved\n";
            return false;
        }
        std::vector<std::byte> payload(size);
        if (size != 0 && vkGetPipelineCacheData(device_, cache_, &size,
                payload.data()) != VK_SUCCESS) {
            std::cout << "Pipeline cache: vkGetPipelineCacheData failed; not saved\n";
            return false;
        }
        payload.resize(size);
        if (!validVulkanPipelineCacheHeader(payload, identity_)) {
            std::cout << "Pipeline cache: the driver's data has an unexpected header; "
                "not saved\n";
            return false;
        }
        if (stats_.state == PipelineCacheState::Warm && size == stats_.loadedBytes &&
            pipelineCacheHash(payload) == loadedHash_) {
            stats_.saveSkippedUnchanged = true;
            std::cout << "Pipeline cache: unchanged (" << size << " bytes); not saved\n";
            return true;
        }
        const std::vector<std::byte> file = encodePipelineCacheFile(payload, identity_);
        std::string error;
        if (!writeFileAtomically(stats_.file, file, error)) {
            std::cout << "Pipeline cache: save failed: " << error << '\n';
            return false;
        }
        stats_.savedBytes = file.size();
        std::cout << "Pipeline cache: saved " << file.size() << " bytes to "
            << stats_.file.string() << '\n';
        return true;
    }

    void VulkanPipelineCache::destroy() noexcept {
        if (cache_ != VK_NULL_HANDLE && device_ != VK_NULL_HANDLE)
            vkDestroyPipelineCache(device_, cache_, nullptr);
        cache_ = VK_NULL_HANDLE;
    }

} // namespace Iridium
