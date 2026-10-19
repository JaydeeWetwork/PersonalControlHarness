// Software-renderer anti-aliasing modes (JD 2026-10-13).
//
// The software renderer is the engine's equalizer lane — it must run from
// retro hardware and basic Chromebooks up to state-of-the-art GPU-less
// CPUs — so AA is a LADDER of named modes behind `render.aa`, not one
// technique. All modes are post/raster-output stages on the CPU
// framebuffer: purely presentational, zero sim interaction.
//
//   off     - no AA. The baseline; also JD's reserved default until he
//             picks from the capture sets.
//   fxaa    - FXAA-class post filter (Lottes lineage): local-contrast
//             gate, subpixel term, edge-direction endpoint search,
//             coverage offset. Cheapest real AA; softens thin lines.
//   cmaa    - FLAGSHIP: conservative morphological coverage restoration
//             (MLAA -> SMAA -> CMAA2 lineage, clean-room variant). Detects
//             edges (luma + the renderer's depth buffer), reconstructs
//             the edge line through each edge pixel from the run's ends
//             and crossing edges, computes the pixel's TRUE coverage by
//             the far side analytically (no lookup textures), and blends
//             by that coverage. Conservative gates keep it off texture
//             detail, so thin trace lines survive.
//   ssaa2x  - true 2x supersampling: the renderer rasterizes at 2x in
//             both axes and box-resolves. Quality reference; ~4x fill.
//
// Hardware classes (JD's inclusion rationale): fxaa = Chromebook / retro
// floor; cmaa = mainstream CPU without GPU; ssaa2x = strong CPU,
// quality-first. See Software-Renderer-AA-2026-10-13.md for the survey,
// per-mode costs, and the clean-room/license notes.
#pragma once

#include <cstdint>
#include <vector>
#include "litholattice/render/software_renderer.h"  // SoftwareFramebuffer

namespace litholattice {

enum class SoftwareAaMode : int { Off = 0, Fxaa = 1, Cmaa = 2, Ssaa2x = 3 };

/// Stable cvar spellings for render.aa ("off" | "fxaa" | "cmaa" | "ssaa2x").
const char* softwareAaModeName(SoftwareAaMode mode);
bool softwareAaModeFromName(const char* name, SoftwareAaMode& out);

/// Reusable scratch for the post passes (avoids per-frame allocation).
struct SoftwareAaScratch {
  std::vector<uint32_t> src;     ///< source frame copy (post passes gather from it)
  std::vector<float> luma;       ///< per-pixel luma (cmaa edge detection)
  std::vector<uint8_t> edgeR;    ///< cmaa: edge between (x,y) and (x+1,y)
  std::vector<uint8_t> edgeB;    ///< cmaa: edge between (x,y) and (x,y+1)
  std::vector<float> acc;        ///< cmaa: 4 floats per pixel (rgb, weight)
};

/// FXAA-class post filter, in place on fb (gathers from a scratch copy).
void applyFxaaPost(SoftwareFramebuffer& fb, SoftwareAaScratch& scratch);

/// Conservative morphological coverage AA, in place on fb. `depth` is the
/// renderer's NDC-z depth buffer at fb resolution (may be null: luma-only
/// edge detection, weaker on same-color geometry steps).
void applyCmaaPost(SoftwareFramebuffer& fb, const float* depth, SoftwareAaScratch& scratch);

/// 2x box resolve: hi (2W x 2H) -> out (W x H), exact 2x2 average.
void resolveSsaa2x(const SoftwareFramebuffer& hi, SoftwareFramebuffer& out);

}  // namespace litholattice
