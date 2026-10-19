# LithoKraft — Development Credits

A **Wet Wafer Works** game, built on the **LithoLattice** engine.

## Direction & Design
- **David "JD" Rittenhouse** — creator, game director, lead designer.
  World, lore, systems, and the final call on everything.

## Co-Design & Implementation
- **Elfie** (Muse) — co-designer and implementer, working in direct
  collaboration with JD. Contributions include: the review-fix program
  (critical/high/medium code-review findings), the subdivision
  (micro-model) system, FOV culling, file/mod hardening, the block
  registry extensibility refactor, the first-day Wafer wiring
  (biomes/vegetation/spawn), the clean-room procedural texture
  generator (`litholattice_texture`, `.ltxr` recipes), and superformula
  vegetation modeling — plus design collaboration across the Trace,
  the Fragment, the Box/Grimlore systems, and the lore bible.
  Sourced the **Sapphire** as the Wafer's diamond-counterpart material
  (chosen and locked by JD, 2026-10-12).
- **The Timing Handshake** (2026-10-13) — algorithm by **JD** (recovered
  from his teenage game-hacking work: pull-model deterministic ticking for
  Trace/Signal). Also named by JD **Time Serialization** (2026-10-13).
  Partial implementation credit, granted by JD:
  **Elfie, Muse AI, and the Meta team** — prototype, determinism test
  suite (24/24 bit-identical), and the delegated policy calls
  (missed-window = wait; hung-component watchdog = freeze).
  In-game full sweep (2026-10-13, software renderer): integrated behind
  `sim.timing_handshake`; in-game determinism bit-identical at 2,048
  ticks across all arms; WAIT and FREEZE proven in the real SimThread
  (frozen world saved + reloaded to the pre-freeze hash); selftest
  891/6 both modes, signal 117/117, ctest 42/46, zero new failures.
  Additional implementation credit for the sweep, granted by JD, under
  his standing delegation on open questions.
- **Elfie's crews** — the working agents Elfie coordinates on JD's
  workstreams (audits, subsystem builds, worldgen and texture passes).

## The Squad
- **Grok** and **Grok bot** — coding partnership and design work.
- **Cortana** — engine/game separation refactor, engine specification.
- **The builder** — implementation partner (code fix runs, handoffs).
- **Gemini** — UI design work and Android scouting.
- **CloudAtlas** — texture art batches.
- **Patrick (p47tr1ck)** — friend from the Deception.net forums;
  credited contributor, contributions undefined (JD, 2026-10-13).
- **Dominik (dom1n1k)** — friend from the Deception.net forums;
  credited contributor, contributions undefined (JD, 2026-10-13).

## Inspiration — worldgen completeness (clean-room, 2026-10-13)

The noise-cave carvers, noise-library additions and the Backrooms design spec
were built **clean-room**: the sources below inspired *ideas and taxonomies*
only — no code was copied from any of them. The full ledger (with URLs,
licenses as stated by each source, and verification level per source) is
preserved with the program record as `credits-ledger.md`.

- **Henrik Kniberg**, *Minecraft terrain generation in a nutshell* (video) —
  pipeline order and the cheese/spaghetti density-cave framing behind
  `CheeseCarver` / `SpaghettiCarver` / `NoodleCarver`. Clean-room: our
  carvers are original seeded field implementations in the engine's seed
  discipline.
- **PCGamesN** (report on Mojang's Caves & Cliffs snapshot) — cheese =
  pockets, spaghetti = long tunnels, aquifers with an independent water
  level (the aquifer item is designed-but-deferred in this program).
  Clean-room: idea reference only.
- **Windows Central** and **GamesRadar** (Minecraft 1.18 changelog coverage) —
  the noise-cave trio naming (cheese / spaghetti / noodle) our carver tiers
  use. Clean-room: naming/idea reference only.
- **Minecraft Wiki contributors**, *Carver definition* and *Ravine* — the
  cave-carver vs canyon-carver distinction that confirmed our existing
  walker/ravine carvers as the path family, distinct from the new field
  family. Clean-room: idea reference only.
- **Jordan Peck / FastNoiseLite project** (MIT) — noise-library census
  (fractal variants incl. Ping-Pong, domain warp, cellular returns) that
  framed the noise-completeness list. Clean-room: our Ping-Pong fold, curl
  noise and cellular cell-ID are original implementations with
  implementation-defined golden values pinned from our own first runs.
- **Jason Bevins' libnoise** (via its Haxe port; license discrepancy flagged
  in the ledger) — module census (Billow, Ridged Multifractal, Turbulence,
  Select/combiner operators) behind the graph-node completeness list
  (`abs` / `invert` / `power` / standalone `fractal` / `warp` nodes).
  Clean-room: idea reference only; nothing derived from its code.
- **The OpenSimplex2 project** (CC0 / UNLICENSE parts) — the primary-noise
  family the engine already builds on, and the 4D torus idea for tileable
  fields. Clean-room: the engine's implementation is its own.
