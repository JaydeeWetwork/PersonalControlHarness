// LithoKraft's saves on litholattice::storage: one region store per dimension
// (<root>/<dimension>/r.x.z.llr) carrying the engine's BLKS payload, the engine's MATS payload
// (per-voxel PBR material bytes, persisted as string IDs), plus the game's SGNL payload
// (signal-sim component memory and timers), and <root>/player.dat (position, look, dimension,
// hotbar). Encoding runs on the caller's thread; file I/O runs on each store's I/O thread.
#pragma once
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <string>
#include <vector>
#include "lithokraft/content/signal.h"
#include "lithokraft/game/engine.h"
#include "lithokraft/game/inventory.h"  // PlayerMeta's v4 inventory block (P2b-1)
#include "litholattice/render/pbr_materials.h"  // MaterialRegistry* (MATS payload registration)
#include "litholattice/core/math.h"
#include "litholattice/storage/block_names.h"
#include "litholattice/storage/storage.h"
#include "litholattice/world/chunk_map.h"

namespace lithokraft {

constexpr uint32_t kSignalTag = storage::fourcc("SGNL");

struct PlayerMeta {
  // v2 adds the stat block; v3 adds the vitals block (health/oxygen/stamina
  // moved from IStat to IVitals). v4 (P2b-1) stores the hotbar as STRING
  // ids (Appendix C M2: name-remapped on load like world blocks, aliases
  // included) and adds the inventory block below. v1–v3 files still load —
  // absent blocks read as the registered defaults, a v1–v3 hotbar reads as
  // the old raw BlockIds, and a pre-v4 file loads with an EMPTY inventory
  // (hasInventory == false; Game migrates the legacy hotbar into it).
  // v5 (wiring pass 2026-10-13, B6/B7) appends the stack-data block: per
  // data-bearing inventory slot, the EtchOR name field ("name") and the
  // applied certificate id list. v1–v4 files load with plain stacks —
  // per-stack data simply did not exist before v5.
  static constexpr uint16_t kVersion = 5;
  Vec3 pos{};
  float yaw = 0.f, pitch = 0.f;
  std::string dimension;
  int hot = 0;
  /// In memory: the BlockId each hotbar slot's stack places (kAir for
  /// empty slots and for non-block stacks like the Fragment). On disk in
  /// v4 these are STRING ids; in v1–v3 they were raw u16 BlockIds.
  std::vector<BlockId> hotbar;
  /// The canonical player inventory (P2b-1, Appendix G H1): every one of
  /// the 50 slots (42 carry + 8 gear, Inventory::allSlots order), written
  /// in v4 as (string id, count) per slot, empty slots explicit. On load,
  /// a stack whose string id resolves to nothing (removed item/block) is
  /// dropped with a log line — the rest of the inventory is never lost
  /// with it. Meaningful only when hasInventory is true.
  Inventory inventory;
  bool hasInventory = false;   ///< the file carried a v4 inventory block
  uint16_t fileVersion = 0;    ///< version actually read (0 = built in memory)
  /// Registered stat base values by stat NAME (player_adapter.h), e.g.
  /// {"kharge": 12}. Names are the permanent key — registry ids are
  /// registration-order and may shift as stats are added. Empty in v1 saves
  /// (and for a player who never touched a stat): everything reads as its
  /// registered default on load.
  std::map<std::string, int32_t> stats;
  /// Registered vital values by vital NAME, e.g. {"health": 40}. Vitals
  /// have no boost layer, so the stored value is the whole story. Empty in
  /// v1/v2 saves: vitals read as their registered defaults on load.
  std::map<std::string, int32_t> vitals;
};

class WorldSave {
 public:
  /// `materials`: the PBR material registry (main.cpp owns it; must outlive this). When
  /// non-null the engine's MATS payload is registered, persisting per-voxel material bytes
  /// as string IDs. Null = no material persistence (tests that don't care about materials).
  explicit WorldSave(std::filesystem::path root, const litholattice::MaterialRegistry* materials = nullptr);
  const std::filesystem::path& root() const { return root_; }

