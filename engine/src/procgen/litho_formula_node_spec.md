// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

# `lithoform` node — procgen graph node spec

Status: **implemented and wired** (2026-10-12, JD-locked for alpha). The
`lithoform` entry is registered in `graph_nodes.cpp` / `graph_nodes.h` and
the node lives in `litho_formula_node.cpp`. This spec records the locked
design the implementation follows.

## 1. What it is

A **generator-style node** named `lithoform` for the procgen node graph
(`engine/src/procgen/graph_nodes.h`). It evaluates the Litho Formula shape
analytically at each sample point and writes a **density-like value** per
point, so it composes with the existing modifier nodes (`select`, `blend`,
`clamp`, `curve`, `scale_bias`, `add`, `multiply`, …) exactly like a `noise`
node does today.

## 2. Node table entry (for the lock decision)

```cpp
{"lithoform", {}, false,
 {"theta", "phi", "radius", "falloff", "rotation", "center"},
 makeLithoform},
```

- **Name:** `lithoform`.
- **Required graph inputs:** none — it is a pure generator, like `noise`
  and `wave`. (An optional `mask` input is discussed in §6 and deliberately
  left OUT of the v1 spec; gating is done downstream with `select`/`blend`.)
- **Variadic:** no.

## 3. Plain parameters

| Param | Type | Default | Meaning |
|---|---|---|---|
| `theta` | object `{a,b,m,n1,n2,n3}` | `{a:1,b:1,m:2,n1:2,n2:2,n3:2}` | Longitude factor of the spherical product |
| `phi` | object `{a,b,m,n1,n2,n3}` | `{a:1,b:1,m:2,n1:2,n2:2,n3:2}` | Latitude factor of the spherical product |
| `radius` | number | `8` | World-unit scale of the unit shape (must be > 0) |
| `falloff` | number | `1` | Boundary smoothing width in world units (≥ 0; 0 = hard mask) |
| `rotation` | `[rx,ry,rz]` radians | `[0,0,0]` | Euler rotation applied in shape-local space |
| `center` | `[x,y,z]` | `[0,0,0]` | Shape center in the same coordinate frame as the sample points |

Nested `theta`/`phi` objects match the existing convention (`noise` takes
nested `fractal`, `warp`, `cellular` objects). `seed` is deliberately
**absent**: the shape is a deterministic analytic function, which is a
feature — the same params give the same shape everywhere, and placement
variety comes from the worldgen scatter stage (different presets, radii,
rotations per instance), not from hidden jitter.

## 4. Eval semantics

Signature (existing pattern): `void eval(const PointsView& points,
std::span<const std::span<const float>> inputs, std::span<float> out) const`.

Per point `p` (structure-of-arrays, like every other node):

1. `q = unrotate(p - center)`; `r = |q| / radius` (shape-local radius).
2. If `r == 0` → `out = 1` (deep inside). Else compute the spherical
   angles of `q`: `phi = asin(qy / |q|)`, `theta = atan2(qz, qx)`.
   The formula's polar axis maps to **world Y (game up)** — the same
   convention as the voxelizer (`litho_formula_voxelize.cpp`: formula
   `(x, y, z)` → grid `(x, z, y)`, so the polar axis becomes grid Y) and
   the stamp path that bakes it. Phi is therefore the latitude off the
   world XZ plane and theta the longitude in that plane.
3. Evaluate the surface radius in that direction with the existing sampler:
   `R = |lithoPoint(theta, phi, thetaParams, phiParams)|`.
   The Litho Formula surface is star-shaped about the origin, so the
   inside/outside test is a single radius comparison: inside iff
   `|q|/radius <= R`. (Degenerate `R <= 0` → outside.)
4. Density: `d = (R - |q|/radius) / (falloff / radius)`; with `falloff = 0`,
   `out = d >= 0 ? 1 : 0`; otherwise `out = clamp01(smoothstep(-1, 1, d))`
   — 1 deep inside, 0 outside, smooth across the boundary width.

