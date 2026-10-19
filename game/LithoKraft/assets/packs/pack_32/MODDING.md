# LithoKraft: texture packs and modding

> **Exact spellings (Oct 9 2026, 2:45 PM ET, current):** the game is **LithoKraft** (capital L and K; caps style LITHOKRAFT). It was formerly LithoCraft and, before that, VOXEL COMPUTE. The engine is **LithoLattice** ("LithoLattice Engine", "Powered by LithoLattice"). The `lithocraft_*` branding is superseded but kept. Use `lithokraft_*` in `textures/branding/`.

> **Exact spellings (Oct 9 2026, 2:44 PM ET):** the game is **LithoCraft** (caps style LITHOCRAFT, formerly VOXEL COMPUTE). The engine is **LithoLattice** (one word, both Ls capitalized), used as "LithoLattice Engine" and "Powered by LithoLattice". Plain "Lattice" is trademarked, so the `lattice_*` files are superseded but kept. New branding: `litholattice_*` and `lithocraft_lockup_mixed_*` in `textures/branding/`.

> **Rename (Oct 9 2026):** the game is now **LITHOCRAFT** (formerly VOXEL COMPUTE) and the engine is **LATTICE**. File and block names don't change. Brand images for both names are in `textures/branding/` (the Voxel Compute ones are kept until the new name is trademark-checked).

## Rules
1. **Every texture is its own PNG.** There's no hand-made atlas. The engine packs the PNGs into a GPU texture array at load time.
2. **One fixed tile size per pack.** Every block texture in a pack is the same size. `pack_64` is the default and `pack_32` is the low option. **16 px isn't recommended**, because the shapes that tell red from blue wire (round vs diamond, solid vs twin rails) break down at that size.
3. **Names are lowercase `snake_case` and match the block, plus face, state or piece.** Mod packs **override by file name**: same relative path, same name, same tile size.
4. **Straight (non-premultiplied) alpha**, smooth anti-aliased edges, and seamless tiling for full blocks.

## Layout
```
pack.json                      name, tile_size, alpha mode, version
manifest.json                  every file: path, size, sha256, what it is
textures/blocks/<name>.png     block textures, all tile_size x tile_size
textures/palettes/*.png        lookup rows (x = value), NOT atlases
textures/masks/*.png           shader masks
textures/ui/, textures/ui/icons/   HUD pieces and item icons (not tile-sized; same in every pack)
```

## Block names
| File | Block / state |
|---|---|
| `silicon`, `dirt`, `stone` | same texture on every face |
| `grass_top`, `grass_side` | grass faces (the side tiles horizontally). Grass, dirt and stone make up the **ORGANIC WORLD** (formerly called "overworld"); the file names didn't change |
| `lamp_off`, `lamp_on` | plain lamp |
| `color_lamp_16_off`, `color_lamp_32_off`, `color_lamp_64_off` | color lamp tiers at strength 0 |
| `color_lamp_16`, `color_lamp_32`, `color_lamp_64` | color lamp **neutral bases**, one per tier. The shader colors the panel from the tier's palette (`palettes/cga16.png`, `vga32.png`, `vga64.png`) inside that tier's `masks/color_lamp_<N>_mask.png` (R = face color area, G = shine, B = colored lip), plus `masks/color_lamp_<N>_rim_mask.png` (faint violet rim on the inner edge; strongest on black, invisible on bright colors) |
| `red_torch`, `red_torch_off`, `blue_torch`, `blue_torch_off` | torches (sprites with transparency) |
| `red_wire`, `red_wire_corner`, `red_wire_t`, `red_wire_cross`, `red_wire_end`, `red_wire_dot` (and the same for `blue_`) | wire pieces, **one neutral-bright texture per piece**. Canonical orientation: wire = N–S, corner = N+E, t = E+S+W, end = arm pointing S. The engine rotates them |