  /// FAIL-CLOSED NAME TABLE (C-4): the semantics every method below follows.
  /// - NO blocks.names file at all (and no crash footprint of one) = the
  ///   documented legacy-identity path: a fresh world or a pre-table save.
  ///   Loads proceed with identity ids; saves write a fresh table.
  /// - A table that EXISTS but is unreadable/corrupt after crash recovery
  ///   AND region data exists for the dimension = fail closed:
  ///   loadDimension returns -1 (not 0, not "generated terrain kept"),
  ///   loadColumn reports ColumnLoad::Unreadable for every column (B-M3: the
  ///   streamer leaves a hole and retries; it never generates over the saved
  ///   column), and saveDimension
  ///   refuses (returns 0, lastSaveOk()==false, nothing is written — the
  ///   table, the regions and player.dat are all left untouched) so a
  ///   regenerated world can never overwrite the saved one. Game::saveWorld
  ///   surfaces the refusal as logError + a false return.
  /// - A corrupt table with NO region data loses nothing by proceeding:
  ///   the next save simply writes a fresh table over it.

  /// Saves `world` (blocks + `signal` state). The caller's part (the sim pause) only exports the
  /// signal memory and copies the columns to save; encoding and writing run on the I/O thread.
  /// `dirty`: save just these columns plus every column holding components (columns never
  /// changed since worldgen are regenerated from the seed on load); nullptr = every column.
  /// `wait`: block until the regions are on disk (exit); else return at once (autosave, F5).
  /// Returns the number of columns queued (0 when the save was refused or the
  /// table/metadata write failed — inspect lastSaveOk()/lastSaveError()).
  /// When a seed was set via setSeed(), the world metadata (level.json) is
  /// written through the file contract as part of the save (SV-H2).
  size_t saveDimension(const std::string& dim, const ChunkMap& world, const SignalSim& signal, bool wait,
                       const std::unordered_set<uint64_t>* dirty = nullptr);
  /// Column eviction save (A2 / B-M1): saves ONLY the given columns
  /// (blocks + their SGNL state, exported from `signal` inside this call)
  /// through the same store path and fail-closed discipline as
  /// saveDimension. The caller (Game::evictFarColumns) invokes this BEFORE
  /// SignalSim::unscanColumn drops the columns' sim state, in the same
  /// sim-stopped window as insertion. Returns the number of columns queued
  /// (0 when refused — inspect lastSaveOk()/lastSaveError()); with
  /// wait=true a 0/short return or !lastSaveOk() means the eviction of the
  /// unsaved columns must NOT proceed.
  size_t saveColumns(const std::string& dim, const std::vector<const ChunkColumn*>& cols, const SignalSim& signal,
                     bool wait);
  /// Outcome of the most recent saveDimension: false when the save was
  /// refused (fail-closed name table), the table/metadata write failed, or —
  /// for wait=true — a region flush failed (SV-H1). For wait=false, region
  /// flushes complete asynchronously; their outcome lands in the stores'
  /// lastFlushOk() and in the bool returned by flush().
  bool lastSaveOk() const { return lastSaveOk_; }
  const std::string& lastSaveError() const { return lastSaveError_; }
  /// Replaces each column of `world` that has a saved copy; returns how many
  /// (only columns actually inserted are counted — L6). Returns -1 when the
  /// load was REFUSED (fail-closed name table, see above). The saved signal
  /// state is returned in `signalState` for SignalSim::importState() after scan().
  int loadDimension(const std::string& dim, ChunkMap& world, std::vector<uint8_t>& signalState);

