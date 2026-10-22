# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION ${CMAKE_VERSION}) # this file comes with cmake

# If CMAKE_DISABLE_SOURCE_CHANGES is set to true and the source directory is an
# existing directory in our source tree, calling file(MAKE_DIRECTORY) on it
# would cause a fatal error, even though it would be a no-op.
if(NOT EXISTS "R:/repos/PersonalControlHarness/build/_deps/angelscript-src")
  file(MAKE_DIRECTORY "R:/repos/PersonalControlHarness/build/_deps/angelscript-src")
endif()
file(MAKE_DIRECTORY
  "R:/repos/PersonalControlHarness/build/_deps/angelscript-build"
  "R:/repos/PersonalControlHarness/build/_deps/angelscript-subbuild/angelscript-populate-prefix"
  "R:/repos/PersonalControlHarness/build/_deps/angelscript-subbuild/angelscript-populate-prefix/tmp"
  "R:/repos/PersonalControlHarness/build/_deps/angelscript-subbuild/angelscript-populate-prefix/src/angelscript-populate-stamp"
  "R:/repos/PersonalControlHarness/build/_deps/angelscript-subbuild/angelscript-populate-prefix/src"
  "R:/repos/PersonalControlHarness/build/_deps/angelscript-subbuild/angelscript-populate-prefix/src/angelscript-populate-stamp"
)

set(configSubDirs Debug)
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "R:/repos/PersonalControlHarness/build/_deps/angelscript-subbuild/angelscript-populate-prefix/src/angelscript-populate-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "R:/repos/PersonalControlHarness/build/_deps/angelscript-subbuild/angelscript-populate-prefix/src/angelscript-populate-stamp${cfgdir}") # cfgdir has leading slash
endif()
