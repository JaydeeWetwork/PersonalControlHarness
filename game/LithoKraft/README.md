# LithoKraft (v0.1.0, preview build 2)

A voxel CPU-building game on the **LithoLattice** engine (repo root: `D:\LithoLattice`; this folder is
`D:\LithoLattice\LithoKraft`). Build, dependencies and the engine source map: root `README.md`.

C++20 + OpenGL 3.3 / Vulkan voxel prototype, **vertical slice**: Red Wire and Blue Wire (signal 0–64, −1 per block,
the two colours never connect), Red Torch and Blue Torch (inverters; each feeds only its own wire colour), Lamp, plus the batch-2 components (Repeater, Thresholdor, Redirector, Pulsor, Memor, Fast Latch, Switch, Button,
Keyboard, Color Lamp 16/32/64, slabs), textures from PNG packs, creative placement, demo circuits at spawn, and two
dimensions (**wafer**, 768 tall, and **organic** = "Organic", formerly overworld; `--dim overworld` still works).
Game modules: game, app; scripts in `LithoKraft/scripts/*.as` (sandboxed AngelScript 2.38.0, `onLoad()` hook). Engine
modules and architecture: `../docs/ARCHITECTURE.md`, section 0. Threads: render (GL owner), sim (owns block data; 32/64/128/256 TPS +
stub 512 Hz chip clock domain), worker pool (worldgen + meshing).

## Controls (Minecraft Java creative defaults + gamepad)
Every control is a named action in the `input` module. Bindings are saved to `LithoKraft/config/input.json` (created from the
defaults on first run, gitignored; several bindings per action, e.g. `"Jump": ["key:Space", "pad:A"]`, plus
deadzones / curve / sensitivity / invertY under `"analog"`). Delete the file to get the defaults back. Gamepads use
GLFW's built-in mappings (Xbox layout names); extra SDL mappings can go in `LithoKraft/config/gamecontrollerdb.txt`.

| Keyboard / mouse | Gamepad | Action |
|---|---|---|
| Mouse | Right stick | look (click in the window to grab the mouse) |
| W A S D | Left stick (analog) | move |
| Space / Left Shift | A / B | jump / sneak while walking (gravity applies); up / down while flying. Double-tap Space toggles flight (the `fly` console command does the same) |
| Left Ctrl (hold) | L3 (left stick click) | sprint |
| Left click / Right click | RT / LT | break / place the hotbar block on the outlined face (reach 8; wire and torches need a solid block below). Directional blocks face **away from you** (front = the direction you look) |
| Right click on Repeater / Switch / Button / Keyboard | X | **use**: repeater delay 1→8→1, switch on/off, button 10-tick pulse, keyboard typing capture |
| Shift + Right click | B + LT | place against a usable block instead of using it |
| Typing (keyboard captured) | — | A–Z, 0–9, symbols → SIXBIT code+1 pulsed out the front for 1 tick; Enter = 1 and Backspace = 2 out the right-hand control port (auto-repeat counts); **Esc** ends capture. Movement, hotbar, E and H are blocked while typing; F-keys still work |
| Middle click | D-pad Up | pick block: selects it if it is already in the hotbar, else puts it in the current slot |
| 1–8, mouse wheel | RB / LB | select hotbar slot (8 slots; wheel down / RB = next) |
| E | Y | creative inventory (all block types; click = put into the selected slot, Shift-click = first free slot; E/Esc closes; mouse is released while open; movement is blocked while open) |
| Q | D-pad Down | clear the selected hotbar slot (no item entities yet) |
| F1 | — | hide / show the whole HUD |
| F3 | Back / View | debug stats overlay on / off |
| F6 | — | debug: switch dimension (Wafer ⇄ Organic) |
| F7 | — | cycle world tick rate 32 → 64 → 128 → 256 TPS (default 32) |
| F8 | — | toggle the stub EUV chip clock domain (512 Hz) |
| F9 | — | debug: hot-reload resource packs (textures, models, palettes, animations, shaders, UI theme) |
| F10 | — | debug: in-game script editor for `scripts/*.as` (dev builds: `LithoKraft/scripts` in the source tree) (Ctrl+S saves and hot-reloads the scripts; Esc closes, asks about unsaved changes) |
| H | D-pad Left | show / hide the controls panel (generated from the live bindings) |
| Esc | Start / Menu | end keyboard typing, else close the inventory, else release the mouse |

