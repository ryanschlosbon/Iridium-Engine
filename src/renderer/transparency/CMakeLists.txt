# iridium_transparency: header-only layered-glass, Ordinary2/deep-tier atlas,
# WeightedOIT and transparency-pyramid contracts. Depends on the RHI (draw
# packets); the RHI no longer includes it (M7R R2.9).
add_library(iridium_transparency INTERFACE)
target_link_libraries(iridium_transparency INTERFACE iridium_rhi)
