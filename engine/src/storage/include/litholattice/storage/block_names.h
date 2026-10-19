// Block name table: a save records the namespaced id ("lithokraft:lamp") of every numeric block
// id it wrote, so a world reopens correctly when a game or mod changes its block order or adds
// blocks. Format "LLBN" v1: count, then each id as u16 length + UTF-8, then a CRC-32 of it all.
//
// PERMANENCE (C-4, 2026-10-13): this file interprets EVERY region file in the
// world, so it goes through the standard file contract like every other
// world file: writeFileAtomic (fsync file + directory, backup rotation,
// temp hygiene) on write, and recoverCrashedWrite with a fail-closed
// magic/version/CRC validator on read, so a torn primary heals from the
// newest intact backup generation. A save without the file at all is a
// pre-table (legacy) save: its ids are taken as the current registry order
// (migration), and the next save writes the table. A file that EXISTS (or
// leaves a crash footprint) but is unreadable after recovery is NOT legacy:
// readBlockRemap/readStableRemap return nullopt + err, and the game layer
// fails closed on it when region data exists (see world_save.h) instead of
// regenerating terrain over the saved columns.
#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "litholattice/world/block.h"
#include "litholattice/world/chunk.h"

namespace litholattice::storage {

constexpr uint32_t kBlockNamesVersion = 1;

bool writeBlockNames(const std::filesystem::path& file, const BlockRegistry& blocks, std::string* err);

struct BlockRemap {
  std::vector<BlockId> map;     ///< saved id -> current id (identity for a legacy save)
  std::vector<std::string> unknown;  ///< saved names the registry no longer has (loaded as air)
  bool legacy = false;          ///< no table on disk: migrated from the pre-table format
  bool identity = true;         ///< nothing to rewrite
};
/// nullopt + `err` if the table is unreadable (bad magic, version, CRC); a missing file is legacy.
std::optional<BlockRemap> readBlockRemap(const std::filesystem::path& file, const BlockRegistry& blocks, std::string* err);
/// Infinite Registry (phase 1): the save-time -> StableId version of BlockRemap.
/// The BLKS decoder parks save-time BlockIds in each sub-chunk's load scratch;
/// remapColumn() consumes the scratch into the StableId paletted voxel storage.
struct StableRemap {
  std::vector<StableId> map;         ///< saved id -> StableId (unknown -> kAirStableId)
  std::vector<std::string> unknown;  ///< saved names the registry no longer has (loaded as air)
  bool legacy = false;               ///< no table on disk: migrated from the pre-table format
  bool identity = true;              ///< nothing to rewrite (still computed; cheap)
};
/// Builds a StableRemap from the LLBN table (format unchanged). nullopt + `err`
/// if the table is unreadable; a missing file is legacy.
std::optional<StableRemap> readStableRemap(const std::filesystem::path& file, const BlockRegistry& blocks,
                                           std::string* err);
/// Consumes every sub-chunk's load scratch: translates save-time BlockIds through
/// `remap` into the StableId paletted voxel storage and clears the scratch.
/// Sub-chunks without scratch (e.g. fresh worldgen) are left alone.
/// Returns false + `err` on corrupt scratch (caller drops the column, same as a
/// decode failure). The quarantine-vs-air fork is NOT implemented: unknown ids
/// still become air (open fork #3).
bool remapColumn(ChunkColumn& column, const StableRemap& remap, std::string* err = nullptr);

}  // namespace litholattice::storage
