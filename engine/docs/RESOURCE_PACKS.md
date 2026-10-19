# Resource packs (modding guide)

Everything the renderer draws comes from **resource packs**: textures, animations, palettes,
block models (which texture goes on which face), wire pieces, shader masks and the GLSL shaders.
No texture or face name is hard-coded in C++.

## The pack stack

Packs are folders. The game stacks them, lowest priority first:

| Order | Pack | Where |
|---|---|---|
| 1 | **base** (engine) | `assets/` (`pack.json`, `shaders/`) |
| 2 | **game** (LithoKraft) | `LithoKraft/assets/` (`pack.json`, `ui/theme.json`, `worldgen/*.json`) |
| 3 | **art pack** | `LithoKraft/assets/packs/<name>/`, chosen with `--pack pack_64` (default) or `--pack pack_32` |
| 4+ | **mods, then the player's pack** | each `--resource-pack <folder>`, in command-line order |

Two rules decide what wins:

1. **Definitions** in `resources.json` are merged by name: if two packs define the texture
   `stone` or the model `repeater`, the higher pack's entry replaces the lower one (models are
   replaced whole, not merged face by face).
2. **Files** are looked up from the top of the stack down: the highest pack that contains
   `textures/blocks/stone.png` provides it. So a pack can reskin a block by shipping just the PNG
   with the same relative path, with no JSON at all.

Paths are always relative to the pack folder; absolute paths and `..` are rejected.

Press **F9** in game to reload every pack (textures, models, palettes, animations and shaders);
the log says what changed (`hot reload: 2 changed (repeater_top stone) ...`).

## Folder layout

```
my_pack/
  pack.json              optional: {"name": "My pack"}
  resources.json         optional: definitions (below)
  textures/...           PNG files (any layout; referenced from resources.json)
  shaders/chunk.vert     optional shader overrides (see "Shaders")
  worldgen/organic.json  optional world generator override (see "World generators")
```

## resources.json (format version 1)

```json
{
  "format": "litholattice.resources",
  "version": 1,
  "tile_size": 64,
  "textures":   { "repeater_top": "textures/blocks/repeater_top.png", "...": "..." },
  "animations": { "pulsar_top_firing": { "file": "textures/blocks/pulsar_top_firing.png" } },
  "palettes":   { "red_wire": "textures/palettes/red_wire.png" },
  "models": {
    "repeater": {
      "faces":  { "top": "repeater_top", "front": "repeater_front", "back": "repeater_back", "side": "repeater_side" },
      "active": { "top": "repeater_top_on" },
      "masks":  { "delay": "repeater_delay_mask" }
    }
  }
}
```

Unknown keys, unknown names and bad values are skipped with a warning in the log; they never stop
the game. A JSON syntax error makes the whole pack fail to open (logged with the file and
position) and the stack continues without it.

### tile_size
Every texture must be `tile_size` x `tile_size` pixels (animation strips: `tile_size` wide, a
multiple of it tall). The highest pack that sets `tile_size` decides; a texture of another size
is shown as the missing texture and logged. Packs of different sizes cannot be mixed.

### textures
`name -> file`. Masks (shader lookup images) are ordinary textures. A texture that fails to load
(missing file, wrong size, not a PNG) keeps its name but shows the **magenta/black checker**.
At most 256 layers in total (textures + animation frames).

### animations
`name -> {"file": strip.png, "frametime_ms": 50, "frames": [0, 1, 2], "loop": true}`. The strip is
vertical (frame 0 on top). Timing can also come from a sidecar `<file>.json` (same keys); values
in `resources.json` override the sidecar. `frames` is the play order (default: every frame in
order). The chunk shader plays up to 8 animations with 64 sequence entries in total.

### palettes
`name -> file`. Row 0 of the image, one colour per pixel; index = shader lookup value
(wire strength 0..64, colour-lamp colour index).

### provenance (Litho certificate)
Any file-backed entry (`textures`, `animations`, `palettes`) may carry a stamp:

```json
"stone": { "file": "textures/blocks/stone.png",
           "provenance": { "origin": "litho-generated", "crc32": "0x1234abcd" } }
```

The CRC32 is over the source file's bytes. At load, the engine verifies the stamp:
a match labels the entry litho-generated; a mismatch logs a warning and the entry is
treated as third-party — **loading is never gated by provenance**. CRC32 is a
non-cryptographic label (it tells content apart; it is not the mod-signing PKI).
Scope: file-backed entries only — `models` reference textures by name and carry no
source file, so they cannot be stamped. `.svg` texture files are supported everywhere
`.png` is (rasterized at `tile_size`).

