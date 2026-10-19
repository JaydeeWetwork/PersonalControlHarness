# Procedural generation (`procgen`)

`src/procgen` is the world generation framework: noise, a JSON node graph, and a staged generator pipeline. It depends
on **core only**: no blocks, chunks or GL. The world module plugs it in (`world/column_generator.h`), naming materials
by block texture key. LithoKraft's Wafer and Organic World are both defined as generator files in `LithoKraft/assets/worldgen/`.

```
procgen (core)                              world                          game / app
noise.h, modifiers.h   -> graph.h  -> pipeline.h, stages.h  -> ColumnGenerator -> Dimension   ResourceManager (packs)
seed.h                              generator_loader.h        (ChunkColumn)                  --seed, --noise-preview
```

## Noise (`noise.h`, `modifiers.h`)
Every noise is an immutable, seeded `INoise` with `sample2 / sample3 / sample4` and `sampleBatch(dims, PointsView, out)`.
`PointsView` is structure-of-arrays (`x[]`, `y[]`, `z[]`, `w[]` spans) so a type can add an SSE/AVX path later without
API changes; every type uses the scalar loop today. All methods are const and thread-safe.

| Type (JSON name) | Notes | Range |
|---|---|---|
| `perlin` | Improved Perlin (2002): quintic fade, seeded permutation; 2D 8 / 3D 12 (+4) / 4D 32 gradients | [-1, 1] |
| `opensimplex2f` | Our own OpenSimplex2-style noise, fast variant: 2D triangle lattice (3 vertices), 3D rotated BCC lattice (two cubic lattices offset by ½), 4D simplex lattice (5 vertices) | [-1, 1] |
| `opensimplex2s` | Smooth variant: same lattices, larger kernel radius, every vertex in reach (2D r² ⅔, 3D r² ¾, 4D r² ¾) | [-1, 1] |
| `value` | Lattice values, cubic smoothstep (C1) | [-1, 1] |
| `cellular` | Worley: one jittered point per cell; `f1`, `f2`, `f2-f1`; `euclidean`, `manhattan`, `chebyshev`; `jitter` 0..1. The search widens past the 3^d neighbourhood only when a farther cell could still be closer, so results are exact and continuous | [0, bound] (raw distance; bound from `range()`) |
| `white` | Hash of the cell (floored coordinates) | [-1, 1) |

The OpenSimplex2 code is written from the published idea (lattice choice, kernel `(r² − d²)⁴ · dot(g, d)`), not ported
from any implementation. The 3D form uses the BCC lattice, which sidesteps the (expired, 2022) simplex-noise patent's 3D
claims; 4D uses the simplex lattice. Output scales were measured empirically so extremes land near ±1, then clamped.

Modifiers wrap any noise: `createFractal(source, {fbm|ridged|billow|turbulence, octaves, lacunarity, gain}, seed)`
(octaves shifted by seed-derived offsets; fbm/billow stay in [-1, 1], ridged/turbulence in [0, 1]) and
`createDomainWarp(source, warp, amplitude, frequency)`.

## Seeding (`seed.h`)
`mix64` (SplitMix64 finalizer), `deriveSeed(base, salt)`, `nameHash` (FNV-1a), `latticeHash`, and `Rng` (xoshiro256**).
- `dimensionSeed(worldSeed, "organic")`: every dimension gets independent noise from one world seed (`--seed n`).
- `chunkSeed(dimensionSeed, cx, cz)`: per-chunk randomness (carvers, features).
- Graph noise nodes: `deriveSeed(dimensionSeed, node "seed" or nameHash(node name))`.
- Features: `deriveSeed(chunkSeed, nameHash(feature name))`, so adding a feature does not shift the others.
Nothing depends on thread scheduling or generation order; the selftest compares serial and JobPool runs.

