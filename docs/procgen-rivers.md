# Rivers (procgen carver)

Noise-driven rivers carved into the heightfield. `RiverCarver` (`engine/src/procgen/stages.h`,
`stages.cpp`; JSON type `"river"` in a generator's `"carvers"` list).

## How it works

1. **Origins.** A coarse lattice of origin cells (`cell_size`, default 256 blocks) covers the
   world. Each cell rolls `chance` (default 0.5, times the per-biome `density`) to spawn one
   river. The start point is the highest of 4 seeded candidates in the cell, so rivers begin
   in the hills. Origins at/below `min_start_height` (default 8) are rejected.
2. **Downhill walk.** From the start, the river follows the descending gradient of a
   *routing height* sampled from the graph (`routing_output`, default `"height"`), one
   `step_len`-block step (default 4) at a time, up to `max_steps` (default 64) steps:
   - momentum smoothing (`momentum`, default 0.3) plus a perpendicular meander
     (`meander` amplitude in blocks, default 2; `meander_freq` radians/step, default ~2π/32).
   - **Never flows uphill:** each step must satisfy `newH <= h + uphill_tol` (default 0.5);
     on a violation the walk retries deflected ±30°/±60°/±90°, then the river ends.
   - Stops at flat ground (`|gradient| < min_slope`, default 0.02), at the `water_table`
     (default 0 — the river drains into the sea), or boxed in by higher ground.
3. **Chunk purity.** The walk is a pure function of `(dimensionSeed, origin cell)` — seeded
   RNG plus graph samples only. Every chunk replays the origin cells within reach
   (`max_steps * (step_len + meander) + width`, in cells, plus one cell margin) and only
   ever writes its own columns, exactly like `WormCaveCarver` replays neighbouring worms.
   Same seed → same rivers, always; generating a chunk alone or with neighbours gives
   identical river cells.
4. **Carving.** The channel is cut with a cosine cross-section: `width` (default 4, half-width
   in blocks), `depth` (default 4, below the routing height at the centerline). Cosine banks
   are sloped by construction — no vertical slots. The carve takes
   `min(actual terrain, channel profile)`, so rivers never raise terrain. The channel floor
   is optionally re-materialed (`bed_material`, default: keep terrain). `maps.height` is
   updated for carved columns.
5. **Water.** The channel fills with the `water_material` up to `freeboard` (default 1)
   below the routing height. Water never rises above the column's pre-carve surface — it
   stays in the channel. Every water cell is emitted at fluid level 16 (source); see the
   fluid seam below.

## JSON

```json
"carvers": [{"type": "river",
  "water_material": "water", "bed_material": "gravel",
  "routing_output": "height",
  "cell_size": 256, "chance": 0.5, "max_steps": 64, "step_len": 4,
  "width": 4, "depth": 4, "freeboard": 1,
  "meander": 2, "meander_freq": 0.19635, "momentum": 0.3,
  "min_slope": 0.02, "uphill_tol": 0.5,
  "water_table": 0, "min_start_height": 8, "max_start_height": 1e9,
  "biomes": {"ocean": 0.0}
}]
```

- `water_material` (required): resolved through the generator's `MaterialResolver`.
- `biomes`: per-biome spawn multiplier, default 1.0 for every biome. The biome is probed
  at the river's start point from the finished pipeline (`onPipelineBuilt`, like
  `StructureFeature`); dimensions without a biome selector probe biome 0.
- All dimensions configure their own carver instance: presence, width, depth and meander
  are per-dimension by construction.

## Routing-output caveat (read this before wiring a dimension)

The walk routes on **one** graph output, but the real terrain may blend several
(`BiomeHeightmapTerrain` picks a height output per biome). The generator JSON should expose
a representative routing output — usually the same `"height"` the terrain uses, or a
smoothed variant. Where routing height and terrain disagree, the carve takes the minimum,
so the failure mode is a locally shallower channel, never floating water or raised
terrain. The bank-shape self-test (`kRiverBankText`) covers the extreme case: flat terrain
with a fully independent sloped routing output.

## Fluid seam (worldgen → fluid sim)

Worldgen has no other fluid-emission path yet, so rivers define the convention:

- Water cells are written as the `water` **material id** with the fluid level in the block
  **state byte's value bits** (`stValue(state)` = level, 1–16 per `world/fluid.h`).
- Worldgen emits level **16** (source, `kMaxFluidLevel`) for every water cell in the
  channel. `ColumnGenerator` already copies the state byte into chunk storage
  (`column_generator.cpp`).
- The world layer maps the `water` material id to its water `BlockDef` (via the same
  `MaterialResolver`, e.g. the `"water"` texture key) and reads the level back out of the
  state byte into `FluidState{FluidType::kWater, level}`.
- The `IFluidSim` settles and tapers levels at runtime; worldgen hands it a fully
  source-filled channel, which is the correct initial condition.

## Tests

`riverChecks` in `engine/src/procgen/procgen_selftest.cpp` (runs under `ctest -R procgen`
via `litholattice_tests procgen`): loader accept/reject cases, `traceRiver` determinism,
no-uphill-flow (sampled elevations monotone within `uphill_tol`), full-pipeline seed
determinism, water placement (level 16, only in carved columns, at/below the pre-carve
surface, bed re-materialed), chunk purity (alone vs with neighbours), and bank shape on
flat terrain with an independent routing output (max adjacent carved step ≤ 3, no isolated
carved columns).
