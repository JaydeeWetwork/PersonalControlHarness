// LithoLattice software renderer: CPU rasterizer, no GPU required.
// Like Half-Life's software mode: projects PackedVertex data and rasterizes
// triangles directly to a framebuffer on CPU. For headless testing, low-end
// devices, and VMs without GPUs.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "litholattice/render/cpu_texture_array.h"
#include "litholattice/render/cull_prototypes.h"
#include "litholattice/render/overlay_geometry.h"
#include "litholattice/render/pbr_quad_mesh.h"
#include "litholattice/render/renderer.h"
#include "litholattice/render/software_shaders.h"
#include "litholattice/render/pbr_software_shaders.h"

namespace litholattice {

// AA ladder + scratch are defined in render/software_aa.h (which includes
// this header for SoftwareFramebuffer, hence the forward declarations).
enum class SoftwareAaMode : int;
struct SoftwareAaScratch;

/// CPU framebuffer: RGBA pixels, written by the software rasterizer.
struct SoftwareFramebuffer {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint32_t> pixels;  ///< RGBA8, row-major

  void resize(uint32_t w, uint32_t h) {
    width = w; height = h;
    pixels.assign(size_t(w) * h, 0xFF000000);  // opaque black
  }
  void clear(uint32_t color = 0xFF000000) {
    std::fill(pixels.begin(), pixels.end(), color);
  }
};

/// Software renderer: IRenderer implemented with CPU rasterization, so a
/// host holding an IRenderer can select it like the GL/Vulkan backends.
/// Two surfaces, one rasterizer (they agree pixel-for-pixel given
/// equivalent inputs):
///  - the IRenderer interface: chunk/entity mesh stores + renderFrame()
///    (sky clear, stored meshes at their (cx,sy,cz) origins, entity verts,
///    FrameParams sprite quads in the flat average-color form);
///  - the direct CPU surface (renderMesh / framebuffer / shading controls)
///    that renderFrame is built on and that headless consumers — MCP vision
///    captures, biome_capture — drive directly.
/// Shading: flat face colors by default; setPerPixelShading(true) runs the
/// tier-selected fragment shader (software_shaders.h basic tier, or the PBR
/// tier via setShadingTier/setPbrContext) sampling textureArray().
/// FrameParams overlays ARE drawn (P1-RENDER H3, 2026-10-13): the HUD layer
/// is rasterized from buildHudVertices() output (solid rects + font-atlas
/// glyphs, alpha-blended in paint order, mirroring the GL33 hud program),
/// plus the block highlight outline and the crosshair as CPU line draws.
/// The direct renderMesh surface stays world-only, so MCP-vision and
/// biome_capture frames (which drive renderMesh directly) are unchanged.
/// PBR (P1-RENDER H2, 2026-10-13): the full IRenderer PBR surface is
/// implemented — setPbrMaterials feeds an owned PbrFragmentContext,
/// uploadPbrChunkMesh stores BinaryQuad meshes whose per-vertex compact
/// material IDs shade through pbrFragmentShader, setPbrTier selects the
/// PBR tier, setAmbientMode feeds the context. The transparency-tier
/// ladder itself is NOT implemented on CPU (see pbr_software_shaders.h
/// ULTRA TIER GAP); setPbrTier logs that once. Frustum culling with the
/// render.frustum_margin_deg margin + the cull prototypes runs in
/// renderFrame, like GL/VK (P1-RENDER M1).
class SoftwareRenderer : public IRenderer {
 public:
  SoftwareRenderer();
  ~SoftwareRenderer() override;

