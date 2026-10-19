// Software renderer: full CPU rasterizer implementation.
// Optimized for voxels: backface culling, depth buffer, scanline rasterization.
// Shading: flat face colors, or per-pixel fragment shaders (basic / PBR tiers)
// sampling the CPU texture array. For baseline testing and GPU-less platforms.
// Implements IRenderer (mesh stores + renderFrame) on top of the same direct
// renderMesh path the headless consumers have always driven.
#include "litholattice/render/software_renderer.h"
#include "litholattice/render/model_dither.h"
#include "litholattice/render/software_aa.h"
#include "litholattice/render/software_ascii.h"
#include "litholattice/render/software_shaders.h"
#include "litholattice/core/log.h"
#include "litholattice/render/ground_pulse.h"
#include "litholattice/render/ground_traces.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace litholattice {

// ---------------------------------------------------------------------------
// CpuTextureArray
// ---------------------------------------------------------------------------

bool CpuTextureArray::upload(const TextureRegistry& textures) {
  const std::span<const Texture> src = textures.textures();
  layers_ = int(src.size());
  texels_.assign(size_t(layers_) * 256, 0xFFFF00FFu);  // magenta = missing
  for (int layer = 0; layer < layers_; ++layer) {
    const Image& img = src[size_t(layer)].image;
    if (img.empty() || img.width() <= 0 || img.height() <= 0) continue;
    uint32_t* dst = &texels_[size_t(layer) * 256];
    // Box filter to 16x16 (identity when the source tile already is 16x16):
    // the same bounds and per-channel integer averaging biome_capture applies
    // when it builds the software texture buffer by hand.
    for (int y = 0; y < kTileSize; ++y) {
      const int y0 = y * img.height() / kTileSize;
      const int y1 = std::max(y0 + 1, (y + 1) * img.height() / kTileSize);
      for (int x = 0; x < kTileSize; ++x) {
        const int x0 = x * img.width() / kTileSize;
        const int x1 = std::max(x0 + 1, (x + 1) * img.width() / kTileSize);
        uint32_t sum[4] = {0, 0, 0, 0};
        int n = 0;
        for (int sy = y0; sy < y1; ++sy)
          for (int sx = x0; sx < x1; ++sx, ++n) {
            const uint8_t* p = img.pixel(sx, sy);
            for (int k = 0; k < 4; ++k) sum[k] += p[k];
          }
        const uint32_t r = n ? sum[0] / uint32_t(n) : 0, g = n ? sum[1] / uint32_t(n) : 0,
                       b = n ? sum[2] / uint32_t(n) : 0, a = n ? sum[3] / uint32_t(n) : 0xFF;
        dst[size_t(y) * kTileSize + size_t(x)] = (a << 24) | (r << 16) | (g << 8) | b;
      }
    }
  }
  return true;
}

void CpuTextureArray::setRaw(const uint32_t* data, uint32_t layers) {
  if (!data || layers == 0) {
    texels_.clear();
    layers_ = 0;
    return;
  }
  layers_ = int(layers);
  texels_.assign(data, data + size_t(layers) * 256);
}

// ---------------------------------------------------------------------------
// SoftwareRenderer
// ---------------------------------------------------------------------------

SoftwareRenderer::SoftwareRenderer() {
  fb_.resize(800, 600);
  depth_.resize(size_t(800) * 600, 1.0f);
}

SoftwareRenderer::~SoftwareRenderer() = default;

void SoftwareRenderer::resize(uint32_t w, uint32_t h) {
  fb_.resize(w, h);
  depth_.assign(size_t(w) * h, 1.0f);
}

void SoftwareRenderer::clear(uint32_t color) {
  fb_.clear(color);
  std::fill(depth_.begin(), depth_.end(), 1.0f);
}

// Transform a PackedVertex position to screen space.
// PackedVertex x,y,z are in 1/16 block units local to sub-chunk.
static void transformVertex(const PackedVertex& v, const float* mvp,
                            float* out, uint32_t width, uint32_t height) {
  // Position in block units (1/16 -> 1.0)
  float x = v.x / 16.0f, y = v.y / 16.0f, z = v.z / 16.0f;
  // MVP transform (column-major 4x4)
  float cx = mvp[0]*x + mvp[4]*y + mvp[8]*z + mvp[12];
  float cy = mvp[1]*x + mvp[5]*y + mvp[9]*z + mvp[13];
  float cz = mvp[2]*x + mvp[6]*y + mvp[10]*z + mvp[14];
  float cw = mvp[3]*x + mvp[7]*y + mvp[11]*z + mvp[15];
  // Preserve the sign: callers reject behind-camera geometry via w <= 0, so
  // clamping a negative w to +1e-6f would defeat near-plane rejection.
  if (std::abs(cw) < 1e-6f) cw = std::copysign(1e-6f, cw);
  // Perspective divide + viewport. out[3] carries the clip-space w so callers
  // can reject behind-camera geometry (w <= 0 projects mirrored; fix 2026-10-11).
  const float invW = 1.0f / cw;
  out[0] = (cx * invW * 0.5f + 0.5f) * width;
  out[1] = (1.0f - (cy * invW * 0.5f + 0.5f)) * height;  // flip Y
  out[2] = cz * invW;  // depth 0..1
  out[3] = cw;
}

// Backface culling: skip if triangle faces away.
// Screen space is y-DOWN (transformVertex flips Y), which is orientation-reversing:
// a triangle wound CCW seen from the face normal (the mesher/GL convention, kept by
// glFrontFace(GL_CCW)) lands CLOCKWISE on screen, i.e. with negative screen-space
// cross. So on screen, CCW (cross >= 0) = backface. (Fix 2026-10-11: this was
// `cross <= 0`, culling every front face.)
static bool isBackface(const float* v0, const float* v1, const float* v2) {
  float cross = (v1[0]-v0[0])*(v2[1]-v0[1]) - (v1[1]-v0[1])*(v2[0]-v0[0]);
  return cross >= 0;  // CCW on the y-down screen = backface
}

// Model dithering (JD 2026-10-13): per-triangle classification against the
// fade band. A fragment's view depth is the perspective-correct interpolation
// of the vertices' clip w, which lies within [min w, max w] of the triangle,
// so: 1 = every fragment at/past far (keep all, zero per-pixel cost),
// 0 = every fragment at/before near (drop the whole triangle — fully faded
// geometry is not rasterized at all), 2 = straddling (per-pixel test).
static int ditherTriClass(bool on, float near, float far, const float* v0,
                          const float* v1, const float* v2) {
  if (!on) return 1;
  const float mn = std::min({v0[3], v1[3], v2[3]});
  const float mx = std::max({v0[3], v1[3], v2[3]});
  if (mn >= far) return 1;
  if (mx <= near) return 0;
  return 2;
}

