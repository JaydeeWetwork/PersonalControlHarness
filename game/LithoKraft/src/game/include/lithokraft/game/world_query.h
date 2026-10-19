// GameWorldQuery: the IWorldQuery backend for LithoKraft (v1).
//
// THREADING (the critical design point): while the sim runs, the sim thread
// OWNS the chunks (SimThread::getBlock's own header: "fine for a handful of
// queries per frame, not for bulk scans"). A region scan therefore runs under
// a brief, explicit sim pause:
//
//   1. setSimPaused(true)                       — no new ticks start
//   2. waitSimSettled(1 tick, 5 s timeout)      — the in-flight tick finishes;
//      on timeout the query FAILS CLOSED (error, no scan, sim restored)
//   3. scan ChunkMap::get directly              — game thread has the chunks
//      to itself; get() is const and returns air for unloaded cells
//   4. setSimPaused(previousState)              — the sim resumes; a scan of
//      the max 4M cells is a few ms of array reads, no visible hitch
//
// The previous pause state is restored, never forced off: a query issued
// while the game is paused leaves it paused. Deterministic ordering:
// blocks x-outermost then y then z; entities by ascending id.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include "litholattice/console/iworldquery.h"

namespace litholattice {
class ChunkMap;
class EntityManager;
}  // namespace litholattice

namespace lithokraft {

class GameWorldQuery : public litholattice::console::IWorldQuery {
 public:
  struct Hooks {
    /// Chunk storage; valid to read while the sim is paused+settled.
    std::function<litholattice::ChunkMap*()> chunks;
    std::function<litholattice::EntityManager*()> entities;
    /// True while the sim thread exists and ticks.
    std::function<bool()> simRunning;
    std::function<void(bool)> setSimPaused;
    /// Was the GAME paused before this query (restore target).
    std::function<bool()> gamePaused;
    /// Blocks until n more sim iterations start (false on timeout).
    std::function<bool(uint64_t, std::chrono::milliseconds)> waitSimSettled;
    /// Pretty entity type name ("item"); empty if unknown.
    std::function<std::string(uint32_t)> entityTypeName;
    /// Resolve a type name to its id; UINT32_MAX if unknown.
    std::function<uint32_t(const std::string&)> entityTypeId;
  };

  explicit GameWorldQuery(Hooks hooks) : hooks_(std::move(hooks)) {}

  litholattice::console::WorldQueryResult queryBlocks(
      uint32_t blockId, int x1, int y1, int z1, int x2, int y2, int z2,
      std::string* err) override;
  litholattice::console::WorldQueryResult queryCount(
      uint32_t blockId, int x1, int y1, int z1, int x2, int y2, int z2,
      std::string* err) override;
  litholattice::console::WorldQueryResult queryEntities(
      const std::string& type, double cx, double cy, double cz, double radius,
      std::string* err) override;

 private:
  /// Pauses the sim and waits for the in-flight tick; returns false (with
  /// *err set) when the sim will not settle — the caller must NOT scan.
  /// On success the caller MUST call resumeSim().
  bool pauseSim(std::string* err);
  void resumeSim();

  Hooks hooks_;
};

}  // namespace lithokraft
