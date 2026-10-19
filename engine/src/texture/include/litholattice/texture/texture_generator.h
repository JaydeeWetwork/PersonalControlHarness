// texture_generator.h: deterministic, seamlessly tileable procedural
// textures from TextureRecipe data (texture_recipe.h). Clean-room
// implementation: value noise, gradient (Perlin-style) noise and voronoi are
// standard mathematical concepts implemented here from scratch on top of an
// integer splitmix64 hash / seeded permutation table. No rand(), no clock, no
// third-party code.
//
// Determinism: the same (recipe, seed, size) produces byte-identical pixels
// on every run of the same binary. All seeding is integer hash based; the
// only platform caveat is float rounding of pow/sqrt in the field maths,
// which is identical for a given binary/libm (documented in the .cpp).
//
// Tiling: every generator is periodic by construction -- lattice coordinates
// are wrapped modulo the (integer) period per octave before hashing, and
// voronoi neighbour cells wrap around the tile, so the field at u and u+1
// (and v / v+1) is the same value.
#pragma once
#include <cstdint>
#include "litholattice/assets/image.h"
#include "litholattice/texture/texture_recipe.h"

namespace litholattice {

/// Generates a square RGBA8 texture of `size` x `size` pixels.
/// size <= 0 returns an empty Image; size is clamped to 4096.
Image generateTexture(const TextureRecipe& recipe, uint64_t seed, int size);

/// Reference implementation of generateTexture: straightforward per-pixel
/// evaluation with fresh allocations per node. Kept for benchmarking and as
/// a correctness cross-check -- its output is byte-identical to
/// generateTexture (the optimised path only memoises lattice values, reuses
/// scratch buffers, and hoists per-octave parameters; it never changes the
/// maths).
Image generateTextureReference(const TextureRecipe& recipe, uint64_t seed, int size);

/// Process-level cache over generateTexture: an LRU of at most 64 entries
/// keyed by (hash of the canonical recipe text, seed, size). Returns pixels
/// identical to generateTexture. The returned reference stays valid until
/// the entry is evicted or the cache is otherwise mutated by a later call.
const Image& generateTextureCached(const TextureRecipe& recipe, uint64_t seed, int size);

/// Number of entries currently held by the generateTextureCached LRU.
size_t textureCacheSize();

/// Periodic field primitives (exposed for tiling tests and tools). (x, y) is
/// in lattice-cell units; `period` is the integer wrap period in cells.
/// textureValueNoise(x + period, y, period, seed) == textureValueNoise(x, y,
/// period, seed) exactly. Value noise returns [0,1]; gradient noise [-1,1].
float textureValueNoise(float x, float y, int period, uint64_t seed);
float textureGradientNoise(float x, float y, int period, uint64_t seed);

}  // namespace litholattice
