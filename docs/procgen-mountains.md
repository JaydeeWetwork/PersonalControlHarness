# Mountains in procgen (2026-10-11, workstream 1)

Per-biome mountain relief for the heightmap terrain system: mountain ranges with
foothill falloff and cone peaks, parameterized per biome, blended smoothly at biome
borders, fully seeded/deterministic.

## Design choice

Extended the **NoiseGraph node table** (`mountain_range`, `peak`) instead of adding a new
`ITerrainStage`. `BiomeHeightmapTerrain` already evaluates one graph output per biome and
weight-averages heights across biome borders — so each biome entry just names an extra
**uplift** output that is scaled and added to its base height *before* blending. No new
stage, no pipeline changes, no duplicated selection/blending logic. A new stage would have
reimplemented the per-biome weight averaging to avoid cliffs; the entry extension reuses it.

## New graph nodes (engine/src/procgen/graph_nodes.cpp)

`mountain_range` — uplift from a ridged [0,1] field gated by a foothill mask:

```
out = amplitude * max(ridges, 0)^power * smoothstep(mask_low, mask_high, mask)
```

- inputs: `ridges` (required), `mask` (required — pass `1` to disable foothill gating)
- params: `amplitude` (default 1), `power` (default 1, >= 0),
  `mask_low` (default 0), `mask_high` (default 1, must be > `mask_low`)
- Typical `ridges`: `{"type": "noise", "noise": "opensimplex2s",
  "fractal": {"type": "ridged", "octaves": 4}, "frequency": 0.008}` (ridged fbm is [0,1]).
- Typical `mask`: low-frequency fbm scaled into [0,1]; where it is low the uplift fades to
  zero (foothills), where high the full relief applies.

`peak` — cone from a distance field (e.g. cellular F1):

```
out = amplitude * max(0, 1 - distance / radius)^sharpness
```

- inputs: `distance` (required)
- params: `amplitude` (default 1), `radius` (default 1, must be > 0),
  `sharpness` (default 1, >= 0; 1 = straight cone, higher = narrower summit)

Both nodes are pure functions of their inputs; seeding/determinism flows through the
graph seed like every other node.

## Per-biome wiring (generator JSON)

`terrain.biomes[]` entries for `biome_heightmap` accept two optional keys:

```json
"terrain": {"type": "biome_heightmap", "depth": 4, "biomes": [
  {"output": "h_plains", "material": "turf"},
  {"output": "h_base", "material": "stone",
   "uplift": "mountain_ranges", "uplift_amplitude": 64},
  {"output": "base", "material": "basalt",
   "uplift": "volcanic_peaks", "uplift_amplitude": 96}]}
```

- `"uplift"`: name of a graph output (usually a `mountain_range`/`peak` node with
  `amplitude: 1`; omit for flat biomes = character "none"). Unknown names are a load error.
- `"uplift_amplitude"`: scales the uplift output into blocks (default 1). This is the
  per-biome amplitude knob the designer tunes; keep the node amplitude at 1 and share one
  uplift subgraph across biomes at different amplitudes. Must be finite; `0` == no uplift.
  Requires `"uplift"` (loader rejects it alone).
- Character vocabulary: none (omit `uplift`), hills (gentle fbm uplift output),
  ranges (`mountain_range` output), peaks (`peak` output) — the biome picks its character
  by which uplift output it references.

Heights stay smooth at borders: uplift is added per biome before the existing
weight-averaged blending, so a mountain biome next to plains ramps down over the blend
band. One-hot (unblended) weights give the full step — the only way to ask for a cliff.

## Example graph fragment

```json
"ridge_field": {"type": "noise", "noise": "opensimplex2s", "frequency": 0.008,
                "fractal": {"type": "ridged", "octaves": 4}},
"range_mask": {"type": "scale_bias", "scale": 0.5, "bias": 0.5,
               "input": {"type": "noise", "noise": "opensimplex2s", "frequency": 0.0016}},
"mountain_ranges": {"type": "mountain_range", "amplitude": 1, "power": 1.5,
                    "mask_low": 0.35, "mask_high": 0.7,
                    "ridges": "ridge_field", "mask": "range_mask"},
"peak_dist": {"type": "noise", "noise": "cellular",
              "cellular": {"return": "f1"}, "frequency": 0.01},
"volcanic_peaks": {"type": "peak", "amplitude": 1, "radius": 0.7, "sharpness": 2,
                   "distance": "peak_dist"}
```

## Tests

`mountainChecks` in `engine/src/procgen/procgen_selftest.cpp` (runs under ctest
`engine_procgen`): node math unit checks (16 / 32 / 0 / 54 / 0 / 96), factory rejections,
loader rejections, determinism (two independent loads, byte-identical), known-seed spot
heights, amplitude linearity (full ≈ 2× half, ±1 floor tolerance), zero-amplitude ≡
omitted uplift, border continuity (max adjacent step 2 ≤ 8 over 128 columns crossing
chunk borders), relief exists (range 34 ≥ 30), seed sensitivity.

## Decisions for JD (game designer) to lock

1. **Amplitude convention**: node `amplitude` stays 1, per-biome `uplift_amplitude` (blocks)
   is the tuning knob. Confirm.
2. **Foothill gating**: `mountain_range` requires a `mask` input (pass `1` to disable).
   Fine as-is, or want the mask optional in the node schema?
3. **Real biome numbers**: test used ranges=64 / peaks=96 blocks on a 256-high world;
   Wafer's actual amplitudes, frequencies, mask windows, and which biomes get uplift are
   still open.
4. **Cliffs**: only possible with one-hot biome weights at a border. If any biome pair
   should read as a wall (e.g. plateau edge), that is a blend-selector setting, not a
   mountain setting.
