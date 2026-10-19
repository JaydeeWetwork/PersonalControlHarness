// Renderer interface. Backends (gl33 here, Vulkan in render_vk) implement IRenderer and
// ITextureArray (texture_array.h).
// Render code knows chunk meshes and frame parameters only: nothing about the sim.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include "litholattice/assets/image.h"
#include "litholattice/core/math.h"
#include "litholattice/render/cull_prototypes.h"

namespace litholattice {
// Core mesh/shading types, declared early: the PBR headers below pull in
// transparency.h, which needs the complete ShadeMode/PackedVertex definitions.
// (Moved up from below; declaration order only, no semantics change.)
// 16-byte vertex: position in 1/16 block units local to the sub-chunk (sub-block shapes),
// texture UV in 1/16 tile units (already rotated), face | ao << 3 | rot << 5 (rot = quarter turns
// applied to the UV, so shaders can recover the unrotated tile UV for masks), texture-array layer,
// shading value (meaning set by the mode: tint strength, palette index, pip / cell count) and shading mode,
// then the 32-bit buffer slot (mesher-side use; not consumed by the classic chunk shaders).
// LAYOUT CONTRACT (fix 2026-10-11): the first 12 bytes (x,y,z,u,v,faceAo,layer,value,mode) are the
// stable GPU vertex-attribute prefix — gl33/vk binding sites read @0/@6/@10. `slot` rides at @12.
// PBR VERTEX-FORMAT NOTE (phase 2, Worker 3 - JD's locked coexistence decision,
// 2026-10-11, SUPERSEDES the phase-1 "Option B" note below): BinaryQuad and
// PackedVertex COEXIST as a user-selectable mesher option; PackedVertex is NEVER
// extended and no parallel attribute stream was added. The BinaryQuad path carries
// material IDs natively (BinaryQuad::material = 8-bit palette index, passthrough)
// through its own PbrQuadVertex layout (render/pbr_quad_mesh.h) into the chunk_pbr
// programs; the PackedVertex path keeps the classic chunk programs, untouched -
// classic-mesh fragments still resolve to the 0xFFFF "none" marker and the PBR tier
// falls back to basic shading per fragment (pinned by the engine tests' pbr-seam
// checks). Phase 1's lean-Option-B deliberation is kept below for the record only.
//   (phase-1 text: A. Extend PackedVertex (e.g. +uint16 material -> 18 bytes)...
//   B. Parallel attribute stream... Lean was B first. Final call was JD's: NEITHER -
//   coexistence via the BinaryQuad path instead.)
#pragma pack(push, 1)
struct PackedVertex { uint16_t x, y, z; uint8_t u, v, faceAo, layer, value, mode; uint32_t slot; };
#pragma pack(pop)
static_assert(sizeof(PackedVertex) == 16, "vertex layout");

enum ShadeMode : uint8_t {
  kShadeTex = 0,      // plain texture
  kShadeEmissive,     // texture, self-lit
  kShadeCutout,       // texture with alpha test (sprites)
  kShadeStripTint0, kShadeStripTint1,                 // neutral strip piece tinted by palette row [value] + ring mask (bindings strip 0 / 1)
  kShadePalette16, kShadePalette32, kShadePalette64,  // palette colour [value] (wrap) inside the tier mask + rim
  kShadePips,         // value 1..8 pips via mask
  kShadeCells,        // value 1..64 cells via mask
  kShadeAnimated,     // animated face: value = animation index (frame strip + timing)
  kShadeGlass,        // translucent (screen-door: alpha vs a 4x4 ordered dither; no sorting needed)
  kShadeAlphaBlend,   // alpha blending (src_alpha, 1-src_alpha); needs back-to-front sorting
  kShadeWeightedOIT,  // weighted blended order-independent transparency; no sorting
  kShadeAdditive,     // additive blending (src_alpha, 1); no sorting; for particles/glow
  kShadeReadout,      // 7-segment readout overlay: layer = segment mask (R = id x16, G = coverage), value = number | 128 lit
  kShadeGroundPulse,  // wafer ground pulse (JD 2026-10-11): textured face + time-driven
                      // emissive add; value = GroundPulseBiome id (1..5, ground_pulse.h).
                      // Appended last: existing modes keep their values.
};
}  // namespace litholattice