void SoftwareRenderer::rasterizeTriangle(const float* v0, const float* v1,
                                         const float* v2, uint32_t color) {
  if (isBackface(v0, v1, v2)) return;  // cull
  const int ditherCls = ditherTriClass(ditherEnabled_, ditherNear_, ditherFar_, v0, v1, v2);
  if (ditherCls == 0) return;  // fully faded: nothing to draw, no depth write

  // Bounding box (clamped)
  int x0 = std::max(0, int(std::floor(std::min({v0[0], v1[0], v2[0]}))));
  int x1 = std::min(int(fb_.width)-1, int(std::ceil(std::max({v0[0], v1[0], v2[0]}))));
  int y0 = std::max(0, int(std::floor(std::min({v0[1], v1[1], v2[1]}))));
  int y1 = std::min(int(fb_.height)-1, int(std::ceil(std::max({v0[1], v1[1], v2[1]}))));

  // Edge function setup
  auto edgeFn = [](const float* a, const float* b, float x, float y) {
    return (b[0]-a[0])*(y-a[1]) - (b[1]-a[1])*(x-a[0]);
  };
  const float area = edgeFn(v0, v1, v2[0], v2[1]);
  if (std::abs(area) < 1e-9f) return;

  // Scanline rasterization with depth test
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      float px = x + 0.5f, py = y + 0.5f;
      float w0 = edgeFn(v1, v2, px, py) / area;
      float w1 = edgeFn(v2, v0, px, py) / area;
      float w2 = edgeFn(v0, v1, px, py) / area;
      if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
        float depth = w0*v0[2] + w1*v1[2] + w2*v2[2];
        size_t idx = size_t(y) * fb_.width + x;
        if (depth < depth_[idx]) {
          if (ditherCls == 2) {
            const float viewDepth = 1.0f / (w0 / v0[3] + w1 / v1[3] + w2 / v2[3]);
            if (!modelDitherKeep(x, y, viewDepth, ditherNear_, ditherFar_)) continue;
          }
          depth_[idx] = depth;
          fb_.pixels[idx] = color;
        }
      }
    }
  }
}

// Face colors: flat shading by face direction (top brightest, bottom darkest).
//
// FLAT-MODE MODE MAP (P3-3, Appendix D L3 — exact, do not over-read):
// the flat path collapses PackedVertex shading modes onto two cases:
//   - kShadeStripTint0 / kShadeStripTint1 (modes 3 / 4): a reddish tint of
//     the value's luminance (parity detail from the shader is NOT
//     reproduced);
//   - EVERY other mode (kShadeTex, kShadePalette16/32/64, kShadePips,
//     kShadeCells, kShadeAnimated, kShadeGlass, ...): plain grayscale
//     from `value` — palette indices, pip/cell counts and animation
//     indices are NOT resolved to their shader colors here.
// Consequence: flat-mode consumers (MCP-vision captures are flat by
// design) see strip-tinted or grayscale geometry, never the palette /
// mask colors the per-pixel path (or GL) shows. That is the documented
// simplification of this path, not a bug — read flat captures as
// geometry/luminance, not as color truth.
static uint32_t faceColor(uint8_t face, uint8_t value, uint8_t mode) {
  // Base color from mode/value (simplified: use value as brightness)
  uint8_t b = value;
  // Face shading: top=1.0, sides=0.7, bottom=0.4
  float shade = 0.7f;
  if (face == 2) shade = 1.0f;      // top
  else if (face == 3) shade = 0.4f; // bottom
  uint8_t v = uint8_t(b * shade);
  // Trace colors: mode determines red/blue tint
  if (mode == 3 || mode == 4) {  // kShadeStripTint0/1
    // Red/blue based on value parity (simplified)
    return 0xFF000000 | (v << 16) | (v >> 1);  // reddish
  }
  return 0xFF000000 | (v << 16) | (v << 8) | v;  // grayscale
}

/// Per-pixel rasterization with shader (slower, higher quality).
/// `groundFx` marks a kShadeGroundPulse quad of the mesh renderFrame is
/// currently drawing: the wafer ground pulse + ground traces emissives
/// (CPU mirrors) add onto the shaded color, mirroring chunk_shading.glsl.
void SoftwareRenderer::rasterizeTriangleShaded(const float* v0, const float* v1,
                                              const float* v2, const ShaderVaryings* s0,
                                              const ShaderVaryings* s1, const ShaderVaryings* s2,
                                              FragmentShaderFn shader, bool groundFx) {
  if (isBackface(v0, v1, v2)) return;
  const int ditherCls = ditherTriClass(ditherEnabled_, ditherNear_, ditherFar_, v0, v1, v2);
  if (ditherCls == 0) return;  // fully faded (JD 2026-10-13): skip shading entirely
  int x0 = std::max(0, int(std::floor(std::min({v0[0], v1[0], v2[0]}))));
  int x1 = std::min(int(fb_.width)-1, int(std::ceil(std::max({v0[0], v1[0], v2[0]}))));
  int y0 = std::max(0, int(std::floor(std::min({v0[1], v1[1], v2[1]}))));
  int y1 = std::min(int(fb_.height)-1, int(std::ceil(std::max({v0[1], v1[1], v2[1]}))));
  auto edgeFn = [](const float* a, const float* b, float x, float y) {
    return (b[0]-a[0])*(y-a[1]) - (b[1]-a[1])*(x-a[0]);
  };
  const float area = edgeFn(v0, v1, v2[0], v2[1]);
  if (std::abs(area) < 1e-9f) return;

  FragmentInput fin{};  // value-init: unset map pointers are null, not indeterminate
  fin.texData = textures_.data(); fin.texLayers = textures_.layers();
  fin.pbr = effectivePbrContext();
  const bool fx = groundFx && groundFx_.active();

  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      float px = x + 0.5f, py = y + 0.5f;
      float w0 = edgeFn(v1, v2, px, py) / area;
      float w1 = edgeFn(v2, v0, px, py) / area;
      float w2 = edgeFn(v0, v1, px, py) / area;
      if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
        float depth = w0*v0[2] + w1*v1[2] + w2*v2[2];
        size_t idx = size_t(y) * fb_.width + x;
        if (depth < depth_[idx]) {
          if (ditherCls == 2) {
            // Screen-door discard BEFORE shading: faded fragments cost no
            // fragment work and write no depth (true discard semantics).
            const float viewDepth = 1.0f / (w0 / v0[3] + w1 / v1[3] + w2 / v2[3]);
            if (!modelDitherKeep(x, y, viewDepth, ditherNear_, ditherFar_)) continue;
          }
          // Interpolate varyings
          fin.varyings.nx = w0*s0->nx + w1*s1->nx + w2*s2->nx;
          fin.varyings.ny = w0*s0->ny + w1*s1->ny + w2*s2->ny;
          fin.varyings.nz = w0*s0->nz + w1*s1->nz + w2*s2->nz;
          fin.varyings.u = w0*s0->u + w1*s1->u + w2*s2->u;
          fin.varyings.v = w0*s0->v + w1*s1->v + w2*s2->v;
          fin.varyings.layer = s0->layer;
          fin.varyings.materialId = s0->materialId;  // flat per quad, like layer
          fin.varyings.wx = w0*s0->wx + w1*s1->wx + w2*s2->wx;
          fin.varyings.wy = w0*s0->wy + w1*s1->wy + w2*s2->wy;
          fin.varyings.wz = w0*s0->wz + w1*s1->wz + w2*s2->wz;
          uint32_t color = shader(fin);
          if (fx) {
            // Wafer ground pulse + traces (P1-RENDER H1): additive emissive
            // at the fragment's world position, from the mesh's baked
            // params — the CPU mirrors of the GLSL in chunk_shading.glsl.
            const float wx = fin.varyings.wx + meshOriginX_;
            const float wz = fin.varyings.wz + meshOriginZ_;
            float er = 0.f, eg = 0.f, eb = 0.f;
            if (groundFx_.pulseOn && groundFx_.pulseP0[2] > 0.f) {
              const GroundPulseParams pp{groundFx_.pulseP0[0], groundFx_.pulseP0[1],
                                         groundFx_.pulseP0[2], groundFx_.pulseP1[0],
                                         groundFx_.pulseP1[1], groundFx_.pulseP1[2]};
              const int ccx = int(std::floor(wx / 16.f)), ccz = int(std::floor(wz / 16.f));
              const auto e = groundPulseEmissive(groundFx_.pulseTimeSec, ccx, ccz, pp);
              er += e[0]; eg += e[1]; eb += e[2];
            }
            if (groundFx_.traceOn && groundFx_.traceP0[2] > 0.f && groundFx_.traceP0[0] > 0.f) {
              const GroundTraceParams tp{groundFx_.traceP0[0], groundFx_.traceP0[1],
                                         groundFx_.traceP0[2], groundFx_.traceP0[3],
                                         groundFx_.traceP1[0], groundFx_.traceP1[1],
                                         groundFx_.traceP1[2]};
              const auto e = groundTraceEmissive(groundFx_.traceTimeSec, wx, wz, tp);
              er += e[0]; eg += e[1]; eb += e[2];
            }
            if (er > 0.f || eg > 0.f || eb > 0.f) {
              auto add = [](uint32_t ch, float e) {
                return uint32_t(std::min(255.f, float(ch) + e * 255.f));
              };
              color = 0xFF000000u | (add((color >> 16) & 0xFF, er) << 16) |
                      (add((color >> 8) & 0xFF, eg) << 8) | add(color & 0xFF, eb);
            }
          }
          depth_[idx] = depth;
          fb_.pixels[idx] = color;
        }
      }
    }
  }
}

