// Mod safety rules (from Cortana's brief, 2026-10-10, locked by JD).
//
// AngelScript bindings:
// 1. Full exposure: whole LithoKraft framework + LithoLattice SDK, so a mod or
//    even a whole game can be written in AngelScript without C++.
// 2. One shared binding layer: C++ and AS bind to the SAME interfaces (IBlock,
//    ITickable, IRandomTickable, ISignalNode, IEntityBehavior). No script-only API.
// 3. Hot loops stay in C++: Trace flood, meshing, pathfinding, storage never
//    run per-cell in script. Scripts get events and high-level calls.
// 4. Scripts run on the main thread only in v1.
// 5. Per-mod budgets: overrun -> throttled, then suspended, with logged reason.
// 6. Deterministic: no wall-clock, no unseeded RNG. Use TickContext RNG + tick counter.
// 7. Sandboxed: no raw fs, network, or process. Engine APIs only. Network goes
//    exclusively through the engine-gated net API (kPermNet, netSend /
//    onNetMessage): no sockets, no fds, no DNS anywhere in the mod API surface,
//    rate-limited per mod, throttled then suspended on abuse (net/confinement.h).
// 8. Naming: litholattice:: / lithokraft::, full -OR names, Trace=wired, Signal=wireless.
// 9. Educational: C++-like and readable.
//
// OPEN (for JD + Elfie):
// - Budget numbers per tick (powers of 2, microseconds).
// - Can scripts define ISignalNode (new Trace components) in v1?
// - Binding generation: hand-written or generated from SDK headers?
#pragma once
// (Documentation only; see mod_interface.h for the C API.)
