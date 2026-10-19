// GameWorldQuery backend: pause-scan-restore region queries. LF line endings.
#include "lithokraft/game/world_query.h"

#include <algorithm>

#include "litholattice/entity/entity.h"
#include "litholattice/world/chunk_map.h"

namespace lithokraft {
namespace {

using litholattice::console::kMaxQueryResults;
using litholattice::console::WorldQueryResult;

}  // namespace

bool GameWorldQuery::pauseSim(std::string* err) {
  if (!hooks_.simRunning || !hooks_.simRunning()) return true;  // no sim: chunks are game-owned
  hooks_.setSimPaused(true);
  // waitLoops guarantees a full iteration ran after our pause request, so no
  // tick is in flight and none will start. Timeout = fail-closed, never hang.
  if (!hooks_.waitSimSettled(1, std::chrono::milliseconds(5000))) {
    resumeSim();
    if (err) *err = "sim did not settle; query aborted";
    return false;
  }
  return true;
}

void GameWorldQuery::resumeSim() {
  if (!hooks_.simRunning || !hooks_.simRunning()) return;
  hooks_.setSimPaused(hooks_.gamePaused ? hooks_.gamePaused() : false);
}

WorldQueryResult GameWorldQuery::queryBlocks(uint32_t blockId, int x1, int y1, int z1, int x2,
                                             int y2, int z2, std::string* err) {
  WorldQueryResult r;
  litholattice::ChunkMap* chunks = hooks_.chunks ? hooks_.chunks() : nullptr;
  if (!chunks) {
    if (err) *err = "no world loaded";
    return r;
  }
  if (!pauseSim(err)) return r;
  // Deterministic order: x outermost, then y, then z.
  for (int x = x1; x <= x2; ++x)
    for (int y = y1; y <= y2; ++y)
      for (int z = z1; z <= z2; ++z) {
        ++r.cellsScanned;
        if (chunks->get(x, y, z) != blockId) continue;
        ++r.totalMatches;
        if (r.blocks.size() < kMaxQueryResults)
          r.blocks.push_back({x, y, z, blockId});
        else
          r.truncated = true;
      }
  resumeSim();
  return r;
}

WorldQueryResult GameWorldQuery::queryCount(uint32_t blockId, int x1, int y1, int z1, int x2,
                                           int y2, int z2, std::string* err) {
  WorldQueryResult r;
  litholattice::ChunkMap* chunks = hooks_.chunks ? hooks_.chunks() : nullptr;
  if (!chunks) {
    if (err) *err = "no world loaded";
    return r;
  }
  if (!pauseSim(err)) return r;
  for (int x = x1; x <= x2; ++x)
    for (int y = y1; y <= y2; ++y)
      for (int z = z1; z <= z2; ++z) {
        ++r.cellsScanned;
        if (chunks->get(x, y, z) == blockId) ++r.totalMatches;
      }
  resumeSim();
  return r;
}

WorldQueryResult GameWorldQuery::queryEntities(const std::string& type, double cx, double cy,
                                              double cz, double radius, std::string* err) {
  WorldQueryResult r;
  litholattice::EntityManager* em = hooks_.entities ? hooks_.entities() : nullptr;
  if (!em) {
    if (err) *err = "no world loaded";
    return r;
  }
  uint32_t typeId = UINT32_MAX;
  if (type != "*") {
    if (!hooks_.entityTypeId) {
      if (err) *err = "entity type names unavailable";
      return r;
    }
    typeId = hooks_.entityTypeId(type);
    if (typeId == UINT32_MAX) {
      if (err) *err = "unknown entity type '" + type + "'";
      return r;
    }
  }
  // Entities live on the game thread; no pause needed.
  auto hits = em->querySphere(cx, cy, cz, radius, typeId);
  // Deterministic order: ascending id (querySphere order is unspecified).
  std::sort(hits.begin(), hits.end(),
            [](const litholattice::Entity* a, const litholattice::Entity* b) { return a->id < b->id; });
  for (const litholattice::Entity* e : hits) {
    ++r.totalMatches;
    if (r.entities.size() < kMaxQueryResults) {
      litholattice::console::EntityHit h;
      h.id = e->id;
      h.type = e->type;
      h.typeName = hooks_.entityTypeName ? hooks_.entityTypeName(e->type) : "";
      h.x = e->x;
      h.y = e->y;
      h.z = e->z;
      r.entities.push_back(std::move(h));
    } else {
      r.truncated = true;
    }
  }
  return r;
}

}  // namespace lithokraft
