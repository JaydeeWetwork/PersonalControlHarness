# litholattice_tests: engine-only tests (links no game code). ctest runs one test per module.
add_executable(litholattice_tests engine_tests.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/render/software_test.cpp)
# PBR tests load the shipped defaults data file by absolute source path.
target_compile_definitions(litholattice_tests PRIVATE
  LITHOKRAFT_MATERIALS_PATH="${LithoLattice_SOURCE_DIR}/assets/materials/lithokraft.materials")
target_link_libraries(litholattice_tests PRIVATE litholattice_render_vk litholattice_storage litholattice_ai litholattice_sim litholattice_render litholattice_world litholattice_procgen
                      litholattice_ui litholattice_assets litholattice_audio litholattice_input litholattice_net litholattice_script litholattice_mods litholattice_console litholattice_crypto litholattice_index litholattice_mcp litholattice_defrag litholattice_subdiv litholattice_anim litholattice_texture litholattice_core)
litholattice_module_options(litholattice_tests)
foreach(module core world render sim ai anim storage procgen trees shrubs water litho_formula ui assets audio input net script mods console crypto index mcp defrag subdiv texture)
  add_test(NAME engine_${module} COMMAND litholattice_tests ${module})
endforeach()
