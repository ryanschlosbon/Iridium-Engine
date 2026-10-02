# Iridium Engine build entry point.
#
# Layout (M7R R1): this file sets up the project, compiler settings and third-party
# dependencies, then delegates to one CMakeLists.txt per module. Each module is a
# STATIC library (see src/CMakeLists.txt for the dependency DAG); executables link
# libraries, so every production source compiles once per configuration.
cmake_minimum_required(VERSION 3.20)

project(IridiumEngine VERSION 0.1 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(IridiumCompilerSettings)
include(IridiumSourceInfo)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# No C++20 named modules are compiled; skipping the dependency scan removes one
# dyndep collation step per target without changing generated code.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(IridiumDependencies)
include(IridiumShaders)

# Defines PROJECT_ROOT_DIR (with a trailing slash) for C++ to find assets. This is
# directory-scoped, so every engine, tool and test target sees the same value; the
# FetchContent dependencies above are added before it and do not.
add_compile_definitions(PROJECT_ROOT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/")

include(IridiumHelpers)
include(CTest)

# Qualification harness (M7R R2): oracles, readback validation, frame captures,
# validators, deterministic benchmark scenes and run reports. OFF builds a
# shipping-shaped engine without that code; C++ sees kQualificationBuild.
option(IRIDIUM_QUALIFICATION
    "Build the qualification harness (oracles, captures, validators, benchmarks)" ON)
if(IRIDIUM_QUALIFICATION)
    set(IRIDIUM_QUALIFICATION_BUILD true)
    set(IRIDIUM_QUALIFICATION_BUILD_VALUE 1)
else()
    set(IRIDIUM_QUALIFICATION_BUILD false)
    set(IRIDIUM_QUALIFICATION_BUILD_VALUE 0)
endif()
configure_file(cmake/BuildFeatures.h.in
    "${CMAKE_BINARY_DIR}/generated/core/BuildFeatures.h" @ONLY)

add_subdirectory(src)
add_subdirectory(tools)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
