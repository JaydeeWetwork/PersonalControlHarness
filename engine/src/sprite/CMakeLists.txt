# Sprite system: sprite-type registry + runtime atlas (PNG / SVG / raw
# Image sources), world sprite instances, seeded particle emitters, and
# billboard quad emission — the visual foundation of the particle system.
# Depends on core (math) and assets (Image decode) only; producers
# (procgen, systerian, mods) compose at their layer via the register*
# calls, never the other way around.
add_library(litholattice_sprite STATIC
  sprite_registry.cpp
  sprite_system.cpp)
target_include_directories(litholattice_sprite PUBLIC include)
# Image and Vec3 appear in the public headers, so both are PUBLIC.
target_link_libraries(litholattice_sprite PUBLIC litholattice_core litholattice_assets)
litholattice_module_options(litholattice_sprite)

add_executable(litholattice_sprite_selftest sprite_selftest.cpp)
# Deliberately no litholattice_render link: the shade-mapping check uses
# only renderer.h's enum constants and the inline shadeModeFor, both
# header-only — the selftest stays buildable independent of the render lib.
# renderer.h transitively needs the world headers (pbr_lights.h), so both
# sibling include dirs are listed for that one check.
target_link_libraries(litholattice_sprite_selftest PRIVATE litholattice_sprite)
target_include_directories(litholattice_sprite_selftest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../render/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../world/include)
litholattice_module_options(litholattice_sprite_selftest)
add_test(NAME engine_sprite COMMAND litholattice_sprite_selftest)
