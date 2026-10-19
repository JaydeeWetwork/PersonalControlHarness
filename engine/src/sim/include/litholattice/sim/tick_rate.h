// Tick rate for Timing Handshake components (rate scaling, JD
// 2026-10-14): a component's pull rate relative to its caller's cycle,
// expressed as pulls per cycle in multiply/divide form — pulled once per
// `divide` cycles, and `multiply` times (sub-pulls) on each cycle it is
// pulled. Both factors are powers of two, like every other rate in the
// engine (tick modes, clock domains): the rate lattice stays binary, so
// divided cadences nest exactly inside the master cycle and multiplied
// sub-pulls subdivide a slot with no fractional drift.
//
// GENERALIZATION, not a parallel system (JD 2026-10-14): TickRate
// GENERALIZES the existing phaseDivider cadence mechanism inside the
// same descriptor (HsComponentDesc) and the same dueFor gate.
// phaseDivider is the divider-only, sub-cycle special case — it
// subdivides phases and cannot express a multiplier or a cycle-axis
// divide; TickRate adds the cycle axis and the multiply direction.
// One descriptor, one gate: a component's cadence class / phaseDivider
// and its TickRate are consulted together by dueFor, never by two
// competing schedulers.
//
// A "rate domain" is the equivalence class of components sharing one
// (group, rate) — DERIVED from declarations, never declared itself:
// no component names its domain, the authority partitions registrations
// by (group, rate) and the class falls out. That equivalence class is
// the future trace JIT's compile unit: the JIT keys compiled traces by
// (component, rate) and schedules whole domains at their divided
// cadence — a domain fires together, holds together, and compiles
// together, so no trace ever has to model a mixed-rate interior.
//
// Crossing rule — CORRECTED CONTRACT (review A-H1, fix run 2026-10-15).
// The prototype's rule was: "A value produced in round r becomes visible
// to every other domain at the round r+1 boundary — never inside round
// r." In the prototype, rounds were GLOBAL: every component read the
// previous round's committed state. In production, sub-pulls are rounds
// INSIDE one item's own phase slot (the structural decision — sub-pulls
// do NOT consume phase slots, so any multiplier up to the cap works at
// any phasesPerCycle, incl. x16 at P=4), and the authority provides NO
// staging buffer, snapshot, or publish step: runItem() is a bare loop
// over the WorkFn. The prototype rule is therefore NOT an authority
// mechanism here, and must not be read as one:
//   * A WorkFn that writes shared state makes that write visible
//     IMMEDIATELY — to later items in its own shard, to items in the
//     SAME phase on other shards running concurrently, and to affine
//     items. An undisciplined rated WorkFn WILL leak its sub-pull
//     intermediates to same-phase readers, nondeterministically.
//   * Final-only visibility is a WorkFn OBLIGATION: a multiplied
//     WorkFn must keep sub-pull intermediates in its own private
//     staging and publish to shared state ONLY on its final sub-pull.
//     The final sub-pull is KNOWABLE, not guessable:
//     TimingHandshakeAuthority::currentSubPullIndex(),
//     currentSubPullCount() and isFinalSubPull() report the executing
//     item's sub-pull context on the calling thread; a WorkFn that
//     prefers it may instead count its own invocations (the sub-pulls
//     of one firing are back-to-back inside one runItem call).
//   * What the barrier DOES guarantee: a phase's work is complete
//     before the commit hook runs and before any later phase is
//     published, so a reader in a LATER phase (or after the barrier)
//     samples exactly the final state a disciplined writer published.
//     A SAME-phase reader racing a disciplined writer may sample
//     either the previously published value or that final value —
//     never an intermediate one. That is the whole contract.
//   * A divided component's outputs HOLD between its pulls because its
//     WorkFn is not invoked on held cycles: its last published state
//     stands, un-interpolated, until its next due firing republishes.
// Rates thus compose through published state only when WorkFns honour
// the staging obligation above. (The game signal layer enforces the
// equivalent mechanically for ComponentSteps: tickComponents applies
// only the final sub-pull's step — see signal.cpp.) The barrier remains
// the one synchroniser, exactly as for neutral (1/1) components.
//
// Rates are CONTENT (declared by components at registration), not save
// state: schedules are derived data, rebuilt at registration from the
// declarations — nothing about a rate or a derived schedule is ever
// serialized into a world save.
#pragma once
#include <cstdint>

namespace litholattice {

/// Rate caps: x16 turns the 512 TPS master into 8192 effective pulls/s
/// (JD's 8192 thesis, 2026-10-14); /1024 pulls once per 1024 cycles.
constexpr uint32_t kMaxRateMultiply = 16;
constexpr uint32_t kMaxRateDivide = 1024;

/// A component's tick rate relative to its caller's cycle (Timing
/// Handshake rate scaling, JD 2026-10-14): the component is pulled once
/// per `divide` cycles, and `multiply` times (sub-pulls, in ascending
/// order) on each cycle it is pulled. Both are powers of two, like every
/// other rate in the engine. Default (1/1) is neutral: every cycle, once.
struct TickRate {
  uint32_t multiply = 1;  ///< sub-pulls per due cycle (xN)
  uint32_t divide = 1;    ///< cycles per pull (/N)
  uint32_t phase = 0;     ///< divide-period offset: due when (cycle % divide) == (phase % divide)
  constexpr bool neutral() const { return multiply == 1 && divide == 1; }
  constexpr bool valid() const {
    constexpr auto pow2 = [](uint32_t v) { return v != 0 && (v & (v - 1)) == 0; };
    return pow2(multiply) && pow2(divide) && multiply <= kMaxRateMultiply && divide <= kMaxRateDivide;
  }
  constexpr double effective() const { return double(multiply) / double(divide); }
};

}  // namespace litholattice
