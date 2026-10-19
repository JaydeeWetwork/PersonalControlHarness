// --noise-preview: renders one output of a noise graph (or of a generator file's graph) to a
// grayscale PNG, headless. Values are normalized min..max -> black..white; the range is logged.
#pragma once
#include "lithokraft/game/engine.h"
#include <cstdint>
#include <string>

namespace lithokraft {

struct NoisePreviewOptions {
  std::string graphFile;              ///< graph JSON, or generator JSON with a "graph" member
  std::string outFile = "noise_preview.png";
  std::string output;                 ///< graph output; empty = "height" if present, else the first
  int size = 512;                     ///< image side in pixels
  float scale = 1.f;                  ///< world blocks per pixel
  float y = 64.f;                     ///< world y of the sampled plane (3D nodes)
  uint64_t seed = 0;                  ///< world seed; graphs are seeded as dimension "preview"
};

/// Returns a process exit code (0 = PNG written).
int runNoisePreview(const NoisePreviewOptions& options);

}  // namespace lithokraft
