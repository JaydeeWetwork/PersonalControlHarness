#include "litholattice/assets/block_model.h"
#include <algorithm>

namespace litholattice {
namespace {

#define LITHOLATTICE_ASSETS_NAME(id, name) name,
constexpr std::string_view kFaceNames[] = {LITHOLATTICE_FACE_SLOTS(LITHOLATTICE_ASSETS_NAME)};
constexpr std::string_view kPieceNames[] = {LITHOLATTICE_WIRE_PIECES(LITHOLATTICE_ASSETS_NAME)};
#undef LITHOLATTICE_ASSETS_NAME

template <typename E, size_t N>
std::optional<E> lookupName(const std::string_view (&names)[N], std::string_view name) {
  const auto it = std::find(std::begin(names), std::end(names), name);
  return it == std::end(names) ? std::nullopt : std::optional(E(it - std::begin(names)));
}

}  // namespace

std::string_view faceSlotName(FaceSlot f) { return kFaceNames[size_t(f)]; }
std::optional<FaceSlot> faceSlotFromName(std::string_view n) { return lookupName<FaceSlot>(kFaceNames, n); }
std::string_view wirePieceName(WirePiece p) { return kPieceNames[size_t(p)]; }
std::optional<WirePiece> wirePieceFromName(std::string_view n) { return lookupName<WirePiece>(kPieceNames, n); }

std::optional<FaceRef> FaceLayout::lookup(FaceSlot f) const {
  if (faces_[size_t(f)]) return faces_[size_t(f)];
  if (side_) return side_;
  return all_;
}

bool FaceLayout::empty() const {
  return !side_ && !all_ && std::none_of(faces_.begin(), faces_.end(), [](const auto& f) { return f.has_value(); });
}

FaceRef BlockModel::face(FaceSlot f, bool isActive) const {
  if (isActive)
    if (const auto r = active.lookup(f)) return *r;
  return base.lookup(f).value_or(FaceRef{});
}

FaceRef BlockModel::wirePiece(WirePiece p) const {
  if (wire[size_t(p)]) return *wire[size_t(p)];
  return wire[size_t(WirePiece::Straight)].value_or(FaceRef{});
}

bool BlockModel::isWire() const {
  return std::any_of(wire.begin(), wire.end(), [](const auto& w) { return w.has_value(); });
}

TextureId BlockModel::mask(std::string_view role) const {
  const auto it = masks.find(role);
  return it == masks.end() ? TextureId{} : it->second;
}

void BlockModelSet::put(BlockModel m) {
  std::string key = m.name;
  models_.insert_or_assign(std::move(key), std::move(m));
}

const BlockModel* BlockModelSet::find(std::string_view name) const {
  const auto it = models_.find(name);
  return it == models_.end() ? nullptr : &it->second;
}

}  // namespace litholattice
