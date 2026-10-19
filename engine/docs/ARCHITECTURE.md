# LithoLattice — Engine Architecture Sketch (v0.1, Oct 9 2026)

LithoLattice is the engine (repo root, `src/`, namespace `litholattice`); **LithoKraft** is the game built on it
(`LithoKraft/`, namespace `lithokraft`). Sections marked "game" describe LithoKraft's use of the engine.

**SDK model (Half-Life style):** the engine is an SDK of generic systems, public virtual interfaces and registries;
a game is a "mod" that fills them in. The engine contains no game content: the only block it knows is `kAir`, it
defines no input actions, sound effects or simulation rules. LithoKraft is the first mod:

| Engine interface / registry (`litholattice::`) | LithoKraft implementation (`lithokraft::`) |
|---|---|
| `BlockRegistry` instance (world/block.h): rows `BlockDef{name, flags, shape, tex, behavior, role, channel, tier}`; engine flags in bits 0..15, game flags from `kFirstGameFlag` | `blockRegistry()` (content/blocks.*): block ids, `kPowerable` / `kCutsWire`, `SimRole` stored in `BlockDef::role` |
| `IWorldSimulation` (sim/world_simulation.h) driven by `SimThread` + `SimConfig` (tick rate, radius, clock domains, below-world fill) | `SignalWorldSimulation` (content/signal_world.*) wrapping `SignalSim` and the component behaviours |
| `IBlockAppearance` + `MeshContext` + `MeshWriter` (render/block_appearance.h, mesher.h) | `LithoKraftAppearance` (content/appearance.*): wire strips, lamps, pips, cells, shader bindings |
| `ActionSet`, `ActionMap`, `ContextRules`, `InputSystem` (input) | `Action` enum, `actionSet()`, `defaultActionMap()`, `InputContext` + `inputContexts()` (game/actions.*) |
| Effects-bus notes (`AudioCommand::noteOn`), `notes.h` (audio) | `Sfx` recipes + `sfxCommand()` (content/sfx.*), `noteForStrength()` (content/music.h) |
| `ColumnGenerator` (materials resolved against the game's registry), `Dimension`, `DimensionRegistry` | `createDimensions()`, generator JSON, demo structures (game/dimensions.*, structures.*) |
| `ScriptEngine` sandbox, `ui::` widgets, `IRenderer` backends | game API bindings, `GameUi`, F10 editor |

C++20 core, performance-first. Baseline target: Jaydee's laptop (RTX 3050 Ti 4 GB, Windows).
If it is smooth there, it scales up. The prototype in `src/` implements the parts marked **[proto]**.

## 0. Module map **[proto]**
Each engine module is its own folder and CMake static library (`litholattice_<module>`) with a small public header set
under `src/<module>/include/litholattice/<module>/`; everything else in the folder is private. The game's modules live
under `LithoKraft/src/` (`lithokraft_content` = the mod's content on the SDK interfaces, headers in
`include/lithokraft/content/`; `lithokraft_game` = wiring, UI and modes, headers in `include/lithokraft/game/`; and
the `lithokraft` exe = LithoKraft.exe). Game code imports the engine namespace with one using-directive
(`lithokraft/engine.h`) so it can say `ChunkMap` or `ui::Button` unqualified; engine code never includes a game header.

| Module (target) | Owns | Public headers |
|---|---|---|
| **core** (`litholattice_core`) | logging, time, math, JSON reader/writer, threading (exception-safe job pool + `JobBatch`, triple buffer, hand-off queue), `HighResTimerScope` | `log.h time.h math.h json.h threading.h timer_resolution.h` |
| **procgen** (`litholattice_procgen`) | noise (Perlin, OpenSimplex2 F/S, value, cellular, white), fractals, domain warp, seeding, JSON node graph, generator pipeline + stages; no world types | `seed.h noise.h modifiers.h graph.h pipeline.h stages.h generator_loader.h procgen_selftest.h` |
| **assets** (`litholattice_assets`) | resource packs + pack stack, Image, TextureRegistry (TextureId / PaletteId / AnimationId), palettes, animations, BlockModel / FaceLayout, shader sources; CPU only, never GL | `image.h resource_pack.h texture_registry.h block_model.h resource_loader.h shader_library.h assets_selftest.h` |
| **ui** (`litholattice_ui`) | retained-mode widgets: tree + layout (anchors, stacks, grids, padding / margins), JSON theme, input routing (hover, capture, focus, keyboard navigation, popups, modals, tooltips), controls, text editing model, JSON layouts; drawing / clipboard / scripts injected | `geometry.h events.h ui_renderer.h theme.h painter.h widget.h ui_root.h controls.h containers.h text_buffer.h text_edit.h lists.h menus.h dialog.h widget_factory.h ui_selftest.h` |
| **input** (`litholattice_input`) | input events, device state, game-declared `ActionSet` + action map + bindings file, game-declared contexts, analog conditioning, replay | `input_system.h action_map.h action.h binding.h input_context.h input_event.h input_backend.h device_state.h analog.h input_types.h input_selftest.h` |
| **platform** (`litholattice_platform`) | window (GL 3.3 context, or no context + Vulkan surface creation), `GlfwInputBackend` (GLFW is private to this module) | `window.h` |
| **world** (`litholattice_world`) | BlockRegistry (instance, filled by the game; engine knows only `kAir`), Chunk/SubChunk, ChunkMap, ColumnGenerator (procgen pipeline → chunk columns), Dimension/DimensionRegistry (with aliases), mesh snapshots, raycast | `block.h chunk.h chunk_map.h column_generator.h dimension.h mesh_input.h raycast.h` |
| **sim** (`litholattice_sim`) | tick scheduler (SimThread, SimConfig), `IWorldSimulation` interface, clock domains | `sim_thread.h world_simulation.h clock_domain.h` |
| **script** (`litholattice_script`) | sandboxed AngelScript host: engine wrapper, `.as` loading, hooks, log binding | `script_engine.h` |
| **render** (`litholattice_render`) | `IRenderer` + `ITextureArray` interfaces, BlockMaterials (models resolved per block), voxel mesher + `IBlockAppearance`, overlay geometry shared by backends (HUD quads, ordered clipped UI draws, font atlas, outline, crosshair), `HudUiRenderer` (ui::IUiRenderer for both backends); the OpenGL 3.3 backend is private (`gl33/`) | `renderer.h texture_array.h block_materials.h block_appearance.h mesher.h overlay_geometry.h hud_ui_renderer.h` |
| **render_vk** (`litholattice_render_vk`) | Vulkan backend: run-time loader, device pick, swapchain, frames in flight, vertex arena, texture array, pipelines from build-time SPIR-V | `vulkan_renderer.h` |
| **content** (`lithokraft_content`, `LithoKraft/src/content`) | the mod's content: block registry, signal sim + component behaviours, `SignalWorldSimulation`, `LithoKraftAppearance`, SFX recipes, note mapping, content selftest | `engine.h blocks.h signal.h component.h signal_world.h appearance.h sfx.h music.h content_selftest.h` |
| **game** (`lithokraft_game`, `LithoKraft/src/game`) | Game (wiring), Player, creative placement, hotbar, debug commands, mesh streaming, controls panel, dimensions (Wafer, Organic World: `createDimensions`, embedded generator files) + demo-circuit structures, ResourceManager, GameUi (HUD, hotbar, inventory on the ui module), UI script bindings, F10 script editor | `engine.h actions.h input_selftest.h game.h player.h sound_director.h dimensions.h structures.h resource_manager.h game_ui.h ui_script_bindings.h script_editor.h game_ui_selftest.h` |
| **app** (`lithokraft` → LithoKraft.exe, `LithoKraft/src/app`) | `main`, options, pack and scripts folder lookup, `--selftest`, `--noise-preview` | none |

**Dependency rules (one-way, enforced by CMake link lists):** game → engine only (nothing in `src/` includes
`lithokraft/` or links `lithokraft_*`).
`app → game → content → {sim, world, render, audio} → core`, `game → {platform, script, input, ui}`, plus `world → procgen → core`, `render → ui → core`, `sim → world`, `render → world`
(read-only block data for meshing), `render → assets → core`, `app → render_vk → render` and `platform → input` (the GLFW
backend implements `IInputBackend`). render never sees sim or platform; sim and world never include GL, Vulkan or GLFW and never
read input; ui knows no renderer, input module, window or script engine (all injected: `IUiRenderer`, `UiEvent`, `IClipboard`, `IScriptHost`); procgen knows no blocks or chunks (materials are numbers, named in JSON and resolved by world); only platform includes GLFW; only `render/gl33` includes GL; only `render_vk` (and platform's surface helper)
include the Vulkan headers. Sim asks for remeshes through a callback carrying an immutable `MeshInput`.

**How to add a block (game):** add a `BlockId` constant in `lithokraft/content/blocks.h` and one row in
`blockRegistry()` (`LithoKraft/src/content/blocks.cpp`): name, engine flags (solid / opaque / transparent /
directional / interactive) plus game flags (powerable / cutsWire), shape (Cube / Slab / Cross, or Custom drawn by
`LithoKraftAppearance::meshCustom`), texture key, behaviour name, `SimRole`, wire channel, tier. A new look is a branch
in `LithoKraftAppearance::faceLook` (shader mode) and, for a new shader feature, the backend shader. New behaviour = a
`ComponentBehavior` class registered in `content/components/` and named by the row; the engine never switches on ids.

**How to write a new game on the SDK:** build a `BlockRegistry`, implement `IWorldSimulation` (and optionally
`IBlockAppearance`), declare an `ActionSet`, default `ActionMap` and `ContextRules`, register dimensions, then hand
them to `SimThread` (`SimConfig`), `meshSubChunk` (`MeshContext`), `BlockMaterials::build` and `InputSystem`.

**How to add a dimension (game):** write a generator file `LithoKraft/assets/worldgen/<id>.json` (graph + stages, see
`docs/PROCGEN.md`; it is embedded as the built-in automatically), add `<id>` to `ResourceManager::kWorldgenDimensions`,
then register `{id, biomes, generator, setup}` in `createDimensions()` (`LithoKraft/src/game/dimensions.cpp`; optional
setup = structures + spawn). `Game::switchDimension(id)` handles stop/unload/load.

**How to add a renderer backend (e.g. GL 4.6):** implement `IRenderer` and `ITextureArray` (upload/clear chunk meshes,
`renderFrame(FrameParams)`, texture array upload, material bindings) in its own module or in `render/<backend>/`, with a
factory, and choose it in `LithoKraft/src/app/main.cpp` (`createDisplay`). Mesh data (`ChunkMesh`, 12-byte `PackedVertex`),
`FrameParams` and the overlay geometry (`overlay_geometry.h`) stay the same. See section 0d for the Vulkan backend.

## 0d. Vulkan backend (`render_vk`) **[proto]**
- **Selection:** `--renderer gl33|vulkan` (default gl33). For Vulkan, `main` creates a no-API window
  (`GraphicsApi::Vulkan`) and passes a `VulkanSurfaceSource` (instance extensions + a surface-creation callback) so
  render never depends on platform. If anything fails (no loader, no device, surface, pipelines), the reason is
  logged (`renderer: Vulkan unavailable (...); falling back to OpenGL 3.3`), the window is recreated with a GL
  context and gl33 is used.
- **Dependencies:** Khronos **Vulkan-Headers** (vulkan-sdk-1.4.363.0) and **glslang 16.6.0** via FetchContent from the
  official KhronosGroup release archives, SHA256-pinned. No Vulkan library is linked: `vk_loader` opens the system loader
  (`vulkan-1.dll` / `libvulkan.so.1`, installed by the GPU driver) at run time and fetches entry points (X-macro tables
  in `vk_loader.h`), so the exe still starts without Vulkan. No SDK install is needed to build or run.
- **Device:** instance (Khronos validation layer + debug messenger when `--vk-validation` or a Debug build, if the layer
  is installed), physical device scored discrete > integrated > virtual > cpu among those with a graphics+present queue
  and `VK_KHR_swapchain`, ties broken by the newest Vulkan version (native driver over a layered one). Startup logs every device and the chosen one: name | type | Vulkan version | driver version |
  instance version.
- **Frames:** 2 frames in flight (command buffer, fence, acquire semaphore, uniform buffer, overlay vertex buffer and
  descriptor set each); a render-done semaphore per swapchain image. The swapchain (+ depth buffer, framebuffers) is
  rebuilt when the window size changes or present reports out-of-date / suboptimal. FIFO with `--vsync`, else MAILBOX /
  IMMEDIATE. B8G8R8A8_UNORM (non-sRGB) to match the GL output.
- **Meshes:** vertices go straight into host-visible 32 MB blocks (`VertexArena`, first-fit with coalescing) so the
  allocation count stays tiny; one shared quad index buffer. A replaced mesh's range is freed only once every frame that
  could read it has finished (frame-number tagged). Frustum culling as in gl33; the sub-chunk offset is a push constant.
- **Textures:** `VkTextureArray` (2D array image, mip chain by blits, mag nearest / min trilinear, plus the palette image);
  F9 hot reload waits for the device, uploads, then rewrites the descriptor sets.
- **Shaders:** separate Vulkan GLSL 450 sources in `assets/shaders/vk/` (explicit locations / bindings, one std140 `Frame`
  block = `vk_frame_uniforms.h`, push constants), compiled to SPIR-V at build time by the fetched glslang into generated
  headers and embedded in the exe. The chunk shading itself is shared with OpenGL: both `chunk.frag` files
  `#include "chunk_shading.glsl"` (expanded by glslang for Vulkan and by `expandShaderIncludes` for GL). The vertex
  shaders keep OpenGL-style matrices and convert clip space (y flipped, z remapped to 0..w). Packs cannot override the
  Vulkan shaders (they are compiled in); pack shader files apply to the GL backend only.
- **Not yet (next steps):** device-local vertex memory via staging (meshes are read over PCIe from host memory today),
  pipeline cache on disk, MSAA, a GPU timing query in the perf line, multi-queue uploads.

## 0c. Logging, perf log and HUD **[proto]**
- **Log:** `core/log` tees every console line to a file; LithoKraft uses `LithoKraft/logs/lithokraft.log` (source tree
  via `LITHOKRAFT_SOURCE_DIR`, else `logs/` next to the exe; `--log` overrides), `[HH:MM:SS.mmm]` timestamps, flushed
  per line, previous run rotated to `lithokraft.prev.log`. Not opened for `--selftest` / `--version`.
- **Perf:** the app loop logs the renderer's `deviceInfo()` (GL vendor | renderer | version, or the Vulkan device line) at startup, one `perf`
  line per second (fps, frame avg/max, TPS achieved/target, tick avg/max from `SimStats`, budget, dropped, chunks,
  meshes, dim), hitches (frame > 2× running average and > 10 ms); the game logs tick-rate changes, EUV toggles and
  dimension switches.
- **HUD:** `render` exposes a generic `HudLayer` (pixel-space rects + text, plus ordered, clipped UI `draws`) in
  `FrameParams`; both backends draw it last with blending, from quads built by `overlay_geometry.h` with an 8×16
  bitmap font baked into `render/font_data.h` (no runtime deps). All game panels are UI widgets now (section 0f):
  stats top-left, controls bottom-left (H), hotbar bottom-centre, inventory and script editor as dialogs; scale 2 at
  ≥ 900 px tall. The renderer knows nothing about the game or the widgets.
- **Input:** see section 11. The controls panel is generated from the live bindings (keyboard / mouse and gamepad
  columns), so it always matches `config/input.json`.

## 0b. Scripting (AngelScript) **[proto]**
- **Library:** AngelScript **2.38.0** (latest stable, Aug 2025), official SDK zip only, fetched by CMake
  FetchContent and pinned: `https://www.angelcode.com/angelscript/sdk/files/angelscript_2.38.0.zip`,
  SHA256 `b33b5dbcda10317ef67d628353d83246984ce6fcac102d4dc2aed121eba52e6f`. Built static with the same MSVC runtime;
  the SDK's `scriptstdstring` add-on supplies the `string` type.
- **Module:** `script` depends on core only. `ScriptEngine` = init/shutdown, message callback → our log, load all
  `*.as` from a folder into one module, `callHook("void onLoad()")` (missing hooks are fine).
- **Sandbox:** scripts see only what we register: `string`, `log(string)`, and the game's bindings (today
  `string currentDimension()`, registered by `Game`, and the `ui_*` widget API from `UiScriptBindings`, see
  `docs/UI.md`). No file, network, or system add-ons are registered. Each hook
  call has a 500 ms time budget (line callback aborts runaway loops).
- **Hot reload:** `ScriptEngine::reload()` rebuilds the folder into a staging module and swaps it in only when it
  compiles, so a typo never leaves the game without scripts; `buildErrors()` lists the compiler errors. Widget
  handlers are called with `callWithString(decl, widgetId)`. The F10 script editor uses both.
- **LithoKraft scripts:** `LithoKraft/scripts/` (`hello.as` logs a greeting at startup). The build copies it next to
  the exe. Dev builds (CMake `LITHOKRAFT_DEV_BUILD=ON`, the default) load the source-tree folder so F10 editor saves land
  in the repo and are never overwritten by that copy; shipping builds (`-DLITHOKRAFT_DEV_BUILD=OFF`, `build.ps1 -Ship`)
  load the exe-local copy. `--scripts source|exe|<dir>` overrides at run time; the startup log names the folder.
- **Selftest:** hello.as loads and `onLoad()` logs; an unregistered `system()` call fails to compile; an infinite
  loop is aborted.
- **How to expose a game function:** in `Game::initScripts()`, call `scripts_.setModPermissions(...)` then
  `scripts_.bindGlobalFunction(decl, fn, ...)` before `loadDirectory`. The declaration must be on the
  script binding allowlist and covered by the mod's permission bitmask (no raw engine access).
  Keep bindings in game, never in script.

## 0f. UI (`ui`) **[proto]**
Retained-mode widget toolkit, core-only. `UiRoot` owns the widget tree plus popup and modal layers and routes
`UiEvent`s (hover, pointer capture, focus with bubbling keys, Tab / arrow navigation, outside-click popups, blocking
modals, tooltips). Layout: anchors (stretch or pivoted box) in any parent, `StackPanel` (flex, margins, spacing),
`GridPanel`, theme padding. Theme: `ui/theme.json` through the pack stack (styles per widget class inheriting from
`default`; F9 reloads). Controls: Panel, Label, Button, Image, Slot, Grid, ScrollView + ScrollBar, TextBox,
TextEditor (over the `TextBuffer` model: caret, selection, clipboard, undo, text position API, line numbers),
Checkbox / Toggle, Slider, NumberField (0–64), Dropdown, TabView, Tooltip, ProgressBar, ListView, TreeView,
Menu / MenuBar / ContextMenu, Dialog, Splitter; JSON layouts via `buildWidget`. Rendering: one `IUiRenderer`
implementation, `render/HudUiRenderer`, emits ordered CPU-clipped `HudDraw`s that gl33 and Vulkan both draw through
the shared overlay geometry. Game side: `GameUi` (HUD, hotbar, inventory), `ui_input_bridge` (InputEvent →
UiEvent), the `UiText` input context, `UiScriptBindings` (AngelScript handlers + `ui_*` API) and the F10
`ScriptEditor` (save + staged hot reload through `ScriptEngine`). Details: `docs/UI.md`.

## 0e. Procedural generation (`procgen`) **[proto]**
Noise (Perlin, our own OpenSimplex2 F/S, value, cellular F1/F2/F2−F1 with three metrics, white; 2D/3D/4D, seeded),
fractal and domain-warp modifiers, SoA batch sampling, a JSON node graph (constant, coordinate, wave, noise, add,
multiply, min, max, clamp, curve, select, blend, terrace, scale_bias, cache) and a generator pipeline: climate → biome
selection (multi-noise nearest) → terrain (heightmap or density) → surface rules → carvers (worm caves; ravine stub) →
features (ore veins; dungeon slot). Seeds: world seed (`--seed`) → `dimensionSeed` → `chunkSeed` → per-feature seeds, so
output is identical on any thread or order. Wafer and Organic World are LithoKraft generator files in `LithoKraft/assets/worldgen/`, loaded
through the pack stack (packs and mods can replace them) with an embedded fallback. `--noise-preview` renders a graph
output to a PNG headlessly. Details, formats and node reference: `docs/PROCGEN.md`.

## 0a. Dimensions **[proto]**
A `Dimension` = id + chunk map + terrain generator (procgen pipeline, section 0e) + biome set + structure setup. The registry holds all of them;
the player is in exactly one. `switchDimension(id)`: stop the sim thread (it owns the active chunks), clear GPU
meshes and queued results (an epoch number drops in-flight mesh jobs), unload the old chunks, then parallel-load
the new dimension and start a new sim thread. Built in: **wafer** (silicon flats + demo circuits) and
**organic** ("Organic World", formerly overworld; placeholder grass/dirt/stone heightmap, biomes plains + hills by
low-frequency noise, worm caves and dirt veins underground; the id `overworld` is still accepted). Debug key F6 or `--dim wafer|organic`; no portals yet. Measured switch time on the box: ~35–45 ms for 256 columns.

## 1. World layout & block storage
- **Chunk column** = 16×16 blocks, height 0–767, split into **48 sub-chunks of 16³** (4096 cells). **[proto]**
  The wafer surface sits at ~layer 256; only a 24-block crust is generated, so the empty sections above and the
  ungenerated depth below cost nothing (null sub-chunks). Packed positions use 10 bits for y.
- **Cell state** `uint16` per cell beside the block id: bits 0–6 value (0–64), 7–8 facing (N −Z, E +X, S +Z, W −X),
  9–11 aux (repeater delay−1 / pulsar animation timer), bit 12 on. Torches and plain lamps use value 1 = lit. **[proto]**
- **Sub-chunk storage:** flat `uint16_t blocks[4096]` (8 KB), y-major index `(y<<8)|(z<<4)|x`, so X runs are
  contiguous and SIMD-friendly. Plus a `nonAir` count; **all-air sub-chunks are `nullptr`** (no memory, no meshing,
  no ticking). **[proto]**
- **Later:** palette + bit-packed indices (1–8 bits/cell) for save files and far chunks; an all-one-block
  sub-chunk becomes "uniform" (1 id, no array). Hot (edited/ticking) sub-chunks stay flat for speed.
- **Chunk map:** `unordered_map<uint64_t, unique_ptr<ChunkColumn>>`, key = `(cx<<32)|cz`. **[proto]**
  Later: open-addressing table + a small "last lookups" cache, and 27-neighbour pointer caches per sub-chunk.

## 2. Block registry & properties
Data-oriented: `BlockId → BlockDef` in a flat array owned by a `BlockRegistry` instance the game builds and passes to
the engine (no global). **[proto]** Engine flags: `Solid`, `Opaque` (culls neighbour faces), `Transparent`,
`Directional`, `Interactive`; LithoKraft adds `Powerable` and `CutsWire` above `kFirstGameFlag`. Each row also holds
the mesh shape, texture key and an opaque game payload (LithoKraft: sim role, wire channel red 1 / blue 2, lamp
tier). **[proto]**
Later fields in parallel arrays: shape (full / half slab / thin), texture ids, light emission, hardness,
component type (wire, torch, repeater, comparator, redirector, latch, switch, button, lamp, display pixel).

## 3. Meshing
- **Per sub-chunk, face-culled** with per-vertex ambient occlusion; rebuilt when the sub-chunk or a neighbour
  edge is marked dirty. **[proto]**
- Mesher copies an **18³ padded opacity cube** (sub-chunk + 1-cell border from 26 neighbours), then emits only
  exposed faces. No hash lookups in the inner loop. **[proto]**
- **4-byte vertices** (x,y,z local 0–16 + face|AO), shared static quad index buffer, sub-chunk origin as a uniform.
  **[proto]** Later: greedy merge of coplanar faces (big win on flat wafer: ~10–50× fewer quads), texture array
  index in the vertex, multi-draw-indirect / one big buffer to cut draw calls.
- CPU frustum culling per sub-chunk AABB. **[proto]** Later: occlusion culling via cave-cull style
  connectivity graph per sub-chunk.

## 4. Tick system
- **World tick-rate modes: 32 / 64 / 128 / 256 TPS** (powers of two), **default 32**. Selectable at runtime
  (F7 cycles 32→64→128→256) or `--tps`; shown in the window title with achieved TPS, average tick ms and sim budget %. **[proto]**
- **Fixed-timestep accumulator** on the sim thread: wall time is added to `acc`; whole steps are run while
  `acc >= step`. **Catch-up cap (spiral-of-death guard):** at most 100 ms of backlog is kept (the excess is counted
  as *dropped ticks*) and at most 16 steps run per wake-up. Tested: a 1 s freeze of the process dropped 462
  master steps and the next second was back at target. **[proto]**
- **High-rate waiting:** sleep until ~1 ms before the next step, then yield-spin (512 Hz = 1.95 ms period).
  Windows: `timeBeginPeriod(1)` for the sim thread's lifetime (default timer is ~15.6 ms). **[proto]**
- **Ticking radius** setting: default **8 chunks (128 blocks)**, max 12+ on strong PCs (12 ch = 192 blocks;
  25×25 = 625 columns). Only sub-chunks flagged **active** (contain components) *and* inside the radius tick. **[proto, stub]**
- **Scheduled-tick queue:** min-heap of `(dueTick, position)` for repeater delays, button release, latch toggles.
  **[proto, stub]** Later: bucketed timing wheel (O(1) insert/pop) since delays are small integers.
- Delays are defined in world ticks, so a repeater's real-time delay halves when the mode doubles; circuits stay
  logically identical across modes.

### 4a. Chip clock domains **[proto: stub]**
- Every lithographed chip belongs to a **clock domain** with its own rate (lithography tier decides the max,
  e.g. **EUV tier = 512 Hz via JIT**). Domains are driven by the sim thread, not by separate OS threads.
- All rates are powers of two, so the sim thread runs one **master clock = max(world TPS, enabled domain rates)**
  and steps each domain every `master / rate` master steps (integer divider: no drift, no phase math). World
  ticks are just another divider. 
- Domain order within a master step: chip domains first, then the world tick. Values cross between a chip and
  world wires only at the world-tick boundary (latched pin buffers), so a 512 Hz chip and a 64 TPS world never
  race.
- Prototype: one stub "EUV" domain at 512 Hz (F8 toggles it, `--no-euv` disables) whose step is a placeholder
  kernel; achieved Hz is shown in the title. Measured on the box: 512/512 Hz alongside every world mode.
- Later: per-domain chip lists compiled to bitsliced kernels; heat model uses domain rate × gate count.

## 3a. Textures and resources **[proto]**
Data-driven; the full schema for modders is in `docs/RESOURCE_PACKS.md`.
- **assets** (core only, no GL): `ResourcePack` (folder + `pack.json` + `resources.json`) and `PackStack` (base
  `assets/` < game pack `LithoKraft/assets/` < art pack `LithoKraft/assets/packs/<name>` < `--resource-pack` mods / player pack). Definitions merge by name and files
  resolve to the highest pack that has them. `loadResources(stack)` builds a `TextureRegistry` (stable `TextureId` =
  array layer, 0 = magenta missing checker; `PaletteId`; `AnimationId` for frame strips with sidecar timing) and a
  `BlockModelSet` (`FaceLayout` per state: face → side → all → missing; active falls back to base; UV rects; wire pieces;
  palette; mask roles). `ShaderLibrary` reads `shaders/<program>.vert|frag` through the stack. Nothing fails hard: bad
  entries become warnings and the missing texture.
- **render**: `BlockMaterials::build(registry, models)` resolves models against the block registry (`BlockDef::tex`) into a
  layer + UV rect per block face and state, wire pieces, shader bindings (mask layers, palette rows) and the animation
  table; immutable and shared by mesh workers via `shared_ptr`. `ITextureArray` is the GPU side (gl33:
  `GL_TEXTURE_2D_ARRAY`, mipmapped, plus a palette texture with one row per `PaletteId`; Vulkan: the same as images).
- **Shaders** are files in `assets/shaders` (chunk / hud / line, plus `chunk_shading.glsl` pulled in with
  `#include "..."`, which `ShaderLibrary` expands). CMake also embeds the same files into the exe
  (`cmake/embed_text_files.cmake`), and gl33 falls back to that built-in copy, with a log line, when a pack's program is
  missing or fails to compile. The Vulkan backend uses `assets/shaders/vk` compiled to SPIR-V at build time (section 0d).
- **Hot reload (F9, Debug context):** `ResourceManager` re-opens the packs, rebuilds registry, models, materials and
  shaders, uploads them, logs a diff (changed / added / removed textures, shaders, models) and asks the sim thread to
  re-snapshot every loaded sub-chunk (`SimThread::requestRemeshAll`), since layer numbers can change. In-flight mesh
  jobs finish with the materials they started with. There is no file watcher (F9 only).
- The art export's `manifest.json` is converted by `tools/convert_pack_manifest.py` into `resources.json`; the art
  files are never edited.
- Vertices are 12 bytes (position, uv, face|AO|uv-rotation, layer, value, shade mode). The chunk shader handles wire
  (palette by strength + C4 ring mask), colour lamps (palette row per tier, wrap mapping, rim), repeater pips, memory
  cells and animated faces (value = animation index), with every layer and palette row supplied as a uniform from the
  models.

## 5. Signal simulation (Red Wire / Blue Wire) **[proto: simple version]**
Prototype rules (sim thread, per world tick, visiting component cells only):
1. **Torch** (Red Torch / Blue Torch; registry `wireChannel` 1/2) = inverter: lit unless the powerable block it
   stands on is powered (wire with signal on top of it or beside it, or a lit torch under it). One-tick delay:
   torches read the previous tick's state. A lit torch feeds only adjacent wire of **its own colour**.
1b. **Strong power (Minecraft-style):** a lit torch of either colour powers the block directly above it if the
   registry marks it `kSolid | kOpaque | kPowerable`. That block emits **colourless** power: adjacent wire of either
   colour (6 neighbours) starts at 64, adjacent lamps light, a torch standing on it turns off (torch tower), and a
   lamp directly above a torch lights. Wire-powered blocks only invert torches (weak power, no re-emission).
2. **Wire** strengths are recomputed instantly by multi-source BFS from lit torches: 64 next to the torch, −1 per
   block, so a signal reaches 64 blocks. Wire connects to the same channel only (**red never connects to blue**),
   climbs/descends one-block stairs, and a block that cuts wire above a lower wire breaks the stair.
3. **Lamp** lights when any of its 6 neighbours is a wire (either colour) carrying signal or a torch-powered block, or a torch is directly below it.
Strength readout: wire brightness scales with strength; the title bar shows the targeted wire's strength.
4. **Components** (batch 2) compute from the previous tick's inputs (two-phase, so order never matters) and then
   act as wire sources at their output strength. Inputs read neighbour wire value, lit torch (64), strong-powered
   block (64) or another component's output toward this cell. Pulsar pulses flood their connected same-colour run
   at full strength (≤ 64 blocks) for one tick. Color lamps take the max adjacent strength and do not pass it on.
   Rules for each block: README "Components". Each block type with behaviour is its own class
   (`LithoKraft/src/content/components/`, private to lithokraft_content); a block names its behaviour in the block registry
   (`BlockDef::behavior`), and `BehaviorCatalog` binds BlockId → behaviour once. `BlockBehavior::describe()`
   gives the HUD look-at readout (wire, torches, lamps and every component); `ComponentBehavior` adds tick /
   emitToward / use. Components may return a `SoundCue` (Pulsar fire, Tone pitch change); edits add Place /
   Break / Click (`SignalWorldSimulation`, the game's `IWorldSimulation`). The sim thread hands them to the game
   thread as engine `SimEvent`s, which the game turns back into `SoundEvent`s. Behaviours are stateless; per-cell
   edge detectors, timers and key queues live in `ComponentMemory`. `SignalSim::tick` is four phases
   (`tickTorches`, `tickComponents`, `propagateWires`, `updateLamps`); the self-test lives in `signal_selftest.cpp`.
Planned: wires grouped into **nets** (graph nodes) built by flood fill on edit, distance precomputed per cell,
event-driven updates (idle circuits cost zero), repeaters / comparators (compare, subtract, threshold), redirector.

## 6. Lithography = JIT
- A lithographed region's net graph is frozen and compiled once:
  1) **Level 1:** topologically sorted, flattened bytecode over a dense state array (SoA, no pointers).
  2) **Level 2:** bitsliced evaluation: 1-bit logic for 64 copies/time-steps per `uint64_t` (AVX2: 256 per op).
  3) **Level 3 (later):** real native code (x64 emitter or a library such as asmjit); inline asm/intrinsics only
     in measured hot loops.
