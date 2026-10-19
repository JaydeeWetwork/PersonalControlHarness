# ai: mob framework core (navigation, movement modes, senses, utility AI + behaviour trees,
# boss-slot hooks, budgeted / LOD'd tick). Engine-generic: games supply predicates and content.
add_library(litholattice_ai STATIC nav.cpp hpa.cpp senses.cpp brain.cpp ai_world.cpp)
target_include_directories(litholattice_ai PUBLIC include)
target_link_libraries(litholattice_ai PUBLIC litholattice_world litholattice_core)
litholattice_module_options(litholattice_ai)
