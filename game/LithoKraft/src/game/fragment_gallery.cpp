// Fragment screenshots gallery: the data model behind the Fragment's
// images/screenshots section (lore page 5). LF line endings.
#include "lithokraft/game/fragment_gallery.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace lithokraft {
namespace {

constexpr char kMagic[4] = {'L', 'F', 'G', '1'};
constexpr uint16_t kVersion = 1;

void putU16(std::vector<uint8_t>& o, uint16_t v) {
  o.push_back(uint8_t(v));
  o.push_back(uint8_t(v >> 8));
}
void putU32(std::vector<uint8_t>& o, uint32_t v) {
  for (int i = 0; i < 4; ++i) o.push_back(uint8_t(v >> (8 * i)));
}
void putU64(std::vector<uint8_t>& o, uint64_t v) {
  for (int i = 0; i < 8; ++i) o.push_back(uint8_t(v >> (8 * i)));
}
void putI32(std::vector<uint8_t>& o, int32_t v) { putU32(o, uint32_t(v)); }
void putF64(std::vector<uint8_t>& o, double v) {
  uint64_t u = 0;
  std::memcpy(&u, &v, 8);
  putU64(o, u);
}
void putStr(std::vector<uint8_t>& o, const std::string& s) {
  putU16(o, uint16_t(s.size()));  // captions/ids are short; 64 KiB is plenty
  o.insert(o.end(), s.begin(), s.end());
}

struct Reader {
  const uint8_t* p;
  size_t n;
  bool ok = true;
  uint8_t u8() {
    if (n < 1) { ok = false; return 0; }
    const uint8_t v = *p++;
    --n;
    return v;
  }
  uint16_t u16() { const uint16_t a = u8(), b = u8(); return uint16_t(a | (b << 8)); }
  uint32_t u32() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= uint32_t(u8()) << (8 * i);
    return v;
  }
  uint64_t u64() {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(u8()) << (8 * i);
    return v;
  }
  int32_t i32() { return int32_t(u32()); }
  double f64() {
    uint64_t u = u64();
    double v = 0;
    std::memcpy(&v, &u, 8);
    return v;
  }
  std::string str() {
    const uint16_t len = u16();
    if (n < len) { ok = false; return {}; }
    std::string s(reinterpret_cast<const char*>(p), len);
    p += len;
    n -= len;
    return s;
  }
  std::vector<uint8_t> bytes() {
    const uint32_t len = u32();
    if (n < len) { ok = false; return {}; }
    std::vector<uint8_t> v(p, p + len);
    p += len;
    n -= len;
    return v;
  }
};

}  // namespace

std::string FragmentGallery::makeId() {
  const std::time_t t =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[64];
  std::snprintf(buf, sizeof buf, "img-%04d%02d%02d-%02d%02d%02d-%03llu",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, (unsigned long long)(++seq_ % 1000));
  std::string id = buf;
  // seq_ keeps ids unique even within the same second; belt and suspenders
  // for a re-added id (add() replaces rather than duplicating anyway).
  while (images_.count(id)) {
    std::snprintf(buf, sizeof buf, "img-%04d%02d%02d-%02d%02d%02d-%03llu",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec, (unsigned long long)(++seq_ % 1000));
    id = buf;
  }
  return id;
}

std::string FragmentGallery::add(FragmentImage img) {
  if (img.id.empty()) img.id = makeId();
  const std::string id = img.id;
  if (!images_.count(id)) order_.push_back(id);
  images_[id] = std::move(img);
  return id;
}

const FragmentImage* FragmentGallery::get(const std::string& id) const {
  const auto it = images_.find(id);
  return it == images_.end() ? nullptr : &it->second;
}

bool FragmentGallery::remove(const std::string& id) {
  const auto it = images_.find(id);
  if (it == images_.end()) return false;
  images_.erase(it);
  for (auto o = order_.begin(); o != order_.end(); ++o) {
    if (*o == id) {
      order_.erase(o);
      break;
    }
  }
  return true;
}

void FragmentGallery::clear() {
  order_.clear();
  images_.clear();
}

std::vector<uint8_t> FragmentGallery::serialize() const {
  std::vector<uint8_t> o;
  o.insert(o.end(), kMagic, kMagic + 4);
  putU16(o, kVersion);
  putU32(o, uint32_t(order_.size()));
  for (const std::string& id : order_) {
    const FragmentImage& im = images_.at(id);
    putStr(o, im.id);
    putStr(o, im.caption);
    putI32(o, im.width);
    putI32(o, im.height);
    putU64(o, im.tick);
    putF64(o, im.x);
    putF64(o, im.y);
    putF64(o, im.z);
    putStr(o, im.dimension);
    putU32(o, uint32_t(im.png.size()));
    o.insert(o.end(), im.png.begin(), im.png.end());
  }
  return o;
}

bool FragmentGallery::deserialize(const uint8_t* data, size_t len,
                                  std::string* err) {
  Reader r{data, len};
  for (int i = 0; i < 4; ++i) {
    if (r.u8() != uint8_t(kMagic[i])) {
      if (err) *err = "not a Fragment gallery blob";
      return false;
    }
  }
  if (r.u16() != kVersion) {
    if (err) *err = "unsupported gallery version";
    return false;
  }
  const uint32_t count = r.u32();
  if (count > 100000) {  // sanity: never that many images
    if (err) *err = "gallery count out of range";
    return false;
  }
  FragmentGallery tmp;
  for (uint32_t i = 0; i < count && r.ok; ++i) {
    FragmentImage im;
    im.id = r.str();
    im.caption = r.str();
    im.width = r.i32();
    im.height = r.i32();
    im.tick = r.u64();
    im.x = r.f64();
    im.y = r.f64();
    im.z = r.f64();
    im.dimension = r.str();
    im.png = r.bytes();
    if (!r.ok || im.id.empty()) {
      if (err) *err = "truncated gallery entry";
      return false;
    }
    tmp.add(std::move(im));
  }
  if (!r.ok || r.n != 0) {
    if (err) *err = "trailing or truncated gallery data";
    return false;
  }
  *this = std::move(tmp);
  return true;
}

}  // namespace lithokraft
