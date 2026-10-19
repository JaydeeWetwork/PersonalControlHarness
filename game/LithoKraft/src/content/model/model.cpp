// .lthm serialization: saveModel / loadModel / modelEquals / ModelBuilder.

#include "model.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include <litholattice/subdiv/subdiv_store.h>

namespace lithokraft::model {
namespace {

using litholattice::subdiv::kMaxLevel;
using litholattice::subdiv::MicroCell;
using litholattice::subdiv::SparseOctree;

// ---- little-endian integer helpers ---------------------------------------

void putU16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

void putU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void putString(std::vector<uint8_t>& out, const std::string& s) {
  // Length-prefixed UTF-8, no NUL terminator. u16 caps strings at 65535 bytes.
  putU16(out, static_cast<uint16_t>(s.size()));
  out.insert(out.end(), s.begin(), s.end());
}

// ---- CRC32 (IEEE 802.3) ---------------------------------------------------

constexpr std::array<uint32_t, 256> makeCrcTable() {
  std::array<uint32_t, 256> t{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    t[i] = c;
  }
  return t;
}

uint32_t crc32(const uint8_t* data, size_t len) {
  static constexpr auto kTable = makeCrcTable();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) c = kTable[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// ---- parsing cursor with bounds checks ------------------------------------

class Cursor {
 public:
  Cursor(const uint8_t* data, size_t len) : p_(data), end_(data + len) {}

  bool take(size_t n, const uint8_t*& out) {
    if (static_cast<size_t>(end_ - p_) < n) return false;
    out = p_;
    p_ += n;
    return true;
  }
  bool u8(uint8_t& v) {
    const uint8_t* q = nullptr;
    if (!take(1, q)) return false;
    v = q[0];
    return true;
  }
  bool u16(uint16_t& v) {
    const uint8_t* q = nullptr;
    if (!take(2, q)) return false;
    v = static_cast<uint16_t>(q[0] | (static_cast<uint16_t>(q[1]) << 8));
    return true;
  }
  bool u32(uint32_t& v) {
    const uint8_t* q = nullptr;
    if (!take(4, q)) return false;
    v = static_cast<uint32_t>(q[0]) | (static_cast<uint32_t>(q[1]) << 8) |
        (static_cast<uint32_t>(q[2]) << 16) | (static_cast<uint32_t>(q[3]) << 24);
    return true;
  }
  bool str(std::string& s) {
    uint16_t n = 0;
    if (!u16(n)) return false;
    const uint8_t* q = nullptr;
    if (!take(n, q)) return false;
    s.assign(reinterpret_cast<const char*>(q), n);
    return true;
  }
  size_t remaining() const { return static_cast<size_t>(end_ - p_); }

 private:
  const uint8_t* p_;
  const uint8_t* end_;
};

std::vector<uint8_t> readFile(const std::string& path) {
  std::vector<uint8_t> out;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return out;
  uint8_t buf[8192];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
    out.insert(out.end(), buf, buf + n);
  }
  std::fclose(f);
  return out;
}

bool writeFile(const std::string& path, const std::vector<uint8_t>& data,
               std::string* err) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    if (err) *err = "cannot open for writing: " + path;
    return false;
  }
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size() &&
            std::fclose(f) == 0;
  if (!ok && err) *err = "write failed: " + path;
  return ok;
}

}  // namespace

