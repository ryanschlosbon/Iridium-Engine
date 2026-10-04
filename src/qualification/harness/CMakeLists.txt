# App-side qualification harness (M7R R2.6): the IFrameObserver implementation
# (app/FrameObserver.h) that owns benchmark content, validators, capture requests,
# end-of-run reports and run-report export. Build provenance comes from
# core/BuildInfo.h.
target_sources(iridium_qualification PRIVATE
        QualificationHarness.cpp
        BenchmarkScene.cpp
        FrameValidators.cpp
        QualificationReport.cpp
        RunReportExport.cpp
        HarnessDetail.cpp
        ScriptedChanges.cpp
        ScriptedChangeEvents.cpp
)
