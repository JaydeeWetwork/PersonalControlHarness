# Headless game harness: builds the full LithoKraft Game without a window or GPU so the
# game is testable in CI. This is the general game-testing fixture — future gameplay
# tests (inventory, combat, dimensions, lithography) live here, not in one-off runners.
#
# --- blocks.cpp scratch copy (G-L3, corrected 2026-10-13) ---
# Earlier docs here claimed a pre-existing registry desync whose row-count
# guard aborted the shipped game at startup. That guard was removed by the
# resolveId registry refactor; the claim is FALSE today and the old "patch"
# had become a no-op. The harness compiles a VERBATIM scratch copy of
# blocks.cpp (see patch_blocks.cmake) and links it FIRST, so its
# blockRegistry() wins over the archived one — the copy exists for link
# order in these standalone test binaries, not to change any code. The tree
# is untouched.
set(HARNESS_BLOCKS_PATCHED ${CMAKE_CURRENT_BINARY_DIR}/blocks_patched.cpp)
add_custom_command(
  OUTPUT ${HARNESS_BLOCKS_PATCHED}
  COMMAND ${CMAKE_COMMAND} -DSRC=${LithoKraft_SOURCE_DIR}/src/content/blocks.cpp
                           -DDST=${HARNESS_BLOCKS_PATCHED}
                           -P ${CMAKE_CURRENT_SOURCE_DIR}/patch_blocks.cmake
  DEPENDS ${LithoKraft_SOURCE_DIR}/src/content/blocks.cpp
          ${CMAKE_CURRENT_SOURCE_DIR}/patch_blocks.cmake
  COMMENT "Headless harness: copying blocks.cpp verbatim (scratch copy; tree untouched)")

add_executable(lithokraft_headless
  harness_main.cpp
  headless_harness.cpp
  harness_selftest.cpp
  ${HARNESS_BLOCKS_PATCHED})
# lithokraft_game's PUBLIC link interface carries every include dir the copied blocks.cpp
# needs. The copied object is linked directly (not from the archive), so its
# blockRegistry() resolves first and blocks.o is never pulled from lithokraft_content.
target_link_libraries(lithokraft_headless PRIVATE lithokraft_game)
target_include_directories(lithokraft_headless PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
litholattice_module_options(lithokraft_headless)
set_target_properties(lithokraft_headless PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# ctest: the headless game suite (menu state machine + frame loop), exit code = failures.
add_test(NAME lithokraft_headless_harness COMMAND lithokraft_headless
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

# MCP wiring self-test (P2c, F-M9): real Game + real McpServer in-process
# (backends, JSON-RPC round trip, grant flow, runStdio over a scripted
# transport). NOT a ctest entry — run manually:
#   ./lithokraft_mcp_wiring_selftest
# (The live binary smoke is blocked in this environment by the release
# test-key boot guard; see the test's header comment.)
add_executable(lithokraft_mcp_wiring_selftest
  mcp_wiring_selftest_main.cpp
  headless_harness.cpp
  ${HARNESS_BLOCKS_PATCHED})
target_link_libraries(lithokraft_mcp_wiring_selftest PRIVATE lithokraft_game)
target_include_directories(lithokraft_mcp_wiring_selftest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
litholattice_module_options(lithokraft_mcp_wiring_selftest)
set_target_properties(lithokraft_mcp_wiring_selftest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
