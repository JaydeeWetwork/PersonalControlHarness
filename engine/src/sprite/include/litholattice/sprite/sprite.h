// LithoLattice sprites: world-anchored textured quads (billboards) and the
// seed of the particle system.
//
// Sprites are how the engine draws things that are not voxels: vegetation
// billboards, decals, item icons in the world, agent markers — and, via the
// emitter half of SpriteSystem, particles (sparks, smoke, glow motes). A
// sprite TYPE is content: a SpriteDef registered by name with an image
// source (PNG bytes / PNG file / SVG text / a decoded Image handed in by
// another engine system). Games and mods register their own sprite types
// first-wins by name, the same rule as stats, vitals and abilities — the
// registry is the modding surface.
//
// SOURCES ARE PRODUCER-AGNOSTIC: the core depends on assets (Image decode)
// and core (math) only. Procedural producers — the 2D Litho Formula's SVG
// shapes, the Systerian glyph SVGs, a mod's hand-written SVG — compose at
// THEIR layer by passing an SVG string or a decoded Image into the matching
// register* call. The sprite core never links procgen or systerian.
//
// RENDER CONTRACT: SpriteSystem::emitQuads turns live instances into
// world-space quads (billboarded per def) carrying atlas UVs, a tint, an
// atlas page index and a shade. The shade is the sprite-local SpriteShade;
// sprite_shade_mode.h maps it to the render module's ShadeMode
// (Cutout -> kShadeCutout, AlphaBlend -> kShadeAlphaBlend,
// Additive -> kShadeAdditive) for consumers that speak render types. The
// core deliberately does NOT include renderer.h: the render module links
// half the engine and will itself consume these quads, so depending on it
// here would invite a cycle.
//
// There is no "emissive" flag on a def: the render shade model has no
// combined emissive+transparent mode (kShadeEmissive is an opaque self-lit
// texture), so a separate flag could not be honored honestly. Additive is
// the glow path; a self-lit opaque sprite is a textured quad's job, not a
// sprite's.
//
// DETERMINISM (house doctrine — replays and saves must reproduce): all
// simulation runs on integer ticks passed in by the caller, never a wall
// clock; tick(n) is exactly n single ticks, so chunking cannot change the
// result; iteration is always spawn order; emitter randomness is a seeded
// splitmix64 whose draw order is fixed and documented in sprite_system.cpp,
// so a given seed reproduces a particle field bit-for-bit.
//
// CONVENTIONS:
//  - Positions are Vec3 in world blocks; sizes are in blocks.
//  - Yaw is RADIANS, Player::forward convention (yaw from +X toward +Z).
//  - Atlas UVs are in IMAGE space: u right, v DOWN from the atlas page's
//    top-left, matching Image row order. A renderer uploading page images
//    row-first pairs corner UVs with corners exactly as emitted.
//  - SpriteQuad corners are ordered counter-clockwise as seen by a viewer
//    facing the sprite's front: [0] bottom-left, [1] bottom-right,
//    [2] top-right, [3] top-left — triangles (0,1,2) and (0,2,3).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "litholattice/assets/image.h"
#include "litholattice/core/math.h"