// Build software-vertex-stage varyings from a packed mesh vertex.
// Face normals match the GL chunk shader (chunk.vert N[6]).
static void varyingsFromPackedVertex(const PackedVertex& v, ShaderVaryings& s) {
  static const float kFaceNormals[6][3] = {
      {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  const uint8_t face = v.faceAo & 7;
  s.nx = kFaceNormals[face][0];
  s.ny = kFaceNormals[face][1];
  s.nz = kFaceNormals[face][2];
  s.u = v.u / 16.0f;
  s.v = v.v / 16.0f;
  s.layer = v.layer;
  s.wx = v.x / 16.0f;
  s.wy = v.y / 16.0f;
  s.wz = v.z / 16.0f;
  // SEAM (JD-locked coexistence, renderer.h): PackedVertex is 16 bytes fully
  // packed and NEVER extended - it carries no material slot by design, so the
  // classic path's fragments resolve to the 0xFFFF "none" marker and the PBR
  // tier falls back to basic shading per fragment. Real per-vertex material
  // IDs exist on the BinaryQuad path only (PbrQuadVertex::material) and reach
  // the varyings through renderPbrMeshes (P1-RENDER H2).
  s.materialId = 0xFFFF;
}

// Transform all vertices to screen space (x, y, depth, clipW per vertex) in
// the reusable screen_ scratch; returns the scratch pointer.
const float* SoftwareRenderer::transformVerts(const PackedVertex* verts, size_t count,
                                              const float* viewProj) {
  screen_.resize(count * 4);
  float* screen = screen_.data();
  for (size_t i = 0; i < count; ++i) {
    transformVertex(verts[i], viewProj, &screen[i*4], fb_.width, fb_.height);
  }
  return screen;
}

void SoftwareRenderer::renderFlatQuads(const PackedVertex* verts, size_t count,
                                       const float* viewProj) {
  if (!verts || count < 4 || !viewProj) return;
  const float* screen = transformVerts(verts, count, viewProj);
  // Reject quads with any vertex behind the camera (w <= 0 mirrors projection).
  auto quadVisible = [&](size_t i) {
    return screen[i*4+3] > 0 && screen[(i+1)*4+3] > 0 &&
           screen[(i+2)*4+3] > 0 && screen[(i+3)*4+3] > 0;
  };
  for (size_t i = 0; i + 3 < count; i += 4) {
    if (!quadVisible(i)) continue;
    uint32_t color = faceColor(verts[i].faceAo & 0x7, verts[i].value, verts[i].mode);
    const float* v0 = &screen[i*4];
    const float* v1 = &screen[(i+1)*4];
    const float* v2 = &screen[(i+2)*4];
    const float* v3 = &screen[(i+3)*4];
    rasterizeTriangle(v0, v1, v2, color);
    rasterizeTriangle(v0, v2, v3, color);
  }
}

void SoftwareRenderer::renderMesh(const PackedVertex* verts, size_t vertCount,
                                  const float* viewProj) {
  if (!verts || vertCount < 3 || !viewProj) return;

  // Flat path (also the sprite pass path).
  if (!perPixel_) {
    renderFlatQuads(verts, vertCount, viewProj);
    return;
  }

  // Per-pixel path: build varyings from the packed vertices and run the
  // tier-selected fragment shader (Basic = existing basicFragmentShader,
  // Pbr = pbrFragmentShader with graceful fallback per fragment).
  const float* screen = transformVerts(verts, vertCount, viewProj);
  // Reject quads with any vertex behind the camera (w <= 0 mirrors projection).
  auto quadVisible = [&](size_t i) {
    return screen[i*4+3] > 0 && screen[(i+1)*4+3] > 0 &&
           screen[(i+2)*4+3] > 0 && screen[(i+3)*4+3] > 0;
  };
  const FragmentShaderFn shader = selectFragmentShader(shadingTier_);
  ShaderVaryings sv[4];
  for (size_t i = 0; i + 3 < vertCount; i += 4) {
    if (!quadVisible(i)) continue;
    for (int k = 0; k < 4; ++k) varyingsFromPackedVertex(verts[i + k], sv[k]);
    // Ground pulse/traces tag: only kShadeGroundPulse quads of a mesh whose
    // baked params renderFrame supplied (direct renderMesh callers leave
    // groundFx_ zeroed, so their frames are unchanged).
    const bool fx = verts[i].mode == kShadeGroundPulse;
    rasterizeTriangleShaded(&screen[i*4], &screen[(i+1)*4], &screen[(i+2)*4],
                            &sv[0], &sv[1], &sv[2], shader, fx);
    rasterizeTriangleShaded(&screen[i*4], &screen[(i+2)*4], &screen[(i+3)*4],
                            &sv[0], &sv[2], &sv[3], shader, fx);
  }
}

void SoftwareRenderer::renderEntityVerts(const float* viewProj) {
  const PackedVertex* verts = entityVerts_.data();
  const size_t count = entityVerts_.size();
  if (count < 3 || !viewProj) return;
  const float* screen = transformVerts(verts, count, viewProj);
  auto triVisible = [&](size_t i) {
    return screen[i*4+3] > 0 && screen[(i+1)*4+3] > 0 && screen[(i+2)*4+3] > 0;
  };
  if (perPixel_) {
    const FragmentShaderFn shader = selectFragmentShader(shadingTier_);
    ShaderVaryings sv[3];
    for (size_t i = 0; i + 2 < count; i += 3) {
      if (!triVisible(i)) continue;
      for (int k = 0; k < 3; ++k) varyingsFromPackedVertex(verts[i + k], sv[k]);
      rasterizeTriangleShaded(&screen[i*4], &screen[(i+1)*4], &screen[(i+2)*4],
                              &sv[0], &sv[1], &sv[2], shader);
    }
    return;
  }
  for (size_t i = 0; i + 2 < count; i += 3) {
    if (!triVisible(i)) continue;
    const uint32_t color = faceColor(verts[i].faceAo & 0x7, verts[i].value, verts[i].mode);
    rasterizeTriangle(&screen[i*4], &screen[(i+1)*4], &screen[(i+2)*4], color);
  }
}

void SoftwareRenderer::setTextures(const uint32_t* data, uint32_t layers) {
  textures_.setRaw(data, layers);
}

void SoftwareRenderer::applyFXAA() {
  // FXAA-lite (P3-3, Appendix D L2): a 3x3 box blend applied ONLY at
  // high-contrast edges — a pixel blends toward its neighbourhood average
  // when the neighbourhood's luminance range exceeds kEdgeThreshold;
  // flat regions pass through untouched. (The v1 code blurred EVERYTHING
  // despite the "edges only" comment — dead-ish and misnamed; this gate is
  // what the name claims. It is still not a full FXAA shader: no luma
  // subpixel/edge-direction search. No production caller yet — the direct
  // CPU surface and tests drive it.)
  // fxaaSrc_ is a reusable member buffer (no per-call full-framebuffer copy).
  constexpr uint32_t kEdgeThreshold = 48;  // luminance range (0..255) that counts as an edge
  auto lum = [](uint32_t p) {
    return (77u * ((p >> 16) & 0xFF) + 150u * ((p >> 8) & 0xFF) + 29u * (p & 0xFF)) >> 8;
  };
  fxaaSrc_ = fb_.pixels;
  const std::vector<uint32_t>& src = fxaaSrc_;
  uint32_t w = fb_.width, h = fb_.height;
  for (uint32_t y = 1; y + 1 < h; ++y) {
    for (uint32_t x = 1; x + 1 < w; ++x) {
      uint32_t r = 0, g = 0, b = 0, lo = 255, hi = 0;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          uint32_t p = src[size_t(y+dy) * w + (x+dx)];
          r += (p >> 16) & 0xFF; g += (p >> 8) & 0xFF; b += p & 0xFF;
          const uint32_t l = lum(p);
          lo = std::min(lo, l);
          hi = std::max(hi, l);
        }
      }
      if (hi - lo < kEdgeThreshold) continue;  // not an edge: keep the source pixel
      fb_.pixels[size_t(y) * w + x] = 0xFF000000 | ((r/9) << 16) | ((g/9) << 8) | (b/9);
    }
  }
}

