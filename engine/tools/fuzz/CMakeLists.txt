# litholattice_fuzz: fuzz harnesses (see fuzz_targets.cpp). Off by default; build with
# -DLITHOLATTICE_BUILD_FUZZERS=ON, ideally together with -DLITHOLATTICE_SANITIZE=address.
add_executable(litholattice_fuzz fuzz_targets.cpp)
target_link_libraries(litholattice_fuzz PRIVATE litholattice_storage litholattice_procgen litholattice_assets litholattice_world litholattice_core)
litholattice_module_options(litholattice_fuzz)
