#!/usr/bin/env python3
"""
LithoLattice + LithoKraft one-command build & test driver.
JD's quick script: checks prerequisites, configures, builds, runs ctest.

Usage:
    python build.py            # full: configure + build + test
    python build.py --build    # configure + build only
    python build.py --test     # run ctest on an existing build
    python build.py --clean    # wipe the build directory first

Needs: CMake >= 3.16, Git, and a C++ compiler
  Windows: Visual Studio 2022 Build Tools ("Desktop development with C++")
  Linux:   gcc/g++ or clang++  (sudo apt install build-essential cmake git)
Dependencies (GLFW, AngelScript, stb, miniaudio, Vulkan-Headers, glslang)
are fetched automatically by CMake — no manual installs.
"""
import os
import platform
import shutil
import subprocess
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(REPO_ROOT, "build")
ENGINE_SRC = os.path.join(REPO_ROOT, "engine")


def run(cmd, cwd=None):
    print(f"$ {' '.join(cmd)}", flush=True)
    r = subprocess.run(cmd, cwd=cwd)
    if r.returncode != 0:
        sys.exit(f"FAILED (exit {r.returncode}): {' '.join(cmd)}")
    return r


def check_tool(name, hint):
    path = shutil.which(name)
    if not path:
        sys.exit(f"Missing '{name}'. {hint}")
    print(f"  [ok] {name}: {path}")


def main():
    args = set(sys.argv[1:])
    is_win = platform.system() == "Windows"

    print("== LithoLattice build driver ==")
    print(f"  repo:  {REPO_ROOT}")
    print(f"  build: {BUILD_DIR}")
    print(f"  os:    {platform.system()} {platform.machine()}")

    print("-- prerequisites --")
    check_tool("cmake", "Install CMake >= 3.16 (https://cmake.org/download/)")
    check_tool("git", "Install Git (https://git-scm.com/downloads)")
    if is_win:
        # MSVC comes via the VS generator; just make sure the generator exists.
        pass
    else:
        if not (shutil.which("g++") or shutil.which("clang++")):
            sys.exit("Missing C++ compiler. Linux: sudo apt install build-essential")

    if "--clean" in args and os.path.isdir(BUILD_DIR):
        print("-- cleaning build dir --")
        shutil.rmtree(BUILD_DIR)

    if "--test" not in args:
        print("-- configure --")
        os.makedirs(BUILD_DIR, exist_ok=True)
        cfg = ["cmake", "-S", ENGINE_SRC, "-B", BUILD_DIR]
        if is_win:
            cfg += ["-G", "Visual Studio 17 2022", "-A", "x64"]
        cfg += ["-DCMAKE_BUILD_TYPE=Release"]
        run(cfg)

        print("-- build (Release) --")
        build_cmd = ["cmake", "--build", BUILD_DIR, "--config", "Release", "--parallel"]
        # '--parallel' without a number uses a sensible default on CMake >= 3.12
        run(build_cmd)

    if "--build" not in args:
        print("-- ctest --")
        ctest = ["ctest", "--output-on-failure"]
        if is_win:
            ctest += ["-C", "Release"]
        ctest += ["--parallel", str(os.cpu_count() or 4)]
        run(ctest, cwd=BUILD_DIR)

    print("== DONE: build clean, tests run ==")


if __name__ == "__main__":
    main()
