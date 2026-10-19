// Fragment state: the data model for the Fragment book's upgrade state.
// The Fragment is the player's starting book (journal, unlock log, recipes,
// chip archive). Design (locked): it starts LithoScript-only; finding the
// Spark (a character, not yet in code) upgrades the Fragment and UNLOCKS
// markdown rendering — an early-game super-upgrade. The Fragment also has
// a TIER, displayed on the Fragment: it starts at 0 and increments once
// per newly applied upgrade. This class is the state side of that design;
// Workstream 2's markdown renderer takes a plain `bool markdownEnabled`
// and reads this flag. LF line endings.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lithokraft {

/// The Fragment book's upgrade state. Default-constructed = a brand-new
/// Fragment: tier 0, LithoScript-only, markdown NOT unlocked.
///
/// GAME THREAD ONLY: no synchronization; call from Game's thread like every
/// other piece of Game-side state. NOTE (G-M6): see docs/FRAGMENT-UI-NEEDS.md
/// for the full remainder (dialog, Spark trigger, persistence).
///
/// PRODUCTION OWNERSHIP (A3 wiring, 2026-10-13, no-decision subset): Game
/// owns the live instance (Game::fragmentState()) and feeds
/// markdown_unlocked() into the Fragment renderer through
/// Game::renderFragmentText() — real production callers. upgradeFragment()
/// deliberately still has NO production caller: its trigger is JD decision
/// C3 (the Spark, or an interim trigger), and persisting this state in
/// player.dat is likewise C3-gated. Do not invent either.
class FragmentState {
 public:
  /// Fragment tier, displayed on the Fragment. Starts at 0; increments by
  /// exactly one for each newly applied upgrade (see upgradeFragment).
  /// Never moves when upgradeFragment() finds nothing new to apply.
  uint8_t tier() const { return tier_; }

  /// Markdown rendering enabled? False until the Spark upgrade (the
  /// Fragment starts LithoScript-only).
  bool markdown_unlocked() const { return markdown_unlocked_; }

  /// Applies ALL known Fragment upgrades not yet applied. This is the
  /// single entry point the Spark's Fragment-upgrade event calls when the
  /// player finds the Spark (progression, TBD) — and the entry point ANY
  /// future Fragment upgrade event calls. A markdown-specific name would
  /// be too narrow: the Fragment will gain other upgrades over time.
  /// Currently known upgrades: markdown rendering unlock.
  /// The tier increments once per newly applied upgrade, and ONLY then:
  /// calling this when nothing new is pending leaves the tier untouched.
  /// Idempotent: upgrading twice is a no-op, never an error.
  void upgradeFragment() {
    // Each known upgrade follows this pattern: apply once, bump the tier
    // once. Future upgrades add their own block here.
    if (!markdown_unlocked_) {
      markdown_unlocked_ = true;
      noteUpgradeApplied();
    }
  }

  /// Binary round-trip for save persistence. Format: magic "LFS1", u16
  /// version (2), u8 flags (bit 0 = markdown unlocked), u8 tier. All
  /// integers little-endian. Versioned so future Fragment fields can
  /// join the blob; v1 blobs are rejected (the format never shipped).
  std::vector<uint8_t> serialize() const;
  /// Inverse of serialize(). False + `*err` on malformed input; the state
  /// is left unchanged on failure.
  bool deserialize(const uint8_t* data, size_t len, std::string* err);

 private:
  /// Records one newly applied upgrade. Saturates at 255, never wraps.
  void noteUpgradeApplied() {
    if (tier_ < 255) ++tier_;
  }

  bool markdown_unlocked_ = false;
  uint8_t tier_ = 0;
};

}  // namespace lithokraft
