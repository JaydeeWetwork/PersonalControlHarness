// PlayerState wire format + PlayerManager implementation.
//
// Wire format (kSerializedSize = 68 bytes, explicit little-endian — never a
// raw struct copy, so the bytes are identical on every platform):
//   [0..8)   u64 playerId
//   [8..56)  6 x f64: x, y, z, vx, vy, vz   (bit-exact via u64 copies)
//   [56..64) 2 x f32: yaw, pitch            (bit-exact via u32 copies)
//   [64]     u8 dimension
//   [65]     u8 health
//   [66..68) reserved, written 0, ignored on read (append-only evolution)
#include "litholattice/player/player_state.h"

#include <algorithm>
#include <cstring>

#include "litholattice/player/player.h"
#include "litholattice/player/vitals.h"

namespace litholattice {
namespace {

void putU64(uint8_t* out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out[i] = uint8_t(v >> (8 * i));
}
void putU32(uint8_t* out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out[i] = uint8_t(v >> (8 * i));
}
uint64_t getU64(const uint8_t* in) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | in[i];
  return v;
}
uint32_t getU32(const uint8_t* in) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | in[i];
  return v;
}
void putF64(uint8_t* out, double d) {
  uint64_t bits;
  std::memcpy(&bits, &d, 8);
  putU64(out, bits);
}
double getF64(const uint8_t* in) {
  const uint64_t bits = getU64(in);
  double d;
  std::memcpy(&d, &bits, 8);
  return d;
}
void putF32(uint8_t* out, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  putU32(out, bits);
}
float getF32(const uint8_t* in) {
  const uint32_t bits = getU32(in);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

}  // namespace

uint32_t PlayerState::serialize(uint8_t* out, uint32_t maxLen) const {
  if (!out || maxLen < kSerializedSize) return 0;
  std::memset(out, 0, kSerializedSize);
  putU64(out + 0, playerId);
  putF64(out + 8, x);
  putF64(out + 16, y);
  putF64(out + 24, z);
  putF64(out + 32, vx);
  putF64(out + 40, vy);
  putF64(out + 48, vz);
  putF32(out + 56, yaw);
  putF32(out + 60, pitch);
  out[64] = dimension;
  out[65] = health;
  return kSerializedSize;
}

uint32_t PlayerState::deserialize(const uint8_t* in, uint32_t len) {
  if (!in || len < kSerializedSize) return 0;  // fail closed: state untouched
  // Decode into a scratch copy first so a future mid-format check can bail
  // without leaving a half-written state behind.
  PlayerState s;
  s.playerId = getU64(in + 0);
  s.x = getF64(in + 8);
  s.y = getF64(in + 16);
  s.z = getF64(in + 24);
  s.vx = getF64(in + 32);
  s.vy = getF64(in + 40);
  s.vz = getF64(in + 48);
  s.yaw = getF32(in + 56);
  s.pitch = getF32(in + 60);
  s.dimension = in[64];
  s.health = in[65];
  *this = s;
  return kSerializedSize;
}

// ---------------------------------------------------------------------------

PlayerManager::PlayerManager() : abilities_(stats_) {
  // Route every registry change into the owning player's listeners. The
  // registries fire synchronously, so listeners observe changes in exactly
  // the order the game makes them.
  vitals_.setChangeCallback(
      [this](uint64_t p, uint32_t v, int32_t o, int32_t n) { routeVitalChanged(p, v, o, n); });
  stats_.setChangeCallback(
      [this](uint64_t p, uint32_t s, int32_t o, int32_t n) { routeStatChanged(p, s, o, n); });
  abilities_.setChangeCallback(
      [this](uint64_t p, uint32_t a, bool active) { routeAbilityChanged(p, a, active); });
}

PlayerManager::~PlayerManager() = default;