// ---------------------------------------------------------------------------
// IRenderer interface
// ---------------------------------------------------------------------------

std::string SoftwareRenderer::deviceInfo() const {
  return "software rasterizer (CPU, no GPU)";
}

void SoftwareRenderer::setMaterials(const BlockMaterials& /*materials*/) {
  // Documented no-op: nothing on the software path consumes BlockMaterials
  // (see the header). PBR material data arrives via setPbrContext().
}

int SoftwareRenderer::loadShaders(const ShaderLibrary& /*shaders*/) {
  return 0;  // no GPU programs; no program can use pack sources
}

// Mesh key identical to the GL/Vulkan backends.
static int64_t swMeshKey(int cx, int sy, int cz) {
  return int64_t((uint64_t(uint32_t(cx)) << 40) ^ (uint64_t(uint32_t(sy)) << 32) ^
                 uint64_t(uint32_t(cz)));
}

void SoftwareRenderer::uploadChunkMesh(const ChunkMesh& mesh) {
  const int64_t key = swMeshKey(mesh.cx, mesh.sy, mesh.cz);
  if (mesh.verts.empty()) {  // empty = remove, like the other backends
    meshes_.erase(key);
    return;
  }
  StoredMesh& s = meshes_[key];
  s.cx = mesh.cx;
  s.sy = mesh.sy;
  s.cz = mesh.cz;
  s.verts = mesh.verts;
  for (int i = 0; i < 4; ++i) {
    s.pulseP0[i] = mesh.pulseP0[i];
    s.pulseP1[i] = mesh.pulseP1[i];
    s.traceP0[i] = mesh.traceP0[i];
    s.traceP1[i] = mesh.traceP1[i];
  }
}

// --- PBR / culling surface (P1-RENDER H2): real implementations, no inherited
// no-ops left on the paths the game calls. -----------------------------------

void SoftwareRenderer::setPbrMaterials(const MaterialRegistry& reg) {
  pbrMaterials_ = &reg;
  ownedPbrCtx_.registry = &reg;
  ownedPbrCtx_.ambientMode = ambientMode_;
  ownedPbrCtx_.emissive = &frameLights_;  // FrameParams::pbrLights, fed per frame
}

void SoftwareRenderer::setPbrTier(PbrTransparencyTier t) {
  pbrTier_ = t;
  // Selecting a PBR transparency tier is the user's PBR opt-in: run the PBR
  // fragment tier on the per-pixel path. Classic PackedVertex fragments carry
  // no material ID (0xFFFF) and fall back to basic per fragment, so classic
  // frames are pixel-identical; BinaryQuad fragments shade real PBR.
  shadingTier_ = SoftwareShadingTier::Pbr;
  // Honest limit (pbr_software_shaders.h ULTRA TIER GAP): the CPU path does
  // not implement the transparency-tier ladder — PBR fragments shade
  // opaque regardless of tier. Loud once, not silent.
  static bool warnedTier = false;
  if (!warnedTier) {
    warnedTier = true;
    logWarn("software: PBR transparency tiers are not implemented on the CPU path; "
            "PBR fragments shade opaque (tier selection still enables the PBR fragment path)");
  }
}

void SoftwareRenderer::setAmbientMode(AmbientMode m) {
  ambientMode_ = m;
  ownedPbrCtx_.ambientMode = m;
}

void SoftwareRenderer::setFrustumMarginDeg(float degrees) {
  frustumMarginDeg_ = std::clamp(degrees, 0.f, 15.f);  // JD-locked 2026-10-11, as GL/VK
}

void SoftwareRenderer::setModelDither(bool enabled, float nearBlocks, float farBlocks) {
  ditherEnabled_ = enabled;
  ditherNear_ = std::clamp(nearBlocks, 0.f, 1024.f);
  ditherFar_ = std::clamp(farBlocks, ditherNear_, 2048.f);
}

// ---------------------------------------------------------------------------
// AA ladder + ASCII pipeline (JD 2026-10-13). Both are presentational
// stages on the finished frame; neither touches sim or mesh state.
// ---------------------------------------------------------------------------
SoftwareAaScratch& SoftwareRenderer::ensureAaScratch() {
  if (!aaScratch_) aaScratch_ = std::make_unique<SoftwareAaScratch>();
  return *aaScratch_;
}

void SoftwareRenderer::setAaMode(const char* modeName) {
  SoftwareAaMode mode;
  if (modeName && softwareAaModeFromName(modeName, mode)) aaMode_ = mode;
}

const char* SoftwareRenderer::aaModeName() const { return softwareAaModeName(aaMode_); }

void SoftwareRenderer::applyAaPostPass() {
  if (aaMode_ == SoftwareAaMode::Fxaa) {
    applyFxaaPost(fb_, ensureAaScratch());
  } else if (aaMode_ == SoftwareAaMode::Cmaa) {
    // depth_ matches fb_ on the direct surface (resize keeps them paired);
    // pass it only when the pairing holds, else run luma-only.
    const float* depth =
        (depth_.size() == fb_.pixels.size()) ? depth_.data() : nullptr;
    applyCmaaPost(fb_, depth, ensureAaScratch());
  }
  // Off: no-op. Ssaa2x: render-time mode — nothing to post-apply.
}

void SoftwareRenderer::setAsciiMode(bool enabled, int cols) {
  asciiEnabled_ = enabled;
  asciiCols_ = std::clamp(cols, 40, 400);
}