## Node graph (`graph.h`)
A graph has named nodes and named outputs. Inputs are a node name, a number (constant) or an inline node object.
Loading normalizes the description (`GraphDesc`): inline nodes are hoisted into named nodes (`<parent>/<input>`),
so `GraphDesc → JSON → GraphDesc` is lossless (selftest: dump → parse → dump is identical and evaluates identically).

```json
{
  "nodes": {
    "base":   {"type": "noise", "noise": "opensimplex2s", "frequency": 0.01, "fractal": {"type": "fbm", "octaves": 5}},
    "height": {"type": "scale_bias", "input": "base", "scale": 20, "bias": 64}
  },
  "outputs": {"height": "height"}
}
```

| Node | Inputs | Parameters |
|---|---|---|
| `constant` | – | `value` |
| `coordinate` | – | `axis` (`x`/`y`/`z`) |
| `wave` | – | `axis` or `direction` [x, y, z], `function` (`sin`/`cos`), `frequency`, `amplitude`, `phase` |
| `noise` | – | `noise` (type), `seed`, `frequency` (number or [x, y, z]), `offset` [x, y, z] (noise space), `dims` (2 = x/z plane, 3, 4), `w` (4D), `fractal` {type, octaves, lacunarity, gain}, `warp` {noise, amplitude, frequency, fractal}, `cellular` {return, metric, jitter} |
| `add`, `multiply`, `min`, `max` | `inputs` [...] | – |
| `clamp` | `input` | `min`, `max` |
| `curve` | `input` | `points` [[x, y], ...] (increasing x), `interpolation` (`linear` / `cubic` = monotone, no overshoot) |
| `select` | `control`, `a`, `b` | `threshold`, `falloff` (smoothstep blend over threshold ± falloff) |
| `blend` | `a`, `b`, `t` | – (t clamped to 0..1) |
| `terrace` | `input` | `step`, `smoothness` (0 = hard steps) |
| `scale_bias` | `input` | `scale`, `bias` |
| `cache` | `input` | – (keeps its values while the same points are re-bound) |

Validation rejects unknown types, unknown parameters (typos), missing inputs, unknown references and cycles, with the node
name in the message. Evaluation: `NoiseGraph::compile(desc, seed)` builds immutable nodes; `evaluate(output, ctx, out)` runs
a batch over the points bound to a per-thread `GraphContext`. Each node is computed once per binding, so several outputs
over the same points (climate + height) share their common nodes.

## Generator pipeline (`pipeline.h`, `stages.h`, `generator_loader.h`)
Per chunk column: **climate → biome selection → terrain → surface rules → carvers → features**.
`GeneratorPipeline::generate(ChunkInfo)` is const and thread-safe and returns a `ColumnBuffer` (material ids, 0 = air,
plus a state byte) and optionally the `ColumnMaps` (height, climate, biome per column).