Player& PlayerManager::ensurePlayer(uint64_t playerId) {
  const auto it = players_.find(playerId);
  if (it != players_.end()) return *it->second;
  auto p = std::make_unique<Player>(playerId, "", vitals_, stats_, abilities_);
  p->attachRouting();
  Player& ref = *p;
  players_.emplace(playerId, std::move(p));
  return ref;
}

Player& PlayerManager::addPlayer(uint64_t playerId, std::string displayName) {
  if (playerId == 0) {  // 0 is "nobody": hand out the next free id instead
    playerId = players_.empty() ? 1 : players_.rbegin()->first + 1;
    if (playerId == 0) playerId = 1;  // id space wrapped (theoretical)
  }
  Player& p = ensurePlayer(playerId);
  p.setDisplayName(std::move(displayName));
  return p;
}

Player& PlayerManager::localPlayer() { return ensurePlayer(localId_); }

PlayerState& PlayerManager::local() { return localPlayer().state(); }

const PlayerState& PlayerManager::get(uint64_t playerId) const {
  const Player* p = player(playerId);
  if (p) return p->state();
  static const PlayerState kNobody;  // playerId 0 sentinel
  return kNobody;
}

Player* PlayerManager::player(uint64_t playerId) {
  const auto it = players_.find(playerId);
  return it == players_.end() ? nullptr : it->second.get();
}

const Player* PlayerManager::player(uint64_t playerId) const {
  const auto it = players_.find(playerId);
  return it == players_.end() ? nullptr : it->second.get();
}

std::vector<uint64_t> PlayerManager::playerIds() const {
  std::vector<uint64_t> out;
  out.reserve(players_.size());
  for (const auto& [id, p] : players_) out.push_back(id);
  return out;  // map order = ascending id
}

uint32_t PlayerManager::healthVitalId() {
  // PLY-4: cache the health vital id in vitalsHealth_ instead of a string
  // map lookup on every vital change. Registration is first-wins and
  // append-only, so once resolved the id can never go stale; before
  // registration findVital returns 0 and the lookup simply retries.
  if (!vitalsHealth_) vitalsHealth_ = vitals_.findVital(kHealthVitalName);
  return vitalsHealth_;
}

void PlayerManager::addOrUpdate(const PlayerState& state) {
  if (state.playerId == 0) return;  // id 0 is "nobody": never a real player
  Player& p = ensurePlayer(state.playerId);
  p.state() = state;
  // Keep the health vital in step with the synced field (see header note).
  if (const uint32_t health = healthVitalId())
    vitals_.set(state.playerId, health, state.health);
}

void PlayerManager::remove(uint64_t playerId) {
  abilities_.removePlayer(playerId);  // first: strips that player's boosts
  stats_.removePlayer(playerId);      // then: purges values
  vitals_.removePlayer(playerId);
  players_.erase(playerId);
}

void PlayerManager::tick(uint32_t dtTicks) { abilities_.tick(dtTicks); }

void PlayerManager::notifyStateChanged(uint64_t playerId) {
  if (Player* p = player(playerId)) p->notifyStateChanged();
}

void PlayerManager::routeVitalChanged(uint64_t playerId, uint32_t vitalId, int32_t oldValue,
                                      int32_t newValue) {
  Player* p = player(playerId);
  if (!p) return;
  // Health mirror: the synced state field always carries the health vital.
  if (vitalId == healthVitalId())
    p->state().health = static_cast<uint8_t>(std::clamp(newValue, 0, 255));
  p->dispatchVitalChanged(vitalId, oldValue, newValue);
}

void PlayerManager::routeStatChanged(uint64_t playerId, uint32_t statId, int32_t oldValue,
                                     int32_t newValue) {
  Player* p = player(playerId);
  if (!p) return;
  p->dispatchStatChanged(statId, oldValue, newValue);
}

void PlayerManager::routeAbilityChanged(uint64_t playerId, uint32_t abilityId, bool active) {
  if (Player* p = player(playerId)) p->dispatchAbilityChanged(abilityId, active);
}

}  // namespace litholattice
