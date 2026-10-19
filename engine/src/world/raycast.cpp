#include "litholattice/world/raycast.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace litholattice {

void flatPickRange(const ChunkMap& w, const BlockRegistry& blocks, int x, int y, int z, float& y0, float& y1) {
  const bool hanging = !blocks.has(w.get(x, y - 1, z), kSolid) && blocks.has(w.get(x, y + 1, z), kSolid);
  y0 = float(y) + (hanging ? 15.f / 16.f : 0.f);
  y1 = y0 + 1.f / 16.f;
}

static bool rayHitsBox(const float o[3], const float d[3], const float lo[3], const float hi[3], float maxDist) {
  float t0 = 0.f, t1 = maxDist;
  for (int a = 0; a < 3; ++a) {
    if (std::abs(d[a]) < 1e-9f) { if (o[a] < lo[a] || o[a] > hi[a]) return false; continue; }
    float ta = (lo[a] - o[a]) / d[a], tb = (hi[a] - o[a]) / d[a];
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta); t1 = std::min(t1, tb);
    if (t0 > t1) return false;
  }
  return true;
}

bool raycast(const ChunkMap& w, float ox, float oy, float oz, float dx, float dy, float dz,
             float maxDist, int hit[3], int prev[3], const BlockRegistry* blocks) {
  int c[3] = {int(std::floor(ox)), int(std::floor(oy)), int(std::floor(oz))};
  const float o[3] = {ox, oy, oz}, d[3] = {dx, dy, dz};
  int step[3]; float tMax[3], tDelta[3];
  for (int a = 0; a < 3; ++a) {
    step[a] = d[a] > 0 ? 1 : -1;
    tDelta[a] = d[a] != 0 ? std::abs(1.f / d[a]) : 1e30f;
    const float edge = d[a] > 0 ? float(c[a] + 1) - o[a] : o[a] - float(c[a]);
    tMax[a] = d[a] != 0 ? edge * tDelta[a] : 1e30f;
  }
  // Test the origin cell first: the DDA below steps before testing, so a ray
  // starting inside a solid block would otherwise miss it entirely.
  {
    const BlockId b0 = w.get(c[0], c[1], c[2]);
    if (b0 != kAir) { hit[0] = c[0]; hit[1] = c[1]; hit[2] = c[2]; return true; }
  }
  float t = 0;
  while (t <= maxDist) {
    prev[0] = c[0]; prev[1] = c[1]; prev[2] = c[2];
    const int a = tMax[0] < tMax[1] ? (tMax[0] < tMax[2] ? 0 : 2) : (tMax[1] < tMax[2] ? 1 : 2);
    c[a] += step[a]; t = tMax[a]; tMax[a] += tDelta[a];
    if (t > maxDist) break;
    const BlockId b = w.get(c[0], c[1], c[2]);
    if (b != kAir && blocks && blocks->has(b, kFlatPick)) {
      float y0, y1;
      flatPickRange(w, *blocks, c[0], c[1], c[2], y0, y1);
      const float lo[3] = {float(c[0]), y0, float(c[2])}, hi[3] = {float(c[0] + 1), y1, float(c[2] + 1)};
      if (!rayHitsBox(o, d, lo, hi, maxDist)) continue;  // looked through the air above / below the wire
    }
    if (b != kAir) { hit[0] = c[0]; hit[1] = c[1]; hit[2] = c[2]; return true; }
  }
  return false;
}

}  // namespace litholattice