bool saveModel(const std::string& path, const Model& m, std::string* err) {
  if (m.name.size() > 0xFFFF || m.author.size() > 0xFFFF) {
    if (err) *err = "name/author too long (max 65535 bytes)";
    return false;
  }
  std::vector<uint8_t> payload;
  payload.insert(payload.end(), std::begin(kMagic), std::end(kMagic));
  putU16(payload, kFormatVersion);
  putU16(payload, kFormatFlags);
  putString(payload, m.name);
  putString(payload, m.author);
  const int clampedLevel =
      std::clamp(m.maxLevel, 0, static_cast<int>(kMaxLevel));
  payload.push_back(static_cast<uint8_t>(clampedLevel));

  const std::vector<MicroCell> leaves = m.geometry.leaves();
  putU32(payload, static_cast<uint32_t>(leaves.size()));
  for (const MicroCell& c : leaves) {
    payload.push_back(c.level);
    payload.push_back(c.x);
    payload.push_back(c.y);
    payload.push_back(c.z);
    payload.push_back(c.material);
  }

  // CRC32 covers the whole payload (everything before the CRC field itself).
  putU32(payload, crc32(payload.data(), payload.size()));
  return writeFile(path, payload, err);
}

LoadResult loadModel(const std::string& path) {
  LoadResult r;
  const std::vector<uint8_t> file = readFile(path);
  if (file.empty()) {
    // Distinguish "no such file" from "empty file": both are I/O-level
    // failures for our purposes (an empty file has no magic anyway).
    FILE* probe = std::fopen(path.c_str(), "rb");
    if (!probe) {
      r.error = LoadError::IoError;
      r.message = "cannot open file: " + path;
      return r;
    }
    std::fclose(probe);
  }

  constexpr size_t kMinSize = 4 + 2 + 2 + 4;  // magic + version + flags + crc
  if (file.size() < kMinSize) {
    if (file.size() >= 4 &&
        std::memcmp(file.data(), kMagic, 4) != 0) {
      r.error = LoadError::BadMagic;
      r.message = "bad magic: not a .lthm file";
    } else {
      r.error = LoadError::Truncated;
      r.message = "file too short to be a .lthm file";
    }
    return r;
  }
  if (std::memcmp(file.data(), kMagic, 4) != 0) {
    r.error = LoadError::BadMagic;
    r.message = "bad magic: not a .lthm file";
    return r;
  }

  const uint16_t version =
      static_cast<uint16_t>(file[4] | (static_cast<uint16_t>(file[5]) << 8));
  if (version != kFormatVersion) {
    r.error = LoadError::UnsupportedVersion;
    r.message = "unsupported .lthm version " + std::to_string(version) +
                " (this build reads version " +
                std::to_string(kFormatVersion) + ")";
    return r;
  }

  // Payload is everything except the trailing 4 CRC bytes.
  const size_t payloadLen = file.size() - 4;

  // Structural parse first (bounds-checked; safe on untrusted bytes), CRC
  // verification second: truncated downloads report Truncated, while files
  // with intact structure but flipped bytes report BadCrc. The octree is only
  // built after the CRC passes.
  Cursor cur(file.data() + 8, payloadLen - 8);  // past magic/version/flags
  uint16_t flags = 0;
  {
    // flags were at bytes 6..7; re-read for the record (currently unused).
    const uint8_t* f = file.data() + 6;
    flags = static_cast<uint16_t>(f[0] | (static_cast<uint16_t>(f[1]) << 8));
  }
  (void)flags;  // reserved for future minor revisions; ignored by v1 readers

  Model m;
  uint8_t maxLevel = 0;
  uint32_t leafCount = 0;
  if (!cur.str(m.name) || !cur.str(m.author) || !cur.u8(maxLevel) ||
      !cur.u32(leafCount)) {
    r.error = LoadError::Truncated;
    r.message = "file ends inside the header fields";
    return r;
  }
  if (maxLevel > kMaxLevel) {
    r.error = LoadError::BadData;
    r.message = "maxLevel out of range";
    return r;
  }
  m.maxLevel = maxLevel;

  if (leafCount > cur.remaining() / 5) {
    r.error = LoadError::Truncated;
    r.message = "file ends inside the leaf list";
    return r;
  }
  std::vector<MicroCell> parsed;
  parsed.reserve(leafCount);
  for (uint32_t i = 0; i < leafCount; ++i) {
    MicroCell c{};
    cur.u8(c.level);
    cur.u8(c.x);
    cur.u8(c.y);
    cur.u8(c.z);
    cur.u8(c.material);
    if (c.level > kMaxLevel || c.x >= (1u << c.level) ||
        c.y >= (1u << c.level) || c.z >= (1u << c.level) ||
        c.material == 0) {
      r.error = LoadError::BadData;
      r.message = "leaf cell out of range or empty at index " +
                  std::to_string(i);
      return r;
    }
    parsed.push_back(c);
  }

  // CRC check: payload is everything except the trailing 4 CRC bytes.
  const uint8_t* crcPtr = file.data() + payloadLen;
  const uint32_t stored = static_cast<uint32_t>(crcPtr[0]) |
                          (static_cast<uint32_t>(crcPtr[1]) << 8) |
                          (static_cast<uint32_t>(crcPtr[2]) << 16) |
                          (static_cast<uint32_t>(crcPtr[3]) << 24);
  if (crc32(file.data(), payloadLen) != stored) {
    r.error = LoadError::BadCrc;
    r.message = "CRC32 mismatch: file is corrupt";
    return r;
  }

  SparseOctree tree;
  for (const MicroCell& c : parsed) tree.set(c.level, c.x, c.y, c.z, c.material);
  m.geometry = tree;
  r.model = std::move(m);
  return r;
}