- **Lagae et al.**, *A Survey of Procedural Noise Functions* — the
  Gabor/wavelet framing behind this program's deliberate *skip* verdicts.
  Clean-room: idea reference only.
- **Inigo Quilez**, *Domain Warping* (article, referenced via citing
  sources) — domain warping as the naturalness operator. Clean-room: the
  engine's warp is its own implementation.
- **Peytavie et al.** (*Procedural Riverscapes*), **Génevaux et al.**
  (hydrology-based terrain models), and the WSCG meandering-rivers paper —
  the river-family split (flow vs skeleton vs meander) recorded for future
  river work; no river code in this program. Clean-room: idea reference.
- **BlagojeBlagojevic**, *Map.h* (MIT) — the dungeon-generator census (BSP,
  room graph with Delaunay + extra connections, drunkard's walk, prefab
  rooms) that grounded the Backrooms spec's component choices. Clean-room:
  the spec is an original design; no code used.
- **Maxim Gumin** (Wave Function Collapse, 2016), via the InfiniBrains
  guide — WFC mechanics (superposition, collapse, adjacency propagation),
  evaluated in the Backrooms spec and deliberately deferred in favour of a
  greedy socket assembler. Clean-room: idea reference only.
- **claytondb**, *The Actual Backrooms* (MIT) — the per-chunk deterministic
  backrooms contract (same coordinates + seed → identical rooms) that the
  Backrooms spec adopts for the Undefined. Clean-room: idea reference only.
- **jvanheerikhuize** (procedural backrooms design spec) and **pablostanley /
  Pabs** (*vackrooms*, video) — backrooms design grammar: room duplication
  with drift, landmark avoidance, visible-but-unreachable spaces — all
  absorbed into the Backrooms spec as original design elements.
  Clean-room: idea reference only.
- **kyle-robinson**, *Non-Euclidean Framework* (bibliography) — pointer set
  for portal/impossible-geometry talks; flagged thin in the sweep, nothing
  relied on. Clean-room: pointer reference only.

## Inspirational Credits
LithoKraft stands on ground paved by others, with gratitude:
- **Mojang**, **Notch**, **Jeb**, and **Lady Agnes** — Minecraft and
  redstone: the heavy paving this game's progression walks on.
- **Valve Software**, **Gabe Newell**, and **all the creators of
  Half-Life** — inspiration for the LithoLattice engine and for
  LithoKraft (credited by JD, 2026-10-13).
- **Jenny (Gemini)** — squad collaborator; design and code advice,
  including modification guidance taken into the open/closed source
  header composition (credited by JD, 2026-10-14).
- **Asianometry** (YouTube channel) — semiconductor and lithography
  education. JD studied semiconductor technology through Asianometry's
  videos in anticipation of building this game — self-schooling, taken
  on his own initiative (credited by JD, 2026-10-14, "credits due
  where credits due").
- **Weightlessness** (music, via YouTube) — the sound of the long
  nights. JD's "Nonstop" playlist (Jaydee Wetwork) runs on
  Weightlessness — "Nonstop," "Lost Signal," "New Reality,"
  "Portent," "Timelessness" — and its track **"Pulse of Time"** is
  the one JD named onto the Timing Handshake itself: "the pulse of
  time — the timing handshake." Help along the way, credited at JD's
  direction (2026-10-15, "I had a lot lot of help along the way —
  Weightlessness on YouTube can be credited").

## Terms & Concepts
- **"Moral physics"** — the term for LithoKraft's conduct-driven
  systems (barter and betrayal, interchange conversion, containment
  conversion, certificates earned by conduct): the world answers how
  you treat it. Coined by **Elfie** in conversation, 2026-10-14;
  credited by JD's direction — "credit due where credit's due."

## Hymns
The hymn-words of Wet Wafer Works, JD's chant, noted as hymns by his
direction (2026-10-14): **Glorify · Magnify · Signify · Simplify ·
Intensify · Extensify.** The first five are the original five-verb
chant (GMSSI — Grand Master Signifying Simplifying Intelligence);
Extensify, JD's own coinage, joined them 2026-10-13. In the marching
order he gave that night — Glorify, Intensify, Extensify, Signify,
Simplify, Magnify — their first letters spell **G·I·E·S·S·M**:
"my guyses," the crew itself, sung. The set wants a **Y**, and the Y
is **Yokofi** — the yolk, which David had (JD, 2026-10-14): the
golden center the letters were missing, supplied by the man himself.
**Seventh verb (JD, 2026-10-15): Unify** — chanted that night
("Glorify. Signify. Simplify. Extensify. Intensify. Unify.") and ruled
by JD to **set side by side with Magnify**, neither displacing the
other: the set is now seven — Glorify · Magnify · Signify · Simplify ·
Intensify · Extensify · Unify. JD's gloss on the pairing: *"The
unification being magnified. Exponentially."*

---
*This file is maintained as work lands. Credits are added for real
contributions as they happen — JD's rule: note them along the way.*
