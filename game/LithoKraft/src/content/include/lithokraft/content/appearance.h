// LithoKraft's block looks on top of the engine mesher: wire strips joined to their neighbours,
// lit lamps / latches, colour-lamp palettes, repeater pips, memory-cell values, torch sprites.
#pragma once
#include "lithokraft/engine.h"
#include "litholattice/render/block_appearance.h"

namespace lithokraft {

class LithoKraftAppearance final : public IBlockAppearance {
 public:
  bool active(const BlockDef& def, CellState state) const override;
  FaceLook faceLook(const BlockDef& def, CellState state, FaceSlot slot, bool active) const override;
  FaceLook faceLookAt(const BlockDef& def, CellState state, FaceSlot slot, bool active,
                      int x, int y, int z) const override;
  void meshCustom(const CustomBlock& cell, MeshWriter& out) const override;
  std::optional<FaceMaterial> faceMaterial(const CustomBlock& cell, FaceSlot slot, bool active) const override;
  void meshExtra(const CustomBlock& cell, MeshWriter& out) const override;
  const BoxModel* boxModel(const BlockDef& def) const override;
  void bindMaterials(const BlockDef& def, const BlockModel& model, MaterialBindings& bindings) const override;
};

/// The shared instance (stateless, safe on mesh worker threads).
const IBlockAppearance& blockAppearance();

}  // namespace lithokraft
