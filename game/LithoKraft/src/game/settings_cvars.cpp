// SettingsCvars bridge: GameSettings <-> live lithoscript cvars.
// LF line endings. Namespace lithokraft (game); console types fully qualified.

#include "lithokraft/game/settings_cvars.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include "litholattice/console/concmd.h"

namespace lithokraft {
namespace {

using litholattice::console::ConsoleRegistry;
using litholattice::console::CVarFlags;
using litholattice::console::CVarType;

// One row per cvar: the name (LOCKED dotted scheme), a getter and a setter
// against GameSettings. The setter writes the field and returns; the caller
// (registerOn) then runs the apply path. All eleven settings are archived.
struct CvarRow {
  const char* name;
  std::function<std::string(const GameSettings&)> get;
  std::function<void(GameSettings&, const std::string&)> set;
  CVarType type;
};

float toFloat(const std::string& s) { return std::stof(s); }
int toInt(const std::string& s) { return std::stoi(s); }
bool toBool(const std::string& s) { return s == "1" || s == "true"; }

}  // namespace

SettingsCvars::SettingsCvars(std::function<void(const GameSettings&)> apply,
                             std::function<GameSettings()> read)
    : apply_(std::move(apply)), read_(std::move(read)) {}

void SettingsCvars::registerOn(ConsoleRegistry& r) {
  registry_ = &r;
  const GameSettings cur = read_();

  // The single table. Names follow the locked subsystem hierarchy:
  // audio.* (audio), hud.* (HUD/UI), client.* (client presentation),
  // sim.* (simulation), render.* (mesher/materials).
  static const CvarRow kRows[] = {
      {"audio.volume",
       [](const GameSettings& s) { return std::to_string(s.masterVolume); },
       [](GameSettings& s, const std::string& v) { s.masterVolume = toFloat(v); },
       CVarType::Float},
      {"audio.muted",
       [](const GameSettings& s) { return s.muted ? "1" : "0"; },
       [](GameSettings& s, const std::string& v) { s.muted = toBool(v); },
       CVarType::Bool},
      {"hud.scale",
       [](const GameSettings& s) { return std::to_string(s.hudScale); },
       // Int, clamped 0-2 (0 = auto): the old Float declaration truncated
       // (hud.scale 1.9 -> 1) and let 99 persist until the next boot clamp.
       [](GameSettings& s, const std::string& v) { s.hudScale = std::clamp(toInt(v), 0, 2); },
       CVarType::Int},
      {"client.display_mode",
       [](const GameSettings& s) { return std::to_string(s.displayMode); },
       [](GameSettings& s, const std::string& v) { s.displayMode = toInt(v); },
       CVarType::Int},
      {"client.vsync",
       [](const GameSettings& s) { return s.vsync ? "1" : "0"; },
       [](GameSettings& s, const std::string& v) { s.vsync = toBool(v); },
       CVarType::Bool},
      {"sim.tickrate",
       [](const GameSettings& s) { return std::to_string(s.tps); },
       // Clamp to a valid pow2 mode: a raw 0 here used to reach
       // SimThread::setTickRate(0) and SIGFPE the process (audit HIGH).
       [](GameSettings& s, const std::string& v) { s.tps = nearestTps(toInt(v)); },
       CVarType::Int},
      {"sim.tickradius",
       [](const GameSettings& s) { return std::to_string(s.tickRadius); },
       [](GameSettings& s, const std::string& v) { s.tickRadius = toInt(v); },
       CVarType::Int},
      {"sim.euv",
       [](const GameSettings& s) { return s.euv ? "1" : "0"; },
       [](GameSettings& s, const std::string& v) { s.euv = toBool(v); },
       CVarType::Bool},
      {"sim.timing_handshake",
       [](const GameSettings& s) { return s.timingHandshake ? "1" : "0"; },
       // Standard switch (hs-design §5; default ON since 2026-10-14, JD's
       // promotion): applies on world load, not live.
       [](GameSettings& s, const std::string& v) { s.timingHandshake = toBool(v); },
       CVarType::Bool},
      {"sim.handshake_phases",
       [](const GameSettings& s) { return std::to_string(s.handshakePhases); },
       [](GameSettings& s, const std::string& v) { s.handshakePhases = std::clamp(toInt(v), 1, 16); },
       CVarType::Int},
      {"render.mesher",
       [](const GameSettings& s) { return std::to_string(s.mesher); },
       [](GameSettings& s, const std::string& v) { s.mesher = toInt(v); },
       CVarType::Int},
      {"render.pbr_tier",
       [](const GameSettings& s) { return std::to_string(s.pbrTier); },
       // Clamped 0..3 (PbrTransparencyTier range), mirroring ambient_mode
       // below: an unclamped 9 used to reach the GL33 tier uniform and
       // misrender transparency (P1-RENDER H5).
       [](GameSettings& s, const std::string& v) { s.pbrTier = std::clamp(toInt(v), 0, 3); },
       CVarType::Int},
      {"render.ambient_mode",
       [](const GameSettings& s) { return std::to_string(s.ambientMode); },
       // 0 = Fraction (cheap default), 1 = NormalizedSun; clamped, archived.
       [](GameSettings& s, const std::string& v) { s.ambientMode = std::clamp(toInt(v), 0, 1); },
       CVarType::Int},
      {"render.frustum_margin_deg",
       [](const GameSettings& s) { return std::to_string(s.frustumMarginDeg); },
       // FOV culling margin in degrees; clamped 0..15, archived. JD 2026-10-11.
       [](GameSettings& s, const std::string& v) {
         s.frustumMarginDeg = std::clamp(toFloat(v), 0.f, 15.f);
       },
       CVarType::Float},
      // Culling-cache prototypes (JD 2026-10-11; all default OFF). Archived so
      // JD's A/B choices persist across runs.
      {"render.comet_tail_frames",
       [](const GameSettings& s) { return std::to_string(s.cometTailFrames); },
       [](GameSettings& s, const std::string& v) {
         s.cometTailFrames = std::clamp(toInt(v), 0, 64);
       },
       CVarType::Int},
      {"render.velocity_margin",
       [](const GameSettings& s) { return s.velocityMargin ? "1" : "0"; },
       [](GameSettings& s, const std::string& v) { s.velocityMargin = toBool(v); },
       CVarType::Bool},
      {"render.velocity_margin_gain",
       [](const GameSettings& s) { return std::to_string(s.velocityMarginGain); },
       [](GameSettings& s, const std::string& v) {
         s.velocityMarginGain = std::clamp(toFloat(v), 0.f, 10.f);
       },
       CVarType::Float},
      {"render.two_tier",
       [](const GameSettings& s) { return s.twoTier ? "1" : "0"; },
       [](GameSettings& s, const std::string& v) { s.twoTier = toBool(v); },
       CVarType::Bool},
      {"render.two_tier_outer_deg",
       [](const GameSettings& s) { return std::to_string(s.twoTierOuterDeg); },
       [](GameSettings& s, const std::string& v) {
         s.twoTierOuterDeg = std::clamp(toFloat(v), 0.f, 30.f);
       },
       CVarType::Float},
      {"render.ground_pulse",
       [](const GameSettings& s) { return s.groundPulse ? "1" : "0"; },
       // Wafer ground pulse (JD 2026-10-11): bool, default on. Archived like the
       // other render knobs; the shader reads it live via FrameParams, no remesh.
       [](GameSettings& s, const std::string& v) { s.groundPulse = toBool(v); },
       CVarType::Bool},
      {"render.ground_traces",
       [](const GameSettings& s) { return s.groundTraces ? "1" : "0"; },
       // Wafer ground traces (JD 2026-10-11; landed 2026-10-13): bool, default
       // on, independent of render.ground_pulse. Archived; backends read it
       // live via FrameParams (groundTraceOn), no remesh.
       [](GameSettings& s, const std::string& v) { s.groundTraces = toBool(v); },
       CVarType::Bool},
      {"render.model_dither",
       [](const GameSettings& s) { return s.modelDither ? "1" : "0"; },
       // Near-camera model dithering (JD 2026-10-13): deterministic
       // screen-door fade for geometry inside the near band, so close-up
       // cameras are not swallowed. Bool, default on. Archived; applied
       // live via IRenderer::setModelDither, no remesh.
       [](GameSettings& s, const std::string& v) { s.modelDither = toBool(v); },
       CVarType::Bool},
      {"render.model_dither_near",
       [](const GameSettings& s) { return std::to_string(s.modelDitherNear); },
       // View depth (blocks) at/below which geometry is fully faded.
       // Clamped 0..64.
       [](GameSettings& s, const std::string& v) {
         s.modelDitherNear = std::clamp(toFloat(v), 0.f, 64.f);
       },
       CVarType::Float},
      {"render.model_dither_far",
       [](const GameSettings& s) { return std::to_string(s.modelDitherFar); },
       // View depth (blocks) at/above which geometry is untouched.
       // Clamped to [near, 256] at apply time in the backends.
       [](GameSettings& s, const std::string& v) {
         s.modelDitherFar = std::clamp(toFloat(v), 0.f, 256.f);
       },
       CVarType::Float},
      {"render.aa",
       [](const GameSettings& s) { return s.aaMode; },
       // Software-renderer anti-aliasing ladder (JD 2026-10-13):
       // "off" | "fxaa" | "cmaa" | "ssaa2x". Named modes from the
       // CPU-AA survey; coverage restoration ("cmaa") is the flagship.
       // String, default "off" (JD picks the default from captures).
       // Applied live via IRenderer::setAaMode; software backend only.
       [](GameSettings& s, const std::string& v) {
         s.aaMode = (v == "fxaa" || v == "cmaa" || v == "ssaa2x") ? v : "off";
       },
       CVarType::String},
      {"render.fps_cap",
       [](const GameSettings& s) { return std::to_string(s.fpsCap); },
       // Frame-rate cap (engine FramePacer, D-M3; game-side wiring
       // 2026-10-13, Team A A4): integer FPS, 0 = off (default, vsync-only),
       // clamped 0..1000 like the pacer itself. Archived; the main loop
       // drives the pacer from Settings live (init + on settings change).
       [](GameSettings& s, const std::string& v) { s.fpsCap = std::clamp(toInt(v), 0, 1000); },
       CVarType::Int},
      {"render.ascii",
       [](const GameSettings& s) { return s.asciiMode ? "1" : "0"; },
       // ASCII output pipeline (experiment tier, JD 2026-10-13): the
       // software renderer converts its frame to a glyph grid — the
       // Daemon Sight low-end configuration / ladder floor. Bool,
       // default off. Applied live via IRenderer::setAsciiMode.
       [](GameSettings& s, const std::string& v) { s.asciiMode = toBool(v); },
       CVarType::Bool},
      {"render.ascii_cols",
       [](const GameSettings& s) { return std::to_string(s.asciiCols); },
       // ASCII glyph columns; clamped 40..400.
       [](GameSettings& s, const std::string& v) {
         s.asciiCols = std::clamp(toInt(v), 40, 400);
       },
       CVarType::Int},
      {"render.ascii_set",
       [](const GameSettings& s) { return s.asciiSet; },
       // Glyph set for the ASCII pipeline (JD 2026-10-13): "ascii" or
       // "systerian" — the Systerian renderer draws the certificate
       // script's glyphs through the engine's ASCII->Systerian map.
       // String, default "ascii". Applied via IRenderer::setAsciiGlyphSet.
       [](GameSettings& s, const std::string& v) {
         s.asciiSet = (v == "systerian") ? "systerian" : "ascii";
       },
       CVarType::String},
      {"render.ascii_value_span",
       [](const GameSettings& s) { return std::to_string(s.asciiValueSpan); },
       // Value span for the ASCII/Systerian luminance -> level mapping
       // (JD-RULED AMENDMENT 2026-10-13): the FULL 0-255 range by
       // default (clamped 16..255) — a deliberate exception to the
       // game's 64-scale conventions, for more variance in the
       // texturing and look of blocks. Applied live via
       // IRenderer::setAsciiValueSpan.
       [](GameSettings& s, const std::string& v) {
         s.asciiValueSpan = std::clamp(toInt(v), 16, 255);
       },
       CVarType::Int},
  };

  // All settings cvars are archived: they persist to config.lth.
  for (const auto& row : kRows) {
    std::string err;
    // Default = current settings value, so a fresh registry matches the file.
    // Args: name, type, default, archived=true, help, flags.
    if (!r.registerCVar(row.name, row.type, row.get(cur), true, "", CVarFlags{}, &err)) {
      r.print(std::string("settings: cvar '") + row.name + "' not registered: " + err);
      continue;
    }
    // set flows through the apply path; the guard stops cvar->setting->cvar echo.
    const std::string name = row.name;
    auto setFn = row.set;
    r.setCVarChangeFn(name, [this, name, setFn](ConsoleRegistry&, const auto& cv) {
      if (syncing_) return;
      GameSettings s = read_();
      try {
        setFn(s, cv.value);
      } catch (...) {
        return;  // coercion already validated; never throws in practice
      }
      apply_(s);
    });
  }
}

void SettingsCvars::syncFrom(const GameSettings& s) {
  if (!registry_) return;
  syncing_ = true;
  // Mirror the table above: push each field into its cvar.
  registry_->setCVar("audio.volume", std::to_string(s.masterVolume));
  registry_->setCVar("audio.muted", s.muted ? "1" : "0");
  registry_->setCVar("hud.scale", std::to_string(s.hudScale));
  registry_->setCVar("client.display_mode", std::to_string(s.displayMode));
  registry_->setCVar("client.vsync", s.vsync ? "1" : "0");
  registry_->setCVar("sim.tickrate", std::to_string(s.tps));
  registry_->setCVar("sim.tickradius", std::to_string(s.tickRadius));
  registry_->setCVar("sim.euv", s.euv ? "1" : "0");
  registry_->setCVar("sim.timing_handshake", s.timingHandshake ? "1" : "0");
  registry_->setCVar("sim.handshake_phases", std::to_string(s.handshakePhases));
  registry_->setCVar("render.mesher", std::to_string(s.mesher));
  registry_->setCVar("render.pbr_tier", std::to_string(s.pbrTier));
  registry_->setCVar("render.ambient_mode", std::to_string(s.ambientMode));
  registry_->setCVar("render.frustum_margin_deg", std::to_string(s.frustumMarginDeg));
  registry_->setCVar("render.comet_tail_frames", std::to_string(s.cometTailFrames));
  registry_->setCVar("render.velocity_margin", s.velocityMargin ? "1" : "0");
  registry_->setCVar("render.velocity_margin_gain", std::to_string(s.velocityMarginGain));
  registry_->setCVar("render.two_tier", s.twoTier ? "1" : "0");
  registry_->setCVar("render.two_tier_outer_deg", std::to_string(s.twoTierOuterDeg));
  registry_->setCVar("render.ground_pulse", s.groundPulse ? "1" : "0");
  registry_->setCVar("render.ground_traces", s.groundTraces ? "1" : "0");
  registry_->setCVar("render.model_dither", s.modelDither ? "1" : "0");
  registry_->setCVar("render.model_dither_near", std::to_string(s.modelDitherNear));
  registry_->setCVar("render.model_dither_far", std::to_string(s.modelDitherFar));
  registry_->setCVar("render.aa", s.aaMode);
  registry_->setCVar("render.ascii", s.asciiMode ? "1" : "0");
  registry_->setCVar("render.ascii_cols", std::to_string(s.asciiCols));
  registry_->setCVar("render.ascii_set", s.asciiSet);
  registry_->setCVar("render.ascii_value_span", std::to_string(s.asciiValueSpan));
  registry_->setCVar("render.fps_cap", std::to_string(s.fpsCap));
  syncing_ = false;
}

// MCP pilot-senses cvars (mcp.* / camera.*). These are MCP-host knobs, not
// player preferences, so they live outside GameSettings — but they follow
// the same registration pattern: int cvars, archived to config.lth.
void registerMcpSensesCvars(litholattice::console::ConsoleRegistry& r) {
  struct Row {
    const char* name;
    const char* def;
    const char* help;
  };
  static const Row kRows[] = {
      {"mcp.vision_width", "160",
       "MCP agent vision capture default width (16..512; a capture may override)"},
      {"mcp.vision_height", "90",
       "MCP agent vision capture default height (16..512; a capture may override)"},
      {"mcp.audio_tier", "0",
       "MCP agent audio downgrade tier 0..3 (8000/16000/24000/48000 Hz); the upgrade ramp"},
      // camera.chunk_radius is NOT registered here: the engine camera
      // module owns it (registerCameraCvars runs first in Game's ctor).
      // The 0..64 clamp hook below still attaches to it by name.
  };
  for (const auto& row : kRows) {
    std::string err;
    // Args: name, type, default, archived=true, help, flags.
    if (!r.registerCVar(row.name, CVarType::Int, row.def, true, row.help, CVarFlags{}, &err))
      r.print(std::string("mcp senses: cvar '") + row.name + "' not registered: " + err);
  }
  // Fix 2026-10-11 (regression, LOW): mcp.audio_tier and camera.chunk_radius
  // are write-only today (the builder wires them later); clamp at registration
  // so raw values (negative, 2^31) can't flow straight into future code.
  // The hook re-sets only when out of range, so it terminates: the clamped
  // set fires onChange again but passes through unchanged.
  auto clampHook = [](int lo, int hi) {
    return [lo, hi](ConsoleRegistry& reg, const auto& c) {
      int v = 0;
      try {
        v = std::stoi(c.value);
      } catch (...) {
        return;
      }
      const int clamped = std::clamp(v, lo, hi);
      if (clamped != v) {
        std::string err;
        reg.setCVar(c.name, std::to_string(clamped), &err);
      }
    };
  };
  r.setCVarChangeFn("mcp.audio_tier", clampHook(0, 3));
  r.setCVarChangeFn("camera.chunk_radius", clampHook(0, 64));
}

// AiWorld host cvar (brief B6 wiring, 2026-10-13): a host knob, not a
// player preference, so it lives outside GameSettings like the MCP senses
// knobs above. Game reads it per frame in update() (the same read pattern
// as mcp.*) and drives AiHost::setEnabled. Default OFF: no AiWorld is
// constructed in the live game until the host turns it on. Archived.
void registerAiCvars(litholattice::console::ConsoleRegistry& r) {
  std::string err;
  // Args: name, type, default, archived=true, help, flags.
  if (!r.registerCVar("ai.enabled", CVarType::Bool, "0", true,
                      "AiWorld production host: 1 = construct and tick the game's AiWorld over the live "
                      "world (8-16 Hz, sim-tick driven); 0 = off (default; nothing is constructed)",
                      CVarFlags{}, &err))
    r.print(std::string("ai: cvar 'ai.enabled' not registered: ") + err);
}

}  // namespace lithokraft