- The chip block is a single **node** with I/O pins and a **clock rate** property. It runs on the sim thread's
  chip scheduler, so it **keeps ticking outside the player radius** (it costs microseconds, not chunk loads).
  The chunk only needs its pins' neighbour nets loaded when something is connected outside.
- Shrink-once rule: a lithographed chip cannot be shrunk again; several chips (plus wiring) can be combined once into an IC at the Lithography Machine.
- Heat: each chip has `heat += f(clock, gates)`, minus cooling from neighbour fans; throttle, then burnout.

## 7. Threading model **[proto: real, not just planned]**
| Thread | Owns | Never touches |
|---|---|---|
| **Main / render** | window, input (polling, actions, contexts), **all GL objects** (via `IRenderer`), player camera, mesh upload queue | block data |
| **Sim (1 thread)** | the active dimension's **ChunkMap** (sole reader/writer while running), master clock, world ticks, chip clock domains, signal state, player edits, raycast | GL, meshes |
| **Workers (N = cores − 2)** | worldgen jobs, meshing jobs (pure functions → results) | GL, live block data |

Sync points (no big lock on the frame path):
1. **Worldgen → main:** columns generated in parallel, handed back through a `HandoffQueue` (O(1) vector swap);
   the main thread inserts them before the sim starts.
