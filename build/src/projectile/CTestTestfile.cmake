# CMake generated Testfile for 
# Source directory: R:/repos/PersonalControlHarness/engine/src/projectile
# Build directory: R:/repos/PersonalControlHarness/build/src/projectile
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(engine_projectile "R:/repos/PersonalControlHarness/build/src/projectile/Debug/litholattice_projectile_selftest.exe")
  set_tests_properties(engine_projectile PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/projectile/CMakeLists.txt;17;add_test;R:/repos/PersonalControlHarness/engine/src/projectile/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(engine_projectile "R:/repos/PersonalControlHarness/build/src/projectile/Release/litholattice_projectile_selftest.exe")
  set_tests_properties(engine_projectile PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/projectile/CMakeLists.txt;17;add_test;R:/repos/PersonalControlHarness/engine/src/projectile/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(engine_projectile "R:/repos/PersonalControlHarness/build/src/projectile/MinSizeRel/litholattice_projectile_selftest.exe")
  set_tests_properties(engine_projectile PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/projectile/CMakeLists.txt;17;add_test;R:/repos/PersonalControlHarness/engine/src/projectile/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(engine_projectile "R:/repos/PersonalControlHarness/build/src/projectile/RelWithDebInfo/litholattice_projectile_selftest.exe")
  set_tests_properties(engine_projectile PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/projectile/CMakeLists.txt;17;add_test;R:/repos/PersonalControlHarness/engine/src/projectile/CMakeLists.txt;0;")
else()
  add_test(engine_projectile NOT_AVAILABLE)
endif()
