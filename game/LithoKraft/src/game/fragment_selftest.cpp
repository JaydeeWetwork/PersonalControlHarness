// Fragment state self-test: the upgrade state machine (incl. tiers),
// binary round-trip, malformed-input handling, and the Spark-upgrade
// simulation hook.
// Headless: no window, no GL context. LF line endings.
#include "lithokraft/game/fragment_selftest.h"

#include <cstdio>
#include <string>
#include <vector>

#include "lithokraft/game/fragment_state.h"

namespace lithokraft {
namespace {

int fails = 0;
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++fails;
}

// -- Spark-upgrade simulation ------------------------------------------------
// Test hook simulating the Spark's Fragment-upgrade event: what the real game
// event will do is call upgradeFragment() on the player's FragmentState.
// Future wiring: when the Spark exists in code (a character the player finds),
// the Spark's Fragment-upgrade event handler (progression, TBD) calls
// state.upgradeFragment() exactly here.
void simulateSparkUpgrade(FragmentState& state) { state.upgradeFragment(); }

// -- State machine ------------------------------------------------------------
void testStateMachine() {
  FragmentState s;
  check(!s.markdown_unlocked(), "state: fresh Fragment starts LithoScript-only (markdown locked)");
  check(s.tier() == 0, "state: fresh Fragment starts at tier 0");
  s.upgradeFragment();
  check(s.markdown_unlocked(), "state: upgradeFragment() -> markdown unlocked");
  check(s.tier() == 1, "state: first upgrade (markdown) -> tier 1");
  s.upgradeFragment();  // idempotent
  check(s.markdown_unlocked(), "state: double unlock stays unlocked (no-op, never an error)");
  check(s.tier() == 1, "state: repeat upgrade with nothing new leaves tier at 1");
}

// -- Serialize round-trip ------------------------------------------------------
void testRoundTrip() {
  // Locked state.
  {
    FragmentState a;
    const std::vector<uint8_t> blob = a.serialize();
    FragmentState b;
    b.upgradeFragment();  // must not leak through the round-trip
    std::string err;
    check(b.deserialize(blob.data(), blob.size(), &err) && err.empty() &&
              !b.markdown_unlocked(),
          "round-trip: locked state survives serialize/deserialize");
  }
  // Unlocked state.
  {
    FragmentState a;
    simulateSparkUpgrade(a);
    const std::vector<uint8_t> blob = a.serialize();
    FragmentState b;
    std::string err;
    check(b.deserialize(blob.data(), blob.size(), &err) && err.empty() &&
              b.markdown_unlocked(),
          "round-trip: unlocked state survives serialize/deserialize");
  }
  // Format: "LFS1" + u16 v2 + u8 flags + u8 tier, little-endian.
  {
    FragmentState a;
    const std::vector<uint8_t> blob = a.serialize();
    check(blob.size() == 8 && blob[0] == 'L' && blob[1] == 'F' &&
              blob[2] == 'S' && blob[3] == '1' && blob[4] == 2 &&
              blob[5] == 0 && blob[6] == 0 && blob[7] == 0,
          "format: locked blob is exactly 'LFS1', u16 v2, flags 0, tier 0");
    a.upgradeFragment();
    const std::vector<uint8_t> blob2 = a.serialize();
    check(blob2.size() == 8 && blob2[6] == 0x01 && blob2[7] == 1,
          "format: upgraded blob sets flags bit 0 and tier 1");
  }
  // Tier persists across save/load.
  {
    FragmentState a;
    a.upgradeFragment();
    const std::vector<uint8_t> blob = a.serialize();
    FragmentState b;
    std::string err;
    check(b.deserialize(blob.data(), blob.size(), &err) && err.empty() &&
              b.tier() == 1 && b.markdown_unlocked(),
          "round-trip: tier survives serialize/deserialize");
  }
}

// -- Malformed input ------------------------------------------------------------
void testMalformed() {
  FragmentState s;
  std::string err;
  const uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  check(!s.deserialize(junk, sizeof junk, &err) && !err.empty() &&
            !s.markdown_unlocked(),
        "malformed: garbage blob fails cleanly, state unchanged (still locked)");
  check(!s.deserialize(nullptr, 0, &err) && !err.empty() &&
            !s.markdown_unlocked(),
        "malformed: empty blob fails cleanly, state unchanged");

  // Bad magic.
  FragmentState u;
  u.upgradeFragment();
  const std::vector<uint8_t> good = u.serialize();
  std::vector<uint8_t> bad = good;
  bad[0] = 'X';
  err.clear();
  check(!u.deserialize(bad.data(), bad.size(), &err) && !err.empty() &&
            u.markdown_unlocked(),
        "malformed: bad magic fails cleanly, unlocked state kept");

  // Wrong version.
  bad = good;
  bad[4] = 3;  // version u16 = 3 (current is 2)
  err.clear();
  check(!u.deserialize(bad.data(), bad.size(), &err) && !err.empty() &&
            u.markdown_unlocked(),
        "malformed: wrong version fails cleanly, unlocked state kept");

  // v1 blob (7 bytes, no tier field) is rejected: the version bump is strict.
  {
    const uint8_t v1[7] = {'L', 'F', 'S', '1', 1, 0, 0};
    FragmentState v;
    err.clear();
    check(!v.deserialize(v1, sizeof v1, &err) && !err.empty() &&
              v.tier() == 0 && !v.markdown_unlocked(),
          "malformed: v1 blob rejected, fresh state unchanged");
  }

  // Truncated at every length 0..6.
  for (size_t cut = 0; cut < good.size(); ++cut) {
    err.clear();
    check(!u.deserialize(good.data(), cut, &err) && !err.empty() &&
              u.markdown_unlocked(),
          "malformed: truncated blob fails cleanly, state unchanged");
  }

  // Trailing byte.
  std::vector<uint8_t> trailing = good;
  trailing.push_back(0xFF);
  err.clear();
  check(!u.deserialize(trailing.data(), trailing.size(), &err) &&
            !err.empty() && u.markdown_unlocked(),
        "malformed: trailing bytes fail cleanly, state unchanged");
}

// -- Spark-upgrade simulation ---------------------------------------------------
void testSparkUpgrade() {
  FragmentState s;
  check(!s.markdown_unlocked(), "spark: Fragment locked before the Spark upgrade");
  simulateSparkUpgrade(s);
  check(s.markdown_unlocked(), "spark: simulation hook drives locked -> unlocked");
  simulateSparkUpgrade(s);  // the event fires once; re-fire is harmless
  check(s.markdown_unlocked(), "spark: repeated upgrade is idempotent");
  // And it persists.
  const std::vector<uint8_t> blob = s.serialize();
  FragmentState afterLoad;
  std::string err;
  check(afterLoad.deserialize(blob.data(), blob.size(), &err) &&
            afterLoad.markdown_unlocked(),
        "spark: post-upgrade state survives a save/load cycle");
}

// -- Tier upgrades --------------------------------------------------------------
// The tier counts genuinely new upgrades: +1 per newly applied upgrade,
// never on a repeat call with nothing pending.
void testTierUpgrades() {
  // Tier starts at 0 on a fresh Fragment.
  {
    FragmentState s;
    check(s.tier() == 0, "tier: fresh Fragment starts at tier 0");
  }
  // First upgradeFragment() (markdown) -> tier 1.
  {
    FragmentState s;
    s.upgradeFragment();
    check(s.tier() == 1 && s.markdown_unlocked(),
          "tier: first upgrade (markdown) moves tier 0 -> 1");
  }
  // Repeat calls with nothing new pending -> tier stays 1.
  {
    FragmentState s;
    s.upgradeFragment();
    s.upgradeFragment();
    s.upgradeFragment();
    check(s.tier() == 1, "tier: repeat upgrades with nothing new leave tier at 1");
  }
  // Simulated future: a second upgrade lands later. Simulated by loading
  // a synthetic v2 blob carrying tier 1 with markdown NOT yet unlocked
  // (as if another upgrade got there first), then calling upgradeFragment():
  // the newly applied markdown upgrade bumps tier 1 -> 2 through the real
  // code path — no test seam, no special-casing.
  {
    const uint8_t synthetic[8] = {'L', 'F', 'S', '1', 2, 0, 0x00, 0x01};
    FragmentState s;
    std::string err;
    check(s.deserialize(synthetic, sizeof synthetic, &err) && err.empty() &&
              s.tier() == 1 && !s.markdown_unlocked(),
          "tier: synthetic prior-upgrade state loads (tier 1, markdown locked)");
    s.upgradeFragment();
    check(s.tier() == 2 && s.markdown_unlocked(),
          "tier: newly applied upgrade moves tier 1 -> 2");
    s.upgradeFragment();
    check(s.tier() == 2, "tier: no further bump once everything is applied");
  }
  // Tier persists across save/load.
  {
    FragmentState s;
    s.upgradeFragment();
    const std::vector<uint8_t> blob = s.serialize();
    FragmentState loaded;
    std::string err;
    check(loaded.deserialize(blob.data(), blob.size(), &err) && err.empty() &&
              loaded.tier() == 1,
          "tier: tier persists across save/load");
  }
  // Malformed blob leaves the tier (and everything else) unchanged.
  {
    FragmentState s;
    s.upgradeFragment();  // tier 1
    const uint8_t junk[8] = {9, 9, 9, 9, 9, 9, 9, 9};
    std::string err;
    check(!s.deserialize(junk, sizeof junk, &err) && !err.empty() &&
              s.tier() == 1 && s.markdown_unlocked(),
          "tier: malformed blob leaves tier unchanged (fail-closed)");
  }
}

}  // namespace

int runFragmentSelfTest() {
  fails = 0;
  std::printf("fragment selftest:\n");
  testStateMachine();
  testRoundTrip();
  testMalformed();
  testSparkUpgrade();
  testTierUpgrades();
  std::printf("%s\n", fails ? "FRAGMENT SELFTEST: FAILED" : "FRAGMENT SELFTEST: OK");
  return fails;
}

}  // namespace lithokraft
