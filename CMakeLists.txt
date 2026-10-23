# Top-level build for the public PersonalControlHarness repo.
# Drives the LithoLattice engine + LithoKraft game (engine/) and the PCH
# test beds (testbeds/) from one CMake project so CLI, VS Code (CMake Tools)
# and Visual Studio (Open Folder / generated .sln) all consume the same tree.
cmake_minimum_required(VERSION 3.25)  # matches CMakePresets.json (presets v6)
project(PersonalControlHarness VERSION 0.0.0.1 LANGUAGES NONE)

option(PCH_BUILD_ENGINE "Build the LithoLattice engine and LithoKraft game (engine/)" ON)
option(PCH_BUILD_TESTBEDS "Register the PCH test beds (testbeds/) with CTest" ON)

include(CTest)  # enable_testing() + BUILD_TESTING

if(PCH_BUILD_ENGINE)
  enable_language(C)
  enable_language(CXX)
  add_subdirectory(engine)
endif()

if(PCH_BUILD_TESTBEDS AND BUILD_TESTING)
  add_subdirectory(testbeds)
endif()