2. **Sim → mesh workers:** when cells change, the sim copies an immutable 18³ `MeshInput` snapshot (ids + state
   bytes, ~17 KB) of each dirty sub-chunk and submits a mesh job. Workers never read the live ChunkMap.
3. **Meshing → render:** workers push `ChunkMesh` into a `HandoffQueue`; the render thread uploads at most
   **64 meshes/frame**; a per-sub-chunk sequence number keeps out-of-order results from overwriting newer ones,
   and a dimension epoch drops results from an unloaded dimension.
4. **Sim → render:** `SimStats` (TPS, tick ms, budget %, dropped ticks, chip Hz, targeted block) via a
   **lock-free triple buffer**.
5. **Render → sim:** player chunk, TPS mode, domain toggle (atomics); camera ray (triple buffer); edits (hand-off
   queue). The sim raycasts and applies edits on its next wake-up (≤ 2 ms with the EUV domain on, ≤ 31 ms at
   32 TPS without it).

## 8. Data-oriented / SIMD hot paths
Contiguous `uint16` arrays, flags in flat tables, SoA net state; candidates for AVX2 intrinsics: opacity-cube
gather, "any non-air / count" reductions, bitsliced chip evaluation, greedy meshing masks. Compile with
`/arch:AVX2` once the baseline CPU is confirmed (most CPUs since 2013). Measure first (Tracy profiler later).

