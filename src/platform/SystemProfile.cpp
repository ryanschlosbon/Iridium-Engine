#include "SystemProfile.h"

#include <algorithm>
#include <array>
#include <cstring>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <intrin.h>
#endif

namespace Iridium {

    SystemProfile querySystemProfile() {
        SystemProfile profile{};
#if defined(_WIN32)
        using RtlGetVersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto rtlGetVersion = ntdll != nullptr
            ? reinterpret_cast<RtlGetVersionFunction>(
                GetProcAddress(ntdll, "RtlGetVersion"))
            : nullptr;
        OSVERSIONINFOW version{};
        version.dwOSVersionInfoSize = sizeof(version);
        if (rtlGetVersion != nullptr && rtlGetVersion(&version) == 0) {
            profile.operatingSystem = "Windows " +
                std::to_string(version.dwMajorVersion) + "." +
                std::to_string(version.dwMinorVersion) + "." +
                std::to_string(version.dwBuildNumber);
        }
        else {
            profile.operatingSystem = "Windows (version unavailable)";
        }

        int registers[4]{};
        __cpuid(registers, 0x80000000);
        const unsigned maximumExtendedLeaf = static_cast<unsigned>(registers[0]);
        if (maximumExtendedLeaf >= 0x80000004) {
            std::array<char, 49> brand{};
            for (unsigned leaf = 0; leaf < 3; ++leaf) {
                __cpuid(registers, static_cast<int>(0x80000002 + leaf));
                std::memcpy(brand.data() + leaf * 16, registers, 16);
            }
            profile.cpuName = brand.data();
            const auto first = profile.cpuName.find_first_not_of(' ');
            const auto last = profile.cpuName.find_last_not_of(' ');
            profile.cpuName = first == std::string::npos
                ? std::string{}
                : profile.cpuName.substr(first, last - first + 1);
        }

        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof(memory);
        if (GlobalMemoryStatusEx(&memory) != FALSE) {
            profile.physicalMemoryBytes = memory.ullTotalPhys;
        }
#else
        profile.operatingSystem = "unavailable";
        profile.cpuName = "unavailable";
#endif
        return profile;
    }

} // namespace Iridium
