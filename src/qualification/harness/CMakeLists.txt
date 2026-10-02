# App-side qualification harness (M7R R2.6): the IFrameObserver implementation
# (app/FrameObserver.h) that owns benchmark content, validators, capture requests,
# end-of-run reports and run-report export.
target_sources(iridium_qualification PRIVATE
        QualificationHarness.cpp
        BenchmarkScene.cpp
        FrameValidators.cpp
        QualificationReport.cpp
        RunReportExport.cpp
        HarnessDetail.cpp
)

# Build/source provenance printed into capture and run metadata. Only the run
# report export reads these, so only it recompiles when they change.
set_property(SOURCE RunReportExport.cpp
        TARGET_DIRECTORY iridium_qualification
        APPEND PROPERTY COMPILE_DEFINITIONS
        IRIDIUM_BUILD_CONFIGURATION="${CMAKE_BUILD_TYPE}"
        IRIDIUM_SOURCE_COMMIT="${IRIDIUM_SOURCE_COMMIT}"
        IRIDIUM_SOURCE_BRANCH="${IRIDIUM_SOURCE_BRANCH}"
        IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE=${IRIDIUM_SOURCE_DIRTY_AT_CONFIGURE}
        IRIDIUM_COMPILER="${IRIDIUM_COMPILER_ID}"
        IRIDIUM_SHADER_COMPILER="${IRIDIUM_SHADER_COMPILER}"
        IRIDIUM_VULKAN_SDK="${IRIDIUM_VULKAN_SDK_VERSION}")
