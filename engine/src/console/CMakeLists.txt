# console: the lithoscript console language (commands, cvars, aliases, exec) plus the
# dropdown debug console widget. Depends on core (log tap), ui (widget framework),
# input (key-name validation for the bind seam), script (AngelScript exposure)
# and defrag (the debug.defrag command + per-tick pump).
add_library(litholattice_console STATIC concmd.cpp completer.cpp delimiter_pairing.cpp debug_console.cpp script_console.cpp console_selftest.cpp)
target_include_directories(litholattice_console PUBLIC include)
target_link_libraries(litholattice_console PUBLIC litholattice_core litholattice_ui litholattice_input litholattice_script litholattice_defrag)
litholattice_module_options(litholattice_console)