## 9. Performance targets (laptop RTX 3050 Ti 4 GB, Windows)
- **≥ 144 FPS** at 8-chunk view, **≥ 60 FPS** at 16+; frame time worst-case < 16 ms while chunks stream.
- **32 TPS locked** (default; 64 for faster builds) with 10k active components in radius; world tick < 50% of the 15.6 ms period;
  256 TPS mode for smaller builds (3.9 ms period). EUV chip domains hold 512 Hz.
- Meshing a 16³ sub-chunk < 0.3 ms per worker; chunk streaming never stalls the render thread.
- VRAM < 1 GB for a 16-chunk view (4-byte vertices + greedy meshing make this easy).
- Lithographed chip: ≥ 1 MHz equivalent single-chip clock at Level 2 on a laptop core.

## 10. Audio **[proto]**
Modules: `litholattice_audio` (synth, SFX, command queue; depends on **core only**) and `litholattice_audio_device` (the platform
backend: **miniaudio 0.11.25**, single header from the official release tag, SHA256-pinned in CMake; the only
code that includes it). The app opens the device; `--mute` (or no device) disables the engine and nothing is opened.

```
sim thread --SoundEvent--> game thread (SoundDirector) --AudioCommand--> SpscQueue --> audio callback
                                                                                       AudioEngine::render
```
- **Voice:** 0.32 fixed-point phase accumulator (wraps exactly like a hardware counter). Waveforms: pulse with
  duty 12.5 / 25 / 50 / 75 %, triangle, saw, LFSR noise (Game Boy CH4 register: 15-bit, or 7-bit "short"
  metallic mode with period 127), and a 32-step 4-bit wave table. Pitch sweep in semitones/s (NES/GB sweep unit).
