#include "litholattice/render/block_materials.h"
#include <algorithm>
#include "litholattice/render/block_appearance.h"

namespace litholattice {

BlockMaterials::BlockMaterials(size_t n) {
  faces_.assign(n, {});
  wire_.assign(n, {});
  swatch_.assign(n, {1.f, 0.f, 1.f});
}

BlockMaterials BlockMaterials::build(const BlockRegistry& reg, const TextureRegistry& textures, const BlockModelSet& models,
                                     std::vector<std::string>* warnings, const IBlockAppearance* appearance) {
  BlockMaterials m(reg.size());
  auto warn = [&](std::string w) { if (warnings) warnings->push_back(std::move(w)); };

  // Animations the shader can play (registry order, up to the shader's table size).
  for (const AnimatedTexture& a : textures.animations()) {
    if (m.animations_.size() == kMaxAnimations || m.frames_.size() + a.timing.sequence.size() > kMaxAnimationFrames) {
      warn("animation '" + a.name + "' exceeds the shader's animation table; drawn static");
      break;
    }
    m.animations_.push_back({int(m.frames_.size()), int(a.timing.sequence.size()), float(a.timing.frameMs)});
    m.frames_.insert(m.frames_.end(), a.timing.sequence.begin(), a.timing.sequence.end());
  }
  auto material = [&](const FaceRef& r) {
    FaceMaterial fm{uint8_t(r.texture.value), r.uv, FaceMaterial::kNotAnimated};
    if (r.animation.valid() && r.animation.value < m.animations_.size()) fm.animation = uint8_t(r.animation.value);
    return fm;
  };

  {
    const auto all = textures.textures();
    for (size_t i = 1; i < all.size() && i < 256; ++i)
      if (!all[i].name.empty()) m.named_.emplace(all[i].name, FaceMaterial{uint8_t(i), {}, FaceMaterial::kNotAnimated});
    if (const FaceMaterial* d = m.named("debug_wire_digits_mask")) m.bindings_.debugDigits = d->layer;
  }
  for (size_t id = 1; id < reg.size(); ++id) {
    const BlockDef def = reg.def(BlockId(id));
    if (def.tex.empty()) continue;
    const BlockModel* model = models.find(def.tex);
    if (!model) {
      warn("no block model for '" + std::string(def.tex) + "' (magenta checker)");
      continue;
    }
    for (size_t f = 0; f < kFaceSlotCount; ++f)
      for (int active = 0; active < 2; ++active)
        m.faces_[id][f][size_t(active)] = material(model->face(FaceSlot(f), active != 0));
    for (size_t p = 0; p < kWirePieceCount; ++p) m.wire_[id][p] = material(model->wirePiece(WirePiece(p)));

    if (appearance) appearance->bindMaterials(def, *model, m.bindings_);

    // HUD swatch.
    if (model->isWire() && model->palette.valid()) {
      const auto& colors = textures.palette(model->palette).colors;
      if (!colors.empty()) {
        const auto& c = colors.back();
        m.swatch_[id] = {c[0] / 255.f, c[1] / 255.f, c[2] / 255.f};
        continue;
      }
    }
    const TextureId top{model->isWire() ? model->wirePiece(WirePiece::Straight).texture : model->face(FaceSlot::Top, true).texture};
    m.swatch_[id] = textures.texture(top).image.averageColor();
  }
  return m;
}

}  // namespace litholattice
