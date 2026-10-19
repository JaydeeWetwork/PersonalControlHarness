#include "litholattice/assets/texture_registry.h"
#include <algorithm>

namespace litholattice {

TextureRegistry::TextureRegistry(int tileSize) : tile_(tileSize) {
  textures_.push_back({"missing", "", Image::checker(tileSize)});
  byName_.emplace("missing", uint16_t{0});
}

std::optional<TextureId> TextureRegistry::add(std::string name, Image image, std::string origin, std::string* err) {
  auto fail = [&](std::string why) -> std::optional<TextureId> {
    if (err) *err = "'" + name + "': " + why;
    if (byName_.emplace(name, uint16_t{0}).second) missing_.push_back(name);
    return std::nullopt;
  };
  if (byName_.contains(name)) return fail("defined twice");
  if (image.width() != tile_ || image.height() != tile_)
    return fail("is " + std::to_string(image.width()) + "x" + std::to_string(image.height()) + ", expected " +
                std::to_string(tile_) + "x" + std::to_string(tile_));
  if (int(textures_.size()) >= kMaxLayers) return fail("texture array full (" + std::to_string(kMaxLayers) + " layers)");
  const TextureId id{uint16_t(textures_.size())};
  byName_.emplace(name, id.value);
  textures_.push_back({std::move(name), std::move(origin), std::move(image)});
  return id;
}

std::optional<AnimationId> TextureRegistry::addAnimation(std::string name, const Image& strip, AnimationTiming timing,
                                                         std::string origin, std::string* err) {
  if (strip.width() != tile_ || strip.height() % tile_ != 0 || strip.height() == 0) {
    if (err) *err = "'" + name + "': animation strip must be " + std::to_string(tile_) + " wide and a multiple of " +
                    std::to_string(tile_) + " tall";
    return std::nullopt;
  }
  const int frames = strip.height() / tile_;
  if (int(textures_.size()) + frames > kMaxLayers) {
    if (err) *err = "'" + name + "': texture array full";
    return std::nullopt;
  }
  if (animByName_.contains(name)) {
    if (err) *err = "'" + name + "': animation defined twice";
    return std::nullopt;
  }
  if (timing.sequence.empty())
    for (int i = 0; i < frames; ++i) timing.sequence.push_back(i);
  std::erase_if(timing.sequence, [&](int f) { return f < 0 || f >= frames; });
  timing.frameMs = std::max(1, timing.frameMs);
  const TextureId first{uint16_t(textures_.size())};
  for (int i = 0; i < frames; ++i) {
    std::string frameName = name + "#" + std::to_string(i);
    byName_.emplace(frameName, uint16_t(textures_.size()));
    textures_.push_back({std::move(frameName), origin, strip.rows(i * tile_, tile_)});
  }
  const AnimationId id{uint16_t(animations_.size())};
  animByName_.emplace(name, id.value);
  animations_.push_back({std::move(name), std::move(origin), first, frames, std::move(timing)});
  return id;
}

PaletteId TextureRegistry::addPalette(std::string name, const Image& image, std::string origin) {
  Palette p{std::move(name), std::move(origin), {}};
  for (int x = 0; x < image.width() && image.height() > 0; ++x) {
    const uint8_t* c = image.pixel(x, 0);
    p.colors.push_back({c[0], c[1], c[2], c[3]});
  }
  if (const auto it = paletteByName_.find(p.name); it != paletteByName_.end()) {
    palettes_[it->second] = std::move(p);
    return PaletteId{it->second};
  }
  const PaletteId id{uint16_t(palettes_.size())};
  paletteByName_.emplace(p.name, id.value);
  palettes_.push_back(std::move(p));
  return id;
}

std::optional<TextureId> TextureRegistry::find(std::string_view name) const {
  const auto it = byName_.find(std::string(name));
  return it == byName_.end() ? std::nullopt : std::optional(TextureId{it->second});
}

std::optional<PaletteId> TextureRegistry::findPalette(std::string_view name) const {
  const auto it = paletteByName_.find(std::string(name));
  return it == paletteByName_.end() ? std::nullopt : std::optional(PaletteId{it->second});
}

std::optional<AnimationId> TextureRegistry::findAnimation(std::string_view name) const {
  const auto it = animByName_.find(std::string(name));
  return it == animByName_.end() ? std::nullopt : std::optional(AnimationId{it->second});
}

int TextureRegistry::paletteWidth() const {
  size_t w = 1;
  for (const Palette& p : palettes_) w = std::max(w, p.colors.size());
  return int(w);
}

std::string RegistryDiff::summary(size_t maxNames) const {
  if (empty()) return "no texture changes";
  std::string s;
  auto part = [&](const std::vector<std::string>& names, const char* what) {
    if (names.empty()) return;
    if (!s.empty()) s += ", ";
    s += std::to_string(names.size()) + " " + what + " (";
    for (size_t i = 0; i < names.size() && i < maxNames; ++i) s += (i ? " " : "") + names[i];
    if (names.size() > maxNames) s += " ...";
    s += ")";
  };
  part(changed, "changed");
  part(added, "added");
  part(removed, "removed");
  return s;
}

RegistryDiff diffRegistries(const TextureRegistry& before, const TextureRegistry& after) {
  RegistryDiff d;
  std::unordered_map<std::string, uint64_t> old;
  for (const Texture& t : before.textures()) old.emplace(t.name, t.image.hash());
  for (const Palette& p : before.palettes()) old.emplace("palette:" + p.name, p.colors.size());
  for (const Texture& t : after.textures()) {
    const auto it = old.find(t.name);
    if (it == old.end()) d.added.push_back(t.name);
    else {
      if (it->second != t.image.hash()) d.changed.push_back(t.name);
      old.erase(it);
    }
  }
  for (const Palette& p : after.palettes()) {
    const std::string key = "palette:" + p.name;
    const auto it = old.find(key);
    if (it == old.end()) d.added.push_back(key);
    else {
      const auto bp = before.findPalette(p.name);
      if (bp && before.palette(*bp).colors != p.colors) d.changed.push_back(key);
      old.erase(it);
    }
  }
  for (const auto& [name, h] : old) d.removed.push_back(name);
  std::sort(d.removed.begin(), d.removed.end());
  return d;
}

}  // namespace litholattice