### models
Keyed by the block's texture name (`BlockDef::tex`: `silicon`, `grass`, `repeater`,
`red_wire`, `color_lamp_16`, ...). A block without a model shows the missing texture
(logged as `no block model for '<name>'`).

| Key | Meaning |
|---|---|
| `faces` | base state: `top`, `bottom`, `front`, `back`, `left`, `right`, `side`, `all` |
| `active` | on / lit / pressed / storing state; same keys; each face falls back to `faces` |
| `wire` | wire strips: `dot`, `end`, `straight`, `corner`, `t`, `cross` (missing pieces use `straight`) |
| `palette` | palette used to tint the block (wires, colour lamps) |
| `masks` | shader masks by role (below) |

**Face fallback:** a face without its own entry uses `side`, then `all`, then the missing
texture. This includes `bottom` (give slabs an explicit `"bottom"`).

**Orientation:** faces are relative to the block's facing. `front` is the output side, and on
the top (and bottom) texture the front is the **top edge** of the image; the engine rotates top
textures with the block. Wire pieces are drawn in a canonical orientation and rotated by their
connections: `end` has its arm pointing south (bottom edge), `t` has its missing arm north (top
edge), `straight` runs north-south, `corner` joins north and east.

**Face values** are a texture name, an animation name, or an object:

```json
"top": { "texture": "atlas_tile", "uv": [0, 0, 8, 8] }
"top": { "animation": "pulsar_top_firing" }
```

`uv` is `[u0, v0, u1, v1]` in 1/16 of a tile (0..16; default the whole tile). An animated face
plays its strip whenever that face is shown (put it in `active` to animate only when on).

**Mask roles** read by the chunk shader:

| Block role | `masks` keys | `palette` |
|---|---|---|
| wire (red = channel 1, blue = channel 2) | `ring` (C4 ring segments) | strength ramp, 65 entries |
| colour lamp (tier 16 / 32 / 64) | `main` (lens), `rim` (dark-colour rim) | lamp colours |
| repeater | `delay` (pips 1..8) | - |
| memory cell | `value` (cells 1..64) | - |

## Shaders

The engine's shaders live in the base pack: `assets/shaders/chunk.vert|frag` (world),
`hud.vert|frag` (overlay) and `line.vert|frag` (block outline), GLSL 3.30 core. A pack can
override a program by shipping `shaders/<program>.vert` and/or `.frag` (stages resolve
separately through the stack). If a program fails to compile, or only one stage can be found,
the renderer logs it and uses its built-in copy of the base shader (compiled into the exe from
the same files), so a broken shader never stops the game. Uniform names are the interface: see
the comments in `assets/shaders/chunk.frag`.

Shaders can share code with `#include "file.glsl"` on a line of its own (resolved from
`shaders/` through the stack, so a pack can also override just an include). The base chunk
program keeps its shading in `shaders/chunk_shading.glsl`.

Shader overrides apply to the OpenGL renderer (the default). The Vulkan renderer
(`--renderer vulkan`) uses SPIR-V compiled into the exe from `assets/shaders/vk` at build time,
so pack shader files are ignored there (it logs that once); textures, models, palettes and
animations work the same in both.

## World generators

A pack can replace a dimension's terrain with `worldgen/wafer.json` or `worldgen/organic.json`
(noise graph + stages: climate, biomes, surface, caves, features). Format and node reference:
`docs/PROCGEN.md`; the shipped files in `LithoKraft/assets/worldgen/` are the starting point. Materials are
named by block texture key (`stone`, `dirt`, `grass`, `silicon`, ...). An invalid file is logged
and the built-in generator is used. Generators are read at startup; F9 re-reads them, but they
only apply to worlds created afterwards. Preview a graph without starting the game:
`LithoKraft --noise-preview my_pack/worldgen/organic.json --preview-out preview.png`.

## Converting an art export

The art team's export (`manifest.json` with `block` / `face` / `lit` / `state` / `piece` per
file) is converted, never hand-edited:

```
python tools/convert_pack_manifest.py LithoKraft/assets/packs/pack_64
python tools/convert_pack_manifest.py <export>/pack_64 --out LithoKraft/assets/packs/pack_64
```

It writes `resources.json` next to (or into `--out`) and leaves `manifest.json` and the art
untouched. Its rules (state names, `<block>_<role>_mask` naming, colour-lamp palettes, slab and
grass bottoms) are documented at the top of the script. `.svg` files convert like `.png`;
a `provenance` object on a manifest file entry is passed through onto the generated entry
(the converter never invents a stamp — see "provenance" above).
