# procgen: noise (Perlin, OpenSimplex2 F/S, value, cellular, white), modifiers (fractals, domain
# warp), the JSON node graph, the generator pipeline (stages, seeding), the procedural tree generator
# (trees.h), the procedural grass/shrub generator (shrubs.h) and the Litho Formula sampler
# (litho_formula.h) plus its flattened 2D form (litho_formula_2d.h: outline / SVG / texture).
# Depends on core and subdiv
# (SparseOctree for tree/shrub output):
# no world or GL types; worlds plug in through material ids and stage objects. Assets is linked
# only for Image::decodeSvg, which rasterizes the 2D formula's generated SVG into textures
# (assets depends on core alone — no cycle).
add_library(litholattice_procgen STATIC noise.cpp perlin.cpp open_simplex2.cpp value_noise.cpp cellular_noise.cpp
            modifiers.cpp graph.cpp graph_nodes.cpp
            litho_formula_gallery.cpp
            pipeline.cpp stages.cpp diversity.cpp generator_loader.cpp procgen_selftest.cpp diversity_selftest.cpp
            struct_registry.cpp struct_registry_selftest.cpp
            struct_block.cpp
            water.cpp water_selftest.cpp
            trees.cpp trees_selftest.cpp shrubs.cpp shrubs_selftest.cpp
            litho_formula.cpp litho_formula_selftest.cpp
            litho_formula_2d.cpp litho_formula_2d_selftest.cpp
            litho_formula_node.cpp litho_formula_node_selftest.cpp
            litho_stamp_feature_selftest.cpp
            litho_formula_voxelize.cpp litho_formula_voxelize_selftest.cpp)
target_include_directories(litholattice_procgen PUBLIC include)
target_link_libraries(litholattice_procgen PUBLIC litholattice_core litholattice_subdiv litholattice_crypto litholattice_assets)
# water_selftest.cpp reads litholattice/world/fluid.h (fluid-level emission convention);
# private include dir only -- procgen still does not link the world module.
target_include_directories(litholattice_procgen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../world/include)
litholattice_module_options(litholattice_procgen)

# The lithoform_stamp self-test loads the game's real wafer.json as an
# integration check. In this repository the game tree sits beside the engine,
# so pass its absolute path in; the LITHOKRAFT_WAFER_JSON env var overrides it.
set(LITHOKRAFT_WAFER_JSON "${LithoLattice_SOURCE_DIR}/../game/LithoKraft/assets/worldgen/wafer.json")
if(EXISTS "${LITHOKRAFT_WAFER_JSON}")
  set_source_files_properties(litho_stamp_feature_selftest.cpp PROPERTIES
    COMPILE_DEFINITIONS "LITHOKRAFT_WAFER_JSON_PATH=\"${LITHOKRAFT_WAFER_JSON}\"")
endif()
