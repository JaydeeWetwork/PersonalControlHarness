# LithoLattice SDK changelog

The SDK is versioned 0.x until launch (decision Q12): any 0.MINOR release may break mods; notes call out every break.

## 0.1.0 (unreleased)
### Added
- `litholattice::ai` core: WalkCache/NavView, movement modes, bounded A* with hops, senses, utility AI + behaviour trees, ILearnedScorer stub, IBossSlot / SurgeSchedule, AiWorld (8-16 Hz, budget, LOD, cap 64/128, deterministic).
- `mod.json` schema 1 (`ModManifest`, `SemVer`, `versionSatisfies`, `resolveMods`, `loadModDirectory`); LithoKraft `mod.json`.
- Attachables: `kAttachable`, attach face in `CellState` bits 13-15, `IWorldSimulation::canAttach`, pop-off on support break; box models (`BoxModel`, `IBlockAppearance::boxModel`).
- Random ticks: `kRandomTicks` block flag, `IWorldSimulation::onRandomTick`, `Dimension::randomTicksPerSection` / `SimConfig::randomTicksPerSection` (default 8 per 16^3 section per world tick), `sim/random_ticks.h`.
- Namespaced block ids: `BlockDef::id`, `BlockRegistry::idOf` / `findById`; save block name table (`storage/block_names.h`).
- `IWorldSimulation::randomTick` (whole-area hook) and `SimThread::takeChangedColumns` (partial saves).
### Changed
- Saves now write `blocks.names`; older saves load as the current block order and gain the table on their next save.
