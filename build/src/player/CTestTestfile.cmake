# CMake generated Testfile for 
# Source directory: R:/repos/PersonalControlHarness/engine/src/player
# Build directory: R:/repos/PersonalControlHarness/build/src/player
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(engine_player "R:/repos/PersonalControlHarness/build/src/player/Debug/litholattice_player_selftest.exe")
  set_tests_properties(engine_player PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/player/CMakeLists.txt;25;add_test;R:/repos/PersonalControlHarness/engine/src/player/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(engine_player "R:/repos/PersonalControlHarness/build/src/player/Release/litholattice_player_selftest.exe")
  set_tests_properties(engine_player PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/player/CMakeLists.txt;25;add_test;R:/repos/PersonalControlHarness/engine/src/player/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(engine_player "R:/repos/PersonalControlHarness/build/src/player/MinSizeRel/litholattice_player_selftest.exe")
  set_tests_properties(engine_player PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/player/CMakeLists.txt;25;add_test;R:/repos/PersonalControlHarness/engine/src/player/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(engine_player "R:/repos/PersonalControlHarness/build/src/player/RelWithDebInfo/litholattice_player_selftest.exe")
  set_tests_properties(engine_player PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/player/CMakeLists.txt;25;add_test;R:/repos/PersonalControlHarness/engine/src/player/CMakeLists.txt;0;")
else()
  add_test(engine_player NOT_AVAILABLE)
endif()
