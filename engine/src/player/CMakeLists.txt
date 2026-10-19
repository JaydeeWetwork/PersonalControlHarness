# player: engine-level IPlayer system — PlayerState + PlayerManager, the
# vital registry (IVitals), the stat registry (IStat), the ability registry
# (IAbility), standard vitals, and the concrete Player. Header-only until
# 2026-10-12; now a compiled library.
add_library(litholattice_player STATIC
  player_state.cpp
  vital_registry.cpp
  stat_registry.cpp
  ability_registry.cpp
  player.cpp
  standard_stats.cpp
  vitals.cpp)
target_include_directories(litholattice_player PUBLIC include)
litholattice_module_options(litholattice_player)

# Selftest (ctest: engine_player). Standalone executable rather than a
# litholattice_tests module: the player library has no other dependents yet,
# so it can build and test in isolation. enable_testing() is called here
# because the engine's top-level enable_testing() runs after this
# subdirectory is processed, which would otherwise drop the add_test.
enable_testing()
add_executable(litholattice_player_selftest player_selftest.cpp)
target_link_libraries(litholattice_player_selftest PRIVATE litholattice_player)
litholattice_module_options(litholattice_player_selftest)
add_test(NAME engine_player COMMAND litholattice_player_selftest)
