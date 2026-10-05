# iridium_profiling: CPU/GPU timing, memory profile and profile export.
add_library(iridium_profiling STATIC
        CpuProfiler.cpp
        GpuTimestamp.cpp
        MemoryProfile.cpp
        CpuProfileExport.cpp
)
target_link_libraries(iridium_profiling PUBLIC iridium_core nlohmann_json::nlohmann_json)
iridium_target_pch(iridium_profiling ENGINE)

# CpuAllocationProfile.cpp replaces the global operator new/delete family. It is an
# OBJECT library linked directly into each executable that tracks allocations
# (iridium_link_allocation_profile), never archived into a static library, so the
# replacement is always linked ahead of the CRT's operator new.
add_library(iridium_allocation_profile OBJECT
        CpuAllocationProfile.cpp
)
target_link_libraries(iridium_allocation_profile PUBLIC iridium_core)
iridium_target_pch(iridium_allocation_profile ENGINE)
