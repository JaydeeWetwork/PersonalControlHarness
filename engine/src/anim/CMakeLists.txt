# anim: procedural rigging (rig.h shared interface).
# Worker 1: skeleton extraction + skinning (riggen.*).
# Worker 2: IK solvers (iksolve.*).
# Depends only on core (Vec3/Mat4).
add_library(litholattice_anim STATIC riggen.cpp riggen_selftest.cpp iksolve.cpp iksolve_selftest.cpp)
target_include_directories(litholattice_anim PUBLIC include)
target_link_libraries(litholattice_anim PUBLIC litholattice_core)
litholattice_module_options(litholattice_anim)