namespace litholattice::sprite {

/// How a sprite instance orients itself in the world.
enum class BillboardMode : uint8_t {
  Spherical,     ///< faces the camera fully (right/up = the camera's)
  CylindricalY,  ///< rotates around the world Y axis only; top edge stays level
  FlatFacing,    ///< fixed plane from the instance's facing yaw (decals, signs)
};

/// How a sprite blends. Maps 1:1 onto render ShadeModes via
/// sprite_shade_mode.h (shadeModeFor). See the header note for why there is
/// no fourth "emissive" value.
enum class SpriteShade : uint8_t {
  Cutout,      ///< kShadeCutout: alpha test, depth-writing, order-independent
  AlphaBlend,  ///< kShadeAlphaBlend: needs back-to-front sorting
  Additive,    ///< kShadeAdditive: glow/particles, no sorting needed
};

/// RGBA tint multiplier (1 = identity). Applied over the atlas texel at
/// draw time; the def carries the base tint, an instance may override it.
struct SpriteTint {
  float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
  friend bool operator==(const SpriteTint&, const SpriteTint&) = default;
};

/// A sprite type: presentation data only. The IMAGE is not in the def —
/// it is supplied by exactly one of the registry's register* entry points
/// (PNG bytes, PNG file, SVG text, decoded Image), which resolves it to
/// per-frame Images at registration time.
struct SpriteDef {
  uint32_t id = 0;             ///< 0 = assign on registration (explicit ids honored if free)
  const char* name = nullptr;  ///< e.g. "lithokraft:leaf_oak"
  float widthBlocks = 1.f;     ///< world width of one instance at scale 1
  float heightBlocks = 1.f;    ///< world height of one instance at scale 1
  BillboardMode billboard = BillboardMode::Spherical;
  SpriteShade shade = SpriteShade::Cutout;
  SpriteTint tint{};  ///< base tint (white = untinted)
  // Animation: the resolved image is a vertical strip of `frameCount`
  // frames (frame i = rows [i*h, (i+1)*h) of the strip, the assets Image
  // strip convention). SVG sources are single-frame: frameCount must be 1.
  uint32_t frameCount = 1;
  uint32_t ticksPerFrame = 8;  ///< sim ticks each frame is shown (0 clamps to 1)
  bool loop = true;            ///< false = hold the last frame when the strip ends
  // Raster size for SVG sources (ignored by PNG/Image sources, which keep
  // their decoded size): the SVG is rasterized at pixelWidth x pixelHeight.
  int pixelWidth = 64;
  int pixelHeight = 64;
};

/// One frame's rectangle inside an atlas page, in image-space UVs
/// (u right, v down from the page's top-left; see header conventions).
struct SpriteUvRect {
  float u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f;
  int page = -1;  ///< index into ISpriteRegistry::atlasPages(); -1 = unplaced
  friend bool operator==(const SpriteUvRect&, const SpriteUvRect&) = default;
};

/// The sprite-type registry: content defs plus the runtime texture atlas
/// built from their resolved images.
class ISpriteRegistry {
 public:
  virtual ~ISpriteRegistry() = default;

  // -- Registration (one entry point per image source) --------------------
  // All four are FIRST-WINS BY NAME: registering an existing name returns
  // the existing id and keeps the original def and image (the new source
  // is not even decoded). Otherwise the source is resolved immediately;
  // on any failure (bad def, undecodable bytes, missing file, empty
  // image, frame strip not divisible) they return 0 and, when `err` is
  // given, explain why. Returned ids are stable for the registry's life.

  /// Source: PNG bytes (decoded via Image::decode).
  virtual uint32_t registerPng(const SpriteDef& def, std::span<const uint8_t> pngBytes,
                               std::string* err = nullptr) = 0;
  /// Source: a PNG file on disk (strictly PNG-decoded, not extension-routed).
  virtual uint32_t registerPngFile(const SpriteDef& def, const std::filesystem::path& file,
                                   std::string* err = nullptr) = 0;
  /// Source: SVG text from any producer, rasterized at the def's
  /// pixelWidth x pixelHeight (Image::decodeSvg). Single-frame only.
  virtual uint32_t registerSvg(const SpriteDef& def, std::string_view svgText,
                               std::string* err = nullptr) = 0;
  /// Source: an already-decoded Image handed in by any engine system
  /// (taken by value; the registry keeps its own copy for atlas rebuilds).
  virtual uint32_t registerImage(const SpriteDef& def, Image image,
                                 std::string* err = nullptr) = 0;

  /// Look up a registered sprite by name. Returns 0 when unknown.
  virtual uint32_t findSprite(std::string_view name) const = 0;
  /// The def for a sprite id, or nullptr when unknown. The pointer stays
  /// valid for the life of the registry (defs are never removed or
  /// renumbered).
  virtual const SpriteDef* spriteDef(uint32_t spriteId) const = 0;
  virtual size_t spriteCount() const = 0;

