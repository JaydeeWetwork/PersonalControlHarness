# camera: engine-level virtual camera system (attachable cameras, orbit
# spring arm, trauma shake, manager blending). Presentation only — depends
# on core (math) and console (the registerCameraCvars seam; the game owns
# the registry instance).
add_library(litholattice_camera STATIC camera.cpp camera_manager.cpp camera_cvars.cpp camera_selftest.cpp)
target_include_directories(litholattice_camera PUBLIC include)
target_link_libraries(litholattice_camera PUBLIC litholattice_core litholattice_console)
litholattice_module_options(litholattice_camera)

add_executable(litholattice_camera_selftest camera_selftest_main.cpp)
target_link_libraries(litholattice_camera_selftest PRIVATE litholattice_camera)
litholattice_module_options(litholattice_camera_selftest)
add_test(NAME engine_camera COMMAND litholattice_camera_selftest)
