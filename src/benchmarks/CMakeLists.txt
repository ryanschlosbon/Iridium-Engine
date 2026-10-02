# iridium_benchmarks: benchmark manifest parsing.
add_library(iridium_benchmarks STATIC
        BenchmarkManifest.cpp
)
target_link_libraries(iridium_benchmarks PUBLIC iridium_core nlohmann_json::nlohmann_json)
iridium_target_pch(iridium_benchmarks ENGINE)
