// BFS light propagation engine.
//
// WHAT: Spreads light through the voxel world using breadth-first search.
// WHY: Faster than naive flood-fill; better cache behavior.
//      Clean-room implementation inspired by Starlight's approach (LGPL-3.0).
//      BFS itself is a 1950s algorithm; our code is original.
// HOW: Queue-based propagation. Light decays by 1 per block, reduced by opacity.
//      1-64 light scale (our theme, not MC's 0-15).
//
#include "litholattice/world/light_engine.h"
#include "litholattice/world/chunk.h"  // packPos / unpackPos for the source registry

namespace litholattice {

LightEngine::LightEngine(int sx, int sy, int sz)
    : sx_(sx), sy_(sy), sz_(sz) {
  light_.assign(size_t(sx) * sy * sz, 0);
  opacity_.assign(size_t(sx) * sy * sz, 0);
}

void LightEngine::addSource(int x, int y, int z, uint8_t level) {
  if (!inBounds(x, y, z)) return;
  if (level > kMaxLight) level = kMaxLight;  // B-L5: clamp to the 1..64 light scale
  sources_[packPos(x, y, z)] = level;
  light_[size_t(idx(x, y, z))] = level;
  bfsQueue_.push({x, y, z, level});
}

void LightEngine::removeSource(int x, int y, int z) {
  if (!inBounds(x, y, z)) return;
  const int si = idx(x, y, z);
  const uint8_t oldLevel = light_[size_t(si)];
  sources_.erase(packPos(x, y, z));  // forget first: the brighten pass re-spreads survivors only
  light_[size_t(si)] = 0;
  if (oldLevel == 0) return;

  // Two-pass removal, Starlight-style.
  // Pass 1 (darken): walk the removed source's reconstructed field and clear every
  // cell whose light could have come from it. The contribution is reconstructed with
  // the same decay the propagate pass uses, so the comparison is exact. A cell
  // brighter than the reconstruction is provably lit by someone else: keep it and
  // hand it to the brighten pass. (Weaker survivors inside the removed field get
  // over-cleared here; pass 2 re-lights them from the source registry.)
  const int d[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
  std::queue<LightPos> dark;
  dark.push({x, y, z, oldLevel});
  while (!dark.empty()) {
    const LightPos cur = dark.front();
    dark.pop();
    for (auto& dd : d) {
      const int nx = cur.x + dd[0], ny = cur.y + dd[1], nz = cur.z + dd[2];
      if (!inBounds(nx, ny, nz)) continue;
      const int ni = idx(nx, ny, nz);
      if (opacity_[size_t(ni)] >= 64) continue;  // fully opaque blocks all
      const int via = int(cur.level) - 1 - int(opacity_[size_t(ni)]) / 16;
      if (via <= 0) continue;
      const uint8_t nl = light_[size_t(ni)];
      if (nl == 0) continue;
      if (int(nl) > via) {
        bfsQueue_.push({nx, ny, nz, nl});  // someone else's light: re-spread it
      } else {
        light_[size_t(ni)] = 0;
        dark.push({nx, ny, nz, uint8_t(via)});
      }
    }
  }

  // Pass 2 (brighten): re-propagate every surviving source, repairing over-darkening.
  for (const auto& [key, level] : sources_) {
    int sx, sy, sz;
    unpackPos(key, sx, sy, sz);
    if (!inBounds(sx, sy, sz)) continue;
    const int i = idx(sx, sy, sz);
    if (level > light_[size_t(i)]) {
      light_[size_t(i)] = level;
      bfsQueue_.push({sx, sy, sz, level});
    }
  }
  propagate();
}

void LightEngine::setOpacity(int x, int y, int z, uint8_t opacity) {
  if (!inBounds(x, y, z)) return;
  opacity_[size_t(idx(x, y, z))] = opacity;
}

size_t LightEngine::propagate() {
  size_t updated = 0;
  const int d[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};

  while (!bfsQueue_.empty()) {
    LightPos cur = bfsQueue_.front();
    bfsQueue_.pop();

    // Light decays by 1 per block, blocked by opacity
    if (cur.level <= 1) continue;

    for (auto& dd : d) {
      int nx = cur.x + dd[0], ny = cur.y + dd[1], nz = cur.z + dd[2];
      if (!inBounds(nx, ny, nz)) continue;

      const int ni = idx(nx, ny, nz);
      // Opacity reduces light: fully opaque blocks all
      if (opacity_[size_t(ni)] >= 64) continue;

      // Signed math: cur.level - 1 - opacity/16 can go <= 0 (unsigned would wrap
      // to 255 and flood phantom light across the whole volume).
      const int nl = int(cur.level) - 1 - int(opacity_[size_t(ni)]) / 16;
      if (nl <= 0) continue;
      const uint8_t newLevel = uint8_t(nl);
      if (newLevel > light_[size_t(ni)]) {
        light_[size_t(ni)] = newLevel;
        bfsQueue_.push({nx, ny, nz, newLevel});
        ++updated;
      }
    }
  }
  return updated;
}

uint8_t LightEngine::getLight(int x, int y, int z) const {
  if (!inBounds(x, y, z)) return 0;
  return light_[size_t(idx(x, y, z))];
}

void LightEngine::clear() {
  std::fill(light_.begin(), light_.end(), 0);
  sources_.clear();
  // Clear the queue
  std::queue<LightPos> empty;
  bfsQueue_.swap(empty);
}

}  // namespace litholattice
