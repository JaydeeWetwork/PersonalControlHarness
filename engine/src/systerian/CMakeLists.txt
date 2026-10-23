# systerian: engine-level Systerian script standard — the certificate-
# signing glyph codec (value/char/text -> stroke geometry -> SVG -> RGBA
# raster via the assets module's SVG rasterizer). Pure and deterministic;
# depends on assets only for Image::decodeSvg.
add_library(litholattice_systerian STATIC systerian.cpp)
target_include_directories(litholattice_systerian PUBLIC include)
target_link_libraries(litholattice_systerian PUBLIC litholattice_assets)
litholattice_module_options(litholattice_systerian)

# Selftest (ctest: engine_systerian). Verifies the codec's geometry against
# the reference generator masters bundled in testdata/reference (see the
# README there); the SYSTERIAN_REFERENCE_DIR env var overrides the path.
add_executable(litholattice_systerian_selftest systerian_selftest.cpp)
target_link_libraries(litholattice_systerian_selftest PRIVATE litholattice_systerian)
target_compile_definitions(litholattice_systerian_selftest PRIVATE
  SYSTERIAN_REFERENCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/testdata/reference")
litholattice_module_options(litholattice_systerian_selftest)
add_test(NAME engine_systerian COMMAND litholattice_systerian_selftest)
