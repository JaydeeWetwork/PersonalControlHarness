// Game settings: the player's preferences, persisted to config/settings.json (next to
// config/input.json). Every field maps to a real control, applied live or at boot:
//   - master volume + mute: AudioEngine (live, via masterGain commands / setEnabled)
//   - HUD scale: the UI theme scale (live; 0 = auto from the window height)
//   - display mode + vsync: the window (display mode live; vsync at boot)
//   - tick rate + EUV domain: the sim (live, via setTickRate / toggleDomain)
//   - tick radius: the sim (boot-only: SimConfig::radiusChunks)
//   - mesher: chunk mesher (boot-only: PackedVertex or BinaryQuad+PBR, world (re)load)
//   - PBR transparency tier: the PBR path's tier (live via setPbrTier; Video page dropdown)
#pragma once
#include <string>
#include "litholattice/render/pbr_transparency_tiers.h"  // PbrTransparencyTier for pbrTier

namespace lithokraft {

/// Display modes in the settings order (names must match Window::parseDisplayMode).
constexpr const char* kSettingsDisplayModes[] = {"Windowed", "Maximized", "Borderless", "Fullscreen"};
constexpr int kSettingsDisplayModeCount = 4;
/// Tick rates offered in the settings UI (kTickModes plus the 512 stress mode).
constexpr int kSettingsTpsModes[] = {32, 64, 128, 256, 512};
constexpr int kSettingsTpsModeCount = 5;
/// Mesher options in the settings order (BinaryQuad/PackedVertex coexistence, JD 2026-10-11).
constexpr const char* kSettingsMesherNames[] = {"PackedVertex", "BinaryQuad"};
constexpr int kSettingsMesherCount = 2;
/// PBR transparency tier names in PbrTransparencyTier order (must match
/// pbrTransparencyTierName; the Video-page dropdown indexes GameSettings::pbrTier
/// directly, so the order here is the enum order).
constexpr const char* kSettingsPbrTierNames[] = {"Simple", "ScreenDoor", "PbrReflections",
                                                 "TrueRefraction"};
constexpr int kSettingsPbrTierCount = 4;
static_assert(kSettingsPbrTierCount == int(litholattice::PbrTransparencyTier::TrueRefraction) + 1,
              "PBR tier dropdown must cover every PbrTransparencyTier in enum order");

/// Nearest valid tick rate to `v` (one of kSettingsTpsModes; ties round down).
/// Used by settings.json load() and the sim.tickrate cvar setter so the pow2
/// invariant holds on every path, not just at boot.
int nearestTps(int v);

struct GameSettings {
  float masterVolume = 0.6f;  ///< 0..1; matches the AudioEngine default gain
  bool muted = false;
  int hudScale = 0;       ///< 0 = auto, else a fixed UI scale (1 or 2)
  int displayMode = 0;    ///< index into kSettingsDisplayModes
  bool vsync = false;     ///< boot-only: the window is created with it
  int tps = 32;           ///< one of kSettingsTpsModes (kDefaultTps)
  int tickRadius = 8;     ///< 0..512 chunks; boot-only
  bool euv = true;        ///< the lithographed EUV-tier sim domain
  int mesher = 0;         ///< index into kSettingsMesherNames; boot-only (world (re)load):
                          ///< 0 = PackedVertex (classic chunk programs), 1 = BinaryQuad
                          ///< (chunk_pbr programs; falls back to 0 if the backend lacks it)
  int pbrTier = 1;        ///< PbrTransparencyTier as int (0..3); live via setPbrTier.
                          ///< Default ScreenDoor (full PBR, no sorting); the Video
                          ///< page "PBR transparency (live)" dropdown drives it.
  int ambientMode = 0;    ///< flood-fill ambient mode (JD 2026-10-11): 0 = Fraction
                          ///< (ambient = skyLevel/64, cheap; default), 1 = NormalizedSun
                          ///< (direction-aware hemisphere via the sun vector). Live.
  float frustumMarginDeg = 5.f;  ///< FOV culling margin in degrees, 0..15 (JD 2026-10-11):
                          ///< chunks inside FOV +/- margin render; the rest cull.
                          ///< Shock absorber against "frustum occlusion tearing". Live.
  // Culling-cache prototypes (JD 2026-10-11; all default OFF — JD A/B tests
  // them on his workstation). See render/cull_prototypes.h. Live.
  int cometTailFrames = 0;       ///< render.comet_tail_frames: temporal frustum
                          ///< union depth, 0..64 (0 = off). Remembers where the
                          ///< camera looked, not just where it is.
  bool velocityMargin = false;   ///< render.velocity_margin: stretch the margin
                          ///< asymmetrically in the direction of rotation.
  float velocityMarginGain = 1.f;  ///< render.velocity_margin_gain: extra margin
                          ///< degrees per rad/s of angular velocity, 0..10.
  bool twoTier = false;   ///< render.two_tier: outer tier keeps meshes but
                          ///< skips draw submission (mesh vs. submit split).
  float twoTierOuterDeg = 10.f;  ///< render.two_tier_outer_deg: outer tier
                          ///< extends this far beyond the inner margin, 0..30.
  bool timingHandshake = true;   ///< sim.timing_handshake (default ON since
                          ///< 2026-10-14, JD's promotion): pull-tick
                          ///< phase scheduling for the circuit domains. Applies
                          ///< at world (re)load — registration is a SimThread
                          ///< construction-time act, never live-swapped.
  int handshakePhases = 4;       ///< sim.handshake_phases: phases per cycle, 1..16
                          ///< (the Signal cycle uses the first four slots).
  bool groundPulse = true;  ///< render.ground_pulse: the Wafer's energized
                          ///< ground pulse (JD 2026-10-11). Live toggle, no
                          ///< remesh; default on.
  bool groundTraces = true;  ///< render.ground_traces: the Wafer's ground
                          ///< trace lines + packets (JD 2026-10-11; landed
                          ///< end-to-end 2026-10-13, P1-RENDER H1). Live
                          ///< toggle, no remesh; default on, independent
                          ///< of groundPulse.
  int fpsCap = 0;         ///< render.fps_cap: frame-rate cap in FPS for the
                          ///< main loop's FramePacer (D-M3; game-side wiring
                          ///< 2026-10-13). 0 = OFF (default: vsync-only
                          ///< pacing, JD perf doctrine 2026-10-12), else
                          ///< 1..1000. Live: the pacer is reconfigured the
                          ///< frame the settings value changes.