Gamepad defaults: stick deadzone 0.2 (radial), trigger deadzone 0.1, response curve 2, look speed 3 rad/s. The
inventory has no gamepad cursor yet (open / close it with Y, pick with the mouse).

Start hotbar (fresh spawn, 8 slots): 1 The Fragment (the starting item, granted on day one), 2 Wafer Silicon, 3 Red Wire, 4 Blue Wire, 5 Red Torch, 6 Blue Torch, 7 Lamp, 8 Repeater — full 512-stacks of the kit blocks; the kit's Switch rides in main inventory slot 1 (everything else is in E). The inventory persists in the save (player.dat v4), so a restored player keeps exactly what they had.
F5 saves the world. F2/F4 are left free (Minecraft uses F2 screenshot).

**HUD (on screen):** 8-slot hotbar at the bottom centre (selected block name above it); top-left debug stats (F3): FPS + frame ms (avg/max), TPS achieved/target, tick ms (avg/max), sim budget %,
dropped ticks, EUV on/off, dimension, meshes, hotbar block, and the **looked-at block** (name + value: wire strength,
repeater delay/output, memory stored value and its SIXBIT char, color lamp tier/strength/colour index, facing, switch
state…; read up to 64 blocks away, marked "(out of reach)" beyond the 8-block edit reach). The inventory tooltip shows
the lamp tier. Bottom-left: controls panel with every key; **H** hides/shows it; **F1** hides the whole HUD. The title bar
carries the same summary. Wire brightness also shows strength.

**Torch rules:** a Red Torch powers only adjacent red wire, a Blue Torch only blue wire. Either torch also powers the
solid opaque block **directly above** it (Minecraft-style); that block gives plain colourless power to adjacent lamps
and to adjacent wire of **either** colour (64), and turns off a torch standing on it. Any powered wire lights lamps
and powers the block it touches. Torches invert: off while the block they stand on is powered (1-tick delay).

**Live log:** `LithoKraft/logs/lithokraft.log` in the repo (`D:\LithoLattice\LithoKraft\logs\lithokraft.log`; gitignored),
timestamped and flushed every line, so you can tail it while playing
(`Get-Content D:\LithoLattice\LithoKraft\logs\lithokraft.log -Wait`). The previous run is kept as `lithokraft.prev.log`.
Contents: GPU/renderer string at startup, one `perf` line per second (fps, frame avg/max ms, TPS achieved/target,
tick avg/max ms, budget %, dropped ticks, chunks, meshes, dimension), plus tick-rate changes, dimension switches and
hitches (a frame over 2× the running average). `--log <file>` overrides the path.

### Components (batch 2)
Every component is a full block that cuts wire; signal passes only through its ports. **Front = output** (the arrow on
the top texture); back = main input; left/right = side ports (relative to the facing).

