# Building and testing

## Start here (beginners)

Everything here uses free tools only: Python, CMake, VS Code, and Visual
Studio Community or Build Tools. No paid services or AI subscriptions.

1. Install Python 3.10+ (python.org) and Git.
2. Clone the repo, then run the simplest example, which needs nothing else:

       cd testbeds
       python tb_shipped_streams_chain.py

   You should see `ALL PASS`. Read `testbeds/examples/01-shipped-streams-chain.md`
   to learn what it checked.
3. Next, try `python tb_receipt_tamper.py` (example 2), then read
   `testbeds/README.md` for the rest.
4. When you want the C++ engine and game too, install Visual Studio
   Community or Build Tools (free) and follow path 1, 2 or 3 below.

One CMake project at the repo root drives everything: the LithoLattice engine
and LithoKraft game (`engine/`, which pulls in `game/LithoKraft`) and the PCH
test beds (`testbeds/`, Python run through CTest). Presets live in
`CMakePresets.json`; build output goes to `out/build/<preset>/`.

Prerequisites (Windows): Visual Studio 2022 or 2026 (or the Build Tools) with
the "Desktop development with C++" workload, which includes CMake and Ninja;
Git; Python 3.10+. CMake 3.25 or newer is required; the `vs2026` preset
(Visual Studio 18 2026 generator, `.slnx` solution) needs CMake 4.2+, which
ships with the VS 2026 Build Tools. CMake fetches GLFW, AngelScript, Vulkan headers, glslang,
stb and miniaudio itself on first configure (needs network). The model-backed
PCH test beds also need `torch` (the DeepSeek-V3 specimen code is vendored; see
`pch/README.md`); without them they report SKIPPED, not failed.

## 1. Command line (CMake + CTest)

From any terminal (the Visual Studio generator finds MSVC on its own):

    cmake --preset vs2026            # or vs2022
    cmake --build --preset vs2026-release
    ctest --preset vs2026-release    # all tests
    ctest --preset pch-testbeds      # PCH test beds only

From a "Developer PowerShell for VS" (Ninja, single-config):

    cmake --preset windows-msvc-release
    cmake --build --preset windows-msvc-release
    ctest --preset windows-msvc-release

PCH test beds only, no C++ compiler needed:

    cmake --preset testbeds-only && ctest --preset testbeds-only

This preset uses CMake's default generator. If CMake reports that it cannot
find a build program (a machine with no compiler toolchain at all), run
`pip install ninja` and add `-G Ninja` to the configure command.

Linux: `cmake --preset linux-release && cmake --build --preset linux-release && ctest --preset linux-release`.
The older one-shot driver `python tools/build.py` still works.

## 2. VS Code

1. Clone with *Git: Clone* (or open the folder), accept the recommended
   extensions (CMake Tools, C/C++).
2. CMake Tools reads `CMakePresets.json`: pick a configure preset in the
   status bar (`windows-msvc-debug` / `-release`, or `vs2026`). CMake Tools
   sets up the MSVC environment for the Ninja presets itself.
3. Build (F7), then run tests from the Testing panel or *CMake: Run Tests*.
   `Terminal > Run Task` also has PCH build/test tasks.

## 3. Visual Studio 2022 / 2026

- **Open Folder:** *File > Open > Folder* on the repo root. VS reads
  `CMakePresets.json`; choose a preset from the toolbar, *Build > Build All*,
  then *Test > Run CTests*.
- **Solution:** `cmake --preset vs2026` writes
  `out/build/vs2026/PersonalControlHarness.slnx` (`vs2022` writes a `.sln`);
  open it, build, and run the `RUN_TESTS` target.

## Test results legend

CTest reports PASS, FAIL or SKIPPED (exit code 77). Skipped tests print why:
a missing prerequisite (torch / specimen), a PCH feature that does not
exist yet, or an OS capability the machine does not grant. See
`testbeds/README.md`.

### Symlink tests on Windows (Developer Mode)

A few engine security checks (mod sandbox escapes, signed mods containing a
symlink) need to create symbolic links. Windows only allows that with
**Developer Mode** on (*Settings > System > For developers > Developer Mode*)
or from an elevated (administrator) shell. Without it those sections print
`[SKIP] ... may not create symlinks`, and `engine_storage` / `engine_assets`
report **Skipped** instead of Passed. Every other check in those tests still
runs, and any real failure still fails the test. Turn on Developer Mode and
re-run `ctest` to exercise the symlink checks too.

## PCH reference-model specimen (optional, for the model-backed test beds)

The PCH harness imports DeepSeek's open reference inference code (MIT
licensed) as a read-only "specimen". It is vendored unmodified in
`third_party/DeepSeek-V3/` (upstream commit
`9b4e9788e4a3a731f7567338ed15d3ec549ce03b`, 2025-08-28; manifest-checked by
`pch.vendored_specimen`), so nothing extra needs cloning. No model weights
are needed: the harness builds a tiny CPU model with seeded fixture weights.
Install CPU torch:

    pip install torch psutil --index-url https://download.pytorch.org/whl/cpu --extra-index-url https://pypi.org/simple

(Checked 2026-10-23: torch 2.14.1+cpu installs on Python 3.14. Do not install the
specimen's `requirements.txt` as-is: it pins `triton`, which is GPU-only.)

(To test another copy, set `PCH_SPECIMEN_DIR` to its `inference/` folder.)

Newer DeepSeek reference code was checked and is **not** compatible with the
CPU harness yet. DeepSeek-V3.2-Exp (GitHub, `87e509a`, 2025-11-18) and
DeepSeek-V4-Flash/Pro (`inference/` on Hugging Face, kernel updated
2026-06-08) call TileLang GPU kernels (`act_quant`, `fp8_index`,
`sparse_attn`, `hc_split_sinkhorn`) on every forward pass, and V4 replaces
the MLA `kv_cache`/`pe_cache` that the harness digests. Stay on the V3 pin.

## Shipped reference streams and platforms

Platform-tagged reference streams (core fix approved by JD 2026-10-23):
the prefill cache digest hashes raw float output, which differs between OS,
CPU arch and torch builds. Stream headers now record `platform` (os, arch,
torch, python) inside the hashed chain, so the tag can't be swapped quietly.
`pch/harness/stream_*.jsonl` stay as the original Linux reference set
(untagged; treated as Linux x86_64, torch 2.14.1+cpu). Windows reference
streams recorded on the dev workstation are in
`pch/harness/reference_streams/windows-amd64-torch2.14.1-cpu/`.
`verify_streams.py` picks the set matching this machine (OS, arch, torch)
and reports SKIPPED, not FAILED, when none matches. `disc_test_llm.py`
now writes fresh streams to a temp folder (or `PCH_OUT_DIR`) and never
overwrites the committed ones. To add a reference set for a new platform:
`PCH_OUT_DIR=reference_streams/<name> python disc_test_llm.py` from `pch/harness/`.
