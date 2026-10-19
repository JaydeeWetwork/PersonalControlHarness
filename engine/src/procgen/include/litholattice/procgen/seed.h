// Deterministic seeding for procedural generation: 64-bit mixing, seed derivation (world seed ->
// dimension -> chunk -> feature), lattice hashing for noise, and a small PRNG. Everything here is
// a pure function of its inputs, identical on every platform and thread.
#pragma once
#include <cstdint>
#include <string_view>

namespace litholattice::procgen {

/// SplitMix64 finalizer: a strong 64-bit bit mixer.
constexpr uint64_t mix64(uint64_t z) {
  z += 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

/// A new seed from `base` and a salt (feature id, octave index, node seed, ...).
constexpr uint64_t deriveSeed(uint64_t base, uint64_t salt) { return mix64(base ^ mix64(salt)); }

/// FNV-1a of a name: stable salts from strings ("organic", "ore_vein").
constexpr uint64_t nameHash(std::string_view s) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (const char c : s) h = (h ^ uint8_t(c)) * 0x100000001B3ull;
  return h;
}

/// World seed + dimension id: every dimension gets independent noise from the same world seed.
constexpr uint64_t dimensionSeed(uint64_t worldSeed, std::string_view dimension) {
  return deriveSeed(worldSeed, nameHash(dimension));
}

/// Dimension seed + chunk coordinate: seeds per-chunk randomness (carvers, features).
constexpr uint64_t chunkSeed(uint64_t dimSeed, int32_t cx, int32_t cz) {
  return deriveSeed(dimSeed, (uint64_t(uint32_t(cx)) << 32) | uint32_t(cz));
}

/// Hash of an integer lattice point (up to 4D) under `seed`; noise gradients and cell points.
constexpr uint32_t latticeHash(uint64_t seed, int32_t x, int32_t y, int32_t z = 0, int32_t w = 0) {
  uint64_t h = seed;
  h = mix64(h ^ (uint64_t(uint32_t(x)) * 0x9E3779B97F4A7C15ull));
  h ^= uint64_t(uint32_t(y)) * 0xC2B2AE3D27D4EB4Full;
  h ^= uint64_t(uint32_t(z)) * 0x165667B19E3779F9ull;
  h ^= uint64_t(uint32_t(w)) * 0xD6E8FEB86659FD93ull;
  return uint32_t(mix64(h) >> 32);
}

/// Maps a hash to [0, 1) and [-1, 1).
constexpr float unitFloat(uint32_t h) { return float(h >> 8) * (1.f / 16777216.f); }
constexpr float signedUnitFloat(uint32_t h) { return unitFloat(h) * 2.f - 1.f; }

/// xoshiro256** PRNG seeded through SplitMix64. Small, fast, reproducible; one per task.
class Rng {
 public:
  explicit Rng(uint64_t seed) {
    for (uint64_t& s : s_) { seed += 0x9E3779B97F4A7C15ull; s = mix64(seed); }
  }
  uint64_t next() {
    const uint64_t result = rotl(s_[1] * 5, 7) * 9, t = s_[1] << 17;
    s_[2] ^= s_[0]; s_[3] ^= s_[1]; s_[1] ^= s_[2]; s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl(s_[3], 45);
    return result;
  }
  /// Uniform in [0, 1).
  float uniform() { return float(next() >> 40) * (1.f / 16777216.f); }
  /// Uniform in [lo, hi).
  float range(float lo, float hi) { return lo + (hi - lo) * uniform(); }
  /// Uniform integer in [lo, hi] (hi >= lo).
  int range(int lo, int hi) { return lo + int(next() % uint64_t(int64_t(hi) - lo + 1)); }

 private:
  static constexpr uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
  uint64_t s_[4]{};
};

}  // namespace litholattice::procgen
