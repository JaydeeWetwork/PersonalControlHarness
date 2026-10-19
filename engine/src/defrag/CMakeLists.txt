# defrag: the engine defrag/restructure service (compact/reorder/reclaim for
# large persistent stores; the Fragment is the first client). Links the text
# indexer (reindex hook) and storage (crash-safe atomic writes).
add_library(litholattice_defrag STATIC defrag.cpp defrag_selftest.cpp)
target_include_directories(litholattice_defrag PUBLIC include)
target_link_libraries(litholattice_defrag PUBLIC litholattice_index litholattice_storage)
litholattice_module_options(litholattice_defrag)