bool modelEquals(const Model& a, const Model& b) {
  if (a.name != b.name || a.author != b.author || a.maxLevel != b.maxLevel) {
    return false;
  }
  const std::vector<MicroCell> la = a.geometry.leaves();
  const std::vector<MicroCell> lb = b.geometry.leaves();
  if (la.size() != lb.size()) return false;
  for (size_t i = 0; i < la.size(); ++i) {
    if (la[i].level != lb[i].level || la[i].x != lb[i].x ||
        la[i].y != lb[i].y || la[i].z != lb[i].z ||
        la[i].material != lb[i].material) {
      return false;
    }
  }
  return true;
}

void placeModel(litholattice::subdiv::SubdivStore& store, int bx, int by,
                int bz, const Model& m) {
  if (m.geometry.empty()) return;  // no-op: never creates or clears entries
  SparseOctree target;
  if (const SparseOctree* cur = store.getBlockOctree(bx, by, bz)) {
    target = *cur;
  }
  for (const MicroCell& c : m.geometry.leaves()) {
    target.set(c.level, c.x, c.y, c.z, c.material);
  }
  store.setBlockOctree(bx, by, bz, std::move(target));
}

ModelBuilder::ModelBuilder(std::string name, std::string author)
    : name_(std::move(name)), author_(std::move(author)) {}

void ModelBuilder::setCell(int level, int x, int y, int z, uint8_t material) {
  if (level < 0 || level > kMaxLevel) return;
  const int span = 1 << level;
  if (x < 0 || x >= span || y < 0 || y >= span || z < 0 || z >= span) return;
  tree_.set(level, x, y, z, material);
}

void ModelBuilder::fillBox(int level, int x0, int y0, int z0, int x1, int y1,
                           int z1, uint8_t material) {
  if (level < 0 || level > kMaxLevel) return;
  const int span = 1 << level;
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);
  if (z0 > z1) std::swap(z0, z1);
  x0 = std::clamp(x0, 0, span - 1);
  y0 = std::clamp(y0, 0, span - 1);
  z0 = std::clamp(z0, 0, span - 1);
  x1 = std::clamp(x1, 0, span - 1);
  y1 = std::clamp(y1, 0, span - 1);
  z1 = std::clamp(z1, 0, span - 1);
  for (int z = z0; z <= z1; ++z)
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) tree_.set(level, x, y, z, material);
}

Model ModelBuilder::build() const {
  Model m;
  m.name = name_;
  m.author = author_;
  m.geometry = tree_;
  int top = 0;
  for (const MicroCell& c : m.geometry.leaves()) {
    if (c.level > top) top = c.level;
  }
  m.maxLevel = top;
  return m;
}

}  // namespace lithokraft::model
