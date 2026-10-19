// StatRegistry implementation. See stat.h for the base/boost value model.
#include "litholattice/player/stat_registry.h"

#include <algorithm>
#include <cstring>

namespace litholattice {
namespace {

int32_t clampTo(const StatDef& def, int64_t v) {
  if (v < def.minValue) return def.minValue;
  if (v > def.maxValue) return def.maxValue;
  return static_cast<int32_t>(v);
}

void putU32(uint8_t* out, uint32_t v) {
  out[0] = uint8_t(v);
  out[1] = uint8_t(v >> 8);
  out[2] = uint8_t(v >> 16);
  out[3] = uint8_t(v >> 24);
}
void putI32(uint8_t* out, int32_t v) { putU32(out, static_cast<uint32_t>(v)); }
uint32_t getU32(const uint8_t* in) {
  return uint32_t(in[0]) | (uint32_t(in[1]) << 8) | (uint32_t(in[2]) << 16) | (uint32_t(in[3]) << 24);
}
int32_t getI32(const uint8_t* in) { return static_cast<int32_t>(getU32(in)); }

}  // namespace

uint32_t StatRegistry::registerStat(const StatDef& def) {
  if (!def.name || def.name[0] == '\0' || def.minValue > def.maxValue) return 0;
  // First-wins by name (same rule as the content registry): a duplicate
  // registration returns the existing id and keeps the original def, so two
  // systems asking for "health" can never end up with split ids.
  if (const uint32_t existing = findStat(def.name)) return existing;
  uint32_t id = def.id;
  if (id == 0) {
    id = nextId_;
  } else if (defs_.count(id)) {
    return 0;  // explicit id collision with a different name: fail closed
  }
  if (id >= nextId_) nextId_ = id + 1;
  OwnedDef owned;
  owned.name = def.name;
  owned.def = def;
  owned.def.id = id;
  owned.def.name = owned.name.c_str();
  // The default must itself be inside the range, or untouched stats would
  // read out-of-band values.
  owned.def.defaultValue = clampTo(owned.def, owned.def.defaultValue);
  defs_.emplace(id, std::move(owned));
  byName_[defs_[id].name] = id;
  return id;
}

uint32_t StatRegistry::findStat(std::string_view name) const {
  const auto it = byName_.find(std::string(name));
  return it == byName_.end() ? 0 : it->second;
}

const StatDef* StatRegistry::statDef(uint32_t statId) const {
  const auto it = defs_.find(statId);
  return it == defs_.end() ? nullptr : &it->second.def;
}

int32_t StatRegistry::baseOrDefault(uint64_t playerId, uint32_t statId, const StatDef& def) const {
  const auto pit = base_.find(playerId);
  if (pit == base_.end()) return def.defaultValue;
  const auto sit = pit->second.find(statId);
  return sit == pit->second.end() ? def.defaultValue : sit->second;
}

int32_t StatRegistry::effective(uint64_t playerId, uint32_t statId, const StatDef& def) const {
  int64_t v = baseOrDefault(playerId, statId, def);
  const auto pit = boosts_.find(playerId);
  if (pit != boosts_.end()) {
    const auto sit = pit->second.find(statId);
    if (sit != pit->second.end())
      for (const auto& [source, amount] : sit->second) v += amount;
  }
  return clampTo(def, v);
}

int32_t StatRegistry::get(uint64_t playerId, uint32_t statId) const {
  const StatDef* def = statDef(statId);
  return def ? effective(playerId, statId, *def) : 0;
}

int32_t StatRegistry::getBase(uint64_t playerId, uint32_t statId) const {
  const StatDef* def = statDef(statId);
  return def ? baseOrDefault(playerId, statId, *def) : 0;
}

void StatRegistry::fireIfChanged(uint64_t playerId, uint32_t statId, int32_t oldEffective) {
  if (!changeFn_) return;
  const StatDef* def = statDef(statId);
  if (!def) return;
  const int32_t now = effective(playerId, statId, *def);
  if (now != oldEffective) changeFn_(playerId, statId, oldEffective, now);
}

void StatRegistry::set(uint64_t playerId, uint32_t statId, int32_t value) {
  const StatDef* def = statDef(statId);
  if (!def) return;
  const int32_t old = effective(playerId, statId, *def);
  base_[playerId][statId] = clampTo(*def, value);
  fireIfChanged(playerId, statId, old);
}

void StatRegistry::adjust(uint64_t playerId, uint32_t statId, int32_t delta) {
  const StatDef* def = statDef(statId);
  if (!def) return;
  // int64 intermediate: base + delta can overflow int32 before clamping.
  set(playerId, statId, clampTo(*def, int64_t(baseOrDefault(playerId, statId, *def)) + delta));
}

void StatRegistry::addBoost(uint64_t playerId, uint32_t statId, int32_t amount, uint64_t source) {
  const StatDef* def = statDef(statId);
  if (!def) return;
  const int32_t old = effective(playerId, statId, *def);
  boosts_[playerId][statId][source] = amount;  // same source replaces, never stacks with itself
  fireIfChanged(playerId, statId, old);
}

void StatRegistry::removeBoost(uint64_t playerId, uint32_t statId, uint64_t source) {
  const StatDef* def = statDef(statId);
  if (!def) return;
  const auto pit = boosts_.find(playerId);
  if (pit == boosts_.end()) return;
  const auto sit = pit->second.find(statId);
  if (sit == pit->second.end() || !sit->second.count(source)) return;
  const int32_t old = effective(playerId, statId, *def);
  sit->second.erase(source);
  if (sit->second.empty()) pit->second.erase(sit);
  if (pit->second.empty()) boosts_.erase(pit);
  fireIfChanged(playerId, statId, old);
}

void StatRegistry::removePlayer(uint64_t playerId) {
  base_.erase(playerId);
  boosts_.erase(playerId);
}

std::vector<uint32_t> StatRegistry::touchedStats(uint64_t playerId) const {
  std::vector<uint32_t> out;
  const auto pit = base_.find(playerId);
  if (pit != base_.end())
    for (const auto& [statId, v] : pit->second) out.push_back(statId);
  return out;  // map order = ascending stat id
}

uint32_t StatRegistry::serializeStats(uint64_t playerId, uint8_t* out, uint32_t maxLen) const {
  const std::vector<uint32_t> ids = touchedStats(playerId);
  const uint32_t need = 4 + static_cast<uint32_t>(ids.size()) * 8;
  if (!out || maxLen < need) return 0;
  putU32(out, static_cast<uint32_t>(ids.size()));
  uint8_t* p = out + 4;
  for (const uint32_t id : ids) {
    const auto pit = base_.find(playerId);
    putU32(p, id);
    putI32(p + 4, pit->second.at(id));
    p += 8;
  }
  return need;
}

uint32_t StatRegistry::deserializeStats(uint64_t playerId, const uint8_t* in, uint32_t len) {
  if (!in || len < 4) return 0;
  const uint32_t count = getU32(in);
  // Validate the full block before mutating anything (fail closed).
  if (4ull + uint64_t(count) * 8ull > len) return 0;
  // Replace semantics: the block IS the player's saved vitals.
  base_.erase(playerId);
  const uint8_t* p = in + 4;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t id = getU32(p);
    const int32_t value = getI32(p + 4);
    p += 8;
    const StatDef* def = statDef(id);
    if (!def) continue;  // stat from a newer build / another game: skip, don't fail
    base_[playerId][id] = clampTo(*def, value);
  }
  return 4 + count * 8;
}

}  // namespace litholattice
