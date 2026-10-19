# entity: entity storage, ticking, spatial queries, save/load hooks.
add_library(litholattice_entity STATIC entity.cpp physics.cpp locomotion.cpp)
target_include_directories(litholattice_entity PUBLIC include)
target_link_libraries(litholattice_entity PUBLIC litholattice_core)
litholattice_module_options(litholattice_entity)

# Selftest (ctest: engine_locomotion). Standalone executable rather than a
# litholattice_tests module (the entity library is not linked there).
# enable_testing() is called here because the engine's top-level
# enable_testing() runs after this subdirectory is processed, which would
# otherwise drop the add_test (same pattern as the player module).
enable_testing()
add_executable(litholattice_locomotion_selftest locomotion_selftest.cpp)
target_link_libraries(litholattice_locomotion_selftest PRIVATE litholattice_entity)
litholattice_module_options(litholattice_locomotion_selftest)
add_test(NAME engine_locomotion COMMAND litholattice_locomotion_selftest)

# EntityManager/moveCollide selftest (H-ENT-5/6 pins, P3-4).
add_executable(litholattice_entity_selftest entity_selftest.cpp)
target_link_libraries(litholattice_entity_selftest PRIVATE litholattice_entity)
litholattice_module_options(litholattice_entity_selftest)
add_test(NAME engine_entity COMMAND litholattice_entity_selftest)
