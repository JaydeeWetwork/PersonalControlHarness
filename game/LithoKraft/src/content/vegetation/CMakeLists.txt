# vegetation: the game-side ProcGen vegetation API (workstream 3, 2026-10-11).
#
# lithokraft_vegetation wraps the engine generators (litholattice_procgen:
# trees.h / shrubs.h, workstreams 1+2) with game palette binding and returns
# ready-to-save lithokraft::model::Model values, plus the biome->vegetation
# mapping table. Neither executable below is a shipping target.

add_library(lithokraft_vegetation STATIC vegetation.cpp vegetation_models.cpp)
target_include_directories(lithokraft_vegetation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lithokraft_vegetation PUBLIC lithokraft_model litholattice_procgen)
litholattice_module_options(lithokraft_vegetation)

# Authoring tool: (re)generates game/LithoKraft/assets/models/{clockwood-wirewood,
# dopant-crystal-tree,trace-forest-tree}.lthm.
# Usage: author_vegetation [outdir]  (defaults to the source tree's assets/models)
add_executable(author_vegetation author_vegetation.cpp)
target_link_libraries(author_vegetation PRIVATE lithokraft_vegetation)
target_compile_definitions(author_vegetation PRIVATE
  LITHOKRAFT_MODELS_SRC_DIR="${LithoKraft_SOURCE_DIR}/assets/models")
litholattice_module_options(author_vegetation)
set_target_properties(author_vegetation PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Self-test: engine->Model wrapping round-trip, family re-author determinism
# vs the checked-in .lthm files, shrub 2D/3D paths, texture set, and the
# 11-biome mapping table (incl. a blendedDensity consumption smoke test).
add_executable(vegetation_selftest vegetation_selftest.cpp)
target_link_libraries(vegetation_selftest PRIVATE lithokraft_vegetation lithokraft_authored_models)
target_include_directories(vegetation_selftest PRIVATE ${LithoKraft_SOURCE_DIR}/src/game/include)
target_compile_definitions(vegetation_selftest PRIVATE
  LITHOKRAFT_MODELS_SRC_DIR="${LithoKraft_SOURCE_DIR}/assets/models")
litholattice_module_options(vegetation_selftest)
set_target_properties(vegetation_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME vegetation_selftest COMMAND vegetation_selftest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
