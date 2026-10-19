// LithoLattice scaling: from phone to supercomputer.
//
// The engine is designed to scale across three axes:
//
// 1. TICK PARALLELISM (multi-core):
//    - Chunks are the unit of parallelism. Each chunk sims independently.
//    - TickContext carries all per-tick state (no globals). Deterministic.
//    - Boundaries sync at tick end (like MPI halo exchange).
//    - The 512 TPS target assumes 8+ cores for large worlds.
//
// 2. SYSTEM PARALLELISM (task graph):
//    - Physics, Signal, Entities run as parallel tasks.
//    - Dependencies are explicit (Signal reads Physics output, etc.).
//    - The tick-domain scheduler (from the GPU brief) assigns systems to cores.
//
// 3. DISTRIBUTED (cluster / supercomputer):
//    - World partitions across nodes. Each node owns a region.
//    - Node-to-node: same protocol as client-server, but at LAN rates.
//    - On a college cluster with InfiniBand, the network tick can go to
//      1024 Hz or higher (LITHOLATTICE_NET_TICK_HZ is configurable).
//    - Deterministic sim means nodes COULD validate each other (anti-cheat
//      becomes anti-corruption for research) — design intent only.
//
// STATUS (F-M8, review 2026-10-13) — read before believing any of the above
// as shipped behaviour: MULTIPLAYER IS NOT IMPLEMENTED. engine/src/net is a
// codec/channel library: there is no transport (no sockets), no server, no
// authoritative/non-authoritative mode switch, and no peer/input validator
// anywhere in the tree. ANTI-CHEAT IS UNIMPLEMENTED: deterministic-sim
// validation is a design property that no net code consumes yet. Traffic
// design is plaintext (NullCipher only) and the handshake offer list is
// unauthenticated until KEX lands. This file describes the scaling DESIGN;
// nothing here is a shipping multiplayer feature.
//
// NETWORK RATES BY ENVIRONMENT:
//   Mobile (cellular):     64-128 Hz
//   Broadband:            256 Hz (default)
//   LAN (gigabit):        512 Hz
//   Cluster (InfiniBand): 1024+ Hz
//
// The protocol negotiates the rate at connect. The sim is always 512 TPS;
// the network rate is how often we sync.
#pragma once
// (Documentation only. See version.h for the configurable rates.)
