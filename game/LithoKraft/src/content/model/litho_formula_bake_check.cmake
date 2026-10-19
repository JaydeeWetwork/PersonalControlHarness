# Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
# concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
# from the mathematical definition; no third-party code referenced. Underlying patent
# expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
# ambition of No Man's Sky (Hello Games).

# CLI smoke test for bake_litho_formula (driven by the litho_formula_bake_cli
# ctest): bakes the gallery into ${BAKE_TMP}, then verifies every .lthm with
# litho_formula_gallery_selftest --verify-dir.
#
# Expected -D variables: BAKE_EXE, CHECK_EXE, GALLERY_JSON, BAKE_TMP.

file(REMOVE_RECURSE "${BAKE_TMP}")
file(MAKE_DIRECTORY "${BAKE_TMP}")

execute_process(
  COMMAND "${BAKE_EXE}" "${BAKE_TMP}" "${GALLERY_JSON}"
  RESULT_VARIABLE bake_rc
  OUTPUT_VARIABLE bake_out
  ERROR_VARIABLE bake_err)
if(NOT bake_rc EQUAL 0)
  message(FATAL_ERROR "bake_litho_formula failed (rc=${bake_rc}):\n${bake_out}\n${bake_err}")
endif()
message(STATUS "bake output:\n${bake_out}")

execute_process(
  COMMAND "${CHECK_EXE}" --verify-dir "${BAKE_TMP}"
  RESULT_VARIABLE check_rc
  OUTPUT_VARIABLE check_out
  ERROR_VARIABLE check_err)
if(NOT check_rc EQUAL 0)
  message(FATAL_ERROR "litho_formula_gallery_selftest --verify-dir failed (rc=${check_rc}):\n${check_out}\n${check_err}")
endif()
message(STATUS "verify output:\n${check_out}")

file(REMOVE_RECURSE "${BAKE_TMP}")
