// SpriteRegistry implementation. See sprite.h for the registration rules
// (first-wins by name) and the atlas contract.
#include "litholattice/sprite/sprite_registry.h"

#include <algorithm>
#include <cmath>
#include <fstream>

namespace litholattice::sprite {
namespace {

/// Padding around each atlas cell, in pixels. The pad ring is filled by
/// replicating the frame's edge pixels, so bilinear sampling and mip
/// levels never bleed a neighbour's colours across the border.
constexpr int kPadding = 1;

int roundUpPow2(int v) {
  // Page sizes cap at 8192 (P3-3, Appendix D L7: the cap was silent).
  // Requests above it get 8192 — frames larger than that take the
  // oversize dedicated-page path in rebuildAtlas instead.
  int p = 64;
  while (p < v && p < 8192) p <<= 1;
  return p;
}

/// Copy `src` into a page buffer (pageW pixels wide) with the image's
/// top-left at (imgX, imgY), replicating edge pixels into the padding
/// ring (clamped sampling of the source).
void blitPadded(std::vector<uint8_t>& buf, int pageW, const Image& src, int imgX, int imgY) {
  const uint8_t* spx = src.pixels().data();
  for (int dy = -kPadding; dy < src.height() + kPadding; ++dy) {
    const int sy = std::clamp(dy, 0, src.height() - 1);
    for (int dx = -kPadding; dx < src.width() + kPadding; ++dx) {
      const int sx = std::clamp(dx, 0, src.width() - 1);
      const uint8_t* s = spx + (static_cast<size_t>(sy) * src.width() + sx) * 4;
      uint8_t* d = buf.data() + (static_cast<size_t>(imgY + dy) * pageW + (imgX + dx)) * 4;
      d[0] = s[0];
      d[1] = s[1];
      d[2] = s[2];
      d[3] = s[3];
    }
  }
}

}  // namespace

SpriteRegistry::SpriteRegistry(int atlasPageSize)
    : pageSize_(atlasPageSize <= 0 ? 1024 : roundUpPow2(atlasPageSize)) {}

uint32_t SpriteRegistry::findSprite(std::string_view name) const {
  const auto it = byName_.find(std::string(name));
  return it == byName_.end() ? 0 : it->second;
}

const SpriteDef* SpriteRegistry::spriteDef(uint32_t spriteId) const {
  const auto it = defs_.find(spriteId);
  return it == defs_.end() ? nullptr : &it->second.def;
}

uint32_t SpriteRegistry::adoptImage(const SpriteDef& def, Image image, bool singleFrame,
                                    std::string* err) {
  // Def validation (fail closed, like the player registries).
  if (!def.name || def.name[0] == '\0') {
    fail(err, "sprite def has no name");
    return 0;
  }
  // First-wins by name: a duplicate registration returns the existing id
  // and keeps the original def + image, whatever the new source holds.
  if (const uint32_t existing = findSprite(def.name)) return existing;
  if (!(def.widthBlocks > 0.f) || !(def.heightBlocks > 0.f) || !std::isfinite(def.widthBlocks) ||
      !std::isfinite(def.heightBlocks)) {
    fail(err, std::string("sprite '") + def.name + "': world size must be positive and finite");
    return 0;
  }
  if (def.frameCount == 0) {
    fail(err, std::string("sprite '") + def.name + "': frameCount is 0");
    return 0;
  }
  if (singleFrame && def.frameCount != 1) {
    fail(err, std::string("sprite '") + def.name + "': SVG sources are single-frame");
    return 0;
  }
  if (image.empty() || image.width() <= 0 || image.height() <= 0) {
    fail(err, std::string("sprite '") + def.name + "': source image is empty");
    return 0;
  }
  if (def.frameCount > 1 && image.height() % static_cast<int>(def.frameCount) != 0) {
    fail(err, std::string("sprite '") + def.name + "': strip height " +
                  std::to_string(image.height()) + " is not divisible by frameCount " +
                  std::to_string(def.frameCount));
    return 0;
  }
  uint32_t id = def.id;
  if (id == 0) {
    id = nextId_;
  } else if (defs_.count(id)) {
    fail(err, std::string("sprite '") + def.name + "': explicit id " + std::to_string(id) +
                  " is taken by another sprite");
    return 0;
  }
  if (id >= nextId_) nextId_ = id + 1;

  OwnedDef owned;
  owned.name = def.name;
  owned.def = def;
  owned.def.id = id;
  owned.def.name = owned.name.c_str();
  if (owned.def.ticksPerFrame == 0) owned.def.ticksPerFrame = 1;
  // Resolve the frame strip now: frame i is rows [i*fh, (i+1)*fh).
  if (def.frameCount == 1) {
    owned.frames.push_back(std::move(image));
  } else {
    const int fh = image.height() / static_cast<int>(def.frameCount);
    owned.frames.reserve(def.frameCount);
    for (uint32_t f = 0; f < def.frameCount; ++f)
      owned.frames.push_back(image.rows(static_cast<int>(f) * fh, fh));
  }
  const auto inserted = defs_.emplace(id, std::move(owned)).first;
  // Rebind def.name to the STORED string: for short names the string's
  // buffer lives inside the string object (SSO), so the pointer captured
  // before the move would dangle into the moved-from temporary.
  inserted->second.def.name = inserted->second.name.c_str();
  byName_[inserted->second.name] = id;
  atlasDirty_ = true;
  return id;
}

uint32_t SpriteRegistry::registerPng(const SpriteDef& def, std::span<const uint8_t> pngBytes,
                                     std::string* err) {
  if (def.name && def.name[0] != '\0')
    if (const uint32_t existing = findSprite(def.name)) return existing;  // no decode on duplicate
  std::string derr;
  std::optional<Image> img = Image::decode(pngBytes, &derr);
  if (!img) {
    fail(err, std::string("sprite '") + (def.name ? def.name : "?") + "': PNG decode failed: " + derr);
    return 0;
  }
  return adoptImage(def, std::move(*img), /*singleFrame=*/false, err);
}

uint32_t SpriteRegistry::registerPngFile(const SpriteDef& def, const std::filesystem::path& file,
                                         std::string* err) {
  if (def.name && def.name[0] != '\0')
    if (const uint32_t existing = findSprite(def.name)) return existing;
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    fail(err, std::string("sprite '") + (def.name ? def.name : "?") + "': cannot open " +
                  file.string());
    return 0;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return registerPng(def, bytes, err);
}

uint32_t SpriteRegistry::registerSvg(const SpriteDef& def, std::string_view svgText,
                                     std::string* err) {
  if (def.name && def.name[0] != '\0')
    if (const uint32_t existing = findSprite(def.name)) return existing;
  if (def.pixelWidth <= 0 || def.pixelHeight <= 0) {
    fail(err, std::string("sprite '") + (def.name ? def.name : "?") +
                  "': pixelWidth/pixelHeight must be positive for SVG sources");
    return 0;
  }
  const auto bytes = std::span<const uint8_t>(
      reinterpret_cast<const uint8_t*>(svgText.data()), svgText.size());
  std::string derr;
  std::optional<Image> img = Image::decodeSvg(bytes, def.pixelWidth, def.pixelHeight, &derr);
  if (!img) {
    fail(err, std::string("sprite '") + (def.name ? def.name : "?") + "': SVG rasterize failed: " +
                  derr);
    return 0;
  }
  return adoptImage(def, std::move(*img), /*singleFrame=*/true, err);
}

uint32_t SpriteRegistry::registerImage(const SpriteDef& def, Image image, std::string* err) {
  return adoptImage(def, std::move(image), /*singleFrame=*/false, err);
}

void SpriteRegistry::ensureAtlas() {
  if (atlasDirty_) rebuildAtlas();
}

void SpriteRegistry::rebuildAtlas() {
  pages_.clear();
  rects_.clear();

  // Pages are accumulated as raw buffers and converted to Images at the
  // end (Image's pixel storage is write-once by design).
  std::vector<std::vector<uint8_t>> bufs;
  std::vector<int> bufW, bufH;
  auto openBuf = [&](int w, int h) {
    bufs.emplace_back(static_cast<size_t>(w) * h * 4, 0);
    bufW.push_back(w);
    bufH.push_back(h);
    return static_cast<int>(bufs.size()) - 1;
  };

  int page = -1;   // buffer index of the current packing page
  int cursorX = 0; // next free x on the current shelf
  int shelfY = 0;  // top of the current shelf
  int shelfH = 0;  // tallest cell on the current shelf
  auto openPage = [&]() {
    page = openBuf(pageSize_, pageSize_);
    cursorX = 0;
    shelfY = 0;
    shelfH = 0;
  };

  // std::map iteration = ascending id = registration order for sequential
  // ids; the packing is a pure function of the def set, so it is
  // deterministic across runs and rebuilds.
  for (auto& [id, owned] : defs_) {
    std::vector<SpriteUvRect>& rects = rects_[id];
    rects.reserve(owned.frames.size());
    for (const Image& frame : owned.frames) {
      const int cellW = frame.width() + 2 * kPadding;
      const int cellH = frame.height() + 2 * kPadding;
      SpriteUvRect rect;
      if (cellW > pageSize_ || cellH > pageSize_) {
        // Oversize frame: a dedicated page of exactly the cell size.
        // Packing then RESUMES on the page that was current before it
        // (P3-3, Appendix D L7: the old code reset page=-1, abandoning
        // the rest of the current page after every oversize frame —
        // correct but wasteful). Buffer indices are stable across
        // openBuf (buffers are only appended), so saving the packing
        // state is enough; order stays a pure function of the def set.
        const int savedPage = page, savedX = cursorX, savedY = shelfY, savedH = shelfH;
        const int dedicated = openBuf(cellW, cellH);
        blitPadded(bufs[dedicated], cellW, frame, kPadding, kPadding);
        rect.page = dedicated;
        rect.u0 = static_cast<float>(kPadding) / cellW;
        rect.v0 = static_cast<float>(kPadding) / cellH;
        rect.u1 = static_cast<float>(kPadding + frame.width()) / cellW;
        rect.v1 = static_cast<float>(kPadding + frame.height()) / cellH;
        page = savedPage;
        cursorX = savedX;
        shelfY = savedY;
        shelfH = savedH;
        rects.push_back(rect);
        continue;
      }
      if (page < 0) openPage();
      if (cursorX + cellW > pageSize_) {  // shelf full: wrap to a new shelf
        cursorX = 0;
        shelfY += shelfH;
        shelfH = 0;
      }
      if (shelfY + cellH > pageSize_) {  // page full: new page
        openPage();
      }
      const int cellX = cursorX, cellY = shelfY;
      blitPadded(bufs[page], pageSize_, frame, cellX + kPadding, cellY + kPadding);
      rect.page = page;
      rect.u0 = static_cast<float>(cellX + kPadding) / pageSize_;
      rect.v0 = static_cast<float>(cellY + kPadding) / pageSize_;
      rect.u1 = static_cast<float>(cellX + kPadding + frame.width()) / pageSize_;
      rect.v1 = static_cast<float>(cellY + kPadding + frame.height()) / pageSize_;
      cursorX += cellW;
      shelfH = std::max(shelfH, cellH);
      rects.push_back(rect);
    }
  }
  pages_.reserve(bufs.size());
  for (size_t i = 0; i < bufs.size(); ++i)
    pages_.push_back(Image(bufW[i], bufH[i], std::move(bufs[i])));
  atlasDirty_ = false;
  ++atlasVersion_;
}

const SpriteUvRect* SpriteRegistry::frameRect(uint32_t spriteId, uint32_t frame) const {
  const auto it = rects_.find(spriteId);
  if (it == rects_.end() || frame >= it->second.size()) return nullptr;
  return &it->second[frame];
}

}  // namespace litholattice::sprite
