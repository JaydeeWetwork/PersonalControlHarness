#include "litholattice/ai/senses.h"
#include <algorithm>
#include <cmath>

namespace litholattice::ai {

int occluders(const ChunkMap& w, const std::function<bool(BlockId)>& opaque, Vec a, Vec b, int stopAt) {
  const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
  // Degenerate segment: nothing to walk (without this the DDA below burns all
  // 512 steps on a zero-length ray).
  if (dx == 0.f && dy == 0.f && dz == 0.f) return 0;
  int x = int(std::floor(a.x)), y = int(std::floor(a.y)), z = int(std::floor(a.z));
  const int ex = int(std::floor(b.x)), ey = int(std::floor(b.y)), ez = int(std::floor(b.z));
  const int sx = dx > 0 ? 1 : -1, sy = dy > 0 ? 1 : -1, sz = dz > 0 ? 1 : -1;
  auto tmax = [](float o, float d, int c, int s) { return d == 0 ? 1e30f : ((s > 0 ? float(c + 1) - o : o - float(c)) / std::abs(d)); };
  float tx = tmax(a.x, dx, x, sx), ty = tmax(a.y, dy, y, sy), tz = tmax(a.z, dz, z, sz);
  const float ddx = dx == 0 ? 1e30f : 1.f / std::abs(dx), ddy = dy == 0 ? 1e30f : 1.f / std::abs(dy), ddz = dz == 0 ? 1e30f : 1.f / std::abs(dz);
  int hits = 0;
  for (int steps = 0; steps < 512; ++steps) {
    if (tx < ty && tx < tz) { x += sx; tx += ddx; }
    else if (ty < tz) { y += sy; ty += ddy; }
    else { z += sz; tz += ddz; }
    if (std::min({tx - ddx, ty - ddy, tz - ddz}) > 1.f) break;
    if (x == ex && y == ey && z == ez) break;
    if (y >= 0 && y < kWorldHeight && opaque(w.get(x, y, z)) && ++hits >= stopAt) break;
  }
  return hits;
}

bool canSee(const ChunkMap& w, const std::function<bool(BlockId)>& opaque, Vec eye, Vec dir, Vec t, SightParams p) {
  const float dx = t.x - eye.x, dy = t.y - eye.y, dz = t.z - eye.z;
  const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (d > p.range) return false;
  if (d > 1e-4f && (dx * dir.x + dy * dir.y + dz * dir.z) / d < p.fovCos) return false;
  return occluders(w, opaque, eye, t, 1) == 0;
}

float hear(const ChunkMap& w, const std::function<bool(BlockId)>& opaque, Vec ear, const Sound& s) {
  const float dx = s.pos.x - ear.x, dy = s.pos.y - ear.y, dz = s.pos.z - ear.z;
  const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (d >= s.loudness) return 0.f;
  float v = 1.f - d / s.loudness;
  for (int i = occluders(w, opaque, ear, s.pos, 6); i > 0; --i) v *= 0.5f;
  return v;
}

FieldHit senseField(const std::function<float(int, int, int)>& field, Cell at, int r) {
  FieldHit best{0.f, at};
  for (int y = -r; y <= r; ++y)
    for (int z = -r; z <= r; ++z)
      for (int x = -r; x <= r; ++x)
        if (const float v = field(at.x + x, at.y + y, at.z + z); v > best.value) best = {v, {at.x + x, at.y + y, at.z + z}};
  return best;
}

}  // namespace litholattice::ai
