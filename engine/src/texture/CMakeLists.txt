# texture: clean-room procedural texture generation (recipe-driven tileable
# value/gradient FBM noise, voronoi, domain warp, field transforms, palette
# mapping, glow/dither post steps, LRU texture cache). Depends on assets
# (Image) only; no third-party code.
add_library(litholattice_texture STATIC texture_recipe.cpp texture_generator.cpp texture_selftest.cpp)
target_include_directories(litholattice_texture PUBLIC include)
target_link_libraries(litholattice_texture PUBLIC litholattice_assets)
litholattice_module_options(litholattice_texture)
