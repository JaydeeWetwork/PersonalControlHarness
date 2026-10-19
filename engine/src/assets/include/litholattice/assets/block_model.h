// Block models: which texture each face of a block uses, per state, as data (resources.json).
//
// Face convention (art spec): faces are named relative to the block's facing. "front" is the
// output side and is drawn along the TOP EDGE of the top texture; the mesher rotates top /
// bottom textures with the facing. A face without its own entry falls back to "side", then
// "all", then the missing texture. The "active" layout (on / lit / pressed) falls back face by
// face to the base layout.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "litholattice/assets/texture_registry.h"

namespace litholattice {

// (name used in resources.json)
#define LITHOLATTICE_FACE_SLOTS(X) X(Top, "top") X(Bottom, "bottom") X(Front, "front") X(Back, "back") X(Left, "left") X(Right, "right")
#define LITHOLATTICE_WIRE_PIECES(X) X(Dot, "dot") X(End, "end") X(Straight, "straight") X(Corner, "corner") X(T, "t") X(Cross, "cross")

#define LITHOLATTICE_ASSETS_ENUMERATOR(id, name) id,
enum class FaceSlot : uint8_t { LITHOLATTICE_FACE_SLOTS(LITHOLATTICE_ASSETS_ENUMERATOR) Count };
/// Wire strip shapes chosen by the mesher from the wire's connections (canonical orientation:
/// End's arm points south, T's missing arm is north; see docs/RESOURCE_PACKS.md).
enum class WirePiece : uint8_t { LITHOLATTICE_WIRE_PIECES(LITHOLATTICE_ASSETS_ENUMERATOR) Count };
#undef LITHOLATTICE_ASSETS_ENUMERATOR
constexpr size_t kFaceSlotCount = size_t(FaceSlot::Count);
constexpr size_t kWirePieceCount = size_t(WirePiece::Count);

std::string_view faceSlotName(FaceSlot f);
std::optional<FaceSlot> faceSlotFromName(std::string_view name);
std::string_view wirePieceName(WirePiece p);
std::optional<WirePiece> wirePieceFromName(std::string_view name);

/// Sub-rectangle of a tile in 1/16 units (0..16); the default is the whole tile.
struct UvRect {
  uint8_t u0 = 0, v0 = 0, u1 = 16, v1 = 16;
  bool operator==(const UvRect&) const = default;
};

/// One face's look: a texture (or an animation's frames) and the UV rect used from it.
struct FaceRef {
  TextureId texture;      ///< first frame when animated
  UvRect uv;
  AnimationId animation;  ///< valid = animated face
  bool operator==(const FaceRef&) const = default;
};

/// Face -> texture table with the side / all fallback.
class FaceLayout {
 public:
  void set(FaceSlot f, FaceRef r) { faces_[size_t(f)] = r; }
  void setSide(FaceRef r) { side_ = r; }
  void setAll(FaceRef r) { all_ = r; }
  /// The face's own entry, else side, else all; nullopt if the layout has none of them.
  std::optional<FaceRef> lookup(FaceSlot f) const;
  bool empty() const;

 private:
  std::array<std::optional<FaceRef>, kFaceSlotCount> faces_{};
  std::optional<FaceRef> side_, all_;
};

struct BlockModel {
  std::string name;               ///< matches BlockDef::tex (e.g. "repeater")
  std::string origin;             ///< pack that defined it
  FaceLayout base, active;        ///< active = on / lit / pressed / storing
  std::array<std::optional<FaceRef>, kWirePieceCount> wire{};  ///< wire models only
  PaletteId palette;              ///< tint ramp (wires, colour lamps)
  std::map<std::string, TextureId, std::less<>> masks;  ///< shader masks by role ("ring", "lamp", ...)

  /// Resolved face: active layout first (if `isActive`), then base, then the missing texture.
  FaceRef face(FaceSlot f, bool isActive) const;
  /// Wire piece, falling back to Straight, then missing.
  FaceRef wirePiece(WirePiece p) const;
  bool isWire() const;
  /// Mask by role, missing texture if absent.
  TextureId mask(std::string_view role) const;
};

/// Models by name (later definitions replace earlier ones: higher packs win).
class BlockModelSet {
 public:
  void put(BlockModel m);
  const BlockModel* find(std::string_view name) const;
  size_t size() const { return models_.size(); }
  const std::map<std::string, BlockModel, std::less<>>& all() const { return models_; }

 private:
  std::map<std::string, BlockModel, std::less<>> models_;
};

}  // namespace litholattice
