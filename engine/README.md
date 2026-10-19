# LithoLattice engine + LithoKraft

**LithoLattice** is a C++20 voxel engine (OpenGL 3.3 and Vulkan renderers, a simulation tick thread, procedural
generation, a retained-mode UI, sandboxed AngelScript, resource packs, a retro synth). **LithoKraft** is the voxel
CPU-building game built on it, in `LithoKraft/` (gameplay, controls, demo circuits: `LithoKraft/README.md`).

```
D:\LithoLattice\               repo root = the engine
  src/<module>/                 engine modules (namespace litholattice, targets litholattice_<module>)
  assets/                       engine base pack: pack.json + shaders/ (GLSL 330, vk/ = GLSL 450 -> SPIR-V)
  cmake/  tools/  docs/         build helpers, build.ps1 + pack converter, engine docs
  LithoKraft/                   the game (namespace lithokraft)
    src/game  src/app           game module (lithokraft_game) and the exe (LithoKraft.exe)
    assets/                     game pack: ui/theme.json, worldgen/*.json, packs/pack_64 + pack_32 (art)
    scripts/  docs/  brief.md   AngelScript, screenshot, design brief
    logs/  config/              live log and input bindings (gitignored)
  build/Release/LithoKraft.exe  Windows build output (build/LithoKraft on Linux)
```

Dependencies only point game -> engine: nothing under `src/` includes a `lithokraft/` header or links a
`lithokraft_*` target. Engine architecture: `docs/ARCHITECTURE.md`; subsystems: `docs/PROCGEN.md`, `docs/UI.md`,
`docs/RESOURCE_PACKS.md`.

Dependencies: CMake ≥ 3.16, a C++20 compiler, Git (CMake downloads **GLFW 3.4** from the official
github.com/glfw/glfw repo) and **AngelScript 2.38.0** from the official angelcode.com SDK zip (SHA256-pinned), and **stb_image v2.30** from the
official github.com/nothings/stb repo at a pinned commit, and **miniaudio 0.11.25** from the official
github.com/mackron/miniaudio release tag (both SHA256-checked). The OpenGL loader is a tiny vendored file (`src/render/gl33/gl_loader.*`).
For the optional Vulkan renderer CMake also fetches **Vulkan-Headers** (vulkan-sdk-1.4.363.0) and **glslang 16.6.0**
(the shader compiler, built once as a host tool) from the official KhronosGroup release archives, SHA256-pinned. No
Vulkan SDK is needed: the engine opens the Vulkan loader that ships with the GPU driver at run time.

## Windows (laptop) build

1. Install **Visual Studio 2022 Build Tools** with the "Desktop development with C++" workload (includes MSVC
   and CMake), plus Git:
   ```powershell
   winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
   winget install --id Git.Git -e
   ```
2. Open **"x64 Native Tools Command Prompt for VS 2022"** (Start menu), then:
   ```bat
   cd D:\LithoLattice
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   build\Release\LithoKraft.exe
   ```
   (Alternative with Ninja: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` then
   `cmake --build build` and run `build\LithoKraft.exe`.)
3. On a laptop with an RTX GPU, make sure it runs on the NVIDIA GPU: Windows Settings → System → Display →
   Graphics → add `LithoKraft.exe` → High performance.

## Linux build
```bash
sudo apt install build-essential cmake git libgl1-mesa-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxkbcommon-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/LithoKraft   # or ./build/LithoKraft --selftest
```

Quick build on TheWorkstation (VS 2022 Build Tools already installed): run `tools\build.ps1` from PowerShell at the
repo root (`-Clean` for a clean build, `-Ship` for a shipping build that loads scripts from next to the exe). It logs to
`%USERPROFILE%\agent-tools\logs\lithokraft-build-<date>.log`.


## Source map (engine)
The engine is an SDK (generic systems, virtual interfaces, registries; no game content). LithoKraft's content on it
(blocks, signal sim and components, appearance, SFX) lives in `LithoKraft/src/content`; see `docs/ARCHITECTURE.md`.

- `src/core` — JSON reader / writer, SPSC queue, logging (console + live log file), time, math, threading (exception-safe job pool + job batches, triple buffer, hand-off queue), high-resolution timer scope
- `src/procgen` — noise (Perlin, OpenSimplex2 F/S, value, cellular, white), fractals, domain warp, seeding, JSON node graph, generator pipeline + stages, procgen selftest (`docs/PROCGEN.md`)
- `src/ui` — retained-mode UI: widget tree, layout, JSON theme, focus / navigation, controls (text editor, menus, dialogs, lists, ...), JSON layouts, ui selftest (`docs/UI.md`)
- `src/assets` — resource packs + pack stack, images (stb_image), texture registry, palettes, animations, block models, shader sources, assets selftest
- `src/audio` — retro synth (voices, ADSR, SID3/DMG presets), note helpers, lock-free command queue; `miniaudio/` = output backend
- `src/input` — input events, device state, game-declared action sets + action map + bindings file, game-declared contexts, deadzones / curves, replay, input SDK selftest
- `src/platform` — window, GL context or Vulkan surface, clipboard, GLFW input backend (only place that includes GLFW)
- `src/world` — block registry (an instance the game fills; the engine knows only `kAir`), chunks, chunk map, column generator (procgen → chunks), dimensions + registry, mesh snapshots, raycast
- `src/sim` — tick scheduler thread (`SimThread`, `SimConfig`), the `IWorldSimulation` interface a game implements, clock domains (stub)
- `src/script` — sandboxed AngelScript host (`log(string)`, hooks, staged hot reload)
- `src/render` — `IRenderer` / `ITextureArray` interfaces, BlockMaterials, voxel mesher + `IBlockAppearance`, overlay geometry (HUD, UI draws, outline), `HudUiRenderer`, `gl33/` backend (only place that includes GL)
- `src/render_vk` — Vulkan backend (run-time loader, device, swapchain, frames in flight, vertex arena, texture array, pipelines)
- `tools/convert_pack_manifest.py` — art `manifest.json` → `resources.json`; `tools/build.ps1` — Windows Release x64 build
