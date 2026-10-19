add_library(litholattice_mods STATIC mod_loader.cpp mod_permissions.cpp extension_registry.cpp extension_loader.cpp extension_budget.cpp extension_runtime.cpp mod_save_data.cpp mods_selftest.cpp)
target_include_directories(litholattice_mods PUBLIC include)
# mod_save_data.h includes storage/file_contract.h: link storage PUBLIC so the
# include dirs and CRC32 framing propagate to dependents.
target_link_libraries(litholattice_mods PUBLIC litholattice_storage litholattice_core)
litholattice_module_options(litholattice_mods)
