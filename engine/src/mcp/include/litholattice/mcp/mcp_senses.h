// MCP pilot senses: agent vision + agent audio seams.
//
// WHAT: the model plays the game through the same eyes and ears as the
// player it pilots. IVisionCapture renders the software renderer from the
// pilot's camera (downgraded by design: lowest settings, low resolution);
// IAudioCapture drains the engine's mixed-output tap (the positional mix
// the pilot hears) at a downgraded sample-rate tier. Together with query
// (world state) and the player_* control tools, this closes the
// see -> think -> act loop.
//
// WHY separate from IPlayerController: observation vs actuation is the
// security boundary. kPermPilotSenses (1<<11) gates see/hear;
// kPermPlayerControl (1<<10) gates act. A monitoring-only agent gets the
// senses without the ability to move a muscle, and vice versa.
//
// MULTI-PILOT (locked 2026-10-11): every tool takes a `pilot` id and each
// seam declares the pilots it serves via pilotIds(). Rate buckets and audit
// logging are keyed by pilot id. v1 wires exactly one pilot ("player").
//
// NPC FUTURE (not built): a companion NPC is a pilot slot driven by game AI
// instead of an MCP client. The senses seams already address pilots, so a
// game-driven pilot reuses the same capture path (game AI "sees" through
// IVisionCapture with no MCP permission involved).
//
// Chunk loading (JD-locked 2026-10-11): cameras CAN load chunks, but in a
// LIMITED radius only - never the player's full view/simulation distances.
// The game's `camera.chunk_radius` cvar (default 4 chunks, tunable) is the
// budget; it scales with hardware (supercomputers get more) per the
// scalable-fidelity rule. A camera over unloaded territory renders sky, not
// a crash: the mesh cache simply has no data there.
//
// Downgrade ladder (JD-locked 2026-10-11):
//   vision: software renderer, flat shading, lowest tier; default 160x90,
//           configurable via the game's mcp.vision_width / mcp.vision_height
//           cvars (dotted scheme). PNG encoding (lossless): JPEG would need
//           a full DCT encoder (too much code) or a vendored lib (breaks
//           the STL-only rule); PNG with stored deflate blocks is ~120
//           lines, clean-room, and universally readable. At 160x90 the raw
//           frame is 43 KiB; PNG overhead is small.
//   audio:  the engine's mixed output tapped post-shed at tier sample rates
//           {8000, 16000, 24000, 48000} Hz mono s16le. Default tier 0 (the
//           lowest). The game's mcp.audio_tier cvar ramps it up later with
//           no code change. Chunk = 1024 frames at the tier rate.
//
// Positional vs global (audio): POSITIONAL by construction. The engine's mix
// is already the listener's mix — the game computes per-voice distance gain
// from the listener position (SoundDirector) before posting commands, so
// the tap hears what the pilot's ears hear, with the 3D spatial mix baked
// in. There is no global mix to tap. Multi-pilot caveat (v1): the engine
// mixes for one listener (the local player); per-pilot mixes arrive with
// pilot #2 — the seam already addresses pilots, the mixer does not yet.
//
// Rate discipline: vision 4 frames/sec/pilot (burst 8), audio 8 chunks/sec/
// pilot (burst 16). Frames are the most expensive thing an agent can
// request; the caps keep a curious model from melting the machine.
//
// Threading: capture*() run on the MCP server's thread and must be
// thread-safe for it. The game implements them against its own render/audio
// state with whatever synchronization it already uses.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace litholattice {
namespace mcp {

/// Downgraded audio sample rates, Hz, indexed by mcp.audio_tier (0..3).
/// Tier 0 is the JD-locked default (cheapest); raising the cvar ramps up.
inline constexpr int kAudioTierRates[4] = {8000, 16000, 24000, 48000};
inline constexpr int kAudioTierCount = 4;
/// Frames per audio chunk at the tier rate (128 ms at tier 0).
inline constexpr size_t kAudioChunkFrames = 1024;

/// One captured vision frame: PNG-encoded RGB from the pilot's camera.
struct VisionFrame {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> png;  ///< PNG bytes (RGB, 8-bit, non-interlaced)
  uint64_t tick = 0;         ///< sim tick the frame was captured on
  double camX = 0, camY = 0, camZ = 0;
  float yaw = 0, pitch = 0;
};

/// A pilot's vision camera: an independent camera placeable anywhere in
/// the world (the agent's location, a companion's, an overwatch position).
/// This is the first instantiation of the queued engine camera-system
/// concept (JD 2026-10-11): per-pilot pose, movable independently of players.
struct CameraPose {
  double x = 0, y = 0, z = 0;
  float yaw = 0;    ///< degrees, same convention as the game's player yaw
  float pitch = 0;  ///< degrees, positive up (same as player pitch)
  float fov = 70;   ///< degrees, 10..120
  bool trackPlayer = true;  ///< true = follow the piloted player (the default)
};

/// Game-side vision: renders the software renderer from a pilot's camera.
class IVisionCapture {
 public:
  virtual ~IVisionCapture() = default;
  /// Pilots this backend can capture (v1: {"player"}).
  virtual std::vector<std::string> pilotIds() const = 0;
  /// Renders one frame at width x height (0 = the game's cvar default) from
  /// the pilot's camera at the lowest settings. Returns false when the pilot
  /// is unknown or the player is not in a capturable state.
  virtual bool captureVisionFrame(std::string_view pilot, int width, int height,
                                  VisionFrame& out) = 0;
  /// Places the pilot's camera at an arbitrary pose (free camera). Returns
  /// false for an unknown pilot or an out-of-range pose. The pose persists
  /// until trackPlayer() or the next setCameraPose().
  virtual bool setCameraPose(std::string_view pilot, const CameraPose& pose) = 0;
  /// Returns the pilot to player-tracking (the default).
  virtual bool trackPlayer(std::string_view pilot) = 0;
  /// Current camera (the pose the next capture will use).
  virtual bool cameraPose(std::string_view pilot, CameraPose& out) const = 0;
};

/// One captured audio chunk: mono s16le at the tier rate.
struct AudioChunk {
  int sampleRate = 8000;
  std::vector<int16_t> samples;  ///< kAudioChunkFrames mono samples
  uint64_t tick = 0;
};

/// Game-side audio: drains the engine's mixed-output tap for a pilot.
class IAudioCapture {
 public:
  virtual ~IAudioCapture() = default;
  /// Pilots this backend can capture (v1: {"player"}).
  virtual std::vector<std::string> pilotIds() const = 0;
  /// Returns one 1024-frame chunk at the tier rate (tier 0..3, clamped).
  /// Returns false when the pilot is unknown or the tap has no data yet.
  virtual bool captureAudioChunk(std::string_view pilot, int tier, AudioChunk& out) = 0;
};

/// Base64 (RFC 4648, standard alphabet, padded). For embedding PNG/PCM
/// bytes in JSON-RPC tool results. Clean-room.
std::string base64Encode(const uint8_t* data, size_t len);

/// Minimal PNG encoder: 8-bit RGB, non-interlaced, filter type 0 (None) on
/// every scanline, DEFLATE with stored (uncompressed) blocks. Deterministic:
/// the same pixels always produce the same bytes (viewpoint tests rely on
/// this). Returns false on bad dimensions. Clean-room.
bool encodePngRgb(int width, int height, const uint8_t* rgb, std::vector<uint8_t>& out);

}  // namespace mcp
}  // namespace litholattice
