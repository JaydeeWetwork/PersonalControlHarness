# CMake generated Testfile for 
# Source directory: R:/repos/PersonalControlHarness/game/LithoKraft/src/content/vegetation
# Build directory: R:/repos/PersonalControlHarness/build/game/src/content/vegetation
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(vegetation_selftest "R:/repos/PersonalControlHarness/build/Debug/vegetation_selftest.exe")
  set_tests_properties(vegetation_selftest PROPERTIES  WORKING_DIRECTORY "R:/repos/PersonalControlHarness/build" _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/game/LithoKraft/src/content/vegetation/CMakeLists.txt;33;add_test;R:/repos/PersonalControlHarness/game/LithoKraft/src/content/vegetation/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(vegetation_selftest "R:/repos/PersonalControlHarness/build/Release/vegetation_selftest.exe")
  set_tests_properties(vegetation_selftest PROPERTIES  WORKING_DIRECTORY "R:/repos/PersonalControlHarness/build" _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/game/LithoKraft/src/content/vegetation/CMakeLists.txt;33;add_test;R:/repos/PersonalControlHarness/game/LithoKraft/src/content/vegetation/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(vegetation_selftest "R:/repos/PersonalControlHarness/build/MinSizeRel/vegetation_selftest.exe")
  set_tests_properties(vegetation_selftest PROPERTIES  WORKING_DIRECTORY "R:/repos/PersonalControlHarness/build" _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/game/LithoKraft/src/content/vegetation/CMakeLists.txt;33;add_test;R:/repos/PersonalControlHarness/game/LithoKraft/src/content/vegetation/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(vegetation_selftest "R:/repos/PersonalControlHarness/build/RelWithDebInfo/vegetation_selftest.exe")
  set_tests_properties(vegetation_selftest PROPERTIES  WORKING_DIRECTORY "R:/repos/PersonalControlHarness/build" _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/game/LithoKraft/src/content/vegetation/CMakeLists.txt;33;add_test;R:/repos/PersonalControlHarness/game/LithoKraft/src/content/vegetation/CMakeLists.txt;0;")
else()
  add_test(vegetation_selftest NOT_AVAILABLE)
endif()
