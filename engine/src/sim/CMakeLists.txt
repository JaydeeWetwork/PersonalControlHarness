# sim: the tick scheduler (SimThread) and the IWorldSimulation interface the game implements.
add_library(litholattice_sim STATIC sim_thread.cpp random_ticks.cpp trace_jit_basic.cpp det_sched.cpp timing_handshake.cpp control_stream.cpp demo_record.cpp)
target_include_directories(litholattice_sim PUBLIC include)
target_link_libraries(litholattice_sim PUBLIC litholattice_world litholattice_core)
litholattice_module_options(litholattice_sim)