  // --- IRenderer interface ---------------------------------------------------
  const char* name() const override { return "software"; }
  std::string deviceInfo() const override;
  /// Frames land in framebuffer(); there is no window or swapchain.
  std::string presentMode() const override { return "framebuffer"; }
  ITextureArray& textureArray() override { return textures_; }
  /// Not consumed: the software fragment inputs carry no BlockMaterials —
  /// mask/palette/strip shading here resolves from the PackedVertex
  /// mode/value fields, and PBR material data arrives via setPbrContext().
  /// Accepted for interface parity (documented no-op).
  void setMaterials(const BlockMaterials& materials) override;
  /// No shader programs on the CPU path (fragment shading is compiled in),
  /// so no program can use pack sources: returns 0 by the contract.
  int loadShaders(const ShaderLibrary& shaders) override;
  void uploadChunkMesh(const ChunkMesh& mesh) override;  // replace by (cx,sy,cz); empty verts = remove
  void uploadEntityMesh(const std::vector<PackedVertex>& verts) override;  // dynamic triangle verts, replaced per upload
  // --- PBR / culling surface (P1-RENDER H2): real overrides, not no-ops ---
  /// True: BinaryQuad uploads shade through the CPU PBR fragment path.
  bool supportsPbrPath() const override { return true; }
  /// Feeds the owned PbrFragmentContext's registry (the context the PBR
  /// fragment path resolves compact material IDs through).
  void setPbrMaterials(const MaterialRegistry& reg) override;
  /// Records the tier and selects the software PBR shading tier. The CPU
  /// path has no transparency-tier ladder (documented gap): PBR fragments
  /// shade opaque; logged once, not silent.
  void setPbrTier(PbrTransparencyTier t) override;
  /// Flood-fill ambient mode, applied to the owned PBR context.
  void setAmbientMode(AmbientMode m) override;
  /// Frustum-culling margin in degrees, clamped 0..15 like GL/VK; used by
  /// renderFrame's per-mesh culling (M1).
  void setFrustumMarginDeg(float degrees) override;
  /// Near-camera model dithering (JD 2026-10-13): real implementation —
  /// the discard runs in both rasterizers from the fragment's view depth
  /// (clip-w interpolated), pattern = model_dither.h IGN. Applies to chunk
  /// meshes, PBR meshes, entities and sprites; HUD/lines are never faded.
  /// The render.model_dither SETTING defaults on (JD's "standard engine
  /// staple" call) and the game applies it at startup; this class starts
  /// with the fade OFF so headless consumers (tests, capture tools) see
  /// the pre-dither output unless they opt in — the same contract the
  /// other renderer state follows.
  void setModelDither(bool enabled, float nearBlocks = 2.f, float farBlocks = 8.f) override;
  bool modelDither() const { return ditherEnabled_; }
  float modelDitherNear() const { return ditherNear_; }
  float modelDitherFar() const { return ditherFar_; }
  /// Anti-aliasing ladder (JD 2026-10-13, render/software_aa.h): named
  /// modes behind render.aa — "off" (default; JD picks the shipped
  /// default from the capture sets), "fxaa" (cheapest post filter),
  /// "cmaa" (flagship: conservative morphological coverage restoration),
  /// "ssaa2x" (2x supersampling, render-time: renderFrame rasterizes at
  /// 2x and box-resolves before the overlay/HUD stage). All presentational.
  void setAaMode(const char* modeName) override;
  const char* aaModeName() const;
  /// Apply the current POST aa mode (fxaa / cmaa) to the framebuffer in
  /// place; Off and Ssaa2x are no-ops here (SSAA happens at render time).
  /// For direct-surface consumers (captures, MCP vision).
  void applyAaPostPass();
  /// ASCII pipeline (EXPERIMENT TIER, JD 2026-10-13,
  /// render/software_ascii.h): when enabled, renderFrame converts the
  /// finished world frame into a glyph visualization and stores the raw
  /// text grid (asciiText()) — the Daemon Sight low-end configuration.
  /// Driven by render.ascii / render.ascii_cols; default off.
  void setAsciiMode(bool enabled, int cols) override;
  void setAsciiGlyphSet(const char* setName) override;
  bool asciiEnabled() const { return asciiEnabled_; }
  int asciiCols() const { return asciiCols_; }
  int asciiGlyphSet() const { return asciiSet_; }  ///< AsciiGlyphSet as int (header cycle)
  /// Value span for the stage's luminance -> level mapping
  /// (render.ascii_value_span; JD amendment 2026-10-13: default 255 =
  /// the full 0-255 range, NOT a 64-step ramp).
  void setAsciiValueSpan(int span) override;
  int asciiValueSpan() const { return asciiSpan_; }
  /// Convert the current framebuffer to its ASCII glyph visualization
  /// and refresh asciiText(). Direct-surface sibling of the renderFrame
  /// ASCII stage.
  void applyAsciiPass();
  const std::string& asciiText() const { return asciiText_; }
  void setCullProtoConfig(const CullProtoConfig& cfg) override;
  CullProtoStats cullProtoStats() const override;
  /// BinaryQuad meshes, stored beside the classic meshes; empty = remove.
  void uploadPbrChunkMesh(const PbrChunkMesh& mesh) override;
  /// Clears the chunk mesh store. The entity verts are NOT cleared, matching
  /// the GL/Vulkan backends (their entity store is separate there too).
  void clearChunkMeshes() override;
  void renderFrame(const FrameParams& p) override;
  RenderStats stats() const override { return stats_; }
  void requestGrab() override { grabArmed_ = true; }
  /// The frame snapshot by the most recent armed renderFrame(), as
  /// top-row-first RGB at framebuffer size. Call on the render thread (the
  /// MCP adapter serializes its renderer externally, as before).
  bool grabFramebuffer(std::vector<uint8_t>& rgb, int& w, int& h) override;

