# subdiv: sparse-octree sub-block subdivision (workstream 1).
# SparseOctree (octree.cpp) + SubdivStore (subdiv_store.cpp) + headless
# self-tests (subdiv_selftest.cpp). Render/detail-level only: the sim ticks
# whole blocks; micro-cells are inert geometry.
add_library(litholattice_subdiv STATIC octree.cpp subdiv_store.cpp subdiv_selftest.cpp)
target_include_directories(litholattice_subdiv PUBLIC include)
litholattice_module_options(litholattice_subdiv)