  bool modelDither = true;  ///< render.model_dither: near-camera screen-door
                            ///< fade (JD 2026-10-13). Live, no remesh;
                            ///< default on (an engine staple).
  float modelDitherNear = 2.f;  ///< render.model_dither_near: view depth
                                ///< (blocks) at/below which geometry is
                                ///< fully faded out.
  float modelDitherFar = 8.f;   ///< render.model_dither_far: view depth
                                ///< (blocks) at/above which geometry is
                                ///< rendered untouched.

  std::string aaMode = "off";  ///< render.aa: software-renderer AA ladder
                               ///< ("off" | "fxaa" | "cmaa" | "ssaa2x"),
                               ///< JD 2026-10-13. Default off — JD reviews
                               ///< the capture sets and picks the default.
  bool asciiMode = false;      ///< render.ascii: ASCII output pipeline
                               ///< (experiment tier, JD 2026-10-13).
  int asciiCols = 160;         ///< render.ascii_cols: ASCII glyph columns,
                               ///< clamped 40..400.
  std::string asciiSet = "ascii";  ///< render.ascii_set: glyph set for the
                               ///< ASCII pipeline — "ascii" | "systerian"
                               ///< (the Systerian renderer, JD 2026-10-13).
  int asciiValueSpan = 255;    ///< render.ascii_value_span: luminance ->
                               ///< level span (JD amendment 2026-10-13:
                               ///< full 0-255 range by default, a deliberate
                               ///< exception to the 64-scale conventions).

  /// Reads `path` (missing or broken -> defaults, never throws). Unknown keys are ignored;
  /// out-of-range values clamp to the nearest valid one.
  static GameSettings load(const std::string& path);
  /// Writes the settings as JSON, creating parent directories. False when unwritable.
  bool save(const std::string& path) const;
};

}  // namespace lithokraft