- **Envelope:** ADSR with linear segments (attack/decay/release times in seconds, sustain level); optional
  hold time for auto note-off (length counter). The SID's exponential decay curve is a later refinement.
- **Synth + presets:** `SID3` = 3 voices, each pulse/triangle/saw/noise with full ADSR (C64-style).
  `DMG` = pulse 1, pulse 2, wave table, noise (Game Boy layout); a waveform a slot can't play is coerced.
  Preset names are internal; player-facing blocks get our own names (research doc: no "SID"/"AdLib" in game).
- **Buses:** music = SID3 (Tone Blocks), effects = DMG (procedural SFX). Mono mix, duplicated to stereo,
  master gain, hard clip.
- **Command queue:** `SpscQueue<AudioCommand, 256>` (core): wait-free single producer (game thread) / single
  consumer (callback). The callback drains it, then renders in 512-sample blocks from a preallocated buffer: no
  locks, no allocation, no I/O.
- **Note mapping** (game: `lithokraft/content/music.h`, on the engine's `audio/notes.h`): strength s = 1..64 → MIDI note s + 35, i.e. 64
  semitones from C2 (65.4 Hz) to D#7 (2489 Hz); s=25 middle C, s=34 A4 = 440 Hz; 0 = silent.
- **Tone Block:** directional component; back input strength = pitch; consumes the signal. It reports a Tone
  cue only when the pitch changes; `SoundDirector` maps each sounding block (by position) to one of the 3 SID3
  voices via `VoiceAllocator` (the oldest is stolen past three), pulse 25 %, ADSR 5 ms / 150 ms / 0.6 / 120 ms.
  Breaking the block releases its voice.
- **SFX recipes** (game: `LithoKraft/src/content/sfx.cpp`; played as notes on the engine's effects bus): place = falling 25 % blip, break = noise burst, button/use = 12.5 % tick,
  Pulsar fire = rising wave-table "pew" pitched one octave above the pulse strength's note. Gain falls off
  linearly from 4 to 48 blocks from the player.
- **Self-test** (`--selftest`, offline, never opens a device): zero-crossing pitch of pulse/triangle/saw/wave,
  duty fractions, ADSR levels at fixed times, LFSR balance and the 127-step short period, preset layouts and
  coercion, 200k items through the SPSC queue across threads, engine stereo pitch + note-off silence, each SFX
  audible then silent, mute, voice stealing; sim side: Tone cues and Pulsar fire cues.

## 11. Input **[proto]**
Module `input` (`litholattice_input`, depends on core only). Game code asks for **named actions**; it never sees keys,
buttons or GLFW. The engine defines no actions or contexts: the game declares them (`ActionSet`, `ContextRules`);
LithoKraft's are in `LithoKraft/src/game/actions.*`.

**Pipeline (once per frame, main thread):**
`IInputBackend::poll()` → `std::vector<InputEvent>` → `DeviceState` → bindings (`ActionMap`) → context gating →
`InputSystem::pressed / held / released / value / axis / text`.
- `IInputBackend` is the OS seam. `GlfwInputBackend` (private to platform) turns GLFW callbacks (key incl. repeat,
  mouse buttons, wheel, cursor, char, focus) into events and diffs `glfwGetGamepadState` for 4 pads (GLFW's built-in
  SDL_GameControllerDB mappings, plus `config/gamecontrollerdb.txt` if present). Connect / disconnect is detected by
  polling presence each frame. XInput, SDL or console backends would implement the same two-method interface.
- `InputEvent` is a small trivially copyable record with a timestamp (`glfwGetTime`). `InputSystem` can record the
  stream and `ReplayInputBackend` plays it back; the selftests drive everything through it, headless.
- `DeviceState` keeps held / pressed / released edges per key, mouse button and pad button, mouse delta / wheel /
  cursor, pad axes and the frame's text. Focus loss releases everything (no stuck keys after Alt-Tab); a pad
  disconnect releases that pad.

**Actions and bindings:** LithoKraft's `Action` (X-macro list in `actions.h`: movement, build, inventory, hotbar 1–9 / next / prev, toggles,
Pause, and `Look`, the one 2D axis). Each action has any number of bindings (`key:W`, `mouse:Left`, `wheel:Down`,
`pad:A`, `padaxis:LeftY-`, `padaxis:RightTrigger+`, `stick:Right`, `mouse:Motion`). Button actions report
pressed / held / released and a 0..1 value (analog sources keep their deflection; `pressThreshold` makes them
digital). `ActionMap::conflicts()` reports two actions sharing an input unless the `ActionSet` marks the pair as
meant to share (LithoKraft: Place / Use share right click: Use on a usable block unless sneaking, else Place).

**Config file:** `config/input.json` under the data root (LithoKraft: `LithoKraft/` when the source tree is present, else next to the
exe; gitignored). Missing → written from the defaults. Present → parsed over the defaults, so actions it does not
mention keep their defaults and new actions appear after an update; unknown names are skipped with a log warning.
A file with a syntax error is reported and left untouched, and the defaults are used.

**Contexts** (declared by the game in priority order; LithoKraft's, top consumes first): `Gameplay` < `Inventory` (opaque; clicks go to the UI as raw events) <
`KeyboardTyping` (opaque, wants text) < `UiText` (opaque, wants text: script editor / focused text field) < `Debug`
(transparent; F-keys and pad Back only, so it never steals a letter). `InputSystem::frameEvents()` hands the frame's
raw events to the UI. An opaque context hides every
context below it, so movement / hotbar keys cannot leak into the inventory or into keyboard typing. Push / pop takes
effect at the start of the next frame: the E that opened the inventory is not seen again by the inventory and
cannot close it in the same frame. Held state is tracked even while an action is hidden, so a key still held when
its context returns does not fire a fresh "pressed".

**Text channel:** separate from actions, fed by GLFW's char callback (layout- and shift-aware), plus Enter /
Backspace counts including auto-repeat. Delivered only while a text-wanting context is on top; the Keyboard block
maps it to SIXBIT.

**Analog:** sticks use a radial deadzone (direction kept, magnitude rescaled from the deadzone edge) and a power
response curve; triggers / single axes a linear deadzone. Look = mouse pixels × `mouseSensitivity` + right stick ×
`stickLookSpeed` × dt (`invertY` optional). All in the `analog` block of `input.json`.

**Sim boundary:** the sim never reads input. `Game` turns actions into `EditCmd`s (break, place, use, key pulses) and
camera / chunk updates; the sim applies them at the start of its next tick, so a tick sees a consistent world
regardless of frame rate.

**No input thread, by design:** GLFW requires event processing (and therefore callbacks and gamepad polling) on the
main thread; polling right before the frame uses the result gives the lowest input latency a frame-locked game can
have; and the only consumer of fresh input is the main thread itself, which hands commands to the sim through the
existing lock-free edit queue. A separate thread would add a hand-off, a lock or queue, and latency, for nothing.
