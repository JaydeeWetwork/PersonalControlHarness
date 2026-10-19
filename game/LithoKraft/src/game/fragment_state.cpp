// Fragment state: the Fragment book's upgrade state (see fragment_state.h).
// LF line endings.
#include "lithokraft/game/fragment_state.h"

namespace lithokraft {
namespace {

constexpr char kMagic[4] = {'L', 'F', 'S', '1'};
constexpr uint16_t kVersion = 2;  // v2: added u8 tier (v1 never shipped)
constexpr uint8_t kFlagMarkdownUnlocked = 0x01;

void putU16(std::vector<uint8_t>& o, uint16_t v) {
  o.push_back(uint8_t(v));
  o.push_back(uint8_t(v >> 8));
}

}  // namespace

std::vector<uint8_t> FragmentState::serialize() const {
  std::vector<uint8_t> o;
  o.insert(o.end(), kMagic, kMagic + 4);
  putU16(o, kVersion);
  o.push_back(markdown_unlocked_ ? kFlagMarkdownUnlocked : uint8_t(0));
  o.push_back(tier_);
  return o;  // 8 bytes: "LFS1" + u16 version + u8 flags + u8 tier
}

bool FragmentState::deserialize(const uint8_t* data, size_t len,
                                std::string* err) {
  const uint8_t* p = data;
  size_t n = len;
  auto u8 = [&]() -> uint8_t {
    if (n < 1) return 0xFF;  // caller checks n before every read
    const uint8_t v = *p++;
    --n;
    return v;
  };
  for (int i = 0; i < 4; ++i) {
    if (n < 1 || u8() != uint8_t(kMagic[i])) {
      if (err) *err = "not a Fragment state blob";
      return false;
    }
  }
  if (n < 2) {
    if (err) *err = "truncated Fragment state version";
    return false;
  }
  const uint16_t version = uint16_t(u8() | (u8() << 8));
  if (version != kVersion) {
    if (err) *err = "unsupported Fragment state version";
    return false;
  }
  if (n < 1) {
    if (err) *err = "truncated Fragment state flags";
    return false;
  }
  const uint8_t flags = u8();
  if (n < 1) {
    if (err) *err = "truncated Fragment state tier";
    return false;
  }
  const uint8_t tier = u8();
  if (n != 0) {
    if (err) *err = "trailing Fragment state data";
    return false;
  }
  FragmentState tmp;
  tmp.markdown_unlocked_ = (flags & kFlagMarkdownUnlocked) != 0;
  tmp.tier_ = tier;
  *this = tmp;
  return true;
}

}  // namespace lithokraft