| Block | Behaviour |
|---|---|
| Repeater | back in → front out, one-way. Output is boosted to a full **64**; delay 1–8 ticks (right-click cycles) |
| Thresholdor | back = input, LEFT = low ref, RIGHT = high ref. One ref: output = input when input ≥ ref. Both: window low ≤ in ≤ high (refs sorted). No ref: passes the input. Output = input strength |
| Redirector | any of back/left/right in (strongest wins) → out the front |
| Pulsor | back = data, left/right = clock. On a rising clock edge with data > 0: a 1-tick pulse of the data strength out the front that floods the connected same-colour wire run **without decay** (up to 64 blocks). Top shows the firing animation |
| Memor | back in, front out. Any new nonzero input (pulse or rising change) is stored and output continuously; 0 = empty; a rising signal on either side clears it |
| Fast Latch | T flip-flop: each rising edge at the back toggles; front outputs 64 while on (directional, placed like the others) |
| Switch / Button | right-click; Switch toggles a steady 64, Button gives a 10-tick 64 pulse; both power all six sides |
| Color Lamp 16/32/64 | colour = (s − 1) mod N from the art palettes (cga16/vga32/vga64), 0 = off (black with rim glow). Lamps terminate the signal |
| Keyboard | see Typing above; front = data out, right side = control out |
| Slabs | silicon/stone/dirt bottom half-slabs (decorative) |
| Tone Block | back input strength = pitch (s=1 C2 … s=25 middle C … s=34 A4 … s=64 D#7, semitone steps), 0 = silent; plays on a 3-voice SID-style synth. Directional (back = input, front = speaker); the front grille animates while a tone plays |

**Sound:** place / break / use clicks / Pulsor fire are small procedural chip sounds; Tone Blocks play notes.
They fade with distance (silent past 48 blocks). `--mute` turns audio off; without an output device the game
runs muted and logs it. Details: `docs/ARCHITECTURE.md` section 10.

Wire look: dark outline with the red (floor 32) / blue (floor 48) palettes, so weak signals stay visible.

### Texture packs and modding
`LithoKraft/assets/packs/pack_64` (default) and `LithoKraft/assets/packs/pack_32` (`--pack pack_32`), art by CloudAtlas (copied unmodified;
the engine reads `resources.json`, generated from the art's `manifest.json` by `tools/convert_pack_manifest.py`).
Packs stack: engine base `assets/` (shaders) < game pack `LithoKraft/assets/` (UI theme, world generators) < the art pack < each `--resource-pack <folder>` (mods, then your own pack); a
file or definition comes from the highest pack that has it, so a pack can reskin one block by shipping one PNG.
Shaders are files too (`assets/shaders`), overridable by packs, with a built-in fallback if one fails to compile.
**F9** reloads everything in game and logs what changed. Missing textures show a magenta/black checker (and a log
line). Schema and rules: `docs/RESOURCE_PACKS.md`.
Packs are synced from art v2.0 (export v11: LithoKraft / LithoLattice branding in `textures/branding/`, not drawn
by the code yet; the HUD shows a "Powered by LithoLattice" text line for the first 5 s).

### Demo circuits (wafer spawn, platform in front of you; torches on the left)
- **Nearest row (torch powers the block above):** Red Torch under a silicon block → the block powers **blue** wire
  on the ledge beside it (colourless block power) → Lamp (lit).
- **Next row:** Blue Torch → blue wire climbing a one-block stair → Lamp (lit).
- **Middle row (inverter):** Red Torch → red wire with a **gap** → silicon block with a Red Torch on top → red wire →
  Lamp. At launch the block is unpowered, so the top torch and the lamp are ON. **Right-click red wire (key 2) into
  the gap:** the block gets powered, the torch turns off and the lamp goes dark. Break the wire to turn it back on.
- **Back row:** Red Torch → 10 red wire → Lamp (lit), with an unpowered blue wire right beside it that stays dark
  (colours never connect).
- **Repeater row:** torch → long wire → Repeater (delay 4) → Lamp; right-click the repeater to change the delay.
- **Window comparator:** Switch → wire (strength 46) → Thresholdor with Memors holding 40 (left/low) and 50
  (right/high) → Lamp lit. Toggle the switch to watch it go dark.
- **Pulsor → Memor:** Memor (23) feeds the Pulsor's data; press the Button (clock) → a 23 pulse runs 24
  wires with no decay → Memor stores 23 → Color Lamp 64 shows colour 22.
- **Lamp wall:** 16 + 16 Color Lamp 16 fed by a red run and a blue run — the decay gradient shows the palette.
- **Tone row (z=30):** Switch (off) → 28 red wires → Tone Block, which plays C5 (s=37) when you flip the switch.
- **Last row:** Color Lamp 32, the three slabs, Keyboard → Memor → Color Lamp 64 (type a letter and look at the
  memor), its control port → Lamp; Button → Fast Latch → Lamp; Switch → Redirector → Lamp.

Command line: `LithoKraft [--version] [--selftest] [--dim wafer|organic] [--size chunks] [--tps 32|64|128|256]
[--no-euv] [--radius chunks] [--seconds s] [--vsync] [--log file] [--pack pack_64|pack_32] [--resource-pack dir]... [--scripts source|exe|dir] [--mute]
[--renderer gl33|vulkan] [--vk-validation] [--seed n] [--noise-preview graph.json ...]` (defaults: wafer, 16 chunks,
32 TPS, EUV on, radius 8, OpenGL 3.3, seed 0).
`--renderer vulkan` uses the Vulkan backend (discrete GPU preferred; the chosen GPU, Vulkan and driver versions are
logged) and falls back to OpenGL 3.3 with a log line if Vulkan is unavailable. `--vk-validation` (default in Debug
builds) enables the Khronos validation layer when it is installed. Pack shader overrides apply to OpenGL only.
`--scripts` picks the AngelScript folder the game loads and the F10 editor saves to: dev builds (default,
CMake `LITHOKRAFT_DEV_BUILD=ON`) use the source tree's `LithoKraft/scripts`, so in-game edits are not overwritten by the
build's copy; shipping builds (`tools\build.ps1 -Ship`) use `scripts/` next to the exe. The startup log names the folder.
`--selftest` runs headless checks (no window) and exits 0 on success. `--seed n` sets the world seed (Organic
terrain, caves and veins; the wafer is seed-independent). `--noise-preview <graph.json> [--preview-out f.png]
[--preview-output name] [--preview-size px] [--preview-scale blocks/px] [--preview-y y]` writes a grayscale PNG of a
noise graph or generator file without opening a window (see `docs/PROCGEN.md`).

## Source map (game)
Engine modules (`src/`) are listed in the root `README.md`.
- `LithoKraft/src/game` — Game (wiring), player camera, creative placement, hotbar, debug keys, controls panel, SoundDirector (sim sound events → audio), dimensions (Wafer, Organic) + demo-circuit structures, ResourceManager, GameUi (HUD / hotbar / inventory widgets), UI script bindings, F10 script editor
- `LithoKraft/src/app` — `main`, options, scripts folder choice, `--selftest` runner, `--noise-preview`
- `LithoKraft/assets` — game pack: `ui/theme.json` (UI theme), `worldgen/` (Wafer and Organic generator files; packs can override; embedded as the fallback), `packs/pack_64`, `packs/pack_32` (art by CloudAtlas)
- `LithoKraft/scripts` — AngelScript (`hello.as` runs at startup)
- `LithoKraft/brief.md` — game design brief

## Box test (Oct 9 2026)
Linux box, **software rendering** (Mesa llvmpipe on the CPU, no GPU), 1280×720: ~30–36 FPS (GPU-less; expect
hundreds of FPS on a real GPU). Worldgen 256 columns in ~5 ms; all 1200 sub-chunk meshes uploaded in the first second.

| Mode | Achieved TPS | Avg world tick | Sim budget | EUV domain |
|---|---|---|---|---|
| 32 | 32.9 (31.9 steady) | 0.029 ms | 0.11% | 512 Hz |
| 64 (default) | 64.0 | 0.021 ms | 0.15% | 512 Hz |
| 128 | 127.8 | 0.011 ms | 0.15% | 512 Hz |
| 256 | 256.9 | 0.007 ms | 0.20% | 513 Hz |
| 256, EUV off | 256.0 | 0.007 ms | 0.19% | off |

Runtime switching (F4 → F1 → F2, the keys at the time; now F7 cycles) took effect within one tick. Catch-up cap test: freezing the process for 1 s
dropped 462 master steps, and the next second ran at target with no runaway. Screenshot: `docs/screenshot.png`.

Vertical slice (same box, Oct 9): `--selftest` all PASS (signal decay, inverter, colours, stairs, demo lamps,
dimension load/unload, mesher, AngelScript hello + sandbox). In the window: 40–125 FPS on llvmpipe, 32.0 TPS at ~0.03 ms/tick; right-click placed a
red wire and left-click removed it (components 48 → 49 → 48); F6 switched wafer → overworld → wafer in ~35–45 ms.
Screenshot: `docs/screenshot.png`.

Preview build 2 (box, Oct 9): `--selftest` all PASS, including every component's logic, the demo (9 plain lamps / 7 lit,
33 of 35 color lamps lit), texture pack face resolution and the pulsar strip. pack_64 = 90 array layers, 0 missing.

### Preview build 2: calls made on open points
- Repeater output is a full 64 (not the input strength). Thresholdor with no refs passes its input.
- Memor stores on any change to a nonzero value; a rising signal on either side clears it.
- Fast Latch = T flip-flop (rising back edge toggles, front 64), aimed like the other components.
- Switch/Button power all six sides at 64; a lit torch counts as 64 into an adjacent component; components also light plain lamps.
- Keyboard pulses are queued one per tick (fast typing never merges characters).
- Pulsor animation runs on the frame clock while the firing flag is held 7 ticks (start phase not tick-synced).
- Wafer is 768 tall with the surface at ~256, but only a 24-block crust is generated (empty sections cost nothing).
- Look-at readout works to 64 blocks; editing/using stays at 8. Slab hitbox/raycast is still a full block; wire on a slab floats at slab-top height only visually.
- Wire glow is not drawn (alpha-tested cutout, no bloom); wire visually connects into components and lamps.