#include "litholattice/render/pbr_lights.h"  // AnalyticLight for FrameParams::pbrLights
#include "litholattice/render/pbr_transparency_tiers.h"  // PbrTransparencyTier for setPbrTier

namespace litholattice {

class BlockMaterials;
class ITextureArray;
class MaterialRegistry;  // pbr_materials.h (forward: only used as a const ref here)
class ShaderLibrary;
struct PbrChunkMesh;  // pbr_quad_mesh.h (forward: only used as a const ref here)


/// Most quads one sub-chunk mesh may hold. The mesher enforces it (and logs if it ever trims);
/// both backends size their shared quad index buffer from it, so they can trust every mesh.
constexpr uint32_t kMaxQuadsPerMesh = 16384;

struct ChunkMesh {
  int cx = 0, sy = 0, cz = 0;
  uint32_t epoch = 0;
  uint64_t seq = 0;
  std::vector<PackedVertex> verts;  // quads (4 verts each); empty = remove the mesh
  /// Wafer ground pulse (JD 2026-10-11): this chunk column's weight-blended
  /// pulse params, baked at mesh time (biome weights are worldgen-static, so no
  /// per-frame work). p0 = (freqHz, sharpness, amplitude, 0),
  /// p1 = (tintR, tintG, tintB, 0). The backends push them per draw call for
  /// kShadeGroundPulse quads; all-zero = no pulse (e.g. no provider wired).
  float pulseP0[4] = {0.f, 0.f, 0.f, 0.f};
  float pulseP1[4] = {0.f, 0.f, 0.f, 0.f};
  /// Wafer ground traces (JD 2026-10-11): this chunk column's weight-blended
  /// trace params, baked at mesh time (same biome weight vector as the pulse).
  /// t0 = (density, speed, amplitude, dirMix), t1 = (tintR, tintG, tintB, 0).
  /// The backends push them per draw call for kShadeGroundPulse quads;
  /// all-zero = no traces. Composes additively with the pulse.
  float traceP0[4] = {0.f, 0.f, 0.f, 0.f};
  float traceP1[4] = {0.f, 0.f, 0.f, 0.f};
};

// 2D overlay in pixels (origin top-left). Generic: the renderer knows nothing about the game.
constexpr int kHudGlyphW = 8, kHudGlyphH = 16;  // baked font cell, multiplied by HudLayer::scale
struct HudRect { float x, y, w, h; float r, g, b, a; };
struct HudText { float x, y; std::string text; float r = 1, g = 1, b = 1, a = 1; };
/// Clip rectangle in pixels; the default clips nothing.
struct HudClip { float x0 = -1e9f, y0 = -1e9f, x1 = 1e9f, y1 = 1e9f; };
/// One retained-UI draw, painted in order after the rects / texts above (so overlapping
/// windows layer correctly) and clipped on the CPU. Empty text = solid rect.
struct HudDraw {
  float x = 0, y = 0, w = 0, h = 0;  ///< rect (w, h unused for text)
  float r = 1, g = 1, b = 1, a = 1;
  float scale = 1.f;                 ///< text: glyph cell multiplier
  std::string text;
  HudClip clip;
};
struct HudLayer {
  float scale = 1.f;
  std::vector<HudRect> rects;
  std::vector<HudText> texts;
  std::vector<HudDraw> draws;  ///< UI (ui::IUiRenderer through HudUiRenderer)
};

/// One world-space textured quad for the dynamic sprite pass (sprite system
/// integration, 2026-10-12). Corners are world blocks, CCW seen from the
/// front ([0] bottom-left, [1] bottom-right, [2] top-right, [3] top-left);
/// uvs are atlas image space (u right, v down from the page's top-left — a
/// backend uploading page images row-first pairs them with the corners
/// exactly as given). `shade` is one of kShadeCutout / kShadeAlphaBlend /
/// kShadeAdditive; `page` indexes FrameParams::spriteAtlasPages. The game's
/// sprite host converts the sprite system's emitted quads into these per
/// frame; backends never see sprite-module types.
struct SpriteDrawQuad {
  Vec3 corners[4]{};
  float uv[4][2]{};
  float r = 1.f, g = 1.f, b = 1.f, a = 1.f;  ///< tint multiplier
  ShadeMode shade = kShadeCutout;
  int32_t page = -1;
  float sortDepth = 0.f;  ///< view depth (larger = farther); producer-sorted
};

struct FrameParams {
  int width = 0, height = 0;
  Mat4 viewProj;
  Vec3 camPos, sky;
  float yawRad = 0.f, pitchRad = 0.f;  ///< camera orientation, radians (Player::forward convention);
                                       ///< feeds the velocity-adaptive culling prototype (all off by default)
  float fogEnd = 200.f;
  bool highlight = false; int hx = 0, hy = 0, hz = 0;  // outlined block
  bool crosshair = true;
  float timeSec = 0;     // animation clock
  /// Wafer ground pulse (JD 2026-10-11): sim-time seconds driving the surface
  /// pulse (worldTick / tick rate - pausing the sim freezes it). groundPulseOn
  /// is the live `render.ground_pulse` cvar (default on); off costs one uniform
  /// branch per fragment, no remesh.
  float groundPulseTimeSec = 0;
  bool groundPulseOn = true;
  /// Wafer ground traces (JD 2026-10-11): sim-time seconds driving the trace
  /// packet flow (same clock as the pulse - pausing the sim freezes it).
  /// groundTraceOn is the live `render.ground_traces` cvar (default on),
  /// independent of `render.ground_pulse`; both add into the fragment.
  float groundTraceTimeSec = 0;
  bool groundTraceOn = true;
  bool debugOverlay = false;  // F3: debug-only shading (wire strength numbers)
  HudLayer hud;
  /// PBR analytic lights for the BinaryQuad path (phase 2, Worker 3): the game fills
  /// these per frame from a LightCollector fed by ChunkEmissiveProvider
  /// (pbr_emissive_provider.h); the GL backend uploads them to the chunk_pbr
  /// uPbrLightPos/uPbrLightColor uniforms. Empty = specular from analytic lights off
  /// (flood-fill irradiance still shades). Backends without a PBR path ignore it.
  static constexpr size_t kMaxPbrFrameLights = 8;  // mirrors PBR_MAX_LIGHTS in the shaders
  AnalyticLight pbrLights[kMaxPbrFrameLights];
  uint32_t pbrLightCount = 0;
  /// Sprite pass (sprite system integration, 2026-10-12): this frame's sprite
  /// quads, pre-ordered by the producer (cutout first in spawn order, then
  /// alpha-blend / additive back-to-front), and the atlas pages they sample.
  /// A backend with a sprite path (GL33) re-uploads the pages when
  /// spriteAtlasVersion differs from the version it last uploaded and draws
  /// the quads after the world, before lines/HUD. Backends without a sprite
  /// path ignore both fields (same standing as the PBR fields above). All
  /// pointed-to / spanned data is borrowed: valid for this renderFrame call.
  const SpriteDrawQuad* spriteQuads = nullptr;
  size_t spriteQuadCount = 0;
  std::span<const Image> spriteAtlasPages{};
  uint64_t spriteAtlasVersion = 0;
};

struct RenderStats { uint32_t meshes = 0, drawn = 0; };

class IRenderer {
 public:
  virtual ~IRenderer() = default;
  virtual const char* name() const = 0;
  virtual std::string deviceInfo() const = 0;  // e.g. GL vendor | renderer | version
  /// How frames are presented (Vulkan: fifo / mailbox / immediate; GL: the window's swap interval).
  virtual std::string presentMode() const { return "swap-interval"; }
  /// The block texture array (layer = TextureId). Upload a TextureRegistry to it on (re)load.
  virtual ITextureArray& textureArray() = 0;
  /// Material uniforms (mask layers, palette rows, animations) from the block models.
  virtual void setMaterials(const BlockMaterials& materials) = 0;
  /// (Re)builds the shader programs from `shaders` (pack files). A program that is missing or
  /// fails to compile falls back to the built-in base copy, with a log line. Returns the number
  /// of programs that use pack sources.
  virtual int loadShaders(const ShaderLibrary& shaders) = 0;
  virtual void uploadChunkMesh(const ChunkMesh& mesh) = 0;  // replace by (cx,sy,cz)
  virtual void uploadEntityMesh(const std::vector<PackedVertex>& verts) = 0;  // dynamic (items, etc.)
  virtual void clearChunkMeshes() = 0;
  /// PBR (BinaryQuad) path (phase 2, Worker 3). Defaults are no-ops / unsupported so the
  /// Vulkan backend, the null/headless renderers and older game builds keep compiling;
  /// the GL33 backend overrides all four. Program selection follows the meshes, not a
  /// mode flag: classic chunk programs draw PackedVertex uploads, chunk_pbr draws
  /// PbrChunkMesh uploads - both maps can coexist in one frame.
  virtual bool supportsPbrPath() const { return false; }  ///< chunk_pbr built and usable
  /// Uploads the MaterialRegistry as the GPU material table (48-byte std430 rows,
  /// pbr_gl_upload.h). Backends re-upload only when reg.generation() moved.
  virtual void setPbrMaterials(const MaterialRegistry& reg) { (void)reg; }
  /// Selects the PBR transparency tier. BACKEND AVAILABILITY (exact, P3-3
  /// Appendix D M7): the tier ladder is implemented ONLY on the GL33
  /// backend for BinaryQuad (PbrChunkMesh) uploads — Simple / ScreenDoor /
  /// PbrReflections via the tier uniform, TrueRefraction as the two-pass
  /// scene-color path. The software backend records the tier and runs its
  /// PBR fragment path but implements NO tier ladder (fragments shade
  /// opaque; logged once in setPbrTier). Vulkan has no PBR chunk path at
  /// all. The CPU helper shadeTransparent() implements Simple /
  /// ScreenDoor / PbrReflections per fragment; its TrueRefraction branch
  /// is a request stub (refractionRequested + black). Default ScreenDoor.
  virtual void setPbrTier(PbrTransparencyTier t) { (void)t; }
  /// Selects the flood-fill ambient computation (Fraction = cheap default,
  /// NormalizedSun = direction-aware hemisphere). Default Fraction.
  /// Backends apply it to the PBR diffuse path (CPU + GPU); the classic path is
  /// unaffected. JD-locked 2026-10-11 (cvar `render.ambient_mode`).
  virtual void setAmbientMode(AmbientMode m) { (void)m; }
  /// Angular frustum-culling margin in degrees (0..15, default 5): chunks inside
  /// FOV +/- margin render; the rest cull. The shock absorber against "frustum
  /// occlusion tearing" (JD 2026-10-11): fast rotation reveals already-rendered
  /// chunks instead of pop-in. JD-locked 2026-10-11 (cvar `render.frustum_margin_deg`).
  virtual void setFrustumMarginDeg(float degrees) { (void)degrees; }
  /// Near-camera model dithering (JD 2026-10-13): geometry whose view depth
  /// falls inside [nearBlocks, farBlocks] fades out through a deterministic
  /// screen-door discard (interleaved gradient noise on the pixel position;
  /// render/model_dither.h) in the shading stage, so close-up cameras —
  /// free cams, MCP/agent cameras, Daemon-vision feeds — are not swallowed
  /// by geometry. Purely presentational: no sim interaction, no remesh.
  /// Default here is an honest no-op (unsupported): the software backend
  /// and GL33 implement it; Vulkan does not yet (its chunk program shares
  /// chunk_shading.glsl, but the band uniforms have no push-constant /
  /// descriptor plumbing on that backend — see Model-Dithering-2026-10-13.md).
  /// Driven by the render.model_dither{,_near,_far} cvars.
  virtual void setModelDither(bool enabled, float nearBlocks = 2.f, float farBlocks = 8.f) {
    (void)enabled;
    (void)nearBlocks;
    (void)farBlocks;
  }
  /// Software-renderer anti-aliasing ladder (JD 2026-10-13): named modes
  /// "off" | "fxaa" | "cmaa" | "ssaa2x" behind the render.aa cvar
  /// (render/software_aa.h). CPU-framebuffer techniques, so only the
  /// software backend implements this; the GPU backends keep their own
  /// MSAA story and inherit this honest no-op.
  virtual void setAaMode(const char* modeName) { (void)modeName; }
  /// ASCII output pipeline (experiment tier, JD 2026-10-13): when on, the
  /// software backend converts its finished frame into a glyph grid
  /// (render/software_ascii.h) — the Daemon Sight low-end configuration.
  /// Driven by render.ascii / render.ascii_cols. No-op elsewhere.
  virtual void setAsciiMode(bool enabled, int cols) {
    (void)enabled;
    (void)cols;
  }
  /// Glyph set for the ASCII pipeline (JD 2026-10-13): "ascii" or
  /// "systerian" (render.ascii_set) — the Systerian set renders the
  /// engine's certificate-signing script through its ASCII layer.
  virtual void setAsciiGlyphSet(const char* setName) { (void)setName; }
  /// Value span for the ASCII/Systerian stage (JD-RULED AMENDMENT
  /// 2026-10-13): the luminance -> level mapping spans the FULL 0-255
  /// range by default (render.ascii_value_span = 255) — a deliberate
  /// exception to the game's 64-scale conventions. No-op elsewhere.
  virtual void setAsciiValueSpan(int span) { (void)span; }
  /// Culling-cache prototype toggles (JD 2026-10-11; all default OFF): comet
  /// tail, velocity-adaptive margin, two-tier margin. See cull_prototypes.h.
  virtual void setCullProtoConfig(const CullProtoConfig& cfg) { (void)cfg; }
  /// Per-frame prototype stats (full / meshOnly / culled / cometKept).
  virtual CullProtoStats cullProtoStats() const { return {}; }
  /// Replace the PBR mesh by (cx,sy,cz); empty verts = remove.
  virtual void uploadPbrChunkMesh(const PbrChunkMesh& mesh) { (void)mesh; }
  /// Draws one frame. OpenGL: the caller presents (Window::swap). Vulkan presents itself.
  virtual void renderFrame(const FrameParams& p) = 0;
  virtual RenderStats stats() const = 0;
  /// Arms a one-shot framebuffer grab: the backend makes the frame drawn by
  /// the next renderFrame() available to grabFramebuffer(). Default: no-op
  /// (grabFramebuffer() then reports false). The screenshot command arms the
  /// grab when it runs (during update, before the frame is drawn).
  virtual void requestGrab() {}
  /// Reads back the most recently rendered frame as top-row-first RGB
  /// (resized to w*h*3). Returns false when unsupported, or when no armed
  /// frame has been drawn yet. OpenGL: glReadPixels of the back buffer —
  /// call after renderFrame(), before present. Vulkan: an image-to-host
  /// copy recorded while the armed frame was drawn (before present). Call on
  /// the render thread.
  virtual bool grabFramebuffer(std::vector<uint8_t>& rgb, int& w, int& h) {
    (void)rgb;
    (void)w;
    (void)h;
    return false;
  }
};

using GLProcLoader = void* (*)(const char* name);
// OpenGL 3.3 core backend. Needs a current GL context; starts with the built-in shaders and an
// empty texture array. Returns nullptr on failure.
/// `arena`: chunk meshes share one growable VBO + one VAO (glBufferSubData, base-vertex
/// draws); false = one VBO + VAO per sub-chunk (the old path, kept for A/B benches).
std::unique_ptr<IRenderer> createGL33Renderer(GLProcLoader loader, bool arena = true);

}  // namespace litholattice
