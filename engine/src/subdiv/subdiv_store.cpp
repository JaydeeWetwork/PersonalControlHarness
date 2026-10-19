// SubdivStore: chunk-scoped block -> SparseOctree map (workstream 1).
// See subdiv_store.h for the serialized format.

#include "litholattice/subdiv/subdiv_store.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace litholattice::subdiv {

namespace {

void putU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back((uint8_t)(v & 0xFF));
  out.push_back((uint8_t)((v >> 8) & 0xFF));
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back((uint8_t)(v & 0xFF));
  out.push_back((uint8_t)((v >> 8) & 0xFF));
  out.push_back((uint8_t)((v >> 16) & 0xFF));
  out.push_back((uint8_t)((v >> 24) & 0xFF));
}

uint16_t getU16(const uint8_t* p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

uint32_t getU32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

constexpr uint16_t kStoreVersion = 1;

}  // namespace

size_t SubdivStore::findIndex(int x, int y, int z) const {
  size_t lo = 0, hi = entries_.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const Entry& e = entries_[mid];
    if (e.z < z || (e.z == z && (e.y < y || (e.y == y && e.x < x))))
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}

void SubdivStore::setBlockOctree(int x, int y, int z, SparseOctree octree) {
  if (!validCoord(x) || !validCoord(y) || !validCoord(z)) return;
  const size_t i = findIndex(x, y, z);
  const bool hit = i < entries_.size() && entries_[i].x == x &&
                   entries_[i].y == y && entries_[i].z == z;
  if (hit) {
    if (octree.empty())
      entries_.erase(entries_.begin() + (ptrdiff_t)i);
    else
      entries_[i].tree = std::move(octree);
  } else if (!octree.empty()) {
    Entry e;
    e.x = (uint8_t)x;
    e.y = (uint8_t)y;
    e.z = (uint8_t)z;
    e.tree = std::move(octree);
    entries_.insert(entries_.begin() + (ptrdiff_t)i, std::move(e));
  }
}

const SparseOctree* SubdivStore::getBlockOctree(int x, int y, int z) const {
  if (!validCoord(x) || !validCoord(y) || !validCoord(z)) return nullptr;
  const size_t i = findIndex(x, y, z);
  if (i < entries_.size() && entries_[i].x == x && entries_[i].y == y &&
      entries_[i].z == z)
    return &entries_[i].tree;
  return nullptr;
}

bool SubdivStore::removeBlockOctree(int x, int y, int z) {
  if (!validCoord(x) || !validCoord(y) || !validCoord(z)) return false;
  const size_t i = findIndex(x, y, z);
  if (i < entries_.size() && entries_[i].x == x && entries_[i].y == y &&
      entries_[i].z == z) {
    entries_.erase(entries_.begin() + (ptrdiff_t)i);
    return true;
  }
  return false;
}

size_t SubdivStore::memoryBytes() const {
  size_t bytes = sizeof(SubdivStore) + entries_.capacity() * sizeof(Entry);
  for (const Entry& e : entries_) bytes += e.tree.memoryBytes();
  return bytes;
}

std::vector<uint8_t> SubdivStore::serialize() const {
  std::vector<uint8_t> out;
  out.reserve(10);
  out.push_back('L');
  out.push_back('T');
  out.push_back('H');
  out.push_back('S');
  putU16(out, kStoreVersion);
  putU32(out, (uint32_t)entries_.size());
  for (const Entry& e : entries_) {
    out.push_back(e.x);
    out.push_back(e.y);
    out.push_back(e.z);
    const std::vector<MicroCell> ls = e.tree.leaves();
    putU32(out, (uint32_t)ls.size());
    for (const MicroCell& c : ls) {
      out.push_back(c.level);
      out.push_back(c.x);
      out.push_back(c.y);
      out.push_back(c.z);
      out.push_back(c.material);
    }
  }
  return out;
}

bool SubdivStore::deserialize(const uint8_t* data, size_t size) {
  if (!data || size < 10) return false;
  if (data[0] != 'L' || data[1] != 'T' || data[2] != 'H' || data[3] != 'S')
    return false;
  if (getU16(data + 4) != kStoreVersion) return false;
  const uint32_t count = getU32(data + 6);
  // Every entry needs at least its 7-byte header (x, y, z + leaf count).
  if (count > (size - 10) / 7) return false;
  SubdivStore tmp;
  size_t off = 10;
  for (uint32_t i = 0; i < count; ++i) {
    if (off + 7 > size) return false;
    const int x = data[off], y = data[off + 1], z = data[off + 2];
    if (!validCoord(x) || !validCoord(y) || !validCoord(z)) return false;
    const uint32_t n = getU32(data + off + 3);
    off += 7;
    if (n > (size - off) / 5) return false;  // 5 bytes per leaf record
    SparseOctree t;
    for (uint32_t k = 0; k < n; ++k) {
      const int level = data[off], cx = data[off + 1], cy = data[off + 2];
      const int cz = data[off + 3], mat = data[off + 4];
      off += 5;
      // leaves() never emits these shapes, so they mark a corrupt stream.
      if (level < 0 || level > kMaxLevel) return false;
      const int extent = 1 << level;
      if (cx < 0 || cx >= extent || cy < 0 || cy >= extent || cz < 0 ||
          cz >= extent)
        return false;
      if (mat == 0) return false;
      t.set(level, cx, cy, cz, (uint8_t)mat);
    }
    if (!t.empty()) tmp.setBlockOctree(x, y, z, std::move(t));
  }
  if (off != size) return false;  // trailing garbage: corrupt stream
  *this = std::move(tmp);
  return true;
}

}  // namespace litholattice::subdiv
