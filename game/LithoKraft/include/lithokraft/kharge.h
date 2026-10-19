// LithoKraft kharge: first IStat. electrical-charge morality for programs.
// Player spawns neutral (0). Actions shift toward negative (-64, red) or
// positive (+64, blue). Visible in UI. Exposed by engine, optional for mods.
#pragma once
#include <cstdint>

namespace lithokraft {

constexpr int8_t kMaxKharge = 64;
constexpr int8_t kMinKharge = -64;

/// Kharge polarity (like electrical charge).
enum class Kharge : int8_t {
  kNegative = -1,  ///< red — harmful actions
  kNeutral = 0,    ///< spawn state — uncharged
  kPositive = 1,   ///< blue — helpful actions
};

/// Charge state for a player. Tied to the player, shown in UI.
struct KhargeState {
  int8_t value = 0;  ///< -64 (red) to +64 (blue), 0 = neutral

  Kharge polarity() const {
    if (value > 0) return Kharge::kPositive;
    if (value < 0) return Kharge::kNegative;
    return Kharge::kNeutral;
  }

  void adjust(int8_t delta) {
    int v = static_cast<int>(value) + delta;
    if (v > kMaxKharge) v = kMaxKharge;
    if (v < kMinKharge) v = kMinKharge;
    value = static_cast<int8_t>(v);
  }
};

/// Charge system: engine exposes, mods can use or omit.
class IKharge {
 public:
  virtual ~IKharge() = default;
  virtual KhargeState khargeFor(uint64_t playerId) const = 0;
  virtual void adjustKharge(uint64_t playerId, int8_t delta) = 0;
};

}  // namespace lithokraft