| Stage interface | Built-in | Status |
|---|---|---|
| `ITerrainStage` | `HeightmapTerrain` (graph output → surface, `depth` blocks thick), `DensityTerrain` (3D: solid where output > 0) | done |
| `IClimateStage` | `GraphClimate` (temperature, humidity, continentalness, erosion from graph outputs; missing = `fallback`) | done |
| `IBiomeSelector` | `MultiNoiseBiomeSelector` (nearest climate point; only listed parameters count) | done |
| | `BlendedBiomeSelector` (Minecraft-style gradient: `blendCount` nearest biomes, softmax over the distance gap to the nearest, `blendRadius` = softness scale) | done |
| `ISurfaceRule` | `LayeredSurface` (top + filler per biome; optional top state = biome index) | done |
| | `OceanFillRule` (ocean water for ocean biomes: noise floor relief, flat fill to `sea_level`, configured depth -- default 196, a settings value not a cap) | done |
| | `LakeFillRule` (still-water lakes in depressions below `water_table`, flat spill-level fill, configured depth -- default 98, a settings value not a cap) | done |
| | `ShorelineRule` (sand/gravel-ish rim around lake + ocean water, after the game's surface rules) | done |
| `ICarver` | `WormCaveCarver` (cross-chunk worms replayed from every chunk in reach, keeps `surface_margin` below the surface) | example |
| | `RavineCarver` | stub (carves nothing) |
| `IFeature` (+ `FeatureRegistry`) | `OreVeinFeature` (random-walk veins replacing one material, inside the chunk) | example |
| | `DungeonSlot` | placeholder (places nothing) |
| | `TraceTreeFeature` (via-like trunk 4-7, 2-4 forking trace branches, red/blue glowing tips; per-biome density) | done |
| | `CapacitorCanFeature` (3-7 squat cans 2-3 tall in radius-2 clusters; per-biome density) | done |
| | `ResonatorSpireFeature` (crystal spike 8-16 tall, 1-2 wide; per-biome density) | done |

Generator file (`worldgen/<dimension>.json` in a pack; LithoKraft ships `LithoKraft/assets/worldgen/`); materials are block texture keys (`stone`, `dirt`, `grass`,
`silicon`, ...), `air`, or namespaced block ids (`lithokraft:trace_trunk`) -- needed when several blocks share one texture key (Wafer biome blocks reuse art):

```json
{
  "graph": {"nodes": {...}, "outputs": {"height": "...", "continentalness": "..."}},
  "terrain": {"type": "heightmap", "output": "height", "material": "stone", "depth": 0},
  "climate": {"continentalness": "continentalness"},
  "biomes": [{"name": "plains", "climate": {"continentalness": 0.25},
              "surface": {"top": "grass", "filler": "dirt", "depth": 3}}],
  "surface": {"top_state": "biome"},
  "carvers": [{"type": "worm_cave", "chance": 0.3, "min_y": 10, "max_y": 50, "surface_margin": 8}, {"type": "ravine"}],
  "features": [{"name": "dirt_veins", "type": "ore_vein", "material": "dirt", "replace": "stone", "count": 6, "size": 14}]
}
```
(`density` terrain takes `min_y` / `max_y`; worm caves also take `min_length`, `max_length`, `min_radius`, `max_radius`.)

### Lakes and oceans (`water.h`)

Water runs as surface rules, in fixed order after the JSON `surface` rules (and any game
`extraSurface` rules): **oceans → lakes → shoreline → carvers → features**.

```json
{
  "oceans": {
    "sea_level": 256, "max_depth": 196, "water": "water", "floor": "sand",
    "depth_frequency": 0.00390625,
    "density": 0.0, "biomes": {"ocean": 1.0}
  },
  "lakes": {
    "water_table": 256, "max_depth": 98, "water": "water",
    "threshold": 0.62, "frequency": 0.015625,
    "density": 0.0, "biomes": {"plains": 0.8}
  },
  "shoreline": {
    "band": 2, "depth": 2,
    "default": {"top": "sand", "filler": "sand", "depth": 2},
    "biomes": {"plains": {"top": "sand", "filler": "gravel", "depth": 2}}
  }
}
```

- **Oceans** (`OceanFillRule`): for ocean-eligible biomes (per-biome `biomes` table, 0..1,
  weight-blended across biome borders), the ocean floor is the eligibility-blended mix of
  the existing terrain height and a large-scale noise floor target
  (`sea_level - oceanDepth`, `oceanDepth` in `[maxDepth/4, maxDepth]`), so shores grade
  smoothly into the weight-averaged height field -- no cliffs at biome borders. Water fills
  flat to `sea_level`. Where terrain lies deeper than the configured depth allows, the floor is raised
  (`floor` material, default: the column's own surface block) so the surface stays flat.
  Columns reaching `sea_level` stay dry (islands). Depths are settings, not engine
  caps (parameterized 2026-10-13): a generator may set any positive `max_depth`, an
  optional `min_depth` (omitted = derive `max_depth/4`, the historical behaviour), and
  `min_solid_below` (default 60). The loader's only check is sanity in those same
  terms: `sea_level - max_depth` must leave >= `min_solid_below` solid layers above
  y 0. LithoKraft ships 196 / 98 / 60 as data in its generator JSON, so at its values
  the old 256 - 196 = 60 relationship holds as a consequence of the chosen data.
- **Lakes** (`LakeFillRule`): a column holds lake water when it is biome-eligible, its
  terrain lies below `water_table`, and the smooth lake mask (`(fbm + 1) / 2 > threshold`,
  eligibility-blended) covers it -- water appears in terrain depressions inside lake-prone
  regions, filled flat to the water table (the spill level). Liquid depth is limited
  to the configured `maxDepth` (default 98, an independent settings field): deeper
  pits hold exactly `maxDepth` of water with air above. Solid
  terrain is never modified, only air becomes water. Where both claim a column, lakes run
  after oceans and win.
- **Shoreline** (`ShorelineRule`): after the surface rules, columns whose solid top lies in
  `[surface - depth, surface + band]` around a lake/ocean water surface get top + filler
  rewritten per biome (`default` + `biomes` overrides; air = leave alone). Water cells are
  never touched. Pure per-column function of (height, biome, region masks): seamless
  across chunk borders.
- **Determinism**: every decision is a pure function of (dimension seed, world x, z) --
  same seed, same water, independent of chunk borders and thread order.
- **Fluid seam**: water is an ordinary material id (the `"water"` block from the game's
  registry; the section fails to load without one). Each water block's state byte carries
  the fluid level; worldgen always writes 16 (source, still, never falling). The sim reads
  it via `stValue(cellState)` (`world/block.h`) and identifies water by block id.

### Biome blending and decoration density

Biome selectors fill `ColumnMaps::biome` (dominant biome) and, when they blend, `ColumnMaps::biomeWeight`
(row-major `columns x biomeCount` weights, usually summing to ~1) via `IBiomeSelector::blendWeights`.
The built-in selectors: `multi_noise` (hard nearest, one-hot weights) and `blended`
(Minecraft-style gradient: softmax over the climate-space distance gap to the nearest biome,
`blend_count` nearest kept, `blend_radius` = softness scale -- smaller = crisper interiors):

```json
"biomes": {"selector": {"type": "blended", "blend_radius": 0.3, "blend_count": 3},
           "list": [{"name": "trace_forest", "climate": {"continentalness": 0.1}}, ...]}
```

Feature density scales with the weight vector: effective density at a column = sum over biomes of
`density[biome] * weight[biome]`, evaluated against the same seeded per-position dither as
everything else (`latticeHash(featureSeed, x, z)`), so a tree eligible in Trace Forest at weight
0.3 gets ~30% of its full density there, deterministically. Feature JSON gives per-biome
densities as `"density"` (default) plus `"biomes": {"name": density}`` overrides:

```json
{"name": "trace_trees", "type": "trace_tree", "trunk": "lithokraft:trace_trunk",
 "branch": "lithokraft:trace_branch", "tip_red": "lithokraft:trace_tip_red",
 "tip_blue": "lithokraft:trace_tip_blue", "surface": "silicon",
 "red_tip_chance": 0.15, "density": 0.004, "biomes": {"trace_forest": 0.06}}
```

Cross-boundary structures use a deterministic overlap margin: each chunk replays its neighbours'
border structures from the same position-keyed seeds and only writes its own columns, so
neighbouring chunks agree without neighbour reads at gen time. Out-of-chunk bases reuse the
nearest in-chunk column for biome weights and surface height (exact for the owning chunk;
a rare, bounded approximation at simultaneous chunk+biome borders).

The `structure` feature type places above-ground structures (ruins, monuments) from a block
blueprint. Candidate bases live on a coarse cell lattice (at most one per cell, so structures
keep their spacing); the base inside the cell comes from a cell-keyed RNG, and the biome gate
uses the dominant biome at the exact base position, probed from the finished pipeline -- so
`StructureFeature::basesInRect()` (used by overview maps) replays exactly what chunk gen
places. Structures sit on the surface and only ever write into air (never overwrite terrain).
Example:

```json
{"name": "transformer_ruin", "type": "structure", "cell_size": 64, "density": 0.08,
 "biomes": {"dead_zones": 1.0, "trace_forest": 0.3}, "rotate": true,
 "blueprint": [{"d": [0, 0, 0], "m": "lithokraft:dead_silicon"},
               {"d": [1, 0, 0], "m": "lithokraft:dead_silicon", "decay": 0.3}]}
```
(`cell_size`: lattice spacing, at most one base per cell; `density`: per-cell probability;
`biomes`: per-biome gate multipliers on the dominant biome at the base, default 1;
`snap`: base x/z snapped to multiples of this (die-grid alignment, 0 = off);
`rotate`: rotate the blueprint 0/90/180/270 by the cell seed; `surface`: required top block
under the base, omitted = any solid; `blueprint`: list of `{d: [dx, dy, dz], m: material}`
with dy >= 0 from the first air above the surface, optional `decay` per-cell skip chance.)

The `ruins` carver buries the old machine's corpse in the crust: fossilized circuit traces (orthogonal
Manhattan routes, 8-24 blocks, with missing segments), capped broken via stubs that never reach the
surface, and thin delaminated bands where the crust alternates materials. All placement replaces crust
1:1 inside the generating chunk (chunk-seed deterministic); nothing is placed within `surface_margin`
of the surface. Example:
```json
{"type": "ruins", "trace": "lithokraft:fossil_trace", "via": "lithokraft:broken_via",
 "crust": ["silicon", "stone", "dirt"], "delam": ["stone", "dirt"],
 "density": {"Trace Forest": 2.0, "Dead Zones": 2.5, "Dicing Plains": 0.35}}
```
(materials resolve by namespaced block id first, then texture key; `density` maps biome names to
multipliers, default 1. Also: `depth_min`/`depth_max`, `traces_per_chunk`, `trace_min_len`/
`trace_max_len`, `gap_chance`, `void_chance`, `vias_per_chunk`, `via_min_h`/`via_max_h`,
`via_trace_chance`, `delam_chance`, `surface_margin`.)

## Packs, built-ins and threading
- `ResourceManager` reads `worldgen/wafer.json` and `worldgen/organic.json` from the pack stack (highest pack wins); the
  game pack is `LithoKraft/assets/`. The game's `createDimensions(WorldgenSettings)` builds each dimension's generator from
  the pack file, or, if it is missing or invalid (logged), from the copy embedded in the exe at build time
  (`cmake/embed_text_files.cmake`). F9 re-reads the files, but generators only change for worlds created afterwards.
- `Dimension::load` runs `ColumnGenerator::generate` for every column on the existing `JobPool`; one shared, immutable
  pipeline serves all workers.

## Debug tool
`LithoKraft --noise-preview <graph.json> [--preview-out f.png] [--preview-output name] [--preview-size 512]
[--preview-scale blocks/px] [--preview-y 64] [--seed n]` writes a grayscale PNG (min..max → black..white, range logged)
without opening a window. It accepts a bare graph or a generator file and uses output `height` (or the first output).

## Selftests (`--selftest`, `procgen_selftest.cpp`)
Every noise type, cellular variant, fractal and domain warp in 2D/3D/4D: finite, inside its range, uses its range, and
continuous (bounded finite-difference step); seeds reproduce and differ; batch == scalar. Graph: every node type parses,
JSON round trip is lossless and evaluates identically, cache, errors (unknown type / parameter / input / output, cycles).
Biome selector cases. Pipeline: chunks identical serially and on the JobPool, surface and state follow biomes, caves and
veins appear, distinct dimension/chunk seeds. The app selftest also checks the shipped wafer/organic generators.

## Not done yet
SIMD batch paths; ravines and dungeons (slots only); AngelScript worldgen hooks; streaming
worldgen beyond the fixed world size.
