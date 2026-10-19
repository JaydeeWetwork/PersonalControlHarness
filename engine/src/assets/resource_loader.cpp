#include "litholattice/assets/resource_loader.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <system_error>

namespace litholattice {
namespace {

/// A definition and the pack it came from.
struct Def {
  const Json* json = nullptr;
  std::string pack;
};
using Defs = std::map<std::string, Def, std::less<>>;

/// Merged definitions of all packs (higher packs replace entries by name).
struct Merged {
  int tileSize = 16;
  Defs textures, animations, palettes, models;
};

constexpr std::string_view kSections[] = {"textures", "animations", "palettes", "models"};

/// Convert a hostile-JSON double to int by clamping IN DOUBLE first
/// (P3-3, Appendix E L4): int(1e30) is UB before any downstream clamp can
/// run. The band is far wider than any legitimate value at these sites.
int numToIntClamped(double v, double lo, double hi) {
  if (!(v >= lo)) return int(lo);  // also catches NaN
  if (v > hi) return int(hi);
  return int(v);
}

Merged mergeManifests(const PackStack& stack, std::vector<std::string>& warn) {
  Merged m;
  for (const ResourcePack& pack : stack.packs()) {
    const Json* man = pack.manifest();
    if (!man) continue;
    const std::string where = pack.name() + "/resources.json";
    if (const int v = numToIntClamped(man->getNum("version", kResourceFormatVersion), -1000000, 1000000); v != kResourceFormatVersion)
      warn.push_back(where + ": version " + std::to_string(v) + " (expected " + std::to_string(kResourceFormatVersion) + "), reading anyway");
    if (const Json* t = man->get("tile_size")) {
      if (t->type == Json::Type::Number && t->num >= 1 && t->num <= 1024) m.tileSize = int(t->num);
      else warn.push_back(where + ": tile_size must be 1..1024");
    }
    for (const auto& [key, value] : man->obj) {
      Defs* target = key == "textures" ? &m.textures : key == "animations" ? &m.animations
                   : key == "palettes" ? &m.palettes : key == "models" ? &m.models : nullptr;
      if (!target) {
        if (key != "format" && key != "version" && key != "tile_size" && key != "note" && key != "generated_by")
          warn.push_back(where + ": unknown key '" + key + "' ignored");
        continue;
      }
      if (value.type != Json::Type::Object) { warn.push_back(where + ": '" + key + "' must be an object"); continue; }
      for (const auto& [name, def] : value.obj) (*target)[name] = Def{&def, pack.name()};
    }
  }
  return m;
}

/// "path" or {"file": "path"} -> path.
std::string fileOf(const Json& j) { return j.type == Json::Type::String ? j.str : j.getStr("file"); }

bool isSvgRel(const std::string& rel) {  // case-insensitive .svg check (Litho SVG routing)
  if (rel.size() < 4) return false;
  std::string ext = rel.substr(rel.size() - 4);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return ext == ".svg";
}

SvgRasterCache& sharedSvgCache() {
  static SvgRasterCache cache;
  return cache;
}

std::optional<Image> loadImage(const PackStack& stack, const std::string& rel, int tileSize, std::string& origin,
                              std::string& err) {
  if (!isSafePackPath(rel)) { err = "unsafe path '" + rel + "'"; return std::nullopt; }
  const ResourcePack* p = stack.owner(rel);
  if (!p) { err = "file not found: " + rel; return std::nullopt; }
  origin = p->name();
  // Open the CHECKED (canonical, contained) path, not the unchecked
  // path(rel): closes the has()->open TOCTOU window (Appendix E L6).
  const auto checked = p->checkedPath(rel);
  if (!checked) { err = "file not found: " + rel; return std::nullopt; }
  const std::filesystem::path path = *checked;
  if (!isSvgRel(rel)) return Image::load(path, &err);
  // Litho SVG: rasterize once per (path, tile size); the tile size is the raster target.
  SvgRasterCache& cache = sharedSvgCache();
  const std::string key = path.generic_string();
  if (auto hit = cache.fetch(key, tileSize)) return hit;
  auto img = Image::loadSvg(path, tileSize, tileSize, &err);
  if (img) cache.store(key, tileSize, *img);
  return img;
}

AnimationTiming parseTiming(const Json* j, AnimationTiming t) {
  if (!j) return t;
  t.frameMs = numToIntClamped(j->getNum("frametime_ms", t.frameMs), 0, 3600000);
  if (const Json* l = j->get("loop"); l && l->type == Json::Type::Bool) t.loop = l->b;
  if (const Json* f = j->get("frames"); f && f->type == Json::Type::Array) {
    t.sequence.clear();
    for (const Json& v : f->arr)
      if (v.type == Json::Type::Number) t.sequence.push_back(numToIntClamped(v.num, 0, 65535));
  }
  return t;
}

class ModelParser {
 public:
  ModelParser(const TextureRegistry& reg, std::vector<std::string>& warn) : reg_(reg), warn_(warn) {}

