# model: the .lthm model format + save/place pipeline (workstream 3, game side).
#
# lithokraft_model links the real engine subdivision library
# (litholattice_subdiv, workstream 1): SparseOctree + SubdivStore. The
# temporary octree_test_stub.cpp is gone — do not reintroduce it; linking it
# alongside the real octree.cpp would duplicate symbols.

add_library(lithokraft_model STATIC model.cpp litho_stamp_models.cpp)
target_include_directories(lithokraft_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lithokraft_model PUBLIC litholattice_subdiv litholattice_procgen)
# Baked gallery location for the lithoform_stamp ModelResolver (same as lithokraft_content).
target_compile_definitions(lithokraft_model PRIVATE LITHOKRAFT_MODELS_DIR="${LithoKraft_SOURCE_DIR}/assets/models")
litholattice_module_options(lithokraft_model)

add_executable(model_selftest model_selftest.cpp)
target_link_libraries(model_selftest PRIVATE lithokraft_model)
target_compile_definitions(model_selftest PRIVATE LITHOKRAFT_MODELS_DIR="${LithoKraft_SOURCE_DIR}/assets/models")
litholattice_module_options(model_selftest)
set_target_properties(model_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME model_selftest COMMAND model_selftest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# --- workstream 5: the first authored models --------------------------------# models.cpp holds the eight author functions (shared by the tool and the
# test below). All three targets below link the real engine subdivision
# library (transitively via lithokraft_model); neither executable is a
# shipping target.
add_library(lithokraft_authored_models STATIC models.cpp)
target_link_libraries(lithokraft_authored_models PUBLIC lithokraft_model)
litholattice_module_options(lithokraft_authored_models)

# Authoring tool: (re)generates game/LithoKraft/assets/models/*.lthm.
# Usage: author_models [outdir]  (defaults to the source tree's assets/models)
add_executable(author_models author_models.cpp)
target_link_libraries(author_models PRIVATE lithokraft_authored_models)
target_compile_definitions(author_models PRIVATE
  LITHOKRAFT_MODELS_SRC_DIR="${LithoKraft_SOURCE_DIR}/assets/models")
litholattice_module_options(author_models)
set_target_properties(author_models PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Self-test for the eight authored models: save/load round-trip, leaf-count
# sanity, bounding checks, determinism (re-author => byte-identical files,
# matching the checked-in assets/models/*.lthm).
add_executable(authored_models_selftest authored_models_selftest.cpp)
target_link_libraries(authored_models_selftest PRIVATE lithokraft_authored_models)
target_compile_definitions(authored_models_selftest PRIVATE
  LITHOKRAFT_MODELS_SRC_DIR="${LithoKraft_SOURCE_DIR}/assets/models")
litholattice_module_options(authored_models_selftest)
set_target_properties(authored_models_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME authored_models_selftest COMMAND authored_models_selftest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# --- Litho Formula preset gallery -------------------------------------------
# litho_formula_bake.cpp holds the shared bake path (voxelize each gallery
# preset -> ModelBuilder -> .lthm), used by the CLI below and the self-test.
# It links the engine procgen module (sampler + voxelizer + gallery loader)
# and the game model module (ModelBuilder / saveModel / loadModel).
add_library(lithokraft_litho_formula_bake STATIC litho_formula_bake.cpp)
target_include_directories(lithokraft_litho_formula_bake PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lithokraft_litho_formula_bake PUBLIC lithokraft_model litholattice_procgen)
litholattice_module_options(lithokraft_litho_formula_bake)

# Baking tool: (re)generates game/LithoKraft/assets/models/<preset>.lthm from
# assets/models/litho_formula_gallery.json.
# Usage: bake_litho_formula [outdir] [gallery.json]
add_executable(bake_litho_formula bake_litho_formula.cpp)
target_link_libraries(bake_litho_formula PRIVATE lithokraft_litho_formula_bake)
target_compile_definitions(bake_litho_formula PRIVATE
  LITHOKRAFT_MODELS_SRC_DIR="${LithoKraft_SOURCE_DIR}/assets/models")
litholattice_module_options(bake_litho_formula)
set_target_properties(bake_litho_formula PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Self-test: every gallery preset voxelizes non-empty and bounded, and the
# bake -> save -> load round-trip is exact and deterministic.
add_executable(litho_formula_gallery_selftest litho_formula_gallery_selftest.cpp)
target_link_libraries(litho_formula_gallery_selftest PRIVATE lithokraft_litho_formula_bake)
target_compile_definitions(litho_formula_gallery_selftest PRIVATE
  LITHOKRAFT_MODELS_SRC_DIR="${LithoKraft_SOURCE_DIR}/assets/models")
litholattice_module_options(litho_formula_gallery_selftest)
set_target_properties(litho_formula_gallery_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME litho_formula_gallery_selftest COMMAND litho_formula_gallery_selftest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# CLI smoke test: run the real bake_litho_formula binary into a temp dir,
# then verify every .lthm with the self-test's --verify-dir mode.
add_test(NAME litho_formula_bake_cli
  COMMAND ${CMAKE_COMMAND}
    -DBAKE_EXE=$<TARGET_FILE:bake_litho_formula>
    -DCHECK_EXE=$<TARGET_FILE:litho_formula_gallery_selftest>
    -DGALLERY_JSON=${LithoKraft_SOURCE_DIR}/assets/models/litho_formula_gallery.json
    -DBAKE_TMP=${CMAKE_BINARY_DIR}/litho-formula-bake-tmp
    -P ${CMAKE_CURRENT_SOURCE_DIR}/litho_formula_bake_check.cmake)

# --- agent models ------------------------------------------------------------
# Figure-scale voxel models for players / agents (32^3 stamp grids, palette
# paint, riggen-ready). First model: "elfie", the designated spawn model.
add_library(lithokraft_agent_models STATIC agent_models.cpp)
target_include_directories(lithokraft_agent_models PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lithokraft_agent_models PUBLIC litholattice_procgen litholattice_anim)
litholattice_module_options(lithokraft_agent_models)

add_executable(agent_models_selftest agent_models_selftest.cpp)
target_link_libraries(agent_models_selftest PRIVATE lithokraft_agent_models)
litholattice_module_options(agent_models_selftest)
set_target_properties(agent_models_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(NAME agent_models_selftest COMMAND agent_models_selftest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
