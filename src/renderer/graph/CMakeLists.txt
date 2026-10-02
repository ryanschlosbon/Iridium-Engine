# iridium_graph: backend-neutral render-graph declaration and compiler.
add_library(iridium_graph STATIC
        RenderGraph.cpp
)
target_link_libraries(iridium_graph PUBLIC iridium_core)
iridium_target_pch(iridium_graph ENGINE)