  BlockModel parse(const std::string& name, const Def& def) {
    BlockModel m;
    m.name = name;
    m.origin = def.pack;
    where_ = def.pack + " model '" + name + "'";
    const Json& j = *def.json;
    if (j.type != Json::Type::Object) { warn_.push_back(where_ + ": must be an object"); return m; }
    for (const auto& [key, value] : j.obj) {
      if (key == "faces") parseLayout(value, m.base);
      else if (key == "active") parseLayout(value, m.active);
      else if (key == "wire") parseWire(value, m);
      else if (key == "palette") parsePalette(value, m);
      else if (key == "masks") parseMasks(value, m);
      else if (key != "note") warn_.push_back(where_ + ": unknown key '" + key + "'");
    }
    return m;
  }

 private:
  std::optional<FaceRef> faceRef(const Json& j) {
    FaceRef r;
    std::string tex, anim;
    if (j.type == Json::Type::String) tex = j.str;
    else if (j.type == Json::Type::Object) {
      tex = j.getStr("texture");
      anim = j.getStr("animation");
      if (const Json* uv = j.get("uv")) {
        if (uv->type != Json::Type::Array || uv->arr.size() != 4) warn_.push_back(where_ + ": uv must be [u0, v0, u1, v1]");
        else {
          uint8_t v[4];
          for (size_t i = 0; i < 4; ++i) v[i] = uint8_t(numToIntClamped(uv->arr[i].num, 0, 16));
          r.uv = {v[0], v[1], v[2], v[3]};
        }
      }
    } else {
      warn_.push_back(where_ + ": a face must be a texture name or an object");
      return std::nullopt;
    }
    if (tex.empty() && anim.empty()) { warn_.push_back(where_ + ": face without texture"); return std::nullopt; }
    if (!anim.empty() || (!reg_.find(tex) && reg_.findAnimation(tex))) {  // a bare name may name an animation
      const std::string& a = anim.empty() ? tex : anim;
      if (const auto id = reg_.findAnimation(a)) { r.animation = *id; r.texture = reg_.animation(*id).firstFrame; }
      else warn_.push_back(where_ + ": unknown animation '" + a + "'");
      return r;
    }
    if (const auto id = reg_.find(tex)) r.texture = *id;
    else warn_.push_back(where_ + ": unknown texture '" + tex + "'");
    return r;
  }

  void parseLayout(const Json& j, FaceLayout& layout) {
    if (j.type != Json::Type::Object) { warn_.push_back(where_ + ": faces must be an object"); return; }
    for (const auto& [face, value] : j.obj) {
      const auto ref = faceRef(value);
      if (!ref) continue;
      if (face == "side") layout.setSide(*ref);
      else if (face == "all") layout.setAll(*ref);
      else if (const auto slot = faceSlotFromName(face)) layout.set(*slot, *ref);
      else warn_.push_back(where_ + ": unknown face '" + face + "'");
    }
  }

  void parseWire(const Json& j, BlockModel& m) {
    if (j.type != Json::Type::Object) { warn_.push_back(where_ + ": wire must be an object"); return; }
    for (const auto& [piece, value] : j.obj) {
      const auto p = wirePieceFromName(piece);
      if (!p) { warn_.push_back(where_ + ": unknown wire piece '" + piece + "'"); continue; }
      m.wire[size_t(*p)] = faceRef(value);
    }
  }

  void parsePalette(const Json& j, BlockModel& m) {
    if (const auto id = reg_.findPalette(j.str); j.type == Json::Type::String && id) m.palette = *id;
    else warn_.push_back(where_ + ": unknown palette '" + j.str + "'");
  }

  void parseMasks(const Json& j, BlockModel& m) {
    if (j.type != Json::Type::Object) { warn_.push_back(where_ + ": masks must be an object"); return; }
    for (const auto& [role, value] : j.obj) {
      const auto id = reg_.find(value.str);
      if (!id) warn_.push_back(where_ + ": unknown mask texture '" + value.str + "'");
      m.masks[role] = id.value_or(TextureId{});
    }
  }