  // --- Direct CPU surface (pre-interface API; semantics unchanged) -----------

  /// Render a mesh to the framebuffer (CPU rasterization). Vertex positions
  /// are sub-chunk-local; `viewProj` carries any world translation.
  void renderMesh(const PackedVertex* verts, size_t vertCount,
                  const float* viewProj);

  /// Get the framebuffer (for display or PNG export).
  const SoftwareFramebuffer& framebuffer() const { return fb_; }
  SoftwareFramebuffer& framebuffer() { return fb_; }

  void resize(uint32_t w, uint32_t h);
  void clear(uint32_t color = 0xFF000000);
  /// Feeds the texture array directly: `layers` 16x16 uint32 tiles
  /// (0xAARRGGBB). Same storage textureArray().upload() fills — one copy.
  void setTextures(const uint32_t* data, uint32_t layers);
  /// FXAA-lite: 3x3 blend gated on neighbourhood luminance contrast
  /// (flat regions untouched). Not a full FXAA shader; no production
  /// caller yet (P3-3, Appendix D L2).
  void applyFXAA();

  /// Enable/disable per-pixel shading (default: flat for speed).
  void setPerPixelShading(bool enable) { perPixel_ = enable; }

  /// Select the fragment shading tier for the per-pixel path (default Basic).
  /// Basic keeps the existing basicFragmentShader path intact; Pbr runs
  /// pbrFragmentShader (graceful per-fragment fallback to basic without material info).
  void setShadingTier(SoftwareShadingTier tier) { shadingTier_ = tier; }
  SoftwareShadingTier shadingTier() const { return shadingTier_; }

  /// PBR context (registry, lights, emissive provider, camera); must outlive the
  /// renderer. Null (default) means the PBR tier falls back to basic shading.
  void setPbrContext(const PbrFragmentContext* ctx) { pbrContext_ = ctx; }

 private:
  SoftwareFramebuffer fb_;
  std::vector<float> depth_;  ///< depth buffer
  std::vector<float> screen_;  ///< reusable vertex transform scratch (fix 2026-10-11:
                               ///< was allocated per renderMesh call)
  std::vector<uint32_t> fxaaSrc_;  ///< reusable applyFXAA source copy (fix 2026-10-11:
                                   ///< was a full-framebuffer copy per call)
  // Model dithering state (JD 2026-10-13); see setModelDither.
  bool ditherEnabled_ = false;  // the game-side setting (default on) enables it
  float ditherNear_ = 2.f;  ///< blocks: at/below this view depth geometry is fully faded
  float ditherFar_ = 8.f;   ///< blocks: at/above this view depth geometry is untouched
  // AA ladder state (JD 2026-10-13); see setAaMode.
  SoftwareAaMode aaMode_{};  ///< value-initialized = 0 = Off
  std::unique_ptr<SoftwareAaScratch> aaScratch_;  ///< lazily built post-pass scratch
  SoftwareFramebuffer aaOut_;                     ///< SSAA resolve target (renderFrame)
  SoftwareAaScratch& ensureAaScratch();
  // ASCII state (JD 2026-10-13); see setAsciiMode / setAsciiValueSpan.
  bool asciiEnabled_ = false;
  int asciiCols_ = 160;
  int asciiSet_ = 0;  ///< AsciiGlyphSet (0 = Ascii, 1 = Systerian) as int
  int asciiSpan_ = 255;  ///< value span (JD amendment: full 0-255 default)
  std::string asciiText_;  ///< last ASCII stage's raw text grid
  bool perPixel_ = false;  ///< per-pixel shading (slower, better quality)
  SoftwareShadingTier shadingTier_ = SoftwareShadingTier::Basic;  ///< fragment tier (per-pixel path)
  const PbrFragmentContext* pbrContext_ = nullptr;  ///< PBR context (registry/lights/camera); may be null

  CpuTextureArray textures_;  ///< the one texture store (setTextures + ITextureArray)

