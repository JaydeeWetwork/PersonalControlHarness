// Box models: small 3D block models (torches, switches, other attachables) made of axis-aligned
// boxes. The JSON is a strict subset of glTF concepts so a glTF importer is mechanical later:
//   {"asset": {"version": "ll-box-1"},
//    "materials": [{"name": "body", "slot": "top"}],          // material -> the block's face slot
//    "meshes": [{"name": "stick", "boxes": [{"min": [7,0,7], "max": [9,10,9], "material": 0}]}],
//    "nodes": [{"mesh": 0, "translation": [0,0,0], "rotation": [0,0,0,1]}]}
// Units are 1/16 block. A model is authored standing on its support (support below, -Y);
// orientedBoxes() turns it toward the actual attach face. Node rotations must be quarter turns
// about Y (quaternion), scale is not supported (glTF "scale" must be absent or [1,1,1]).
#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "litholattice/assets/block_model.h"

namespace litholattice {

struct BoxModel {
  struct Material { std::string name; FaceSlot slot = FaceSlot::Top; };
  struct Box { int min[3] = {0, 0, 0}, max[3] = {16, 16, 16}; int material = 0; };
  struct Mesh { std::string name; std::vector<Box> boxes; };
  struct Node { int mesh = -1; int translation[3] = {0, 0, 0}; int quarterTurnsY = 0; };
  std::vector<Material> materials;
  std::vector<Mesh> meshes;
  std::vector<Node> nodes;

  /// nullopt + `err` on malformed JSON or anything outside the subset.
  static std::optional<BoxModel> parse(const std::string& json, std::string* err);
  /// Every node's boxes in cell space for attach face `attach` (stAttach: 1 support below,
  /// 2 above, 3 -Z, 4 +Z, 5 -X, 6 +X; 0 is treated as 1), each with min < max per axis.
  std::vector<Box> orientedBoxes(int attach) const;
};

}  // namespace litholattice