  const TextureRegistry& reg_;
  std::vector<std::string>& warn_;
  std::string where_;
};

// --- Litho certificate (provenance) -------------------------------------------
const uint32_t* crc32Table() {
  // Magic static: thread-safe one-time init (the old bool-flag version was a
  // data race when two threads first called crc32 concurrently).
  static const auto t = [] {
    std::array<uint32_t, 256> tab{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      tab[i] = c;
    }
    return tab;
  }();
  return t.data();
}

std::string toHex8(uint32_t v) {
  char b[11];
  std::snprintf(b, sizeof b, "0x%08x", v);
  return b;
}

// Parses the "crc32" marker field: "0x..." hex string, plain hex string, or JSON number.
bool parseCrc32Field(const Json& c, uint32_t& out, std::string& err) {
  if (c.type == Json::Type::Number) {
    const double d = c.num;
    if (d < 0 || d > 4294967295.0 || d != double(uint32_t(d))) {
      err = "crc32 number out of range"; return false;
    }
    out = uint32_t(d); return true;
  }
  if (c.type != Json::Type::String) { err = "crc32 must be a hex string or number"; return false; }
  std::string s = c.str;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s = s.substr(2);
  if (s.empty() || s.size() > 8) { err = "bad crc32 hex '" + c.str + "'"; return false; }
  uint32_t v = 0;
  for (char ch : s) {
    v <<= 4;
    if (ch >= '0' && ch <= '9') v |= uint32_t(ch - '0');
    else if (ch >= 'a' && ch <= 'f') v |= uint32_t(ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F') v |= uint32_t(ch - 'A' + 10);
    else { err = "bad crc32 hex '" + c.str + "'"; return false; }
  }
  out = v; return true;
}

// Source file bytes, resolved with the same rules as loadImage.
bool readSourceBytes(const PackStack& stack, const std::string& rel, std::vector<uint8_t>& out,
                     std::string& err) {
  if (!isSafePackPath(rel)) { err = "unsafe path '" + rel + "'"; return false; }
  const ResourcePack* p = stack.owner(rel);
  if (!p) { err = "file not found: " + rel; return false; }
  const auto checked = p->checkedPath(rel);  // TOCTOU-checked path (E L6)
  if (!checked) { err = "file not found: " + rel; return false; }
  std::ifstream f(*checked, std::ios::binary);
  if (!f) { err = "cannot read: " + rel; return false; }
  // Cap enforced during the read (P3-3, Appendix E M3): provenance sources
  // are texture files, bounded by the same 64 MiB as the image decoders.
  constexpr size_t kCap = size_t(64) << 20;
  out.clear();
  std::array<char, 64 << 10> chunk;
  while (f) {
    f.read(chunk.data(), std::streamsize(chunk.size()));
    const std::streamsize n = f.gcount();
    if (n > 0) out.insert(out.end(), chunk.data(), chunk.data() + n);
    if (out.size() > kCap) { err = "file too large: " + rel; return false; }
  }
  return !f.bad();
}

}  // namespace

size_t SvgRasterCache::KeyHash::operator()(const Key& k) const noexcept {
  size_t h = std::hash<std::string>{}(k.path);
  h ^= std::hash<int>{}(k.tile) + 0x9e3779b9 + (h << 6) + (h >> 2);
  h ^= std::hash<uintmax_t>{}(k.size) + 0x9e3779b9 + (h << 6) + (h >> 2);
  // mtime joins the hash (P3-3, E L7): equality already compared it, but a
  // hash that omits it clusters every revision of a file into one bucket.
  h ^= std::hash<long long>{}(static_cast<long long>(k.mtime.time_since_epoch().count())) +
       0x9e3779b9 + (h << 6) + (h >> 2);
  return h;
}

SvgRasterCache::Key SvgRasterCache::makeKey(const std::string& absPath, int tileSize) {
  SvgRasterCache::Key k;
  // Canonical key (P3-3, E L7): the same file reached via different
  // relative spellings (or through a symlinked pack dir) keys identically.
  // A file that cannot be canonicalised (missing) keeps its given path —
  // it will miss the cache and fail at load time as before.
  std::error_code ec;
  const auto canon = std::filesystem::canonical(absPath, ec);
  k.path = ec ? absPath : canon.generic_string();
  k.tile = tileSize;
  k.size = std::filesystem::file_size(absPath, ec);
  k.mtime = std::filesystem::last_write_time(absPath, ec);
  return k;
}

std::optional<Image> SvgRasterCache::fetch(const std::string& absPath, int tileSize) {
  const Key key = makeKey(absPath, tileSize);
  std::lock_guard<std::mutex> lock(mutex_);
  const auto it = map_.find(key);
  if (it == map_.end()) return std::nullopt;
  lru_.splice(lru_.begin(), lru_, it->second.lru);  // mark most-recent
  return it->second.image;
}

void SvgRasterCache::store(const std::string& absPath, int tileSize, Image img) {
  const Key key = makeKey(absPath, tileSize);
  std::lock_guard<std::mutex> lock(mutex_);
  if (const auto it = map_.find(key); it != map_.end()) {
    it->second.image = std::move(img);
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return;
  }
  lru_.push_front(key);
  map_.emplace(key, Entry{std::move(img), lru_.begin()});
  while (map_.size() > kMaxEntries) {  // bounded: evict least-recently used
    map_.erase(lru_.back());
    lru_.pop_back();
  }
}

void SvgRasterCache::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  map_.clear();
  lru_.clear();
}

size_t SvgRasterCache::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return map_.size();
}

