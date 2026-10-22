# CMake generated Testfile for 
# Source directory: R:/repos/PersonalControlHarness/engine/src/camera
# Build directory: R:/repos/PersonalControlHarness/build/src/camera
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(engine_camera "R:/repos/PersonalControlHarness/build/src/camera/Debug/litholattice_camera_selftest.exe")
  set_tests_properties(engine_camera PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/camera/CMakeLists.txt;13;add_test;R:/repos/PersonalControlHarness/engine/src/camera/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(engine_camera "R:/repos/PersonalControlHarness/build/src/camera/Release/litholattice_camera_selftest.exe")
  set_tests_properties(engine_camera PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/camera/CMakeLists.txt;13;add_test;R:/repos/PersonalControlHarness/engine/src/camera/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(engine_camera "R:/repos/PersonalControlHarness/build/src/camera/MinSizeRel/litholattice_camera_selftest.exe")
  set_tests_properties(engine_camera PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/camera/CMakeLists.txt;13;add_test;R:/repos/PersonalControlHarness/engine/src/camera/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(engine_camera "R:/repos/PersonalControlHarness/build/src/camera/RelWithDebInfo/litholattice_camera_selftest.exe")
  set_tests_properties(engine_camera PROPERTIES  _BACKTRACE_TRIPLES "R:/repos/PersonalControlHarness/engine/src/camera/CMakeLists.txt;13;add_test;R:/repos/PersonalControlHarness/engine/src/camera/CMakeLists.txt;0;")
else()
  add_test(engine_camera NOT_AVAILABLE)
endif()
