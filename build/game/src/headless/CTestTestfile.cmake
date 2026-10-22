# CMake generated Testfile for 
# Source directory: R:/repos/PersonalControlHarness/game/LithoKraft/src/headless
# Build directory: R:/repos/PersonalControlHarness/build/game/src/headless
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(lithokraft_headless_harness "R:/repos/PersonalControlHarness/build/Debug/lithokraft_headless.exe")
  set_tests_properties(lithokraft_headless_harness PROPERTIES  WORKING_DIRECTORY "R:/repos/PersonalControlHarness/build" _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/game/LithoKraft/src/headless/CMakeLists.txt;39;add_test;R:/repos/PersonalControlHarness/game/LithoKraft/src/headless/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(lithokraft_headless_harness "R:/repos/PersonalControlHarness/build/Release/lithokraft_headless.exe")
  set_tests_properties(lithokraft_headless_harness PROPERTIES  WORKING_DIRECTORY "R:/repos/PersonalControlHarness/build" _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/game/LithoKraft/src/headless/CMakeLists.txt;39;add_test;R:/repos/PersonalControlHarness/game/LithoKraft/src/headless/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(lithokraft_headless_harness "R:/repos/PersonalControlHarness/build/MinSizeRel/lithokraft_headless.exe")
  set_tests_properties(lithokraft_headless_harness PROPERTIES  WORKING_DIRECTORY "R:/repos/PersonalControlHarness/build" _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/game/LithoKraft/src/headless/CMakeLists.txt;39;add_test;R:/repos/PersonalControlHarness/game/LithoKraft/src/headless/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(lithokraft_headless_harness "R:/repos/PersonalControlHarness/build/RelWithDebInfo/lithokraft_headless.exe")
  set_tests_properties(lithokraft_headless_harness PROPERTIES  WORKING_DIRECTORY "R:/repos/PersonalControlHarness/build" _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/game/LithoKraft/src/headless/CMakeLists.txt;39;add_test;R:/repos/PersonalControlHarness/game/LithoKraft/src/headless/CMakeLists.txt;0;")
else()
  add_test(lithokraft_headless_harness NOT_AVAILABLE)
endif()