void clearSharedSvgRasterCache() { sharedSvgCache().clear(); }

uint32_t lithoCrc32(std::span<const uint8_t> bytes) {
  const uint32_t* t = crc32Table();
  uint32_t crc = 0xFFFFFFFFu;
  for (uint8_t b : bytes) crc = t[(crc ^ b) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

Provenance verifyTextureProvenance(const PackStack& stack, const std::string& rel,
                                  const Json& def, std::string& err) {
  const Json* p = def.get("provenance");
  if (!p) return Provenance::Absent;
  if (p->type != Json::Type::Object) { err = "provenance must be an object"; return Provenance::Mismatch; }
  const std::string origin = p->getStr("origin");
  if (origin != "litho-generated") { err = "unknown origin '" + origin + "'"; return Provenance::Mismatch; }
  const Json* c = p->get("crc32");
  uint32_t want = 0;
  if (!c || !parseCrc32Field(*c, want, err)) {
    if (err.empty()) err = "missing crc32";
    return Provenance::Mismatch;
  }
  std::vector<uint8_t> bytes;
  if (!readSourceBytes(stack, rel, bytes, err)) return Provenance::Mismatch;
  const uint32_t got = lithoCrc32(bytes);
  if (got != want) {
    err = "crc32 mismatch: marker " + toHex8(want) + ", source " + toHex8(got);
    return Provenance::Mismatch;
  }
  return Provenance::Verified;
}

ResourceSet loadResources(const PackStack& stack) {
  std::vector<std::string> warn;
  const Merged m = mergeManifests(stack, warn);
  ResourceSet rs{TextureRegistry(m.tileSize), {}, {}};
  TextureRegistry& reg = rs.textures;
  std::string err, origin;

  for (const auto& [name, def] : m.textures) {
    const std::string rel = fileOf(*def.json);
    auto img = loadImage(stack, rel, m.tileSize, origin, err);
    if (!img) {
      warn.push_back("texture '" + name + "': " + err);
      reg.add(name, Image{}, def.pack, nullptr);  // remembered as missing
    } else if (!reg.add(name, std::move(*img), origin, &err)) {
      warn.push_back("texture " + err);
    } else if (def.json->type == Json::Type::Object && def.json->get("provenance")) {
      // Litho certificate: a bad marker only re-labels the texture as third-party.
      // Loading already succeeded -- provenance never gates it.
      std::string verr;
      if (verifyTextureProvenance(stack, rel, *def.json, verr) == Provenance::Mismatch)
        warn.push_back("texture '" + name + "': provenance " + verr + " -- treated as third-party");
    }
  }
  for (const auto& [name, def] : m.animations) {
    const std::string rel = fileOf(*def.json);
    auto img = loadImage(stack, rel, m.tileSize, origin, err);
    if (!img) { warn.push_back("animation '" + name + "': " + err); continue; }
    // Provenance markers are honoured on animations and palettes too
    // (P3-3, Appendix E L8 — previously textures only, other sections'
    // markers were silently ignored). Still labelling only, never gating.
    if (def.json->type == Json::Type::Object && def.json->get("provenance")) {
      std::string verr;
      if (verifyTextureProvenance(stack, rel, *def.json, verr) == Provenance::Mismatch)
        warn.push_back("animation '" + name + "': provenance " + verr + " -- treated as third-party");
    }
    AnimationTiming timing;
    if (const auto side = stack.readText(rel + ".json")) {  // sidecar first, inline values override
      Json sj;
      if (Json::parse(*side, sj, &err)) timing = parseTiming(&sj, timing);
      else warn.push_back("animation '" + name + "': sidecar " + rel + ".json: " + err);
    }
    timing = parseTiming(def.json->type == Json::Type::Object ? def.json : nullptr, timing);
    if (!reg.addAnimation(name, *img, std::move(timing), origin, &err)) warn.push_back("animation " + err);
  }
  for (const auto& [name, def] : m.palettes) {
    const std::string rel = fileOf(*def.json);
    auto img = loadImage(stack, rel, m.tileSize, origin, err);
    if (!img) { warn.push_back("palette '" + name + "': " + err); continue; }
    if (def.json->type == Json::Type::Object && def.json->get("provenance")) {
      std::string verr;
      if (verifyTextureProvenance(stack, rel, *def.json, verr) == Provenance::Mismatch)
        warn.push_back("palette '" + name + "': provenance " + verr + " -- treated as third-party");
    }
    reg.addPalette(name, *img, origin);
  }
  ModelParser parser(reg, warn);
  for (const auto& [name, def] : m.models) rs.models.put(parser.parse(name, def));
  rs.warnings = std::move(warn);
  return rs;
}

}  // namespace litholattice
