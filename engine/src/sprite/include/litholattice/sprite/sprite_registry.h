// SpriteRegistry: the concrete ISpriteRegistry. One registry serves the
// whole process; defs are keyed by id in ordered maps, so iteration (and
// therefore atlas packing) is deterministic by registration order.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "litholattice/sprite/sprite.h"

namespace litholattice::sprite {

class SpriteRegistry final : public ISpriteRegistry {
 public:
  /// `atlasPageSize` is the edge of the (square) atlas pages: rounded up
  /// to a power of two, minimum 64; <= 0 selects the default 1024. A frame
  /// larger than the page gets a dedicated page of its own exact size.
  explicit SpriteRegistry(int atlasPageSize = 1024);

  uint32_t registerPng(const SpriteDef& def, std::span<const uint8_t> pngBytes,
                       std::string* err = nullptr) override;
  uint32_t registerPngFile(const SpriteDef& def, const std::filesystem::path& file,
                           std::string* err = nullptr) override;
  uint32_t registerSvg(const SpriteDef& def, std::string_view svgText,
                       std::string* err = nullptr) override;
  uint32_t registerImage(const SpriteDef& def, Image image, std::string* err = nullptr) override;

  uint32_t findSprite(std::string_view name) const override;
  const SpriteDef* spriteDef(uint32_t spriteId) const override;
  size_t spriteCount() const override { return defs_.size(); }

  void ensureAtlas() override;
  void rebuildAtlas() override;
  bool atlasDirty() const override { return atlasDirty_; }
  uint64_t atlasVersion() const override { return atlasVersion_; }
  std::span<const Image> atlasPages() const override { return pages_; }
  const SpriteUvRect* frameRect(uint32_t spriteId, uint32_t frame) const override;

  int atlasPageSize() const { return pageSize_; }

 private:
  struct OwnedDef {
    SpriteDef def;             ///< def.name points into `name` below
    std::string name;          ///< owned copy: callers may pass temporary strings
    std::vector<Image> frames;  ///< resolved per-frame images (frame strip split)
  };
  /// Shared tail of all four register* entry points: validates the def,
  /// applies first-wins, assigns the id, stores the resolved frames.
  /// `image` is the resolved full image (strip not yet split);
  /// `singleFrame` rejects def.frameCount > 1 (SVG sources). Returns the
  /// sprite id, or 0 with `err` set.
  uint32_t adoptImage(const SpriteDef& def, Image image, bool singleFrame, std::string* err);
  static bool fail(std::string* err, const std::string& why) {
    if (err) *err = why;
    return false;
  }

  std::map<uint32_t, OwnedDef> defs_;       // sprite id -> def (id order)
  std::map<std::string, uint32_t> byName_;  // name -> sprite id
  uint32_t nextId_ = 1;                     // 0 is the "no sprite" sentinel

  int pageSize_;
  std::vector<Image> pages_;
  std::map<uint32_t, std::vector<SpriteUvRect>> rects_;  // sprite id -> per-frame rects
  bool atlasDirty_ = true;  // nothing built yet
  uint64_t atlasVersion_ = 0;
};

}  // namespace litholattice::sprite