  // -- Runtime atlas --------------------------------------------------------
  // Resolved frames pack into power-of-two atlas pages (shelf packing in
  // registration order, 1px edge-replicated padding, so packing is fully
  // deterministic). Registration marks the atlas dirty; ensureAtlas()
  // rebuilds it when dirty, rebuildAtlas() forces a rebuild. atlasVersion()
  // increments on every rebuild — a renderer compares it against the
  // version it last uploaded to know when to re-upload the pages.
  virtual void ensureAtlas() = 0;
  virtual void rebuildAtlas() = 0;
  virtual bool atlasDirty() const = 0;
  virtual uint64_t atlasVersion() const = 0;
  /// The atlas page images (RGBA, ready to upload). Empty until the first
  /// build — call ensureAtlas() first.
  virtual std::span<const Image> atlasPages() const = 0;
  /// The atlas rect for one frame of one sprite, or nullptr when the id
  /// is unknown or the atlas has not been built since it registered.
  virtual const SpriteUvRect* frameRect(uint32_t spriteId, uint32_t frame) const = 0;
};

/// One drawable quad for one live instance, produced by
/// SpriteSystem::emitQuads. Corners are world-space, ordered
/// counter-clockwise seen from the front: [0] bottom-left, [1] bottom-right,
/// [2] top-right, [3] top-left; uvs[i] pairs with corners[i].
struct SpriteQuad {
  Vec3 corners[4]{};
  float uvs[4][2]{};
  SpriteTint tint{};
  SpriteShade shade = SpriteShade::Cutout;
  int page = -1;         ///< atlas page to sample (see SpriteUvRect::page)
  float sortDepth = 0.f;  ///< view-space depth used for sorting (larger = farther)
  uint64_t instanceId = 0;
  uint32_t defId = 0;
};

/// Parameters for one spawned instance (SpriteSystem::spawn).
struct SpriteSpawn {
  Vec3 position{};
  Vec3 velocity{};             ///< blocks per tick
  uint32_t lifetimeTicks = 0;  ///< 0 = immortal (world sprites); else the instance
                               ///< integrates exactly this many ticks, then dies
  float scale = 1.f;           ///< multiplies the def's world size
  float rollRad = 0.f;         ///< in-plane rotation, radians, CCW seen from the front
  float facingYawRad = 0.f;    ///< plane yaw for BillboardMode::FlatFacing
  /// nullopt = the def's tint; set = this instance's tint instead.
  std::optional<SpriteTint> tintOverride;
  float gravityScale = 1.f;  ///< multiplier on the system's gravity
  float drag = 0.f;          ///< per-tick velocity fraction removed, clamped [0,1]
};

/// A live instance's full state (SpriteSystem owns these; consumers read).
struct SpriteInstance {
  uint64_t id = 0;  ///< spawn handle; monotonic, never reused; 0 = none
  uint32_t defId = 0;
  Vec3 position{};
  Vec3 velocity{};
  uint32_t ageTicks = 0;
  uint32_t lifetimeTicks = 0;
  float scale = 1.f;
  float rollRad = 0.f;
  float facingYawRad = 0.f;
  SpriteTint tint{};  ///< resolved at spawn: override, else the def's tint
  float gravityScale = 1.f;
  float drag = 0.f;
};

/// A particle emitter type: the particle system's seed. An emitter sits at
/// an origin and produces SpriteSystem instances of one sprite def —
/// `burstCount` immediately when added, then `ratePerTick` continuously.
/// Per-particle values are drawn from the seeded RNG in a fixed order (see
/// sprite_system.cpp), so a seed fully determines the field.
struct ParticleEmitterDef {
  uint32_t spriteDef = 0;
  uint32_t burstCount = 0;    ///< spawned once, at addEmitter time
  float ratePerTick = 0.f;    ///< continuous rate (fractional; accumulates)
  uint32_t durationTicks = 0;  ///< 0 = emit until removed; else stops after this many ticks
  uint32_t lifetimeMinTicks = 20;  ///< per-particle lifetime range (ticks)
  uint32_t lifetimeMaxTicks = 40;
  float speedMin = 0.f;  ///< initial speed range, blocks per tick
  float speedMax = 0.f;
  Vec3 direction{0.f, 1.f, 0.f};  ///< emission cone axis (normalized on add)
  float coneAngleRad = 3.14159265358979323846f;  ///< cone half-angle; pi = full sphere
  float gravityScale = 1.f;  ///< per-particle gravity multiplier
  float drag = 0.f;          ///< per-particle drag
  float scaleMin = 1.f;      ///< per-particle scale range
  float scaleMax = 1.f;
  /// Per-particle tint range, multiplied component-wise by the def's tint.
  SpriteTint tintMin{};
  SpriteTint tintMax{};
};

}  // namespace litholattice::sprite
