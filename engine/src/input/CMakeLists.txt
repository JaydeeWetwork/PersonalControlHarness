# Input: events, action map, contexts, analog conditioning, bindings file (depends on core only).
# OS backends implement IInputBackend elsewhere (GLFW: platform module).
add_library(litholattice_input STATIC input_types.cpp device_state.cpp analog.cpp binding.cpp action_map.cpp
            input_system.cpp input_selftest.cpp input_thread.cpp mod_input.cpp)
target_include_directories(litholattice_input PUBLIC include)
target_link_libraries(litholattice_input PUBLIC litholattice_core)
litholattice_module_options(litholattice_input)
