// LithoKraft model format (.lthm) + save/place pipeline.
//
// Workstream 3 (2026-10-11): the versioned model format and the save/place
// pipeline for the sub-block subdivision modeling system
// (subdivide -> add detail -> save -> place). Binary layout is documented in
// FORMAT.md next to this file.
//
// World permanence: the format is versioned from day one so saved models
// survive engine updates. Readers reject unknown versions cleanly instead
// of guessing.

#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include <litholattice/subdiv/octree.h>

namespace litholattice::subdiv {
class SubdivStore;  // engine/src/subdiv/include/litholattice/subdiv/subdiv_store.h
}

namespace lithokraft::model {

/// Magic bytes at the start of every .lthm file.
inline constexpr char kMagic[4] = {'L', 'T', 'H', 'M'};
/// Format version this build writes. Bump when the layout changes.
inline constexpr uint16_t kFormatVersion = 1;
/// Flags field value written by this build (all reserved bits clear).
inline constexpr uint16_t kFormatFlags = 0;

// Re-exported for authoring convenience: consumers work with these types
// through lithokraft::model without reaching into the engine namespace.
using litholattice::subdiv::MicroCell;
using litholattice::subdiv::SparseOctree;

/// A named, authored subdivision model: one block's worth of micro-cell
/// detail, ready to stamp into the world.
struct Model {
  std::string name;    ///< display name (UTF-8)
  std::string author;  ///< author credit (UTF-8, may be empty)
  /// LOD hint: highest subdivision level carrying useful detail, in
  /// [0, litholattice::subdiv::kMaxLevel].
  int maxLevel = 0;
  litholattice::subdiv::SparseOctree geometry;
};

/// Why a load failed. Every failure path returns a message, never throws.
enum class LoadError {
  Ok = 0,
  IoError,             ///< file could not be opened/read
  BadMagic,            ///< first 4 bytes are not "LTHM"
  UnsupportedVersion,  ///< version field != kFormatVersion (incl. future ones)
  Truncated,           ///< file ends before the declared fields do
  BadData,             ///< structurally invalid (bad lengths, out-of-range cell)
  BadCrc,              ///< CRC32 mismatch: file is corrupt
};

struct LoadResult {
  Model model;
  LoadError error = LoadError::Ok;
  std::string message;  ///< human-readable detail; set on every error
};

/// Serialize `m` to `path` (.lthm). Deterministic: the same Model always
/// produces byte-identical output. Returns false + `*err` on I/O failure.
bool saveModel(const std::string& path, const Model& m, std::string* err = nullptr);

/// Deserialize a .lthm file. Bad magic, unknown versions, truncation and
/// CRC mismatches are reported via LoadResult::error; a version newer than
/// kFormatVersion yields UnsupportedVersion with a clean message, never a
/// crash.
LoadResult loadModel(const std::string& path);

/// Deep equality: name, author, maxLevel and the full leaf stream.
bool modelEquals(const Model& a, const Model& b);

/// Stamp a model's geometry into a chunk-scoped subdiv store at block
/// coordinates (bx, by, bz) — chunk-local, 0..15 per axis; out-of-range
/// coordinates are a no-op.
///
/// Merge semantics: every non-empty leaf of the model overwrites the target
/// cell (via SparseOctree::set, which also clears finer cells beneath the
/// stamped cell). Target cells the model does not touch are left exactly as
/// they were. An empty model is a no-op: it neither creates nor clears an
/// entry.
void placeModel(litholattice::subdiv::SubdivStore& store, int bx, int by,
                int bz, const Model& m);

/// Authoring helper for building Models in code (workstream 5 authors the
/// first models through this). Deterministic: the same call sequence always
/// yields the same Model, hence a byte-identical .lthm file.
class ModelBuilder {
 public:
  explicit ModelBuilder(std::string name, std::string author = "");

  void setAuthor(std::string author) { author_ = std::move(author); }

  /// Set one cell (material 0 clears). Out-of-range level/coords are ignored.
  void setCell(int level, int x, int y, int z, uint8_t material);
  void clearCell(int level, int x, int y, int z) { setCell(level, x, y, z, 0); }

  /// Fill an inclusive box at one level. Bounds are order-normalized, so
  /// (x0 > x1) etc. is fine; coordinates are clamped into the level's range.
  /// An out-of-range level is a no-op.
  void fillBox(int level, int x0, int y0, int z0, int x1, int y1, int z1,
               uint8_t material);

  /// Finish: maxLevel becomes the highest level holding any cell
  /// (0 for an empty model).
  Model build() const;

 private:
  std::string name_;
  std::string author_;
  litholattice::subdiv::SparseOctree tree_;
};

}  // namespace lithokraft::model
