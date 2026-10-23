# Black Box Mystery Solved: standalone C11 prototype (tamper-evident, replayable
# decision log). Not wired into the engine. Builds on Windows (MSVC) and Linux.
# PCH test bed 16 (replay + tamper) is the bbtest binary.
# The checkpoint signer here is a TEST-ONLY HMAC with a public key. It is not a
# real signature and has nothing to do with the Release signing check.
add_library(ll_blackbox STATIC blackbox.c sha256.c)
target_include_directories(ll_blackbox PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(ll_blackbox PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
  # C11 <stdatomic.h> needs this switch on MSVC; _CRT_SECURE_NO_WARNINGS for sprintf/fopen.
  target_compile_options(ll_blackbox PUBLIC /experimental:c11atomics /W3)
  target_compile_definitions(ll_blackbox PUBLIC _CRT_SECURE_NO_WARNINGS)
else()
  find_package(Threads REQUIRED)
  target_compile_options(ll_blackbox PRIVATE -Wall -Wextra)
  target_link_libraries(ll_blackbox PUBLIC Threads::Threads)
endif()

foreach(tool bbtest bbverify bbbench)
  if(tool STREQUAL "bbtest")
    add_executable(${tool} bbtest.c toysim.c)
  else()
    add_executable(${tool} ${tool}.c)
  endif()
  set_target_properties(${tool} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  target_link_libraries(${tool} PRIVATE ll_blackbox)
endforeach()

if(BUILD_TESTING)
  # Own working dir: bbtest writes its .bbx files to the current directory, so
  # no other test shares that folder (see R-TST-4 in docs/BLOCKERS.md).
  set(_bbdir "${CMAKE_CURRENT_BINARY_DIR}/bed16")
  file(MAKE_DIRECTORY "${_bbdir}")
  add_test(NAME pch.blackbox.replay_tamper COMMAND bbtest WORKING_DIRECTORY "${_bbdir}")
  set_tests_properties(pch.blackbox.replay_tamper PROPERTIES TIMEOUT 300 LABELS "pch;testbed;blackbox")
endif()
