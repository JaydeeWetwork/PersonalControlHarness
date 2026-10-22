// Software shaders: basic per-pixel lighting for the CPU renderer.
// Vertex shader: transform + pass varyings (normal, UV, world pos).
// Fragment shader: texture sample + diffuse lighting (N.L) + ambient.
// Enough for Chromebooks and GPU-less machines to run the game.
#pragma once
#include <cmath>  // std::sqrt (MSVC has no __builtin_sqrtf)
#include <cstdint>

namespace litholattice {

/// PBR per-pixel context (defined in pbr_software_shaders.h); FragmentInput only holds
/// a pointer so this header stays dependency-free.
struct PbrFragmentContext;

/// Varyings interpolated across a triangle (per-pixel inputs).
struct ShaderVaryings {
  float nx, ny, nz;  ///< normal (interpolated)
  float u, v;        ///< texture UV
  uint32_t layer;    ///< texture layer
  float wx, wy, wz;  ///< world position (for future point lights)
  uint16_t materialId = 0xFFFF;  ///< compact MaterialRegistry ID (flat per quad, like
                                 ///  layer); 0xFFFF = none -> basic shading
};

/// Fragment shader input/output.
struct FragmentInput {
  ShaderVaryings varyings;
  const uint32_t* texData;  ///< texture array (color)
  uint32_t texLayers;
  const uint32_t* normalData;  ///< normal map array (optional, RGB = XYZ normal)
  uint32_t normalLayers;
  const uint8_t* heightData;  ///< heightmap array (optional, 8-bit height per texel)
  uint32_t heightLayers;
  float bumpScale = 1.0f;     ///< bump intensity
  const PbrFragmentContext* pbr = nullptr;  ///< PBR context for pbrFragmentShader
                                            ///  (pbr_software_shaders.h); null = basic
};

/// Sample a normal map and decode to XYZ (RGB 0-255 -> -1.0 to 1.0).
inline void sampleNormalMap(const FragmentInput& in, float u, float v, uint32_t layer,
                            float* nx, float* ny, float* nz) {
  if (!in.normalData || layer >= in.normalLayers) {
    *nx = in.varyings.nx; *ny = in.varyings.ny; *nz = in.varyings.nz;
    return;
  }
  uint32_t tx = uint32_t(u * 15.0f) & 15;
  uint32_t ty = uint32_t(v * 15.0f) & 15;
  uint32_t n = in.normalData[layer * 256 + ty * 16 + tx];
  *nx = ((n >> 16) & 0xFF) / 127.5f - 1.0f;
  *ny = ((n >> 8) & 0xFF) / 127.5f - 1.0f;
  *nz = (n & 0xFF) / 127.5f - 1.0f;
}

/// Bump mapping: sample heightmap, compute gradient, perturb the normal.
/// Uses central differences on the 8-bit heightfield.
inline void applyBumpMap(const FragmentInput& in, float u, float v, uint32_t layer,
                         float* nx, float* ny, float* nz) {
  if (!in.heightData || layer >= in.heightLayers) return;
  auto heightAt = [&](int tx, int ty) -> float {
    tx = (tx < 0) ? 0 : (tx > 15 ? 15 : tx);
    ty = (ty < 0) ? 0 : (ty > 15 ? 15 : ty);
    return in.heightData[layer * 256 + ty * 16 + tx] / 255.0f;
  };
  int tx = int(u * 15.0f), ty = int(v * 15.0f);
  float hL = heightAt(tx-1, ty), hR = heightAt(tx+1, ty);
  float hD = heightAt(tx, ty-1), hU = heightAt(tx, ty+1);
  float dx = (hR - hL) * in.bumpScale;
  float dy = (hU - hD) * in.bumpScale;
  // Perturb: assume tangent space aligns with UV
  *nx -= dx; *ny -= dy;
  // Renormalize
  float len = *nx**nx + *ny**ny + *nz**nz;
  if (len > 1e-6f) { len = 1.0f / std::sqrt(len); *nx*=len; *ny*=len; *nz*=len; }
}

/// Basic shader: diffuse + ambient + texture.
/// Light dir is fixed (top-down-ish). Returns RGBA.
inline uint32_t basicFragmentShader(const FragmentInput& in) {
  // Texture sample (nearest neighbor for speed; bilinear is future)
  uint32_t texColor = 0xFFFFFFFF;
  if (in.texData && in.varyings.layer < in.texLayers) {
    uint32_t tx = uint32_t(in.varyings.u * 15.0f) & 15;
    uint32_t ty = uint32_t(in.varyings.v * 15.0f) & 15;
    texColor = in.texData[in.varyings.layer * 256 + ty * 16 + tx];
  }

  // Normal mapping: sample normal map if available, else use interpolated normal
  float nx, ny, nz;
  sampleNormalMap(in, in.varyings.u, in.varyings.v, in.varyings.layer, &nx, &ny, &nz);
  // Bump mapping: perturb with heightfield gradient (if heightmap present)
  applyBumpMap(in, in.varyings.u, in.varyings.v, in.varyings.layer, &nx, &ny, &nz);

  // Diffuse: N.L with fixed light direction (normalized ~ (0.5, 1.0, 0.3))
  float nl = nx * 0.44f + ny * 0.88f + nz * 0.26f;
  if (nl < 0) nl = 0;

  // Ambient + diffuse
  float light = 0.35f + 0.65f * nl;  // 0.35 ambient, 0.65 diffuse

  uint8_t tr = (texColor >> 16) & 0xFF;
  uint8_t tg = (texColor >> 8) & 0xFF;
  uint8_t tb = texColor & 0xFF;
  uint8_t r = uint8_t(tr * light);
  uint8_t g = uint8_t(tg * light);
  uint8_t b = uint8_t(tb * light);
  return 0xFF000000 | (r << 16) | (g << 8) | b;
}

/// Shader type: function pointer for the fragment stage.
/// (Vertex stage is fixed: transform + interpolate varyings.)
using FragmentShaderFn = uint32_t (*)(const FragmentInput&);

}  // namespace litholattice
