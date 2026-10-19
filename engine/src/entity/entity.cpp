#include "litholattice/entity/entity.h"

#include <algorithm>

namespace litholattice {

uint64_t EntityManager::spawn(uint32_t type, double x, double y, double z) {
  const uint64_t id = nextId_++;
  Entity e;
  e.id = id;
  e.type = type;
  e.x = x;
  e.y = y;
  e.z = z;
  entities_[id] = e;
  sortedIds_.push_back(id);  // ids are monotonic: append keeps the list sorted
  if (factory_) behaviors_[id] = factory_(type);
  return id;
}

void EntityManager::despawn(uint64_t id) {
  auto it = entities_.find(id);
  if (it != entities_.end()) it->second.dead = true;
}

Entity* EntityManager::get(uint64_t id) {
  auto it = entities_.find(id);
  return it == entities_.end() ? nullptr : &it->second;
}

std::vector<Entity*> EntityManager::querySphere(double x, double y, double z, double radius,
                                                uint32_t type) {
  // Visit in id order via the persistent sorted id list: unordered_map bucket
  // order is a function of insertion/erase history, and query results must
  // not depend on it. (H-ENT-5: no per-call rebuild + sort.)
  std::vector<Entity*> out;
  const double r2 = radius * radius;
  for (uint64_t id : sortedIds_) {
    auto it = entities_.find(id);
    if (it == entities_.end()) continue;
    Entity& e = it->second;
    if (e.dead) continue;
    if (type != UINT32_MAX && e.type != type) continue;
    const double dx = e.x - x, dy = e.y - y, dz = e.z - z;
    if (dx * dx + dy * dy + dz * dz <= r2) out.push_back(&e);
  }
  return out;
}

void EntityManager::tick(double dt) {
  // Tick in id order (the persistent sorted list): behavior execution order
  // is then a pure function of the entity set, stable across STL builds,
  // edit histories, and save/load round-trips.
  // Iterate a COPY of the id list: a behavior may spawn() during its tick
  // (appending to sortedIds_) — the copy keeps this pass stable, and the
  // newborn entity first ticks next call.
  const std::vector<uint64_t> ids = sortedIds_;
  for (uint64_t id : ids) {
    auto it = entities_.find(id);
    if (it == entities_.end() || it->second.dead) continue;
    Entity& e = it->second;
    ++e.age;
    auto bit = behaviors_.find(id);
    if (bit != behaviors_.end() && bit->second) {
      // Exception boundary (H-ENT-5): a throwing behavior must not abort
      // the pass mid-way (the old code left age half-advanced and the
      // dead-cull unrun). Contain it: the offender is marked dead and
      // culled below; every other entity still ticks.
      try {
        bit->second->onTick(e, dt);
      } catch (...) {
        e.dead = true;
      }
    }
  }
  bool anyDead = false;
  for (auto it = entities_.begin(); it != entities_.end();) {
    if (it->second.dead) {
      behaviors_.erase(it->first);
      it = entities_.erase(it);
      anyDead = true;
    } else {
      ++it;
    }
  }
  if (anyDead) {
    sortedIds_.erase(std::remove_if(sortedIds_.begin(), sortedIds_.end(),
                                    [this](uint64_t id) {
                                      return entities_.find(id) == entities_.end();
                                    }),
                     sortedIds_.end());
  }
}

void EntityManager::clear() {
  entities_.clear();
  behaviors_.clear();
  sortedIds_.clear();
}

}  // namespace litholattice