void SoftwareRenderer::setAsciiGlyphSet(const char* setName) {
  AsciiGlyphSet set;
  if (setName && asciiGlyphSetFromName(setName, set)) asciiSet_ = static_cast<int>(set);
}

void SoftwareRenderer::setAsciiValueSpan(int span) {
  asciiSpan_ = std::clamp(span, 1, kAsciiMaxValueSpan);  // JD amendment 2026-10-13
}

void SoftwareRenderer::applyAsciiPass() {
  framebufferToAsciiImage(fb_, asciiCols_, static_cast<AsciiGlyphSet>(asciiSet_),
                          &asciiText_, nullptr, asciiSpan_);
}

void SoftwareRenderer::setCullProtoConfig(const CullProtoConfig& cfg) {
  cullProto_.configure(cfg);
}

CullProtoStats SoftwareRenderer::cullProtoStats() const { return cullProto_.stats(); }

void SoftwareRenderer::uploadPbrChunkMesh(const PbrChunkMesh& mesh) {
  const int64_t key = swMeshKey(mesh.cx, mesh.sy, mesh.cz);
  if (mesh.verts.empty()) {  // empty = remove, like the classic store
    pbrMeshes_.erase(key);
    return;
  }
  pbrMeshes_[key] = mesh;
}

void SoftwareRenderer::uploadEntityMesh(const std::vector<PackedVertex>& verts) {
  entityVerts_ = verts;  // dynamic: each upload replaces the previous batch
}

void SoftwareRenderer::clearChunkMeshes() {
  meshes_.clear();  // entity verts kept, matching GL/Vulkan clear semantics
  pbrMeshes_.clear();
}

static Mat4 swTranslation(float x, float y, float z) {
  Mat4 t = Mat4::identity();
  t.m[12] = x;
  t.m[13] = y;
  t.m[14] = z;
  return t;
}

// FrameParams sky (0..1 floats) as the framebuffer's 0xAARRGGBB clear color:
// sky {0.58, 0.70, 0.86} -> 0xFF94B3DB, the wafer day sky the MCP capture
// path clears with.
static uint32_t swSkyColor(const Vec3& sky) {
  auto ch = [](float c) {
    return uint32_t(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f));
  };
  return 0xFF000000u | (ch(sky.x) << 16) | (ch(sky.y) << 8) | ch(sky.z);
}

void SoftwareRenderer::rasterizeBlendSprite(const BlendSprite& s) {
  // Two triangles (0,1,2) + (0,2,3), barycentric like rasterizeTriangle,
  // with backface culling, a depth TEST against the world depth buffer,
  // and NO depth write: a nearer blended sprite must not occlude a farther
  // one drawn after it (producer order is back-to-front).
  const int tris[2][3] = {{0, 1, 2}, {0, 2, 3}};
  for (const auto& t : tris) {
    const float* v0 = s.v[t[0]];
    const float* v1 = s.v[t[1]];
    const float* v2 = s.v[t[2]];
    if (isBackface(v0, v1, v2)) continue;
    const int x0 = std::max(0, int(std::floor(std::min({v0[0], v1[0], v2[0]}))));
    const int x1 = std::min(int(fb_.width) - 1, int(std::ceil(std::max({v0[0], v1[0], v2[0]}))));
    const int y0 = std::max(0, int(std::floor(std::min({v0[1], v1[1], v2[1]}))));
    const int y1 = std::min(int(fb_.height) - 1, int(std::ceil(std::max({v0[1], v1[1], v2[1]}))));
    auto edgeFn = [](const float* a, const float* b, float x, float y) {
      return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
    };
    const float area = edgeFn(v0, v1, v2[0], v2[1]);
    if (std::abs(area) < 1e-9f) continue;
    for (int y = y0; y <= y1; ++y) {
      for (int x = x0; x <= x1; ++x) {
        const float px = x + 0.5f, py = y + 0.5f;
        const float w0 = edgeFn(v1, v2, px, py) / area;
        const float w1 = edgeFn(v2, v0, px, py) / area;
        const float w2 = edgeFn(v0, v1, px, py) / area;
        if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
        const float depth = w0 * v0[2] + w1 * v1[2] + w2 * v2[2];
        const size_t idx = size_t(y) * fb_.width + size_t(x);
        if (!(depth < depth_[idx])) continue;  // test only; depth_ untouched
        const uint32_t dst = fb_.pixels[idx];
        const float dr = float((dst >> 16) & 0xFF), dg = float((dst >> 8) & 0xFF),
                    db = float(dst & 0xFF);
        float r, g, b;
        if (s.additive) {
          r = dr + s.r * s.a * 255.f;
          g = dg + s.g * s.a * 255.f;
          b = db + s.b * s.a * 255.f;
        } else {
          r = dr + (s.r * 255.f - dr) * s.a;
          g = dg + (s.g * 255.f - dg) * s.a;
          b = db + (s.b * 255.f - db) * s.a;
        }
        auto ch = [](float v) { return uint32_t(std::clamp(v, 0.f, 255.f) + 0.5f); };
        fb_.pixels[idx] = 0xFF000000u | (ch(r) << 16) | (ch(g) << 8) | ch(b);
      }
    }
  }
}

