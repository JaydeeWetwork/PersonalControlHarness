# Biome → Vegetation Mapping

Workstream 3, 2026-10-11. Code: `biome_vegetation.h`
(`lithokraft::vegetation::kBiomeVegetationTable`, 11 rows).

**Scope: mapping ONLY.** Which family grows where, in which render style,
at what density. Placement integration into worldgen (a decoration stage
that rolls per column and stamps models) is a **later step**, explicitly
not implemented here.

## The table

| Biome | Dim. | In `wafer.json`? | Tree family | Tree density | Shrub (preset) | Shrub style | Shrub density |
|---|---|---|---|---|---|---|---|
| TraceForest | Wafer | ✅ | TraceForestTree | 0.060 | Berry | 3D | 0.100 |
| DicingPlains | Wafer | ✅ | — | 0.000 | DryTuft | 2D | 0.012 |
| CapacitorGroves | Wafer | ✅ | ClockwoodWirewood | 0.045 | Twisted | 3D | 0.060 |
| ResonatorSpires | Wafer | ✅ | — | 0.000 | DryTuft | 2D | 0.020 |
| DeadZones | Wafer | ✅ | — | 0.000 | — | — | 0.000 |
| EtchedPlains | Wafer | ❌ new | — | 0.000 | DryTuft | 2D | 0.025 |
| ClockwoodForest | Wafer | ❌ new | ClockwoodWirewood | 0.085 | Berry | 3D | 0.075 |
| DopantGroves | Wafer | ❌ new | DopantCrystalTree | 0.075 | Twisted | 3D | 0.065 |
| OxideDunes | Wafer | ❌ new | — | 0.000 | DryTuft | 2D | 0.030 |
| StaticMarsh | Wafer | ❌ new | — | 0.000 | MeadowTuft | 2D | 0.110 |
| FabRuins | **Organic** | ❌ new | — | 0.000 | DryTuft | 2D | 0.020 |

Densities are per-column placement probabilities in [0, 1].

## Per-biome notes

- **TraceForest** — the dense circuit-tree biome: copper trunks, red/blue
  glowing tips (the procgen family of the hand-authored `trace_tree.lthm`).
- **DicingPlains** — sparse first-spawn territory; scrub tufts hug the
  die-grid scribe lines. No trees.
- **CapacitorGroves** — maze-like groves of tall thin wirewood with
  twisted copper scrub.
- **ResonatorSpires** — the quartz spires are the feature; only sparse dry
  tufts at their feet. No trees.
- **DeadZones** — grey, unpowered, no glow. Nothing grows: tree and shrub
  density are both exactly 0.
- **EtchedPlains** *(new)* — etched flats; wind-combed dry scrub.
- **ClockwoodForest** *(new)* — the densest wirewood stands on the Wafer.
- **DopantGroves** *(new)* — faceted blue/amber crystal groves with
  white-hot apex cores.
- **OxideDunes** *(new)* — dune grass tufts; no trees on shifting oxide.
- **StaticMarsh** *(new)* — reed beds; 2D crossed quads read better over
  water than voxel shrubs.
- **FabRuins** *(new, Organic)* — weeds in the ruins. This biome is in the
  **Organic**, not the Wafer — it will never appear in `wafer.json`.

## How densities are consumed (later)

`fillTreeDensity()` / `fillShrubDensity()` copy the table's densities into
a `procgen::BiomeDensity` at the live Wafer biome indices (0–4, matching
the `WaferBiome` enum and `wafer.json` order); all other entries are 0.
A placement stage then reads the effective per-column density with the
existing engine primitive:

```cpp
float d = procgen::blendedDensity(densities, maps, column);  // weight-aware
if (posRoll(featSeed, wx, wz) < d) { /* stamp vegetation */ }
```

`blendedDensity` folds the biome blend weights in automatically, so
vegetation thins out naturally across biome borders. When the 6 new
biomes land in `wafer.json`, their indices get entries in the fill
functions; the table already carries their densities.

## Style rationale (3D vs 2D)

- **3D** (`Modeled3D`) where the plant is a feature: berry/twisted shrubs
  in forests and groves, where the player walks among them.
- **2D** (`CrossedQuads`) where the plant is ground texture: grass tufts,
  dune grass, reed beds, weeds — cheap, and the per-plant tint plus the
  procedural texture set (`procgen_grass_tuft`, `procgen_fern`,
  `procgen_dry_scrub`) carry the visual variety.
- **None** where the biome's identity forbids it: DeadZones (dead),
  spires/dunes/marsh/plains/ruins (no trees — the terrain or ruins are
  the feature).