  /// Chunk meshes by (cx,sy,cz), keyed exactly like the GPU backends.
  struct StoredMesh {
    int cx = 0, sy = 0, cz = 0;
    std::vector<PackedVertex> verts;  // sub-chunk-local quads
    // Wafer ground pulse + trace params baked into the ChunkMesh at mesh
    // time; renderFrame adds both emissives for kShadeGroundPulse quads
    // through the CPU mirrors (ground_pulse.h / ground_traces.h).
    float pulseP0[4] = {0.f, 0.f, 0.f, 0.f};
    float pulseP1[4] = {0.f, 0.f, 0.f, 0.f};
    float traceP0[4] = {0.f, 0.f, 0.f, 0.f};
    float traceP1[4] = {0.f, 0.f, 0.f, 0.f};
  };
  std::map<int64_t, StoredMesh> meshes_;
  std::map<int64_t, PbrChunkMesh> pbrMeshes_;  ///< BinaryQuad uploads, same keying

  // IRenderer PBR/culling state (H2/M1).
  float frustumMarginDeg_ = 5.f;  ///< culling margin, degrees (JD 2026-10-11)
  CullProtoState cullProto_;      ///< culling-cache prototypes; all default off
  PbrTransparencyTier pbrTier_ = PbrTransparencyTier::ScreenDoor;  ///< recorded tier
  AmbientMode ambientMode_ = AmbientMode::Fraction;
  const MaterialRegistry* pbrMaterials_ = nullptr;  ///< setPbrMaterials registry
  PbrFragmentContext ownedPbrCtx_;  ///< context fed by setPbrMaterials/setAmbientMode
  /// FrameParams::pbrLights as an IEmissiveProvider for the owned context:
  /// the game already reduces its emissive scan into that array per frame.
  struct FrameLightsProvider : IEmissiveProvider {
    const FrameParams* frame = nullptr;
    void forEachEmissive(const std::function<void(float, float, float, float, float, float,
                                                  float)>& sink) const override {
      if (!frame) return;
      for (uint32_t i = 0; i < frame->pbrLightCount; ++i) {
        const AnalyticLight& l = frame->pbrLights[i];
        sink(l.x, l.y, l.z, l.r, l.g, l.b, l.intensity);
      }
    }
  };
  FrameLightsProvider frameLights_;
  /// The context fragment shading actually reads: an externally supplied
  /// setPbrContext() wins; otherwise the owned context (when a registry
  /// was supplied via setPbrMaterials); otherwise null (basic fallback).
  const PbrFragmentContext* effectivePbrContext() const {
    if (pbrContext_) return pbrContext_;
    return pbrMaterials_ ? &ownedPbrCtx_ : nullptr;
  }

  // Ground pulse/trace state for the mesh currently being drawn by
  // renderFrame (single-threaded rasterizer; set per mesh, read by the
  // per-pixel shader glue below). Zero params = no contribution.
  struct GroundFx {
    float pulseP0[4] = {0.f, 0.f, 0.f, 0.f};
    float pulseP1[4] = {0.f, 0.f, 0.f, 0.f};
    float traceP0[4] = {0.f, 0.f, 0.f, 0.f};
    float traceP1[4] = {0.f, 0.f, 0.f, 0.f};
    float pulseTimeSec = 0.f, traceTimeSec = 0.f;
    bool pulseOn = false, traceOn = false;
    bool active() const {
      return (pulseOn && pulseP0[2] > 0.f) || (traceOn && traceP0[2] > 0.f && traceP0[0] > 0.f);
    }
  };
  GroundFx groundFx_;
  float meshOriginX_ = 0.f, meshOriginY_ = 0.f, meshOriginZ_ = 0.f;  ///< world origin of the mesh being drawn

