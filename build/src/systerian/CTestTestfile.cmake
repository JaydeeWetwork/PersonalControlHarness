# CMake generated Testfile for 
# Source directory: R:/repos/PersonalControlHarness/engine/src/systerian
# Build directory: R:/repos/PersonalControlHarness/build/src/systerian
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(engine_systerian "R:/repos/PersonalControlHarness/build/src/systerian/Debug/litholattice_systerian_selftest.exe")
  set_tests_properties(engine_systerian PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/systerian/CMakeLists.txt;18;add_test;R:/repos/PersonalControlHarness/engine/src/systerian/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(engine_systerian "R:/repos/PersonalControlHarness/build/src/systerian/Release/litholattice_systerian_selftest.exe")
  set_tests_properties(engine_systerian PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/systerian/CMakeLists.txt;18;add_test;R:/repos/PersonalControlHarness/engine/src/systerian/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(engine_systerian "R:/repos/PersonalControlHarness/build/src/systerian/MinSizeRel/litholattice_systerian_selftest.exe")
  set_tests_properties(engine_systerian PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/systerian/CMakeLists.txt;18;add_test;R:/repos/PersonalControlHarness/engine/src/systerian/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(engine_systerian "R:/repos/PersonalControlHarness/build/src/systerian/RelWithDebInfo/litholattice_systerian_selftest.exe")
  set_tests_properties(engine_systerian PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/systerian/CMakeLists.txt;18;add_test;R:/repos/PersonalControlHarness/engine/src/systerian/CMakeLists.txt;0;")
else()
  add_test(engine_systerian NOT_AVAILABLE)
endif()
