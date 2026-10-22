# CMake generated Testfile for 
# Source directory: R:/repos/PersonalControlHarness/engine/src/sprite
# Build directory: R:/repos/PersonalControlHarness/build/src/sprite
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(engine_sprite "R:/repos/PersonalControlHarness/build/src/sprite/Debug/litholattice_sprite_selftest.exe")
  set_tests_properties(engine_sprite PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/sprite/CMakeLists.txt;26;add_test;R:/repos/PersonalControlHarness/engine/src/sprite/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(engine_sprite "R:/repos/PersonalControlHarness/build/src/sprite/Release/litholattice_sprite_selftest.exe")
  set_tests_properties(engine_sprite PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/sprite/CMakeLists.txt;26;add_test;R:/repos/PersonalControlHarness/engine/src/sprite/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(engine_sprite "R:/repos/PersonalControlHarness/build/src/sprite/MinSizeRel/litholattice_sprite_selftest.exe")
  set_tests_properties(engine_sprite PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/sprite/CMakeLists.txt;26;add_test;R:/repos/PersonalControlHarness/engine/src/sprite/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(engine_sprite "R:/repos/PersonalControlHarness/build/src/sprite/RelWithDebInfo/litholattice_sprite_selftest.exe")
  set_tests_properties(engine_sprite PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/sprite/CMakeLists.txt;26;add_test;R:/repos/PersonalControlHarness/engine/src/sprite/CMakeLists.txt;0;")
else()
  add_test(engine_sprite NOT_AVAILABLE)
endif()
