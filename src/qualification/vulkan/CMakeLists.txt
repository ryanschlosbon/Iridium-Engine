# Vulkan qualification extension sources (M7R R2.7/R2.8): frame captures and
# capture-validation readbacks, the indirect/LOD/occlusion/VSM oracle, and the
# installer that main() calls to register the extension with the legacy
# backend factory overload.
target_sources(iridium_qualification PRIVATE
        VulkanIndirectOracle.cpp
        VulkanQualificationExtension.cpp
        VulkanQualificationInstall.cpp
        VulkanReadbackAnalysis.cpp)