void SoftwareRenderer::drawSprites(const FrameParams& p) {
  if (!p.spriteQuads || p.spriteQuadCount == 0) return;
  // The game sprite host's software conversion (sprite_host.cpp), mirrored:
  // each SpriteDrawQuad becomes 4 PackedVerts relative to a 16-aligned
  // origin 1024 blocks below the camera on each axis (so live sprites land
  // inside the uint16 local range on the positive side; quads outside are
  // skipped), shaded flat with the luminance of the sprite's atlas-average
  // color x tint on face 2 (faceColor's unshaded face). The host averages
  // the def's frame-0 atlas rect; here the average is taken over the quad's
  // own uv rect on its atlas page — the same region for frame 0, and the
  // drawn frame's region for later animation frames.
  const Vec3 origin{std::floor((p.camPos.x - 1024.f) / 16.f) * 16.f,
                    std::floor((p.camPos.y - 1024.f) / 16.f) * 16.f,
                    std::floor((p.camPos.z - 1024.f) / 16.f) * 16.f};
  // Average-color cache (D-M2): keyed by (atlas version, page, pixel
  // rect); a version move clears it wholesale. The average itself is
  // summed directly over the page rect — no per-quad temp Image/vector.
  if (spriteAvgVersion_ != p.spriteAtlasVersion) {
    spriteAvgCache_.clear();
    spriteAvgVersion_ = p.spriteAtlasVersion;
  }
  auto avgColorOf = [&](const SpriteDrawQuad& q) -> std::array<float, 4> {
    std::array<float, 4> avg{1.f, 1.f, 1.f, 1.f};  // fallback: white (as the host)
    if (q.page < 0 || size_t(q.page) >= p.spriteAtlasPages.size()) return avg;
    const Image& page = p.spriteAtlasPages[size_t(q.page)];
    if (page.empty() || page.width() <= 0 || page.height() <= 0) return avg;
    float u0 = q.uv[0][0], v0 = q.uv[0][1], u1 = u0, v1 = v0;
    for (int i = 1; i < 4; ++i) {
      u0 = std::min(u0, q.uv[i][0]);
      v0 = std::min(v0, q.uv[i][1]);
      u1 = std::max(u1, q.uv[i][0]);
      v1 = std::max(v1, q.uv[i][1]);
    }
    const int x0 = std::clamp(int(std::lround(u0 * float(page.width()))), 0, page.width());
    const int y0 = std::clamp(int(std::lround(v0 * float(page.height()))), 0, page.height());
    const int x1 = std::clamp(int(std::lround(u1 * float(page.width()))), 0, page.width());
    const int y1 = std::clamp(int(std::lround(v1 * float(page.height()))), 0, page.height());
    if (x1 <= x0 || y1 <= y0) return avg;
    const SpriteAvgKey key{p.spriteAtlasVersion, q.page, x0, y0, x1, y1};
    if (const auto it = spriteAvgCache_.find(key); it != spriteAvgCache_.end()) return it->second;
    // Same weighting as Image::averageColor (alpha-weighted RGB), plus the
    // plain mean alpha the blend modes need.
    double s[3] = {0, 0, 0}, wsum = 0, asum = 0;
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) {
        const uint8_t* px = page.pixel(x, y);
        const double a = px[3] / 255.0;
        for (int c = 0; c < 3; ++c) s[c] += a * px[c] / 255.0;
        wsum += a;
        asum += a;
      }
    if (wsum > 0) {
      const double n = double(x1 - x0) * double(y1 - y0);
      avg = {float(s[0] / wsum), float(s[1] / wsum), float(s[2] / wsum), float(asum / n)};
    }
    spriteAvgCache_.emplace(key, avg);
    return avg;
  };
  spriteScratch_.clear();
  spriteScratch_.reserve(p.spriteQuadCount * 4);
  spriteBlendScratch_.clear();
  const Mat4 mvp = mul(p.viewProj, swTranslation(origin.x, origin.y, origin.z));
  for (size_t qi = 0; qi < p.spriteQuadCount; ++qi) {
    const SpriteDrawQuad& q = p.spriteQuads[qi];
    const std::array<float, 4> avg = avgColorOf(q);
    PackedVertex pv[4];
    bool inRange = true;
    for (int i = 0; i < 4; ++i) {
      const float lx = (q.corners[i].x - origin.x) * 16.f;
      const float ly = (q.corners[i].y - origin.y) * 16.f;
      const float lz = (q.corners[i].z - origin.z) * 16.f;
      if (lx < 0.f || ly < 0.f || lz < 0.f || lx > 65535.f || ly > 65535.f || lz > 65535.f)
        inRange = false;
      pv[i].x = uint16_t(std::clamp(lx, 0.f, 65535.f));
      pv[i].y = uint16_t(std::clamp(ly, 0.f, 65535.f));
      pv[i].z = uint16_t(std::clamp(lz, 0.f, 65535.f));
      pv[i].u = 0;
      pv[i].v = 0;
      pv[i].faceAo = 2;  // face 2 = top: faceColor's unshaded (1.0) face
      pv[i].layer = 0;
      pv[i].mode = kShadeTex;
      pv[i].slot = 0;
    }
    if (!inRange) continue;
    const bool blended = q.shade == kShadeAlphaBlend || q.shade == kShadeAdditive;
    if (!blended) {
      const float lum = std::clamp(0.2126f * avg[0] * q.r + 0.7152f * avg[1] * q.g +
                                       0.0722f * avg[2] * q.b,
                                   0.f, 1.f);
      for (PackedVertex& v : pv) v.value = uint8_t(std::lround(lum * 255.f));
      for (const PackedVertex& v : pv) spriteScratch_.push_back(v);
    } else {
      // Blended: keep the quad's TRUE average color x tint (the flat
      // luminance collapse is the opaque path's documented form; blending
      // a gray would lose the sprite's hue for no benefit).
      BlendSprite bs;
      bs.r = std::clamp(avg[0] * q.r, 0.f, 1.f);
      bs.g = std::clamp(avg[1] * q.g, 0.f, 1.f);
      bs.b = std::clamp(avg[2] * q.b, 0.f, 1.f);
      bs.a = std::clamp(avg[3] * q.a, 0.f, 1.f);
      bs.additive = q.shade == kShadeAdditive;
      if (ditherEnabled_) {
        // Model dithering for blended sprites (JD 2026-10-13): fade the
        // whole billboard by its NEAREST corner's distance to the camera —
        // per-sprite, not per-pixel (the opaque paths dither per-pixel in
        // the rasterizers; a translucent quad reads better as a fade).
        float dmin = 1e30f;
        for (int i = 0; i < 4; ++i) {
          const float dx = q.corners[i].x - p.camPos.x;
          const float dy = q.corners[i].y - p.camPos.y;
          const float dz = q.corners[i].z - p.camPos.z;
          dmin = std::min(dmin, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        bs.a *= modelDitherFade(dmin, ditherNear_, ditherFar_);
        if (bs.a <= 0.f) continue;  // fully faded: skip the quad entirely
      }
      const float* screen = transformVerts(pv, 4, mvp.m);
      bool visible = true;
      for (int i = 0; i < 4; ++i) {
        if (screen[i * 4 + 3] <= 0) visible = false;
        for (int k = 0; k < 4; ++k) bs.v[i][k] = screen[i * 4 + k];
      }
      if (visible) spriteBlendScratch_.push_back(bs);
    }
  }
  // Producer order is cutout-first, then blended back-to-front: drawing
  // the opaque batch first and the blended quads in order reproduces the
  // FrameParams contract exactly.
  if (!spriteScratch_.empty()) renderFlatQuads(spriteScratch_.data(), spriteScratch_.size(), mvp.m);
  for (const BlendSprite& bs : spriteBlendScratch_) rasterizeBlendSprite(bs);
}

void SoftwareRenderer::renderPbrMeshes(const FrameParams& p) {
  // BinaryQuad (PBR) meshes: positions convert to PackedVertex-shaped verts
  // for the shared transform; the varyings carry the REAL per-vertex compact
  // material ID (PbrQuadVertex::material) — the fix for the classic path's
  // 0xFFFF seam (H2). Shading is pbrFragmentShader through the effective
  // context (external setPbrContext, else the owned setPbrMaterials one);
  // with neither, it falls back to basic per fragment, never black.
  frameLights_.frame = &p;
  if (pbrMaterials_) {
    ownedPbrCtx_.camX = p.camPos.x;
    ownedPbrCtx_.camY = p.camPos.y;
    ownedPbrCtx_.camZ = p.camPos.z;
  }
  for (const auto& [key, m] : pbrMeshes_) {
    (void)key;
    const Vec3 mn{m.cx * 16.f, m.sy * 16.f, m.cz * 16.f};
    if (cullProto_.decide(p.viewProj, mn, mn + Vec3{16, 16, 16}, p.camPos,
                          frustumMarginDeg_) != CullDecision::Full)
      continue;
    const Mat4 mvp = mul(p.viewProj, swTranslation(mn.x, mn.y, mn.z));
    pbrScratch_.clear();
    pbrScratchMat_.clear();
    pbrScratch_.reserve(m.verts.size());
    pbrScratchMat_.reserve(m.verts.size());
    for (const PbrQuadVertex& v : m.verts) {
      PackedVertex pv{};
      pv.x = v.x; pv.y = v.y; pv.z = v.z;
      pv.faceAo = uint8_t(v.face & 7);
      pbrScratch_.push_back(pv);
      pbrScratchMat_.push_back(v.material);
    }
    const float* screen = transformVerts(pbrScratch_.data(), pbrScratch_.size(), mvp.m);
    auto quadVisible = [&](size_t i) {
      return screen[i*4+3] > 0 && screen[(i+1)*4+3] > 0 &&
             screen[(i+2)*4+3] > 0 && screen[(i+3)*4+3] > 0;
    };
    static const float kFaceNormals[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    ShaderVaryings sv[4];
    for (size_t i = 0; i + 3 < pbrScratch_.size(); i += 4) {
      if (!quadVisible(i)) continue;
      // Per-quad flood-fill levels ride the owned context (its block/sky
      // fields are context-global; the rasterizer is synchronous, so
      // setting them per quad is exact). External contexts are the
      // caller's own — theirs stay untouched.
      if (!pbrContext_ && pbrMaterials_) {
        ownedPbrCtx_.blockLight = m.verts[i].blockLight;
        ownedPbrCtx_.skyLight = m.verts[i].skyLight;
      }
      for (int k = 0; k < 4; ++k) {
        const PbrQuadVertex& v = m.verts[i + k];
        const uint32_t face = v.face < 6 ? v.face : 2;
        sv[k].nx = kFaceNormals[face][0];
        sv[k].ny = kFaceNormals[face][1];
        sv[k].nz = kFaceNormals[face][2];
        sv[k].u = 0.f; sv[k].v = 0.f; sv[k].layer = 0;
        sv[k].wx = mn.x + v.x / 16.f;  // world position (view dir + lights)
        sv[k].wy = mn.y + v.y / 16.f;
        sv[k].wz = mn.z + v.z / 16.f;
        sv[k].materialId = uint16_t(pbrScratchMat_[i + k] & 0xFFFFu);
      }
      rasterizeTriangleShaded(&screen[i*4], &screen[(i+1)*4], &screen[(i+2)*4],
                              &sv[0], &sv[1], &sv[2], &pbrFragmentShader);
      rasterizeTriangleShaded(&screen[i*4], &screen[(i+2)*4], &screen[(i+3)*4],
                              &sv[0], &sv[2], &sv[3], &pbrFragmentShader);
    }
    ++stats_.drawn;
  }
  frameLights_.frame = nullptr;
}

// ---------------------------------------------------------------------------
// HUD overlay (P1-RENDER H3): CPU rasterizer for buildHudVertices output.
// Mirrors the GL33 hud program: pixel-space triangles, no depth test,
// src-over blending in paint order; u < 0 = solid rect, otherwise the font
// atlas coverage (NEAREST, like GL) multiplies the vertex alpha.
// ---------------------------------------------------------------------------

void SoftwareRenderer::blendPixel(int x, int y, float r, float g, float b, float a) {
  if (x < 0 || y < 0 || x >= int(fb_.width) || y >= int(fb_.height)) return;
  if (a <= 0.f) return;
  uint32_t& dst = fb_.pixels[size_t(y) * fb_.width + size_t(x)];
  if (a >= 1.f) {
    dst = 0xFF000000u | (uint32_t(std::lround(std::clamp(r, 0.f, 1.f) * 255.f)) << 16) |
          (uint32_t(std::lround(std::clamp(g, 0.f, 1.f) * 255.f)) << 8) |
          uint32_t(std::lround(std::clamp(b, 0.f, 1.f) * 255.f));
    return;
  }
  const float ia = 1.f - a;
  const float dr = float((dst >> 16) & 0xFF) / 255.f, dg = float((dst >> 8) & 0xFF) / 255.f,
              db = float(dst & 0xFF) / 255.f;
  dst = 0xFF000000u |
        (uint32_t(std::lround(std::clamp(r * a + dr * ia, 0.f, 1.f) * 255.f)) << 16) |
        (uint32_t(std::lround(std::clamp(g * a + dg * ia, 0.f, 1.f) * 255.f)) << 8) |
        uint32_t(std::lround(std::clamp(b * a + db * ia, 0.f, 1.f) * 255.f));
}

void SoftwareRenderer::rasterizeHudTriangle(const HudVertex& A, const HudVertex& B,
                                            const HudVertex& C) {
  const float area = (B.x - A.x) * (C.y - A.y) - (B.y - A.y) * (C.x - A.x);
  if (std::abs(area) < 1e-9f) return;
  const int x0 = std::max(0, int(std::floor(std::min({A.x, B.x, C.x}))));
  const int x1 = std::min(int(fb_.width) - 1, int(std::ceil(std::max({A.x, B.x, C.x}))));
  const int y0 = std::max(0, int(std::floor(std::min({A.y, B.y, C.y}))));
  const int y1 = std::min(int(fb_.height) - 1, int(std::ceil(std::max({A.y, B.y, C.y}))));
  auto edge = [](const HudVertex& a, const HudVertex& b, float x, float y) {
    return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
  };
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const float px = x + 0.5f, py = y + 0.5f;
      const float w0 = edge(B, C, px, py) / area;
      const float w1 = edge(C, A, px, py) / area;
      const float w2 = edge(A, B, px, py) / area;
      if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;
      const float u = w0 * A.u + w1 * B.u + w2 * C.u;
      const float v = w0 * A.v + w1 * B.v + w2 * C.v;
      const float r = w0 * A.r + w1 * B.r + w2 * C.r;
      const float g = w0 * A.g + w1 * B.g + w2 * C.g;
      const float b = w0 * A.b + w1 * B.b + w2 * C.b;
      float a = w0 * A.a + w1 * B.a + w2 * C.a;
      if (u >= 0.f) {  // glyph: font-atlas coverage multiplies alpha (hud.frag)
        if (!hudAtlasReady_) {
          hudAtlas_ = buildHudFontAtlas();
          hudAtlasReady_ = true;
        }
        const int tx = std::clamp(int(u * float(hudAtlas_.width)), 0, hudAtlas_.width - 1);
        const int ty = std::clamp(int(v * float(hudAtlas_.height)), 0, hudAtlas_.height - 1);
        a *= float(hudAtlas_.pixels[size_t(ty) * size_t(hudAtlas_.width) + size_t(tx)]) / 255.f;
      }
      blendPixel(x, y, r, g, b, a);
    }
  }
}

