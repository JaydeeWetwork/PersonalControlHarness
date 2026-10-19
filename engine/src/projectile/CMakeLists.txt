# projectile: engine-side projectile system (P2c 2026-10-13, review
# ENT-1 — previously an INTERFACE-only library whose manager was declared
# and never defined). Real STATIC library now: ProjectileManager with
# swept per-tick movement, hit/despawn semantics, and chunk-ahead
# requests through an injectable seam (no game/streamer dependency).
add_library(litholattice_projectile STATIC projectile.cpp projectile_selftest.cpp)
target_include_directories(litholattice_projectile PUBLIC include)
litholattice_module_options(litholattice_projectile)

# enable_testing() here for the same reason as the entity module: this
# subdirectory is processed before the engine's top-level
# enable_testing(), which would otherwise drop the add_test below.
enable_testing()
add_executable(litholattice_projectile_selftest projectile_selftest_main.cpp)
target_link_libraries(litholattice_projectile_selftest PRIVATE litholattice_projectile)
litholattice_module_options(litholattice_projectile_selftest)
add_test(NAME engine_projectile COMMAND litholattice_projectile_selftest)
