// How the game's blocks look beyond their block model: which model faces a cell state shows, the
// chunk shader's per-face mode, meshing of Shape::Custom blocks and the shader bindings taken
// from the models. The mesher calls it from worker threads, so implementations must be stateless
// (or immutable) and thread-safe. Without one, every block is a plain textured shape.
#pragma once
#include "litholattice/assets/box_model.h"
#include <cstdint>
#include <vector>
#include "litholattice/assets/block_model.h"
#include "litholattice/render/block_materials.h"
#include <optional>
#include "litholattice/render/renderer.h"
#include "litholattice/world/mesh_input.h"

namespace litholattice {

/// Chunk-shader mode and value for one face (see ShadeMode in renderer.h).
struct FaceLook {
  uint8_t mode = kShadeTex;
  uint8_t value = 0;
};

/// A Shape::Custom cell being meshed: its id, state and read access to its 1-cell neighbourhood.
struct CustomBlock {
  const MeshInput& in;
  const BlockRegistry& blocks;
  const BlockMaterials& materials;
  BlockId id = kAir;
  CellState state = 0;
  int x = 0, y = 0, z = 0;  ///< cell inside the sub-chunk (0..15)
  BlockId neighbor(int dx, int dy, int dz) const { return in.id[meshPadIndex(x + 1 + dx, y + 1 + dy, z + 1 + dz)]; }
  CellState neighborState(int dx, int dy, int dz) const { return in.st[meshPadIndex(x + 1 + dx, y + 1 + dy, z + 1 + dz)]; }
  BlockDef def() const { return blocks.def(id); }  // by value: BlockRegistry::def() returns by value
};

/// Writes quads for a custom block (implemented by the mesher).
class MeshWriter {
 public:
  explicit MeshWriter(std::vector<PackedVertex>& out) : out_(out) {}
  /// One axis-aligned face `face` (0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z) of the box [lo, hi) given in
  /// 1/16 block units relative to the cell's corner, textured with `material`, rotated `rot`
  /// quarter turns, without ambient occlusion.
  void face(const CustomBlock& cell, int face, const int lo[3], const int hi[3], const FaceMaterial& material,
            FaceLook look, int rot = 0);

 private:
  std::vector<PackedVertex>& out_;
};

class IBlockAppearance {
 public:
  virtual ~IBlockAppearance() = default;
  /// Whether a cell shows its model's active faces. Default: the standard `on` bit.
  virtual bool active(const BlockDef& /*def*/, CellState state) const { return stOn(state); }
  /// Shader mode of one Cube / Slab face (animated faces are handled by the mesher). Default: plain.
  virtual FaceLook faceLook(const BlockDef& /*def*/, CellState /*state*/, FaceSlot /*slot*/, bool /*active*/) const { return {}; }
  /// Position-aware faceLook: world-space block coords (x, y, z) of the face's
  /// cell. Default delegates to faceLook, so existing implementations are
  /// unaffected. The mesher calls this (not faceLook) for Cube / Slab faces and
  /// box-model faces. Used for looks that vary with position - e.g. the wafer
  /// ground pulse, whose per-face biome id comes from the chunk coords.
  /// Called on mesh worker threads: implementations must stay stateless.
  virtual FaceLook faceLookAt(const BlockDef& def, CellState state, FaceSlot slot, bool active,
                              int /*x*/, int /*y*/, int /*z*/) const {
    return faceLook(def, state, slot, active);
  }
  /// Cube / Slab face material override for this cell (connected textures, state variants). Default: the model face.
  virtual std::optional<FaceMaterial> faceMaterial(const CustomBlock& /*cell*/, FaceSlot /*slot*/, bool /*active*/) const { return std::nullopt; }
  /// Extra quads after a Cube / Slab cell (overlays, readouts). Default: none.
  virtual void meshExtra(const CustomBlock& /*cell*/, MeshWriter& /*out*/) const {}
  /// Meshes a Shape::Custom cell. Default: nothing is drawn.
  virtual void meshCustom(const CustomBlock& /*cell*/, MeshWriter& /*out*/) const {}
  /// Box model of an attachable block (kAttachable with an attach face), or nullptr to keep its
  /// Shape. The mesher orients it toward the support and draws every box face (no culling).
  virtual const BoxModel* boxModel(const BlockDef& /*def*/) const { return nullptr; }
  /// While BlockMaterials are built: fills the shader bindings from block `def`'s model.
  virtual void bindMaterials(const BlockDef& /*def*/, const BlockModel& /*model*/, MaterialBindings& /*bindings*/) const {}
};

}  // namespace litholattice
