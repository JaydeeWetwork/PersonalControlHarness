# index: the engine text indexer (inverted index over text corpora: Fragment
# journal pages, unlock log, recipes, chip designs — any DocId-keyed corpus).
# Header-only-ish dependency footprint: STL only, no other engine modules.
add_library(litholattice_index STATIC text_indexer.cpp index_selftest.cpp)
target_include_directories(litholattice_index PUBLIC include)
litholattice_module_options(litholattice_index)
