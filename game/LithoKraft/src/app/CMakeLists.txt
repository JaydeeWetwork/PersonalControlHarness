# Dev builds (default) load scripts from the source tree so F10 edits stay in the repo; shipping
# builds use the scripts/ copied next to the exe. --scripts source|exe|<dir> overrides at run time.
option(LITHOKRAFT_DEV_BUILD "Scripts (and the F10 editor) use the source tree's scripts/" ON)
add_executable(lithokraft main.cpp scripts_dir.cpp selftest.cpp perf_headless.cpp noise_preview.cpp handshake_test.cpp control_latency.cpp control_stream_test.cpp demo_record_test.cpp)
# Exe icon (Windows): lithokraft.rc embeds LithoKraft/assets/icons/lithokraft.ico.
set(LITHOKRAFT_ICO ${LithoKraft_SOURCE_DIR}/assets/icons/lithokraft.ico)
if(WIN32 AND EXISTS ${LITHOKRAFT_ICO})
  configure_file(${LITHOKRAFT_ICO} ${CMAKE_CURRENT_BINARY_DIR}/lithokraft.ico COPYONLY)
  configure_file(lithokraft.rc ${CMAKE_CURRENT_BINARY_DIR}/lithokraft.rc COPYONLY)
  target_sources(lithokraft PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/lithokraft.rc)
elseif(WIN32)
  message(WARNING "lithokraft.ico not found: the exe has no icon")
endif()
target_link_libraries(lithokraft PRIVATE lithokraft_game litholattice_audio_device litholattice_render_vk)
target_compile_definitions(lithokraft PRIVATE LITHOKRAFT_VERSION="${PROJECT_VERSION}"
                          LITHOKRAFT_SOURCE_DIR="${LithoKraft_SOURCE_DIR}"
                          LITHOKRAFT_ENGINE_ASSETS_DIR="${LithoLattice_SOURCE_DIR}/assets")
