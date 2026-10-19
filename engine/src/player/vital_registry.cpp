// VitalRegistry implementation. See vital.h for the value model.
#include "litholattice/player/vital_registry.h"

#include <algorithm>

namespace litholattice {
namespace {

int32_t clampTo(const VitalDef& def, int64_t v) {
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

uint32_t VitalRegistry::registerVital(const VitalDef& def) {
  if (!def.name || def.name[0] == '\0' || def.minValue > def.maxValue) return 0;
  // First-wins by name (same rule as stats, abilities, and the content
  // registry): a duplicate registration returns the existing id and keeps
  // the original def, so a mod and the engine asking for "health" can
  // never end up with split ids.
  if (const uint32_t existing = findVital(def.name)) return existing;
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
  // The default must itself be inside the range, or untouched vitals would
  // read out-of-band values.
  owned.def.defaultValue = clampTo(owned.def, owned.def.defaultValue);
  defs_.emplace(id, std::move(owned));
  byName_[defs_[id].name] = id;
  return id;
}

uint32_t VitalRegistry::findVital(std::string_view name) const {
  const auto it = byName_.find(std::string(name));
  return it == byName_.end() ? 0 : it->second;
}

const VitalDef* VitalRegistry::vitalDef(uint32_t vitalId) const {
  const auto it = defs_.find(vitalId);
  return it == defs_.end() ? nullptr : &it->second.def;
}

int32_t VitalRegistry::valueOrDefault(uint64_t playerId, uint32_t vitalId,
                                      const VitalDef& def) const {
  const auto pit = values_.find(playerId);
  if (pit == values_.end()) return def.defaultValue;
  const auto vit = pit->second.find(vitalId);
  return vit == pit->second.end() ? def.defaultValue : vit->second;
}

int32_t VitalRegistry::get(uint64_t playerId, uint32_t vitalId) const {
  const VitalDef* def = vitalDef(vitalId);
  return def ? valueOrDefault(playerId, vitalId, *def) : 0;
}

void VitalRegistry::set(uint64_t playerId, uint32_t vitalId, int32_t value) {
  const VitalDef* def = vitalDef(vitalId);
  if (!def) return;
  const int32_t old = valueOrDefault(playerId, vitalId, *def);
  const int32_t now = clampTo(*def, value);
  values_[playerId][vitalId] = now;
  if (changeFn_ && now != old) changeFn_(playerId, vitalId, old, now);
}

void VitalRegistry::adjust(uint64_t playerId, uint32_t vitalId, int32_t delta) {
  const VitalDef* def = vitalDef(vitalId);
  if (!def) return;
  // int64 intermediate: value + delta can overflow int32 before clamping.
  set(playerId, vitalId, clampTo(*def, int64_t(valueOrDefault(playerId, vitalId, *def)) + delta));
}

void VitalRegistry::removePlayer(uint64_t playerId) { values_.erase(playerId); }

std::vector<uint32_t> VitalRegistry::touchedVitals(uint64_t playerId) const {
  std::vector<uint32_t> out;
  const auto pit = values_.find(playerId);
  if (pit != values_.end())
    for (const auto& [vitalId, v] : pit->second) out.push_back(vitalId);
  return out;  // map order = ascending vital id
}

uint32_t VitalRegistry::serializeVitals(uint64_t playerId, uint8_t* out, uint32_t maxLen) const {
  const std::vector<uint32_t> ids = touchedVitals(playerId);
  const uint32_t need = 4 + static_cast<uint32_t>(ids.size()) * 8;
  if (!out || maxLen < need) return 0;
  putU32(out, static_cast<uint32_t>(ids.size()));
  uint8_t* p = out + 4;
  const auto pit = values_.find(playerId);
  for (const uint32_t id : ids) {
    putU32(p, id);
    putI32(p + 4, pit->second.at(id));
    p += 8;
  }
  return need;
}

uint32_t VitalRegistry::deserializeVitals(uint64_t playerId, const uint8_t* in, uint32_t len) {
  if (!in || len < 4) return 0;
  const uint32_t count = getU32(in);
  // Validate the full block before mutating anything (fail closed).
  if (4ull + uint64_t(count) * 8ull > len) return 0;
  // Replace semantics: the block IS the player's saved vitals.
  values_.erase(playerId);
  const uint8_t* p = in + 4;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t id = getU32(p);
    const int32_t value = getI32(p + 4);
    p += 8;
    const VitalDef* def = vitalDef(id);
    if (!def) continue;  // vital from a newer build / another game: skip, don't fail
    values_[playerId][id] = clampTo(*def, value);
  }
  return 4 + count * 8;
}

}  // namespace litholattice
