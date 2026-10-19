// Fragment screenshots gallery: the data model behind the Fragment's
// images/screenshots section (Fragment lore page 5: screenshots). No
// Fragment UI exists yet; this is the model that UI will bind to — stable
// ids, insertion order, embedded PNG bytes, capture metadata, and a binary
// round-trip for the save system. Not a parallel bolt-on: the future
// Fragment page reads straight from FragmentGallery.
// LF line endings.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>  // std::move
#include <vector>

namespace lithokraft {

/// One image in the Fragment's screenshots gallery.
struct FragmentImage {
  std::string id;        ///< unique within the gallery; assigned by add() when empty
  std::string caption;   ///< player caption (may be empty)
  int width = 0;         ///< PNG dimensions
  int height = 0;
  uint64_t tick = 0;     ///< sim world tick at capture (0 when the sim is down)
  double x = 0, y = 0, z = 0;  ///< player position at capture
  std::string dimension;       ///< dimension id at capture
  std::vector<uint8_t> png;    ///< PNG-encoded RGB bytes (see screenshot.h)
};

/// An ordered gallery of Fragment images.
class FragmentGallery {
 public:
  /// Adds an image; assigns a unique id ("img-YYYYMMDD-HHMMSS-nnn") when
  /// img.id is empty. Returns the id the image is stored under. Re-adding
  /// an existing id replaces the entry but keeps its position.
  std::string add(FragmentImage img);
  /// Lookup by id; nullptr when unknown.
  const FragmentImage* get(const std::string& id) const;
  /// Removes an entry; false when the id was unknown.
  bool remove(const std::string& id);
  /// Drops everything.
  void clear();
  size_t size() const { return order_.size(); }
  /// Ids in insertion order (gallery display order).
  const std::vector<std::string>& ids() const { return order_; }

  /// Binary round-trip for save/UI persistence. Format: magic "LFG1",
  /// u16 version (1), u32 count, then per entry: u16 id, u16 caption, i32
  /// w, i32 h, u64 tick, 3x f64 xyz, u16 dimension, u32 pngLen, png bytes.
  /// All integers little-endian.
  std::vector<uint8_t> serialize() const;
  /// Inverse of serialize(). False + `*err` on malformed input; the gallery
  /// is left unchanged on failure.
  bool deserialize(const uint8_t* data, size_t len, std::string* err);

 private:
  std::string makeId();
  std::vector<std::string> order_;
  std::unordered_map<std::string, FragmentImage> images_;
  uint64_t seq_ = 0;
};

}  // namespace lithokraft
