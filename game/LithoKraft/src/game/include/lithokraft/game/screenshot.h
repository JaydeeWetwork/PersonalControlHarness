// Screenshot capture pipeline for the lithoscript `screenshot` command.
// Grabs the current framebuffer from any IRenderer backend (GL: glReadPixels
// of the back buffer; Vulkan: image-to-host copy), optionally downscales to
// agent-vision fidelity, and PNG-encodes with the engine's clean-room
// encodePngRgb (engine/src/mcp/senses.cpp) — no new PNG encoder here.
// LF line endings.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace litholattice {
class IRenderer;
}  // namespace litholattice

namespace lithokraft {

/// What a `screenshot` lithoscript call asked for (parsed by the command
/// shim in game_commands.cpp; executed by Game::requestScreenshot).
struct ScreenshotRequest {
  std::string path;    ///< empty = auto-name into screenshots/
  bool lowres = false;     ///< downscale to mcp.vision_width x mcp.vision_height
  bool toFragment = false; ///< capture into the Fragment gallery, not a file
  std::string caption;     ///< gallery caption (toFragment only)
};

/// A captured, PNG-encoded shot.
struct CapturedShot {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> png;  ///< PNG bytes (RGB, 8-bit, non-interlaced)
};

/// Grabs the armed frame from `r` (requestGrab() must have been called and a
/// frame rendered since), downscales to (visionW x visionH) when `lowres`,
/// and PNG-encodes. `visionW`/`visionH` come from the mcp.vision_width /
/// mcp.vision_height cvars. Returns false + `*err` when the backend cannot
/// grab or the PNG encode fails.
bool captureScreenshot(litholattice::IRenderer& r, bool lowres, int visionW,
                       int visionH, CapturedShot& out, std::string* err);

/// Box-average downscale (srcW x srcH -> dstW x dstH), RGB top-first both
/// sides. Area averaging, so the lowres agent shot sees the average color,
/// not a point sample. Also upscales sanely (box < 1 px -> nearest).
void downscaleBoxAverage(const uint8_t* src, int srcW, int srcH,
                         std::vector<uint8_t>& dst, int dstW, int dstH);

/// Auto-name: "screenshots/lithokraft-YYYYMMDD-HHMMSS.png" ("-lowres" before
/// the extension for the agent-fidelity variant). Local time.
std::string autoScreenshotPath(bool lowres);

/// Result of confining a user-requested screenshot path (P1b, 2026-10-13,
/// Appendix F M6): `path` is ALWAYS inside the screenshots root.
struct ScreenshotPathResolution {
  std::filesystem::path path;  ///< confined destination, inside the root
  bool fellBack = false;       ///< requested path escaped/was invalid and
                               ///< the confined auto-name was used instead
};

/// Confines `requested` to the screenshots directory `root` (the game's
/// screenshots area — Game passes <config dir>/screenshots, or the plain
/// relative "screenshots" when no config dir is set):
///   - empty request -> the auto-name directly under `root`;
///   - a relative path that stays inside `root` after normalization
///     (subdirectories allowed, e.g. "trips/day1.png") -> root / requested;
///   - an ABSOLUTE path, a ".." escape, or anything else that resolves
///     outside `root` -> REFUSED: fellBack=true and the confined auto-name
///     under `root` is used instead, so the shot still lands somewhere
///     discoverable and no byte is ever written outside the root.
/// Containment is canonical-style (mirroring ResourcePack::has /
/// saveArchived): both sides are weakly_canonical-normalized and the
/// result must stay component-wise under the root.
ScreenshotPathResolution resolveScreenshotPath(const std::filesystem::path& root,
                                               const std::string& requested,
                                               bool lowres);

/// Writes `bytes` to `path`, creating parent directories. False + `*err`
/// on failure. Overwrites an existing file.
bool writeFileBytes(const std::string& path, const std::vector<uint8_t>& bytes,
                    std::string* err);

}  // namespace lithokraft
