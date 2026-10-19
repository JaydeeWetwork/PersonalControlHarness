# Voxel Compute — Engine Architecture Sketch (v0.1, Oct 9 2026)

C++20 core, performance-first. Baseline target: Jaydee's laptop (RTX 3050 Ti 4 GB, Windows).
If it is smooth there, it scales up. `proto/` implements the parts marked **[proto]**.

## 1. World layout & block storage
- **Chunk column** = 16×16 blocks, height 0–511, split into **32 sub-chunks of 16³** (4096 cells). **[proto]**
- **Sub-chunk storage:** flat `uint16_t blocks[4096]` (8 KB), y-major index `(y<<8)|(z<<4)|x`, so X runs are
  contiguous and SIMD-friendly. Plus a `nonAir` count; **all-air sub-chunks are `nullptr`** (no memory, no meshing,
  no ticking). **[proto]**
- **Later:** palette + bit-packed indices (1–8 bits/cell) for save files and far chunks; an all-one-block
  sub-chunk becomes "uniform" (1 id, no array). Hot (edited/ticking) sub-chunks stay flat for speed.
- **Chunk map:** `unordered_map<uint64_t, unique_ptr<ChunkColumn>>`, key = `(cx<<32)|cz`. **[proto]**
  Later: open-addressing table + a small "last lookups" cache, and 27-neighbour pointer caches per sub-chunk.

## 2. Block registry & properties
Data-oriented: `BlockId → uint32 flags` in a flat array (one cache line covers 16 blocks). **[proto]**
Flags: `Solid`, `Opaque` (culls neighbour faces), `Transparent` (separate render pass), `Powerable`, `CutsDust`.
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
- **World tick-rate modes: 32 / 64 / 128 / 256 TPS** (powers of two), **default 64**. Selectable at runtime
  (F1–F4) or `--tps`; shown in the window title with achieved TPS, average tick ms and sim budget %. **[proto]**
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
- Prototype: one stub "EUV" domain at 512 Hz (F5 toggles it, `--no-euv` disables) whose step is a placeholder
  kernel; achieved Hz is shown in the title. Measured on the box: 512/512 Hz alongside every world mode.
- Later: per-domain chip lists compiled to bitsliced kernels; heat model uses domain rate × gate count.

## 5. Signal simulation (Red Wire / Blue Wire)
- Wires are not ticked per block. On edit, flood-fill connected wire cells of the same color into a **net**
  (graph node); Red and Blue never merge; components accept either color.
- **Net graph:** nodes = nets + components (torch, repeater, comparator, redirector, latch, lamp...);
  edges = which net drives which input. Strength 0–64 drops 1 per block along the net (distance precomputed
  per wire cell during the flood fill, so a strength update is one subtraction, no re-walk).
- **Event-driven:** only components whose input changed are put on the queue for the next tick (or after
  their delay). Idle circuits cost zero. Comparator modes: compare / subtract / threshold.
- Graph rebuild is incremental: an edit only re-floods the touched nets.

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
| **Main / render** | GLFW window, input, **all GL objects**, camera, GPU mesh list | block arrays (write), sim state |
| **Sim (1 thread)** | master clock, world tick loop, chip clock domains, active list, scheduled-tick queue, net graph, chips; later the authoritative block edits | GL, meshes |
| **Workers (N = cores − 2)** | worldgen jobs, meshing jobs (pure functions → results) | GL, sim state |

Sync points (no big lock on the frame path):
1. **Worldgen → main:** columns are generated in parallel (pure function of coords) and handed back through a
   `HandoffQueue` (mutex held only for an O(1) vector swap); the main thread inserts them into the map.
2. **Meshing → render:** workers push `MeshData` into a `HandoffQueue`; the render thread swaps it out once per
   frame and uploads at most **64 meshes/frame** (budget keeps frame time flat). GL calls happen only here.
3. **Sim → render:** `SimStats` (TPS, tick ms, budget %, dropped ticks, chip-domain Hz) (and later light-weight render state such as lamp/pixel colors) is published every
   tick through a **lock-free triple buffer**: writer and reader never wait on each other.
4. **Render → sim:** player chunk position, requested TPS mode and domain toggles via atomics.
5. **Block data:** in the prototype the world is immutable after worldgen, so meshers and sim read it freely.
   **Next step:** sim thread becomes sole writer; edits bump a per-sub-chunk version and meshers get a
   copy-on-write 8 KB snapshot (or the 18³ padded cube copied by the sim thread at dirty time) so no reader ever
   sees a half-written sub-chunk. Player edits go render → sim via an SPSC command queue.

## 8. Data-oriented / SIMD hot paths
Contiguous `uint16` arrays, flags in flat tables, SoA net state; candidates for AVX2 intrinsics: opacity-cube
gather, "any non-air / count" reductions, bitsliced chip evaluation, greedy meshing masks. Compile with
`/arch:AVX2` once the baseline CPU is confirmed (most CPUs since 2013). Measure first (Tracy profiler later).

## 9. Performance targets (laptop RTX 3050 Ti 4 GB, Windows)
- **≥ 144 FPS** at 8-chunk view, **≥ 60 FPS** at 16+; frame time worst-case < 16 ms while chunks stream.
- **64 TPS locked** (default) with 10k active components in radius; world tick < 50% of the 15.6 ms period;
  256 TPS mode for smaller builds (3.9 ms period). EUV chip domains hold 512 Hz.
- Meshing a 16³ sub-chunk < 0.3 ms per worker; chunk streaming never stalls the render thread.
- VRAM < 1 GB for a 16-chunk view (4-byte vertices + greedy meshing make this easy).
- Lithographed chip: ≥ 1 MHz equivalent single-chip clock at Level 2 on a laptop core.
