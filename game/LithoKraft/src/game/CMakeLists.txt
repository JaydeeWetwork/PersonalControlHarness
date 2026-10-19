# game: the game's generator files (assets/worldgen/*.json) are embedded as built-in fallbacks
# (generated header, rebuilt when a generator file changes).
set(LITHOKRAFT_WORLDGEN_DIR ${LithoKraft_SOURCE_DIR}/assets/worldgen)
file(GLOB LITHOLATTICE_WORLDGEN_FILES CONFIGURE_DEPENDS ${LITHOKRAFT_WORLDGEN_DIR}/*.json)
set(LITHOKRAFT_BUILTIN_WORLDGEN ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_worldgen.h)
add_custom_command(OUTPUT ${LITHOKRAFT_BUILTIN_WORLDGEN}
  COMMAND ${CMAKE_COMMAND} -DDIR=${LITHOKRAFT_WORLDGEN_DIR} "-DPATTERNS=*.json" -DOUT=${LITHOKRAFT_BUILTIN_WORLDGEN}
          -DSTRUCT=BuiltinWorldgenFile -DARRAY=kBuiltinWorldgenFiles -P ${LithoLattice_SOURCE_DIR}/cmake/embed_text_files.cmake
  DEPENDS ${LITHOLATTICE_WORLDGEN_FILES} ${LithoLattice_SOURCE_DIR}/cmake/embed_text_files.cmake
  COMMENT "Embedding built-in world generators")

add_library(lithokraft_game STATIC game.cpp world_save.cpp chunk_streamer.cpp bench_stream.cpp actions.cpp input_selftest.cpp dimensions.cpp wafer_biomes.cpp wafer_pulse.cpp structures.cpp controls_panel.cpp player.cpp resource_manager.cpp sound_director.cpp
            game_ui.cpp game_ui_selftest.cpp script_editor.cpp ui_input_bridge.cpp ui_script_bindings.cpp settings.cpp extensible_facet.cpp
            settings_cvars.cpp game_commands.cpp world_query.cpp silica_biome.cpp
            etching.cpp signing.cpp wiring_selftest.cpp
            screenshot.cpp fragment_gallery.cpp screenshot_selftest.cpp
            fragment_state.cpp fragment_selftest.cpp
            fragment_markdown.cpp fragment_markdown_selftest.cpp
            scout.cpp map_render.cpp scout_selftest.cpp
            mcp_player_control.cpp mcp_senses_adapter.cpp camera_controller.cpp
            ${LITHOKRAFT_BUILTIN_WORLDGEN})
target_include_directories(lithokraft_game PUBLIC include PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(lithokraft_game PUBLIC lithokraft_content litholattice_ai litholattice_sim litholattice_world litholattice_render litholattice_platform litholattice_script litholattice_audio litholattice_input litholattice_ui litholattice_console litholattice_core litholattice_entity litholattice_mcp litholattice_camera litholattice_projectile)
litholattice_module_options(lithokraft_game)

# IPlayer integration (player_adapter.{h,cpp}): engine player-system wiring
# (adapter, vitals/oxygen tick, stat persistence, MCP snapshot source).
target_sources(lithokraft_game PRIVATE player_adapter.cpp)
target_link_libraries(lithokraft_game PUBLIC litholattice_player)

# Locomotion integration (player_locomotion.{h,cpp}): the local player's
# Entity body + engine LocomotionController, stepped per sim tick by Game.
# locomotion_selftest.cpp joins the app selftest.
target_sources(lithokraft_game PRIVATE player_locomotion.cpp locomotion_selftest.cpp)

# Sprite system integration (sprite_host.{h,cpp}): registry/system host,
# starter defs from every source kind (Litho Formula 2D images, Systerian
# glyph, pack icon, SVG text, animated strip), `sprite` console commands,
# per-frame quad conversion for the render sprite pass and the software
# (MCP vision) path. sprite_host_selftest.cpp joins the app selftest.
target_sources(lithokraft_game PRIVATE sprite_host.cpp sprite_host_selftest.cpp)
target_link_libraries(lithokraft_game PUBLIC litholattice_sprite litholattice_systerian litholattice_procgen)

# AiWorld production host (ai_host.{h,cpp}): Game-side ownership of the
# engine AiWorld (brief B6, 2026-10-13) — bind to the live dimension,
# ai.enabled gate, advance() driven from elapsed sim world ticks.
# ai_host_selftest.cpp joins the app selftest. litholattice_ai is already
# linked PUBLIC above.
target_sources(lithokraft_game PRIVATE ai_host.cpp ai_host_selftest.cpp)

# Deceitor mob (deceitor.{h,cpp}): game-owned behaviour for the first
# AiWorld mob content — barter / betrayal / failed-click / pack
# retaliation brains, liked-items + barter tables, director registry.
# deceitor_selftest.cpp joins the app selftest.
target_sources(lithokraft_game PRIVATE deceitor.cpp deceitor_selftest.cpp)

# Interchanges (interchange.{h,cpp}): basic Wafer settlements -- site
# lattice, pad/plaza/building stamping, population manifest data.
# interchange_selftest.cpp joins the app selftest.
target_sources(lithokraft_game PRIVATE interchange.cpp interchange_selftest.cpp)