  /// World metadata (level.json, SV-H2): the world seed + save format
  /// version, written through writeFileAtomic whenever the world is saved.
  /// The seed is stored as a decimal STRING: the engine Json number type is
  /// a double and cannot hold every uint64 seed exactly.
  static constexpr int kWorldMetaVersion = 1;
  void setSeed(uint64_t seed) { seed_ = seed; hasSeed_ = true; }
  bool writeWorldMeta(std::string* err) const;  ///< no-op success when no seed was set
  /// The persisted seed for the save at `root`, or nullopt when there is no
  /// level.json (pre-metadata save: the caller's seed stands). A level.json
  /// that exists but is corrupt after crash recovery yields nullopt with
  /// `err` SET (distinct from absent) so the caller can warn loudly.
  static std::optional<uint64_t> readWorldSeed(const std::filesystem::path& root, std::string* err);
  /// Outcome of a streamed single-column load (B-M3): the three cases the
  /// streamer MUST distinguish. NotSaved = no saved copy exists, generate
  /// fresh. Unreadable = a saved copy (or the block-name table interpreting
  /// it) exists but cannot be read: fail loudly, NEVER generate over it and
  /// never treat the key as handled — the column stays a hole and is retried.
  enum class ColumnLoad { Loaded, NotSaved, Unreadable };
  struct LoadedColumn {
    ColumnLoad status = ColumnLoad::NotSaved;
    std::unique_ptr<ChunkColumn> column;  ///< non-null only when status == Loaded
    /// This column's SGNL payload bytes (component memory/timers for exactly
    /// this column's components, self-delimiting entries as in loadDimension's
    /// blob). Surfaced per column (SW-H1 / C-M4) so the caller can scanColumn
    /// + importState them on insertion; empty when the column had none.
    std::vector<uint8_t> signal;
  };
  /// Streaming load (chunk streaming): one saved column, remapped to the live
  /// registry, with its SGNL payload. Thread-safe: callable from job-pool
  /// workers while saves run on the store's I/O thread (loads serialise
  /// against each other and against loadDimension on loadMutex_, so the SGNL
  /// decode sink always names exactly one call's buffer — C-M4).
  LoadedColumn loadColumn(const std::string& dim, int cx, int cz);

  /// v1 -> v2 signal state migration (adds channel=1, beacon=0). Exposed for tests.
  static std::optional<storage::Bytes> migrateSignalV1ToV2(const storage::Bytes& v1);
  /// Every queued write on disk; returns false when any store's flush failed
  /// (SV-H1 — the failed regions keep their pending records for a retry).
  bool flush();

  /// player.dat goes through the file contract (SV-H3): writePlayer uses
  /// writeFileAtomic (fsync + backup rotation); readPlayer first runs
  /// recoverCrashedWrite with a magic+CRC validator, so a torn primary
  /// heals from the newest intact backup generation instead of losing the
  /// player. writePlayer always writes v4 (string-id hotbar + the v4
  /// inventory block); readPlayer accepts v1–v4 and the CRC gate is
  /// unchanged.
  bool writePlayer(const PlayerMeta& m, std::string* err) const;
  std::optional<PlayerMeta> readPlayer(std::string* err) const;

  const storage::PayloadRegistry& payloads() const { return payloads_; }

 private:
  storage::WorldStore& store(const std::string& dim);
  std::filesystem::path root_;
  storage::PayloadRegistry payloads_;
  std::mutex m_;
  std::map<uint64_t, std::vector<uint8_t>> exporting_;  // SGNL save source (caller's thread)
  // SGNL load sink (C-M4): NOT a shared accumulator any more. The decode
  // lambda appends only into the buffer the currently-executing load call has
  // armed here (guarded by m_); loads are serialised by loadMutex_, so a
  // streamed decode can never leak into a dimension load's blob (or vice
  // versa), and no residue accumulates between calls. A decode with no sink
  // armed drops its bytes (no load path does that: every decode runs under
  // a WorldSave load call).
  std::vector<uint8_t>* signalSink_ = nullptr;
  std::mutex loadMutex_;  ///< serialises loadColumn vs loadDimension (and loadColumn vs itself)
  std::map<std::string, std::unique_ptr<storage::WorldStore>> stores_;
  std::mutex storesMutex_;  ///< guards stores_ (loadColumn runs on job threads)
  uint64_t seed_ = 0;       ///< world seed for level.json (SV-H2); set by Game from cfg.seed
  bool hasSeed_ = false;
  bool lastSaveOk_ = true;  ///< outcome of the last saveDimension (see above)
  std::string lastSaveError_;
  // Cached block-name remap for loadColumn (the table is written by the first
  // save and only ever rewritten with the same live registry afterwards).
  // Only a SUCCESS is cached (B-M3): a failed read is reported per column as
  // ColumnLoad::Unreadable and re-attempted on the next call, so a repaired
  // table heals in-session instead of latching a session-wide downgrade.
  std::optional<storage::StableRemap> streamRemap_;
  std::mutex streamRemapMutex_;
};

}  // namespace lithokraft
