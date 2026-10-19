// IWorldQuery: the seam between the general lithoscript `query` command and
// the engine backend that can SEE the world ("SQL for voxels").
//
// The console module owns the command, its syntax validation, and its caps:
// region bounds are MANDATORY (fail-closed: no unbounded scans), result sets
// are capped, ordering is deterministic. The game registers the backend via
// ConsoleRegistry::setWorldQuery(). This is also the primitive the future
// agent/proxy builders use to SEE the world — read-only, budgeted, logged.
//
// TRUST: queries are read-only. A semi-trusted (Fragment-authored) script may
// query; it cannot mutate the world through this interface. The hardening
// layer (step budget etc.) applies to the `query` command like any other.
//
// Provisional syntax (flaggable for JD):
//   query blocks <blockId> <x1> <y1> <z1> <x2> <y2> <z2>
//   query count  <blockId> <x1> <y1> <z1> <x2> <y2> <z2>
//   query entities <type|*> <cx> <cy> <cz> <radius>
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace litholattice::console {

/// Max cells scanned by one block query: 2^22 (e.g. a 256x128x128 region).
/// Named constant, not a magic number. Bounds are mandatory AND capped:
/// fail-closed on unbounded queries ("no scanning the infinite Void").
inline constexpr uint64_t kMaxQueryCells = 4194304;
/// Max hits returned per query; the result reports totalMatches + truncated.
inline constexpr size_t kMaxQueryResults = 1024;

struct BlockHit {
  int x = 0, y = 0, z = 0;
  uint32_t blockId = 0;
};

struct EntityHit {
  uint64_t id = 0;
  uint32_t type = 0;
  std::string typeName;  ///< backend-provided pretty name, may be empty
  double x = 0, y = 0, z = 0;
};

struct WorldQueryResult {
  std::vector<BlockHit> blocks;
  std::vector<EntityHit> entities;
  uint64_t cellsScanned = 0;
  uint64_t totalMatches = 0;  ///< matches before the kMaxQueryResults cap
  bool truncated = false;
};

/// Read-only world query backend, implemented by the game.
class IWorldQuery {
 public:
  virtual ~IWorldQuery() = default;

  /// Scan the inclusive box for `blockId`. Deterministic order: x outermost,
  /// then y, then z. Unloaded cells read as air (never an error).
  virtual WorldQueryResult queryBlocks(uint32_t blockId, int x1, int y1, int z1,
                                       int x2, int y2, int z2,
                                       std::string* err) = 0;
  /// Same scan, counting only (no per-cell hits returned).
  virtual WorldQueryResult queryCount(uint32_t blockId, int x1, int y1, int z1,
                                      int x2, int y2, int z2,
                                      std::string* err) = 0;
  /// Entities within `radius` of (cx, cy, cz); `typeName` "*" matches all.
  /// Deterministic order: ascending entity id.
  virtual WorldQueryResult queryEntities(const std::string& type, double cx,
                                         double cy, double cz, double radius,
                                         std::string* err) = 0;
};

}  // namespace litholattice::console
