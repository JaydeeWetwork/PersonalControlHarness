// Vulkan sprite pass: separable quad -> vertex / run conversion logic.
//
// The conversion is deliberately free of Vulkan types so the engine tests
// can pin the contract on any machine (no GPU, no loader): GL33's sprite
// pass (render/gl33) and this backend must agree on
//   - corner expansion order {0,1,2, 0,2,3} (two triangles, CCW as emitted),
//   - input order preserved exactly (blend/additive quads arrive
//     pre-sorted far-to-near by the sprite system; never re-sorted here),
//   - runs = maximal consecutive spans of equal (shade, page), drawn in
//     order, one pipeline + atlas-layer switch per run,
//   - shade mapping: kShadeCutout -> cutout pipeline (alpha test 0.5,
//     depth write on), kShadeAlphaBlend -> blend pipeline,
//     kShadeAdditive -> additive pipeline (both: alpha test 0, no depth
//     write). Any other ShadeMode is NOT a sprite shade: it maps to no
//     pipeline and its quads are skipped (and counted), never misdrawn.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "litholattice/render/renderer.h"  // SpriteDrawQuad, ShadeMode

namespace litholattice::vk_sprite {

/// Interleaved sprite vertex for the Vulkan pass: world position, atlas
/// image-space UV, tint, and the atlas page as a float (fragment receives
/// it flat and uses it as the texture-array layer).
struct SpriteVertex {
  float x = 0, y = 0, z = 0;
  float u = 0, v = 0;
  float r = 1, g = 1, b = 1, a = 1;
  float page = 0;
};

/// Corner expansion for one quad: two triangles, matching GL33 exactly.
inline constexpr std::array<int, 6> kSpriteCornerOrder = {0, 1, 2, 0, 2, 3};

/// Pipeline slot for a sprite shade: 0 cutout, 1 alpha-blend, 2 additive,
/// -1 = not a sprite shade (quad must be skipped, never drawn).
inline int spritePipelineForShade(ShadeMode shade) {
  switch (shade) {
    case kShadeCutout: return 0;
    case kShadeAlphaBlend: return 1;
    case kShadeAdditive: return 2;
    default: return -1;
  }
}

/// Alpha-test threshold per shade (mirrors GL33's uAlphaTest): 0.5 for the
/// cutout pass, 0 (test disabled) for the blended passes.
inline float spriteAlphaTestForShade(ShadeMode shade) {
  return shade == kShadeCutout ? 0.5f : 0.f;
}

/// Appends one quad's 6 vertices (kSpriteCornerOrder) to `out`, in the
/// quad's own order. Tint and UVs are copied verbatim from the contract.
inline void appendSpriteQuad(std::vector<SpriteVertex>& out, const SpriteDrawQuad& q) {
  for (int vi : kSpriteCornerOrder) {
    const Vec3& c = q.corners[vi];
    SpriteVertex v;
    v.x = c.x; v.y = c.y; v.z = c.z;
    v.u = q.uv[vi][0]; v.v = q.uv[vi][1];
    v.r = q.r; v.g = q.g; v.b = q.b; v.a = q.a;
    v.page = float(q.page);
    out.push_back(v);
  }
}

/// Converts a whole frame's quads: 6 vertices per quad, input order
/// preserved (no sorting — the producer's order is the contract).
inline void buildSpriteVertices(const SpriteDrawQuad* quads, size_t count,
                                std::vector<SpriteVertex>& out) {
  out.clear();
  if (!quads) return;
  out.reserve(count * 6);
  for (size_t i = 0; i < count; ++i) appendSpriteQuad(out, quads[i]);
}

/// One draw run: consecutive quads sharing (shade, page). firstVertex /
/// vertexCount index the vertex array built by buildSpriteVertices.
struct SpriteRun {
  ShadeMode shade = kShadeCutout;
  int32_t page = -1;
  uint32_t firstVertex = 0;
  uint32_t vertexCount = 0;
  int pipeline = 0;  ///< spritePipelineForShade(shade); -1 = skip this run
};

/// Groups quads into runs in input order. Runs with an out-of-range page
/// (page < 0 or >= pageCount) or a non-sprite shade keep pipeline -1 /
/// page -1 marking so the caller can skip-and-count them exactly like
/// GL33 skips out-of-range pages.
inline std::vector<SpriteRun> spriteRuns(const SpriteDrawQuad* quads, size_t count,
                                         size_t pageCount) {
  std::vector<SpriteRun> runs;
  if (!quads) return runs;
  size_t i = 0;
  while (i < count) {
    size_t j = i + 1;
    while (j < count && quads[j].shade == quads[i].shade && quads[j].page == quads[i].page) ++j;
    SpriteRun run;
    run.shade = quads[i].shade;
    run.page = quads[i].page;
    run.firstVertex = uint32_t(i * 6);
    run.vertexCount = uint32_t((j - i) * 6);
    run.pipeline = spritePipelineForShade(quads[i].shade);
    if (quads[i].page < 0 || size_t(quads[i].page) >= pageCount) run.pipeline = -1;
    runs.push_back(run);
    i = j;
  }
  return runs;
}

}  // namespace litholattice::vk_sprite