Cost is ~6 `pow` + a few trig calls per point — comparable to one noise
octave, and worldgen samples it sparsely (only near a placement). The
analytic field agrees with the voxelizer's baked shape up to voxelization
error, so graph-placed and `.lthm`-stamped instances of the same preset
match visually. This agreement is a tested contract: the node self-test
(`litho_formula_node_selftest.cpp`) bakes a non-spherical preset with the
voxelizer and asserts the node's occupancy agrees with the baked cells
(spherical presets cannot catch a polar-axis swap, so the test preset
must stay non-spherical).

`makeLithoform` validates params at graph-build time and reports errors
through the existing `std::string& err` channel: non-positive `radius`,
negative `falloff`, missing `theta`/`phi` numbers, malformed
`rotation`/`center` arrays. The node itself is immutable and thread-safe
(const eval, no shared mutable state).

## 5. JSON example — a Litho Formula rock arch blended into terrain

```json
{
  "nodes": {
    "base": {"type": "noise", "noise": "perlin", "frequency": 0.02,
             "fractal": {"type": "fbm", "octaves": 4}},
    "arch": {"type": "lithoform",
             "theta": {"a": 1, "b": 1, "m": 5, "n1": 0.9, "n2": 1, "n3": 1},
             "phi": {"a": 1, "b": 1, "m": 2, "n1": 2, "n2": 2, "n3": 2},
             "radius": 6, "falloff": 1.5,
             "rotation": [0, 0.6, 0],
             "center": [128, 70, -40]},
    "arch_masked": {"type": "select", "control": "arch",
                    "a": "arch", "b": "base",
                    "threshold": 0.5, "falloff": 0.1},
    "terrain": {"type": "blend", "a": "base", "b": "arch_masked",
                "t": {"type": "clamp", "input": "arch", "min": 0, "max": 1}}
  },
  "outputs": {"density": "terrain"}
}
```

Reading it: `arch` is the analytic density field of one starfish-like
preset instance; `select` keeps terrain where the field is below 0.5 and
takes the arch density above it; `blend` merges the two with the raw field
as the interpolation weight. Every node in the chain already exists except
`lithoform`.

## 6. How worldgen would place results (brief)

Two paths, deliberately separated:

- **(a) Baked `.lthm` stamping (recommended for alpha).** The gallery
  already bakes every preset to `assets/models/<preset>.lthm` via
  `bake_litho_formula`. The existing structure-placement stage
  (`game/LithoKraft/src/game/structures.cpp`) can scatter those models
  like any other authored structure — position, rotation, and rarity are
  data, no graph changes needed. This is the low-risk path: it reuses the
  tested save/place pipeline and keeps chunk generation untouched.
- **(b) Analytic node in chunk density graphs (later).** `lithoform`
  instances live in a chunk's generator graph with per-placement `center` /
  `radius` / `rotation` bound by the scatter stage. Good for large,
  terrain-blended formations (rock arches, crystal clusters) where a smooth
  density blend beats a hard stamp. Needs scatter-stage support for
  per-instance param binding, which does not exist yet.

Recommendation: lock (a) for alpha if worldgen scatter is wanted at all;
(b) stays a design until the scatter stage can bind per-instance params.

## 7. Open questions for JD

1. **Wire now or later?** Ship core + voxelizer + gallery first and wire
   the node after the fix program, or wire `lithoform` now?
2. **Worldgen use in alpha?** May worldgen JSON scatter Litho Formula
   structures (ruins, flora, rock formations), or do they stay
   authored `.lthm` models only?
3. **Density semantics:** smooth 0..1 field with `falloff` (recommended —
   composes with every modifier) vs. a hard inside/outside mask?
4. **Optional `mask` input:** v1 spec says no (gate downstream with
   `select`/`blend` instead). Keep it out, or add an optional single
   `mask` input that multiplies the field?
5. **Per-instance variety:** is preset + radius + rotation variety enough,
   or should a future revision add seeded param jitter for scatter?
6. **Preset authorship:** JD curates the gallery by hand, or crew proposes
   and JD vetoes? (The 12 shipped presets are the crew's opening set.)