  // HUD overlay state (H3): built lazily, reused across frames.
  std::vector<HudVertex> hudVerts_;
  HudFontAtlas hudAtlas_;
  bool hudAtlasReady_ = false;
  std::vector<PackedVertex> entityVerts_;  ///< dynamic triangle verts (3 per triangle)
  std::vector<PackedVertex> spriteScratch_;  ///< renderFrame sprite conversion scratch
  // Sprite average-color cache (P3-3, Appendix D M2): the average of a
  // quad's atlas rect, keyed by (atlas version, page, pixel rect) — the
  // old code rebuilt a temp Image from a fresh vector per quad per frame.
  // Cleared wholesale when the atlas version moves.
  struct SpriteAvgKey {
    uint64_t version = 0;
    int page = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool operator<(const SpriteAvgKey& o) const {
      if (version != o.version) return version < o.version;
      if (page != o.page) return page < o.page;
      if (x0 != o.x0) return x0 < o.x0;
      if (y0 != o.y0) return y0 < o.y0;
      if (x1 != o.x1) return x1 < o.x1;
      return y1 < o.y1;
    }
  };
  std::map<SpriteAvgKey, std::array<float, 4>> spriteAvgCache_;  ///< RGBA average (RGB alpha-weighted)
  uint64_t spriteAvgVersion_ = UINT64_MAX;  ///< atlas version the cache belongs to
  /// One AlphaBlend/Additive sprite quad in screen space, drawn in
  /// producer order with depth TEST but no depth WRITE (D-M2).
  struct BlendSprite {
    float v[4][4];  // screen-space verts (x, y, depth, clipW)
    float r = 1.f, g = 1.f, b = 1.f, a = 1.f;
    bool additive = false;
  };
  std::vector<BlendSprite> spriteBlendScratch_;
  std::vector<PackedVertex> pbrScratch_;     ///< renderPbrMeshes position conversion scratch
  std::vector<uint32_t> pbrScratchMat_;      ///< per-vertex material ID parallel to pbrScratch_
  RenderStats stats_;
  bool grabArmed_ = false;   ///< requestGrab() arms the next renderFrame()
  bool grabReady_ = false;   ///< an armed frame has been snapshotted
  std::vector<uint8_t> grabRgb_;  ///< top-row-first RGB of the armed frame
  int grabW_ = 0, grabH_ = 0;

  /// Rasterize a single triangle (flat shaded, depth tested, backface culled).
  void rasterizeTriangle(const float* v0, const float* v1, const float* v2,
                         uint32_t color);
  /// Rasterize with per-pixel shader (slower, higher quality). `groundFx`
  /// adds the wafer ground pulse + traces emissives for a tagged quad.
  void rasterizeTriangleShaded(const float* v0, const float* v1, const float* v2,
                               const ShaderVaryings* s0, const ShaderVaryings* s1,
                               const ShaderVaryings* s2, FragmentShaderFn shader,
                               bool groundFx = false);
  /// Transform `count` verts to screen space in the screen_ scratch
  /// (x, y, depth, clipW per vertex); returns the scratch pointer.
  const float* transformVerts(const PackedVertex* verts, size_t count, const float* viewProj);
  /// Transform `count` verts into screen_ and draw them as flat-shaded quads
  /// (4 verts each). The flat path shared by renderMesh and the sprite pass.
  void renderFlatQuads(const PackedVertex* verts, size_t count, const float* viewProj);
  /// Draw the stored entity triangle verts through the current shading path.
  void renderEntityVerts(const float* viewProj);
  /// FrameParams sprite quads in the flat average-color form (the same
  /// conversion the game's sprite host feeds the MCP capture path).
  /// Shade handling (P3-3, Appendix D M2): Cutout (and any non-blend mode)
  /// draws opaque with depth write, as before; AlphaBlend and Additive
  /// quads are drawn in producer order (back-to-front) with depth test
  /// but NO depth write, blended into the framebuffer — previously every
  /// mode was flattened to opaque and occluded like world geometry.
  void drawSprites(const FrameParams& p);
  /// Rasterize one blended sprite triangle pair: depth-tested against
  /// depth_, never writes depth; src-over (AlphaBlend) or additive.
  void rasterizeBlendSprite(const BlendSprite& s);
  /// Draw the stored BinaryQuad (PBR) meshes: per-vertex material IDs shade
  /// through pbrFragmentShader via the effective PBR context.
  void renderPbrMeshes(const FrameParams& p);
  /// CPU HUD rasterizer (H3): buildHudVertices triangles alpha-blended in
  /// paint order (u < 0 = solid; otherwise font-atlas coverage, NEAREST like
  /// the GL33 hud program), no depth test — drawn after the world.
  void drawHud(const FrameParams& p);
  /// One HUD triangle: barycentric blend over fb_ with interpolated
  /// uv/color; coverage from the font atlas for glyph triangles.
  void rasterizeHudTriangle(const HudVertex& a, const HudVertex& b, const HudVertex& c);
  /// Block highlight outline (depth-tested) + crosshair (on top), drawn as
  /// CPU line segments through the same projections the GPU backends use.
  void drawOverlayLines(const FrameParams& p);
  /// Blend one pixel (src-over, straight alpha) at (x, y) if inside fb_.
  void blendPixel(int x, int y, float r, float g, float b, float a);
  /// Plot a depth-tested world-space line segment (highlight outline).
  void drawWorldLine(const FrameParams& p, const Vec3& a, const Vec3& b, uint32_t color);
  /// Plot an NDC line segment ignoring depth (crosshair).
  void drawNdcLine(const Vec3& a, const Vec3& b, uint32_t color);
};

}  // namespace litholattice
