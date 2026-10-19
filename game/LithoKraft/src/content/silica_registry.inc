// Silica registry hookup (workstream 5, 2026-10-11) -- for the coordinator.
//
// The ChipOR workstream owns game/LithoKraft/src/content/blocks.cpp + blocks.h
// right now, so the Silica rows were NOT edited in directly. Apply the three
// snippets below AFTER the ChipOR workstream's rows (their ids 51-52) have
// landed, then delete the moved lines from silica.h as noted.
//
// The silica self-test (app/silica_selftest_main.cpp) builds a test-local
// BlockRegistry with EXACTLY these rows/flags/ids and passes against it, so
// these snippets are already verified -- this file is just the mechanical move.

// === SNIPPET 1: blocks.h -- append after the kChipOR constant block =========
// (i.e. right before the "/// Game flags" comment), keeping append order:
//
//   // Silica (2026-10-11, workstream 5): the Wafer's grass equivalent -- silicon
//   // fern ground cover -- plus tilled silicon farmland. Ids follow the ChipOR rows.
//   constexpr BlockId kSilicaGrass = 53;      ///< silicon fern ground cover ("silica grass")
//   constexpr BlockId kSilicaTilledSoil = 54; ///< tilled silicon farmland (plantable)
//
// THEN DELETE from include/lithokraft/content/silica.h the two lines:
//   constexpr BlockId kSilicaGrass = 53;      ///< silicon fern ground cover ("silica grass")
//   constexpr BlockId kSilicaTilledSoil = 54; ///< tilled silicon farmland (plantable)
// (silica.h keeps including blocks.h, so everything keeps resolving; kItemSilicaSeed
// stays in silica.h -- non-block item ids are not BlockIds and get no registry row.)

// === SNIPPET 2: blocks.cpp -- append after the dead_memory_cell row ==========
// (i.e. right before the "// Save migration" comment), keeping the same order
// as the BlockId constants (append-only):
//
//     // Silica (2026-10-11, workstream 5). Order must match the BlockId constants
//     // in blocks.h (append-only). The grass is ground cover, not a solid block:
//     // Cross sprite like the torches, transparent, attachable (mounts on the soil
//     // below), random-ticking for growth/spread. Tilled silicon is ordinary soil.
//     r.add(def("lithokraft:silica_grass", "Silica Grass", kTransparent | kAttachable | kRandomTicks, S::Cross, "silica_grass", "", R::None, 0, 0));
//     r.add(def("lithokraft:tilled_silicon", "Tilled Silicon", kTerrain, S::Cube, "silica_tilled", "", R::None, 0, 0));

// === SNIPPET 3: blocks.cpp -- update the two size guards =====================
//   r.reserve(size_t(kDeadMemoryCell) + 1);
// becomes:
//   r.reserve(size_t(kSilicaTilledSoil) + 1);
// and
//   if (r.size() != size_t(kChipOR) + 1) std::abort();  // ids in blocks.h out of sync with the rows
// becomes:
//   if (r.size() != size_t(kSilicaTilledSoil) + 1) std::abort();  // ids in blocks.h out of sync with the rows
//
// (If the ChipOR workstream added a second row at id 52, e.g. kChip, it sits
// between kChipOR and kSilicaGrass above; the Silica ids 53/54 are unchanged.)

// === verification after applying ============================================
// Rebuild and run: ctest -R silica  (lithokraft_silica_selftest)
// The self-test's registry section asserts these exact rows, so a mis-ordered
// append fails loudly there instead of corrupting saves.
