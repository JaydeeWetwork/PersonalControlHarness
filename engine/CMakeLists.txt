cmake_minimum_required(VERSION 3.16)
# LithoLattice engine (src/, assets/) and the LithoKraft game built on it (LithoKraft/).
project(LithoLattice VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Static MSVC runtime for everything (GLFW too) so LithoKraft.exe needs no extra DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)  # extracted files get extraction-time timestamps
endif()
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
if(UNIX AND NOT APPLE)
  # X11 only keeps the Linux dependency list short; Wayland can be enabled later.
  set(GLFW_BUILD_WAYLAND OFF CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.4
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw)

# AngelScript 2.38.0, official SDK zip from angelcode.com, pinned by SHA256.
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)   # honour the static MSVC runtime in the SDK's older CMake project
set(AS_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(angelscript
  URL https://www.angelcode.com/angelscript/sdk/files/angelscript_2.38.0.zip
  URL_HASH SHA256=b33b5dbcda10317ef67d628353d83246984ce6fcac102d4dc2aed121eba52e6f
  SOURCE_SUBDIR angelscript/projects/cmake)  # the zip's top folder sdk/ is stripped
FetchContent_MakeAvailable(angelscript)

# stb_image v2.30 (single header), official github.com/nothings/stb, pinned commit + SHA256.
set(LITHOLATTICE_STB_DIR ${CMAKE_BINARY_DIR}/_deps/stb)
if(NOT EXISTS ${LITHOLATTICE_STB_DIR}/stb_image.h)
  file(DOWNLOAD
    https://raw.githubusercontent.com/nothings/stb/2c980bb59875b0d32144a71867fbdebb2f77cd20/stb_image.h
    ${LITHOLATTICE_STB_DIR}/stb_image.h
    EXPECTED_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3
    TLS_VERIFY ON)
endif()

# miniaudio 0.11.25 (single header), official github.com/mackron/miniaudio release tag + SHA256.
set(LITHOLATTICE_MINIAUDIO_DIR ${CMAKE_BINARY_DIR}/_deps/miniaudio)
if(NOT EXISTS ${LITHOLATTICE_MINIAUDIO_DIR}/miniaudio.h)
  file(DOWNLOAD
    https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h
    ${LITHOLATTICE_MINIAUDIO_DIR}/miniaudio.h
    EXPECTED_HASH SHA256=ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89
    TLS_VERIFY ON)
endif()

# Vulkan backend dependencies (official KhronosGroup release archives, pinned by SHA256).
# Headers only: the backend loads the system Vulkan loader (vulkan-1.dll / libvulkan.so.1, shipped
# with the GPU driver) at run time, so nothing Vulkan is linked and the game still starts without it.
FetchContent_Declare(vulkan_headers
  URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.363.0.tar.gz
  URL_HASH SHA256=4a078be12bef21cfebc09d878b77a63cff9d68f899254a0b00d0e37ef73e7f7e)
FetchContent_MakeAvailable(vulkan_headers)

# glslang 16.6.0: the reference GLSL -> SPIR-V compiler, built as a host tool so the Vulkan
# shaders (assets/shaders/vk) compile at build time. No optimizer / HLSL / tests / install.
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(ENABLE_PCH OFF CACHE BOOL "" FORCE)
set(ENABLE_GLSLANG_BINARIES ON CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glslang
  URL https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz
  URL_HASH SHA256=9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2)
# Third-party build tool: its own GCC/Clang warnings are not ours to fix, so they are silenced for
# glslang's directories only (flags restored right after). MSVC builds it warning-free as is
# (adding /w there would itself warn, D9025).
set(LITHOLATTICE_SAVED_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
if(NOT MSVC)
  string(APPEND CMAKE_CXX_FLAGS " -w")
endif()
FetchContent_MakeAvailable(glslang)
set(CMAKE_CXX_FLAGS "${LITHOLATTICE_SAVED_CXX_FLAGS}")

# One static library per engine module; dependencies are one-way and never reach the game:
#   {sim, world, render, platform, script, audio, input, ui} -> core   (render -> world read-only; sim -> world;
#   platform -> input: the GLFW input backend; world -> procgen -> core: noise, graphs, generator pipeline)
#   render -> ui -> core (render implements ui::IUiRenderer); render -> assets -> core
#   audio_device (miniaudio backend) -> audio; render_vk (Vulkan backend) -> render
# The game (LithoKraft/): lithokraft (exe) -> lithokraft_game -> engine modules.
# Optimisation comes from CMake's per-config flags (Release: /O2 or -O3 and NDEBUG; Debug: /Od /RTC1
# or -O0 -g), never forced here: /O2 next to Debug's /RTC1 is MSVC error D8016. GCC/Clang non-Debug
# builds keep -O2 as before. LITHOLATTICE_WERROR (off by default, on in CI) turns warnings into errors.
option(LITHOLATTICE_WERROR "Treat warnings in LithoLattice / LithoKraft code as errors" OFF)
# Verbose thread/job-system debug logging (job lifecycle, steals, lock order, wake/sleep,
# queue depths, affinity). Compile-time gated: zero overhead when off. The self-tests
# build and pass both ways.
option(LITHOLATTICE_THREAD_DEBUG "Verbose compile-time thread/job-system debug logging" OFF)
# GCC / Clang: LITHOLATTICE_SANITIZE=address (ASan + UBSan) or thread (TSan) for test runs.
set(LITHOLATTICE_SANITIZE "" CACHE STRING "Sanitizer for GCC/Clang builds: address | thread | empty")
if(LITHOLATTICE_SANITIZE AND NOT MSVC)
  if(LITHOLATTICE_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  elseif(LITHOLATTICE_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
  else()
    message(FATAL_ERROR "LITHOLATTICE_SANITIZE must be address or thread")
  endif()
endif()
function(litholattice_module_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- $<$<BOOL:${LITHOLATTICE_WERROR}>:/WX>)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra $<$<NOT:$<CONFIG:Debug>>:-O2>
                           $<$<BOOL:${LITHOLATTICE_WERROR}>:-Werror>)
  endif()
endfunction()

add_subdirectory(src/core)
add_subdirectory(src/procgen)
add_subdirectory(src/ui)
add_subdirectory(src/assets)
add_subdirectory(src/audio)
add_subdirectory(src/input)
add_subdirectory(src/platform)
add_subdirectory(src/script)
add_subdirectory(src/mods)
add_subdirectory(src/console)
add_subdirectory(src/crypto)  # Ed25519/SHA-2 for mod signing
add_subdirectory(src/index)
add_subdirectory(src/mcp)
add_subdirectory(src/defrag)
add_subdirectory(src/player)
add_subdirectory(src/net)
add_subdirectory(src/projectile)
add_subdirectory(src/world)
add_subdirectory(src/sim)
add_subdirectory(src/entity)
add_subdirectory(src/ai)
add_subdirectory(src/anim)
add_subdirectory(src/storage)
add_subdirectory(src/subdiv)  # sparse-octree sub-block subdivision (render/detail-level only)
add_subdirectory(src/texture)  # clean-room procedural texture generation (recipes, tileable noise/voronoi)
add_subdirectory(src/render)
add_subdirectory(src/render_vk)
# The game (LithoKraft/) builds on the engine modules above; nothing in src/ depends on it.
enable_testing()  # ctest: engine tests (tests/, one per module) and the game selftest (LithoKraft/src/app)
add_subdirectory(src/camera)  # engine-level virtual camera system; after enable_testing() so its add_test registers
add_subdirectory(src/systerian)  # certificate-signing script codec; after enable_testing() so its add_test registers
add_subdirectory(src/sprite)  # sprite registry/atlas + instances + particle emitters; after enable_testing() so its add_test registers
option(LITHOLATTICE_BUILD_TESTS "Build the engine-only test executable litholattice_tests" ON)
if(LITHOLATTICE_BUILD_TESTS)
  add_subdirectory(tests)
endif()
option(LITHOLATTICE_BUILD_FUZZERS "Build the litholattice_fuzz harnesses (tools/fuzz)" OFF)
if(LITHOLATTICE_BUILD_FUZZERS)
  add_subdirectory(tools/fuzz)
endif()
add_subdirectory(LithoKraft game)  # binary dir "game": the exe LithoKraft sits in the build root