## How wire strength is drawn (you only paint ONE texture per piece)
- The wire PNG is **neutral**. The fill is light grey (≈ #D9D9D9), the core highlight is white, and the outline is dark.
- The game tints the fill with `palettes/<color>_wire.png` at x = strength (0–64) and blends the white core toward the hot color at high strength.
- Glow comes from a blurred (low-mip) copy of the texture's alpha.
- The exact-strength C4 ring is drawn from `masks/<color>_wire_ring_mask.png`.

So when you paint a wire: keep the parts that should take the wire color **light grey**, the "hot core" **white**, and anything that must stay dark **dark**. To change the colors themselves, edit `palettes/red_wire.png` or `palettes/blue_wire.png`. These are 65x1 PNGs, where pixel x is the color at strength x.
Wire arms must stay centred, with the same width at the tile edge, or neighbouring pieces won't join.

## Color lamp tiers (16 / 32 / 64)
| Tier | Palette | Look |
|---|---|---|
| 16 | `palettes/cga16.png` (16x1, standard CGA/EGA order 0–15, 6 = brown #AA5500) | large face, **rounded** corners, darkest gap |
| 32 | `palettes/vga32.png` (32x1: CGA16, then 16 saturated hues from the VGA default palette) | large face, **square** corners |
| 64 | `palettes/vga64.png` (64x1, RRGGBB: r=(i>>4)&3, g=(i>>2)&3, b=i&3) | large face, **chamfered** corners, lightest gap |

Color lamps are mostly used as **pixels in player-built screens**. All tiers share one look: the lit face covers about 85% of the tile, with only a thin dark gap around it, so a wall of lamps reads as a pixel grid. The tier cue is deliberately quiet (corner shape plus a slight change in gap brightness), so you'll see it up close but not from far away. If you repaint lamps, keep the face large and the gap dark.
Because the face shape differs per tier, each tier has its own masks. If you change a face, repaint both its masks too.

Each palette has a `.json` next to it that lists the colors and how strength maps to an index. Strength 0 always shows `color_lamp_<N>_off`. For 64, idx = s−1. For 16 and 32 the mapping is **wrap**: idx = (s−1) mod N. Strength 1 is black, and strength N+1 starts the palette again. The alternate modes `clamp` (idx = min(s,N)−1) and `scale` (idx = ceil(s·N/64)−1) are still documented and available in the shader (`uMapMode` 0 and 1), but they aren't the default.
To recolor a tier, edit its palette PNG (pixel i = color at index i). To restyle it, repaint `color_lamp_<N>.png`, but keep the panel where the mask expects it.

**Renamed in art v1.2:** `color_lamp.png` is now `color_lamp_64.png`, and `color_lamp_off.png` is now `color_lamp_64_off.png`. Mod packs that override the old names need to rename their files.

## Animations
A texture is animated when it's a **vertical strip** of square frames (width = tile_size, height = tile_size × frames) **and** has a sidecar file `<name>.png.json`:
```json
{ "frametime_ms": 125, "frames": [0, 1, 2, 3], "interpolate": false, "loop": true }
```
`frames` lists frame indices from top to bottom and can repeat (e.g. `[0,1,2,1]`). If it's missing, all frames play in order. Without a sidecar, a non-square PNG is an error. The default packs ship no animations yet; see `export/reference/examples/` for an example.

## Making a mod pack
1. Copy only the files you want to change into a new folder with the same relative paths, e.g. `my_pack/textures/blocks/stone.png`.
2. Keep the pack's tile size: all of your block PNGs must be the same size (64 or 32) and match the base pack you target.
3. Add a `pack.json` with `name` and `tile_size`. Files you don't include fall back to the default pack.
4. Use PNG with straight alpha (what every normal image editor saves). Don't premultiply.

## Not in the packs
Strength-level wire PNGs, baked C4 rings, the baked color-lamp colors for every tier , tiling previews and shader previews are QA references only. They live in `export/reference/`.


## Batch 2 components (art v1.5, preview build; names are temporary but stable)
**Orientation:** for directional blocks, the **front (output) is the top edge of the `*_top` texture** (north), the back (input) is the bottom edge, and left/right are the side edges. The engine rotates the top to match the block's facing. Any face without its own file uses `*_side`, and that includes the bottom.

| Block | Face layout | Files | States |
|---|---|---|---|
| `repeater` | **top+side+front+back** | top: repeater_top / repeater_top_on; front (output): repeater_front; back (input): repeater_back; left/right/bottom: repeater_side | unpowered/powered (top); delay 1-8 via masks/repeater_delay_mask.png |
| `thresholder` | **top+front+back+left+right** | top: thresholder_top / thresholder_top_on; front (output): thresholder_front; back (input): thresholder_back; LEFT (reference LOW): thresholder_low; RIGHT (reference HIGH): thresholder_high; bottom: thresholder_side | one side powered = threshold (out when input >= ref); both = window comparator (out when low <= input <= high; engine uses min/max so swapped values still work) |
| `redirector` | **top+side+front** | top: redirector_top / _on (arrow points to output); front (output): redirector_front / _on; other 4 faces: redirector_side (target rings) | idle/emitting; aimed on placement (no click-rotate) |
| `pulsar` | **top+side+front+back** | top: pulsar_top (+ pulsar_top_firing strip, 6 frames); front: pulsar_front; back: pulsar_back; left+right (clock ports): pulsar_side; bottom: pulsar_side | idle/firing animation |
| `memory_cell` | **top+side+front+back** | top: memory_cell_top_empty (0) / memory_cell_top (neutral base, value via masks/memory_cell_value_mask.png); front (output): memory_cell_front; back (input): memory_cell_back; left/right/bottom: memory_cell_side | 0 empty, 1-64 stored; aimed on placement |
| `fast_latch` | **all-same** | all faces: fast_latch_off / fast_latch_on | off/on |
| `switch` | **top+side** | top: switch_top_off / switch_top_on; sides+bottom: switch_side | off/on |
| `button` | **top+side** | top: button_top / button_top_pressed; sides+bottom: button_side | up/pressed |
| `keyboard` | **top+side+front+control** | top: keyboard_top (keycaps); front (output, SIXBIT codes): keyboard_front; RIGHT side (control codes): keyboard_control; left/back/bottom: keyboard_side | SIXBIT code pulses out the front (s = code+1); control code pulses out the right (s = code: 1 Enter, 2 Backspace) |
| `color_lamp_16/32/64` | **all-same** | all faces: color_lamp_<N> (lit, shader-tinted) / color_lamp_<N>_off | 0 off, 1-64 wrap/direct |
| `silicon_slab / stone_slab / dirt_slab` | **top+side (half)** | top+bottom: <name>_slab_top (= full texture); sides: <name>_slab_side (lower half opaque, upper half transparent) | bottom slab |
| `tone_block` | **top+side+front+back** | top: tone_block_top / tone_block_top_playing; front (speaker grille): tone_block_front / tone_block_front_playing (anim strip); back (input = pitch): tone_block_back; left/right/bottom: tone_block_side | s0 silent; s1-64 = 64 semitones from C2 (s1 C2, s13 C3, s25 C4, s37 C5, s49 C6, s61 C7, s64 D#7); aimed on placement |

Port language (the same on every block): a **filled** socket or tab = output, a **hollow** ring = input, a **violet diamond** = thresholder reference input (left = LOW: down chevron + floor bar; right = HIGH: up chevron + ceiling bar), an **orange square** = keyboard control port, a **cyan diamond** = pulsar clock input, and a **yellow** arrow/port = powered.

**Placement / aiming:** every directional block (repeater, thresholder, pulsar, memory_cell, redirector, keyboard) faces the direction it's placed in. There's no click-to-rotate. Thresholder: one side powered = threshold (output when input ≥ reference); both sides powered = **window comparator** (output when LOW ≤ input ≤ HIGH). Left is marked LOW and right HIGH; the game sorts the two values itself, so swapping them still works. Keyboard: SIXBIT codes come out the front (s = code + 1), and control codes come out the **right** side (s = code: 1 Enter, 2 Backspace, 3–8 reserved for Up/Down/Left/Right/Tab/Escape, 9–64 reserved). The memory cell takes input at the back and outputs at the front.

### Shader-driven states (one texture plus a mask instead of N textures)
- Repeater delay 1–8: `masks/repeater_delay_mask.png`. R = pip index × 28 (pips 1–8, left to right), G = coverage. A pip is lit (#FFD36B) when its index ≤ delay. Baked delay1–8 references are in `reference/batch2/`.
- Memory cell value 1–64: `masks/memory_cell_value_mask.png`. R = cell index × 4 − 1 (cells 1–64, row-major from top-left), G = coverage. Cell i is lit (#8FD6E8) when i ≤ value. A value of 0 uses `memory_cell_top_empty.png`.
- GLSL for both is in `docs/batch2_shaders.frag`.

### Animation
`blocks/pulsar_top_firing.png` is a 6-frame vertical strip with the sidecar `pulsar_top_firing.png.json` (50 ms per frame, plays once per pulse, then the block goes back to `pulsar_top.png`).

### Wire brightness floor
Every wire color has the same dark outline (6/128 per side). Any powered wire (s ≥ 1) is at least as bright as the old strength-32 color for red and strength-48 for blue, so a strength-1 wire stays traceable from a distance, including blue on gray silicon. Strength 0 (unpowered) is unchanged. The floor is baked into `palettes/red_wire.png` and `palettes/blue_wire.png`: pixel x = s shows ramp(F + (s−1)·(64−F)/63), where F = 32 for red and 48 for blue. The exact strength is still shown by the C4 ring. If you edit those palettes, keep pixel 1 clearly brighter than pixel 0.

### Slabs
`<name>_slab_top.png` is a full tile and is also used for the bottom. `<name>_slab_side.png` has the lower half opaque and the upper half transparent (straight alpha).

### Removed in art v1.6
`ui/icons/lamp_color.png` (the legacy v1 64-color lamp icon) was removed. Use `ui/icons/color_lamp_16.png`, `color_lamp_32.png` and `color_lamp_64.png`.

### Open questions
None: all batch-2 questions were closed on Oct 9 (see `docs/batch2_spec_v3.md`).

### tone_block (art v1.8)
A speaker/synth block. The back input sets the pitch: s = 0 is silent, and s = 1–64 plays 64 semitones starting at C2 (s1 = C2 65.41 Hz, s13 = C3, s25 = C4, s34 = A4 440 Hz, s64 = D#7 ≈ 2489 Hz). It's directional with repeater-style ports, aimed on placement, and the front is the speaker grille. While a tone plays, the engine shows `tone_block_top_playing` and the animated `tone_block_front_playing` strip (4 frames, 100 ms, looping).

### Branding (`textures/branding/`, not tile-sized)
`lithocraft_icon_{dark,light}_{64,256}.png`, `lithocraft_lockup_{dark,light}.png`, `lattice_mark_{color_dark,color_light,mono_white,mono_dark}.png` and `powered_by_lattice_{dark,light}.png`. The previous-name files `voxelcompute_icon_*` and `voxelcompute_lockup_*` are kept. Full sets (every size, .ico, SVG) are in the art repo under `logo/lithocraft/` and `logo/lattice/`.

## Batch 3 components (art v2.1, Oct 10 2026; names chosen from existing conventions, pending confirmation)
Same orientation rules as batch 2: front (output) = top edge of `*_top`, back (input) = bottom edge, faces without their own file use `*_side`.

| Block (id) | Face layout | Files | States |
|---|---|---|---|
| `catchor` (`lithokraft:catchor`) | **top+side+front+back** | top: catchor_top / catchor_top_firing (2-digit 7-seg readout; digits via masks/catchor_readout_mask.png); front (output): catchor_front / catchor_front_firing; back (input): catchor_back; left/right/bottom: catchor_side | idle / firing (exact match); setting 0-64 shown on the readout; aimed on placement |
| `redirector` (update) | **top+side+front** + overlays | split-mode top: redirector_top_split; split front: redirector_front_split / _red_on / _blue_on / _both_on; output overlays: redirector_overlay_{red,blue}_{n,e,s,w} (lit) and _off | each colour's output direction cycles separately (click); red lane = left of centre, round tip; blue lane = right, diamond tip |
| `red_via`, `blue_via` (`lithokraft:red_via`, `lithokraft:blue_via`) | **top+bottom+side, connected sides** | top: <c>_via_top; bottom: <c>_via_bottom; sides: <c>_via_side (= _side_single) or connected <c>_via_side_single / _side_top / _side_mid / _side_bottom; core masks: masks/<c>_via_core_mask_top.png, masks/via_core_mask_side.png | static colour core; optional strength tint through the core masks |
| `glass`, `frosted_glass`, `ceramic` | **all-same** | glass (transparent), frosted_glass (translucent), ceramic (opaque) | wire-carrying, never powered |
| `pattern_rom` + `pattern_{circles,checkers,spheres,cells,delaunay}` | **top+side+front+back** | top: pattern_rom_top / pattern_<family>_top; side: pattern_rom_side; front (row out): pattern_rom_front; back (clock in, cyan diamond): pattern_rom_back | (lower priority, preview art) |

Connected via sides: within a vertical run of the SAME via colour, the lowest block uses `_side_bottom`, the highest `_side_top`, everything between `_side_mid`, and a lone block `_side_single`. `_side_mid` tiles seamlessly top-to-bottom. Engines without connected textures can use `<c>_via_side` everywhere.
Wire on glass: the wire overlay textures are unchanged and stay neutral grey (shader-tinted); the glass itself is never tinted or lit.
