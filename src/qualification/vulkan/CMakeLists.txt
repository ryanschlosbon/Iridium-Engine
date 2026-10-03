# Vulkan qualification extension sources (M7R R2.7-R2.9): frame captures and
# capture-validation readbacks and the indirect/LOD/occlusion/VSM oracle. The
# harness owns the extension and attaches it through RenderBackendCreateInfo.
target_sources(iridium_qualification PRIVATE
        VulkanIndirectOracle.cpp
        VulkanIndirectStreamDigest.cpp
        VulkanQualificationExtension.cpp
        VulkanReadbackAnalysis.cpp)