void SoftwareRenderer::drawHud(const FrameParams& p) {
  if (p.hud.rects.empty() && p.hud.texts.empty() && p.hud.draws.empty()) return;
  buildHudVertices(p.hud, hudVerts_);
  for (size_t i = 0; i + 2 < hudVerts_.size(); i += 3)
    rasterizeHudTriangle(hudVerts_[i], hudVerts_[i + 1], hudVerts_[i + 2]);
}

// World-space line segment, depth-tested against depth_ (highlight outline:
// GL draws it with depth test on). Projected with the frame viewProj;
// segments crossing the near plane are clipped coarsely (endpoint w test —
// the outline is 12 short edges around one block, so a fully-inside test
// per endpoint with 32 subdivisions is ample).
void SoftwareRenderer::drawWorldLine(const FrameParams& p, const Vec3& a, const Vec3& b,
                                     uint32_t color) {
  auto project = [&](const Vec3& v, float out[3]) {
    const float* m = p.viewProj.m;
    const float cx = m[0]*v.x + m[4]*v.y + m[8]*v.z + m[12];
    const float cy = m[1]*v.x + m[5]*v.y + m[9]*v.z + m[13];
    const float cz = m[2]*v.x + m[6]*v.y + m[10]*v.z + m[14];
    const float cw = m[3]*v.x + m[7]*v.y + m[11]*v.z + m[15];
    if (cw <= 1e-6f) return false;
    const float inv = 1.f / cw;
    out[0] = (cx * inv * 0.5f + 0.5f) * float(fb_.width);
    out[1] = (1.f - (cy * inv * 0.5f + 0.5f)) * float(fb_.height);
    out[2] = cz * inv;
    return true;
  };
  constexpr int kSteps = 32;
  float prev[3];
  bool havePrev = project(a, prev);
  for (int i = 1; i <= kSteps; ++i) {
    const float t = float(i) / kSteps;
    const Vec3 v{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    float cur[3];
    const bool ok = project(v, cur);
    if (ok && havePrev) {
      const int x = int(std::lround(cur[0] - 0.5f)), y = int(std::lround(cur[1] - 0.5f));
      if (x >= 0 && y >= 0 && x < int(fb_.width) && y < int(fb_.height)) {
        const size_t idx = size_t(y) * fb_.width + size_t(x);
        if (cur[2] < depth_[idx]) fb_.pixels[idx] = color;
      }
    }
    if (ok) std::memcpy(prev, cur, sizeof prev);
    havePrev = ok;
  }
}

// NDC line segment, no depth test (crosshair: GL draws it on top). NDC y is
// up; the framebuffer is y-down.
void SoftwareRenderer::drawNdcLine(const Vec3& a, const Vec3& b, uint32_t color) {
  constexpr int kSteps = 64;
  for (int i = 0; i <= kSteps; ++i) {
    const float t = float(i) / kSteps;
    const float nx = a.x + (b.x - a.x) * t, ny = a.y + (b.y - a.y) * t;
    const int x = int(std::lround((nx * 0.5f + 0.5f) * float(fb_.width) - 0.5f));
    const int y = int(std::lround((1.f - (ny * 0.5f + 0.5f)) * float(fb_.height) - 0.5f));
    if (x >= 0 && y >= 0 && x < int(fb_.width) && y < int(fb_.height))
      fb_.pixels[size_t(y) * fb_.width + size_t(x)] = color;
  }
}

void SoftwareRenderer::drawOverlayLines(const FrameParams& p) {
  if (p.highlight) {
    const auto lines = blockOutlineLines(p.hx, p.hy, p.hz);
    for (size_t i = 0; i + 1 < lines.size(); i += 2)
      drawWorldLine(p, lines[i], lines[i + 1], 0xFF0D0D14u);  // GL line color (0.05,0.05,0.08)
  }
  if (p.crosshair && p.height > 0) {
    const auto lines = crosshairLines(p.width, p.height);
    for (size_t i = 0; i + 1 < lines.size(); i += 2)
      drawNdcLine(lines[i], lines[i + 1], 0xFFFFFFFFu);
  }
}

void SoftwareRenderer::renderFrame(const FrameParams& p) {
  if (p.width <= 0 || p.height <= 0) return;  // as the Vulkan backend
  // SSAA2x (JD 2026-10-13): the world stage rasterizes at 2x in both axes
  // and box-resolves to the target size before the overlay/HUD stage, so
  // HUD and crosshair stay at native resolution (never supersampled).
  const bool ssaa = aaMode_ == SoftwareAaMode::Ssaa2x;
  const uint32_t targetW = uint32_t(p.width), targetH = uint32_t(p.height);
  const uint32_t renderW = ssaa ? targetW * 2 : targetW;
  const uint32_t renderH = ssaa ? targetH * 2 : targetH;
  if (fb_.width != renderW || fb_.height != renderH) resize(renderW, renderH);
  clear(swSkyColor(p.sky));
  // Culling prototypes + frustum margin (M1): beginFrame arms the comet /
  // velocity state; decide() then culls per mesh exactly like GL/VK. All
  // prototypes default off, so decide() is the plain margin test.
  cullProto_.beginFrame(p.viewProj, p.yawRad, p.pitchRad, p.timeSec);
  stats_.meshes = uint32_t(meshes_.size() + pbrMeshes_.size());
  stats_.drawn = 0;
  // Stored chunk meshes at their sub-chunk origins (the same per-mesh
  // translation the MCP capture path bakes into its MVP), in key order.
  // Depth testing makes the order immaterial for the opaque world.
  // stats_.drawn counts only meshes actually submitted (post-cull), like
  // the GPU backends (fixes the L6 comparability gap).
  for (const auto& [key, m] : meshes_) {
    (void)key;
    const Vec3 mn{m.cx * 16.f, m.sy * 16.f, m.cz * 16.f};
    if (cullProto_.decide(p.viewProj, mn, mn + Vec3{16, 16, 16}, p.camPos,
                          frustumMarginDeg_) != CullDecision::Full)
      continue;
    const Mat4 mvp = mul(p.viewProj, swTranslation(mn.x, mn.y, mn.z));
    // Ground pulse + traces for this mesh (per-pixel path only; the flat
    // path has no fragment hook and stays as it was).
    groundFx_ = GroundFx{};
    for (int i = 0; i < 4; ++i) {
      groundFx_.pulseP0[i] = m.pulseP0[i];
      groundFx_.pulseP1[i] = m.pulseP1[i];
      groundFx_.traceP0[i] = m.traceP0[i];
      groundFx_.traceP1[i] = m.traceP1[i];
    }
    groundFx_.pulseTimeSec = p.groundPulseTimeSec;
    groundFx_.traceTimeSec = p.groundTraceTimeSec;
    groundFx_.pulseOn = p.groundPulseOn;
    groundFx_.traceOn = p.groundTraceOn;
    meshOriginX_ = mn.x; meshOriginY_ = mn.y; meshOriginZ_ = mn.z;
    renderMesh(m.verts.data(), m.verts.size(), mvp.m);
    ++stats_.drawn;
  }
  groundFx_ = GroundFx{};
  meshOriginX_ = meshOriginY_ = meshOriginZ_ = 0.f;
  renderPbrMeshes(p);
  if (!entityVerts_.empty()) {
    renderEntityVerts(p.viewProj.m);  // entity verts carry their own frame (offset 0, as GL)
    ++stats_.drawn;
  }
  drawSprites(p);
  // AA + ASCII stages (JD 2026-10-13): the world frame is finished here.
  // SSAA resolves down to target size; fxaa/cmaa post-filter in place;
  // ASCII (when on) then converts the AA'd frame to its glyph
  // visualization. Overlays/HUD draw after, at native resolution.
  if (ssaa) {
    aaOut_.resize(targetW, targetH);
    resolveSsaa2x(fb_, aaOut_);
    fb_.width = targetW;
    fb_.height = targetH;
    fb_.pixels = std::move(aaOut_.pixels);
    depth_.assign(size_t(targetW) * targetH, 1.0f);  // overlay depth tests start fresh
  } else {
    applyAaPostPass();
  }
  if (asciiEnabled_) applyAsciiPass();
  // Overlays (H3): highlight + crosshair lines, then the HUD layer — the
  // same order the GPU backends draw them (lines before HUD).
  drawOverlayLines(p);
  drawHud(p);
  if (grabArmed_) {
    grabArmed_ = false;
    grabW_ = int(fb_.width);
    grabH_ = int(fb_.height);
    grabRgb_.resize(size_t(grabW_) * size_t(grabH_) * 3);
    for (size_t i = 0, n = size_t(grabW_) * size_t(grabH_); i < n; ++i) {
      const uint32_t px = fb_.pixels[i];
      grabRgb_[3 * i] = uint8_t((px >> 16) & 0xFF);
      grabRgb_[3 * i + 1] = uint8_t((px >> 8) & 0xFF);
      grabRgb_[3 * i + 2] = uint8_t(px & 0xFF);
    }
    grabReady_ = true;
  }
}

bool SoftwareRenderer::grabFramebuffer(std::vector<uint8_t>& rgb, int& w, int& h) {
  if (!grabReady_) return false;
  rgb = grabRgb_;
  w = grabW_;
  h = grabH_;
  return true;
}

}  // namespace litholattice
