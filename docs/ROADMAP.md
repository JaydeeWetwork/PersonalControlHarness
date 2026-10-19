# Roadmap and Implementation Goals (as of Oct 10, 2026)

## Done (on bench/high-tps)
- Engine/game split (litholattice:: / lithokraft::), namespaced block IDs, random ticks (8 per section), attachables (3D torch, switch, button), mod.json and content mods, art batches 1-3, wire dots.
- Save system: async, compressed, crash-safe, versioned.
- Trace sim: change-skipping, per-network incremental reflood, flat arrays, cached neighbor lookups. Max went from about 1,500 to 8192 TPS in static scenes. The 64x64 wall tick went from 2.2 ms to 0.63 ms.
- Upload fix (no hitches), lamp afterglow, through-block hard power (repeater into silicon), Trace support rule (no floating), ceiling Trace, staircasing, glass/frosted/ceramic, Red/Blue Via (lossless, reach-free), Catchor (auto-connect, L/C/R output, sneak to set value), Redirector (cycling, split lanes, per-lane off, colorless output for red-to-blue transfer), Catchor-to-Redirector link.
- Pause menu on focus loss with Quit, look-at block inspector, thin Trace selection, debug F3 strength digits.
- Bench scenes: display, framebuffer, wall-pixel, pattern-wall, sawtooth, redirector, wire-torture. Flags: --tps-unlock up to 8192, --present fifo/mailbox/immediate, flyovers.

## Parked (next, in order)
1. Pattern ROM + Dual Catchor + animated pattern wall (design done, code was lost when the PC dropped and must be rewritten).
2. GPU-side Trace strength buffer (dots update without a remesh).
3. --perf-headless honoring --bench-scene, 16384 TPS sweep, per-thread CPU.
4. FPS cap check (about 1,500 looks capped), render distance x screen size sweep, showcase preset (60 FPS target, 30 floor) for YouTube.
5. True one-row-per-block screens (a Trace support design that lets rows stack).
6. Tick-interface refactor (IBlock/ITickable/ISignalNode) + graph compile and JIT profiler.
7. Merge bench/high-tps into main after the builder reviews it.

## Engine features planned
- SDK: DLL-ready IMod boundary, AngelScript SDK bindings (full LithoKraft and engine exposure), permissions and budgets.
- Materials: normal, height, and parallax maps, tessellation. Advanced lighting tiers ("our own Vibrant Visuals"), deterministic gameplay light.
- Volumetrics and particles (instanced, deterministic opacity layer that affects vision), script textures (TexGen-style, our own code), radio (Transmitter, Dish multiblock, channels unlock).
- NPC AI wiring into LithoKraft. The Daemon: record and replay, training pipeline.
- Moore's-law tick progression as a core mechanic.
