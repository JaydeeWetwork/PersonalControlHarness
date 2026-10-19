// SpriteHost: LithoKraft's host for the engine sprite system
// (litholattice::sprite). Owns the game's SpriteRegistry + SpriteSystem,
// registers the starter sprite defs (proving every image source the core
// offers), ticks instances from the game's sim-tick stream, and converts
// each frame's emitted quads into the two draw forms the game feeds:
//
//   - drawQuads(): render SpriteDrawQuads for FrameParams — the GL33
//     backend's sprite pass textures them from the registry atlas.
//   - softwareVerts(): the same quads flattened to PackedVertex (4-vert
//     quads, origin-relative) for the software renderer — the MCP senses
//     adapter renders them into agent-vision captures. The software path
//     is flat-shaded (no atlas sampling), so a sprite reads there as a
//     billboard of its average color x tint; texture fidelity lives in
//     the GL pass. Vulkan has no sprite path yet (its entity channel is
//     likewise unimplemented), so sprites do not draw on Vulkan.
//
// Presentation only, like the camera system: sprites are driven by the
// sim-tick stream (they freeze when the sim pauses) but never feed the
// sim, and no sprite state is saved — a reloaded world starts clean.
//
// Threading: game thread only (constructed, ticked and read inside
// Game::update / Game::frameParams). The MCP adapter receives a copied
// vertex snapshot through its own mutex (setSpriteVerts).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "litholattice/core/math.h"  // Vec3
#include "litholattice/render/renderer.h"  // SpriteDrawQuad, PackedVertex
#include "litholattice/sprite/sprite.h"
#include "litholattice/sprite/sprite_registry.h"
#include "litholattice/sprite/sprite_system.h"

namespace litholattice {
namespace console {
class ConsoleRegistry;
}  // namespace console
}  // namespace litholattice

namespace lithokraft {

class ResourceManager;

class SpriteHost {
 public:
  SpriteHost();

  /// Register the starter defs (idempotent: the registry is first-wins,
  /// so a second call keeps the originals). Sources, one per kind:
  ///   fx:star5 / fx:star5_orange / fx:star5_pale - ONE Litho Formula 2D
  ///     star image (Image source), three def tints.
  ///   fx:flower      - Litho Formula 2D flower (Image source, alpha blend).
  ///   fx:diamond     - Litho Formula 2D diamond via SVG text (SVG source).
  ///   fx:glyph_a     - Systerian glyph 'A', violet on dark (Image source).
  ///   fx:icon_certificate_table - the certificate table's pack icon,
  ///     decoded from textures/ui/icons/certificate_table.png by the
  ///     ResourceManager (Image source; the raw PNG entry points are
  ///     exercised against the pack file in the host selftest).
  ///   fx:pulse       - animated 4-frame star strip built in code
  ///     (Image source, frameCount 4, additive).
  ///   fx:spark       - small additive mote, the burst workhorse.
  /// Failures are logged (and the def skipped), never fatal.
  void registerStarterSprites(const ResourceManager& resources);

  // -- console-facing operations (positions are world blocks) --------------
  /// Spawn `count` immortal markers of def `name` scattered deterministically
  /// around `at`. Returns a console result line.
  std::string spawnNamed(std::string_view name, litholattice::Vec3 at, int count);
  /// Fire a one-shot particle burst of def `name` at `at`.
  std::string burstNamed(std::string_view name, litholattice::Vec3 at);
  /// Remove every live instance. (Host bursts are one-shot emitters with
  /// durationTicks 1 — they retire on their next tick on their own.)
  std::string clearAll();
  /// One line per registered def (name, size, shade, frames).
  std::string listDefs() const;

  // -- per-frame flow (game thread) ----------------------------------------
  /// Advance by `ticks` sim ticks elapsed since the last frame (the same
  /// delta Game computes for the player adapter's vitals).
  void tickDelta(uint32_t ticks);
  /// Emit this frame's quads against the active camera pose and rebuild
  /// both draw forms. `camUp` is the camera's true up vector.
  void buildFrame(litholattice::Vec3 camPos, litholattice::Vec3 camFwd, litholattice::Vec3 camUp);

  const std::vector<litholattice::SpriteDrawQuad>& drawQuads() const { return drawQuads_; }
  /// Software-path quads (PackedVertex, 4 per quad) relative to
  /// softwareOrigin(); empty when nothing is live.
  const std::vector<litholattice::PackedVertex>& softwareVerts() const { return softwareVerts_; }
  litholattice::Vec3 softwareOrigin() const { return softwareOrigin_; }

  litholattice::sprite::ISpriteRegistry& registry() { return registry_; }
  const litholattice::sprite::ISpriteRegistry& registry() const { return registry_; }
  litholattice::sprite::SpriteSystem& system() { return system_; }
  size_t instanceCount() const { return system_.instanceCount(); }

 private:
  /// A def's software-path flat color: the alpha-weighted average of its
  /// frame-0 atlas rect, computed lazily from the built atlas and cached
  /// (uniform across source kinds — Image, SVG and PNG defs alike).
  std::array<float, 3> avgColorFor(uint32_t defId);

  litholattice::sprite::SpriteRegistry registry_;
  litholattice::sprite::SpriteSystem system_;
  std::vector<litholattice::SpriteDrawQuad> drawQuads_;
  std::vector<litholattice::PackedVertex> softwareVerts_;
  litholattice::Vec3 softwareOrigin_{};
  std::vector<std::array<float, 3>> avgColorByDef_;  // by def id; {0,0,0} = none
  uint64_t burstSeed_ = 0x5eed5eedull;  // command bursts: presentation-only seed stream
};

/// Console surface (mirrors registerCameraCommands):
///   sprite list                 - registered defs
///   sprite spawn <name> [count] - markers at the caller's target point
///   sprite burst <name>         - particle burst at the target point
///   sprite clear                - remove all instances
struct SpriteCommandBindings {
  std::function<std::string(std::string_view name, int count)> spawn;
  std::function<std::string(std::string_view name)> burst;
  std::function<std::string()> clear;
  std::function<std::string()> list;
};
void registerSpriteCommands(litholattice::console::ConsoleRegistry& r, SpriteCommandBindings b);

/// Host self-test (app selftest section): registration from every source
/// kind, spawn/tick/emit counts, atlas build, and a software-renderer
/// capture proving a sprite lands in a rendered world frame. Writes the
/// capture + atlas PNGs to ~/workspace/user/files/. Returns failures.
int runSpriteHostSelfTest(const std::vector<std::filesystem::path>& packDirs);

}  // namespace lithokraft