set_target_properties(lithokraft PROPERTIES OUTPUT_NAME LithoKraft
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
if(LITHOKRAFT_DEV_BUILD)
  target_compile_definitions(lithokraft PRIVATE LITHOKRAFT_DEV_SCRIPTS_DIR="${LithoKraft_SOURCE_DIR}/scripts")
endif()
litholattice_module_options(lithokraft)
# Ship scripts/ next to the exe (used by shipping builds and --scripts exe).
# Third-party license texts ship next to the exe.
add_custom_command(TARGET lithokraft POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LithoLattice_SOURCE_DIR}/THIRD_PARTY_LICENSES.md $<TARGET_FILE_DIR:lithokraft>/THIRD_PARTY_LICENSES.md)
add_custom_command(TARGET lithokraft POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${LithoKraft_SOURCE_DIR}/mod.json $<TARGET_FILE_DIR:lithokraft>/mod.json)
add_custom_command(TARGET lithokraft POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${LithoKraft_SOURCE_DIR}/scripts $<TARGET_FILE_DIR:lithokraft>/scripts)
# ctest -C <config>: the full headless self-test suite (engine modules + game), exit code = failures.
add_test(NAME lithokraft_selftest COMMAND lithokraft --selftest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# Screenshot self-test: headless (no window / GL context), its own ctest entry.
add_executable(lithokraft_screenshot_selftest screenshot_selftest_main.cpp)
target_link_libraries(lithokraft_screenshot_selftest PRIVATE lithokraft_game)
set_target_properties(lithokraft_screenshot_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
litholattice_module_options(lithokraft_screenshot_selftest)
add_test(NAME lithokraft_screenshot_selftest COMMAND lithokraft_screenshot_selftest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# Scout self-test (locatebiome/genmap/structures): headless, its own ctest entry.
add_executable(lithokraft_scout_selftest scout_selftest_main.cpp)
target_link_libraries(lithokraft_scout_selftest PRIVATE lithokraft_game)
set_target_properties(lithokraft_scout_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
litholattice_module_options(lithokraft_scout_selftest)
add_test(NAME lithokraft_scout_selftest COMMAND lithokraft_scout_selftest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# ChipOR viewport self-test (workstream 4): headless, its own ctest entry.
add_executable(lithokraft_chipor_selftest chipor_selftest_main.cpp)
target_link_libraries(lithokraft_chipor_selftest PRIVATE lithokraft_game)
set_target_properties(lithokraft_chipor_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
litholattice_module_options(lithokraft_chipor_selftest)
add_test(NAME lithokraft_chipor_selftest COMMAND lithokraft_chipor_selftest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# Fragment state self-test (workstream 3): headless, its own ctest entry.
add_executable(lithokraft_fragment_selftest fragment_selftest_main.cpp)
target_link_libraries(lithokraft_fragment_selftest PRIVATE lithokraft_game)
set_target_properties(lithokraft_fragment_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
litholattice_module_options(lithokraft_fragment_selftest)
add_test(NAME lithokraft_fragment_selftest COMMAND lithokraft_fragment_selftest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# Fragment markdown renderer self-test (workstream 2): headless, its own ctest entry.
add_executable(lithokraft_markdown_selftest fragment_markdown_selftest_main.cpp)
target_link_libraries(lithokraft_markdown_selftest PRIVATE lithokraft_game)
set_target_properties(lithokraft_markdown_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
litholattice_module_options(lithokraft_markdown_selftest)
add_test(NAME lithokraft_markdown_selftest COMMAND lithokraft_markdown_selftest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# Silica self-test (workstream 5): headless, its own ctest entry.
add_executable(lithokraft_silica_selftest silica_selftest_main.cpp)
target_link_libraries(lithokraft_silica_selftest PRIVATE lithokraft_game)
set_target_properties(lithokraft_silica_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
litholattice_module_options(lithokraft_silica_selftest)
add_test(NAME lithokraft_silica_selftest COMMAND lithokraft_silica_selftest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# Items self-test (2026-10-12): headless item registry / models / fresh-spawn
# inventory acceptance tests, its own ctest entry.
add_executable(lithokraft_items_selftest items_selftest_main.cpp)
target_link_libraries(lithokraft_items_selftest PRIVATE lithokraft_game)
target_compile_definitions(lithokraft_items_selftest PRIVATE LITHOKRAFT_MODELS_DIR="${LithoKraft_SOURCE_DIR}/assets/models")
set_target_properties(lithokraft_items_selftest PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
litholattice_module_options(lithokraft_items_selftest)
add_test(NAME lithokraft_items_selftest COMMAND lithokraft_items_selftest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
# Biome capture: headless 3D screenshots of the 5 Wafer biomes (not a ctest entry;
# run manually: ./biome_capture <outdir>).
add_executable(biome_capture biome_capture.cpp)
target_link_libraries(biome_capture PRIVATE lithokraft_game)
target_compile_definitions(biome_capture PRIVATE LITHOKRAFT_SOURCE_DIR="${LithoKraft_SOURCE_DIR}"
                          LITHOKRAFT_ENGINE_ASSETS_DIR="${LithoLattice_SOURCE_DIR}/assets")
set_target_properties(biome_capture PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
litholattice_module_options(biome_capture)
# Interchange capture: headless screenshots of a generated Wafer
# interchange + its population manifest (not a ctest entry;
# run manually: ./interchange_capture <outdir>).
add_executable(interchange_capture interchange_capture.cpp)
target_link_libraries(interchange_capture PRIVATE lithokraft_game)
target_compile_definitions(interchange_capture PRIVATE LITHOKRAFT_SOURCE_DIR="${LithoKraft_SOURCE_DIR}"
                          LITHOKRAFT_ENGINE_ASSETS_DIR="${LithoLattice_SOURCE_DIR}/assets")
set_target_properties(interchange_capture PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
litholattice_module_options(interchange_capture)
# Texture generator: builds block textures from assets/texture_recipes/*.ltxr
# into a pack (not a ctest entry; run manually per pack size, e.g.
# ./texture_gen --recipes ../game/LithoKraft/assets/texture_recipes --pack ../game/LithoKraft/assets/packs/pack_64 --size 64).
add_executable(texture_gen texture_gen.cpp)
target_link_libraries(texture_gen PRIVATE lithokraft_game litholattice_texture)
set_target_properties(texture_gen PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
litholattice_module_options(texture_gen)
