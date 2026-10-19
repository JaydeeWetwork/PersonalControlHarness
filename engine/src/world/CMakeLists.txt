# world: blocks, chunks, dimensions and worldgen glue (procgen pipelines -> chunk columns). Which
# dimensions exist and their generator files are the game's business.
add_library(litholattice_world STATIC column_generator.cpp dimension.cpp mesh_input.cpp raycast.cpp light_engine.cpp hash.cpp)
target_include_directories(litholattice_world PUBLIC include)
target_link_libraries(litholattice_world PUBLIC litholattice_procgen litholattice_core)
litholattice_module_options(litholattice_world)
