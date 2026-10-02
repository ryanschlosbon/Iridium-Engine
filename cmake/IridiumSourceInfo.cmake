# Source provenance recorded at configure time (capture sidecars, benchmark
# reports). Only the translation units that print these values receive them as
# compile definitions (see src/app and tools/), so a new commit recompiles those
# files instead of every engine source.

find_package(Git QUIET)
set(IRIDIUM_SOURCE_COMMIT "unknown")
set(IRIDIUM_SOURCE_BRANCH "unknown")
set(IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE 0)
if(Git_FOUND)
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --verify HEAD
            OUTPUT_VARIABLE IRIDIUM_SOURCE_COMMIT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
    )
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" rev-parse --abbrev-ref HEAD
            OUTPUT_VARIABLE IRIDIUM_SOURCE_BRANCH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
    )
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}" status --porcelain
            OUTPUT_VARIABLE IRIDIUM_SOURCE_STATUS
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
    )
    if(NOT IRIDIUM_SOURCE_STATUS STREQUAL "")
        set(IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE 1)
    endif()
endif()

set(IRIDIUM_COMPILER_ID "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
