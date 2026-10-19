#include "lithokraft/content/appearance.h"
#include <string>
#include "lithokraft/content/blocks.h"
#include "lithokraft/content/chipor_viewport.h"

namespace lithokraft {
namespace {

int tierIndex(int tier) { return tier == 64 ? 2 : tier == 32 ? 1 : 0; }

}  // namespace

const IBlockAppearance& blockAppearance() {
  static const LithoKraftAppearance appearance;
  return appearance;
}

bool LithoKraftAppearance::active(const BlockDef& def, CellState st) const {
  switch (roleOf(def)) {
    case SimRole::Lamp: return (st & 1) != 0;  // lamps keep "lit" in bit 0
    case SimRole::Torch: return stValue(st) != 0;
    default: return stOn(st);
  }
}

FaceLook LithoKraftAppearance::faceLook(const BlockDef& def, CellState st, FaceSlot slot, bool active) const {
  // Glowing trace-tree tips (kTraceTipRed = 40, kTraceTipBlue = 41): always-on
  // emissive, matched by stable block id - they are SimRole::None terrain, so
  // the role switch below would never catch them.
  if (def.id == "lithokraft:trace_tip_red" || def.id == "lithokraft:trace_tip_blue")
    return {kShadeEmissive, 0};
  const bool top = slot == FaceSlot::Top;
  switch (roleOf(def)) {
    case SimRole::ColorLamp:
      if (active) return {uint8_t(kShadePalette16 + tierIndex(def.tier)), uint8_t(stValue(st))};
      break;
    case SimRole::Lamp:
    case SimRole::FastLatch:
      if (active) return {kShadeEmissive, 0};
      break;
    case SimRole::Repeater:
      if (top) return {kShadePips, uint8_t(stAux(st) + 1)};
      break;
    case SimRole::Memor:
      if (top && active) return {kShadeCells, uint8_t(stValue(st))};
      break;
    case SimRole::None:  // glass family: solid, see-through, never powered -> translucent pass
      if ((def.flags & kSolid) && (def.flags & kTransparent)) return {kShadeGlass, 0};
      break;
    default: break;
  }
  return {};
}

// Position-aware look: the Wafer's silicon surface breathes trace energy (the
// ground pulse). The per-face biome blend is baked into the ChunkMesh at mesh
// time (waferPulseParams), so the face only needs the kShadeGroundPulse tag -
// the shader reads the blended params the mesh carries. Dead Zones use dead
// silicon, which never takes this path: unpowered is the point.
FaceLook LithoKraftAppearance::faceLookAt(const BlockDef& def, CellState st, FaceSlot slot, bool active,
                                          int /*x*/, int /*y*/, int /*z*/) const {
  if (def.id == "lithokraft:silicon") return {kShadeGroundPulse, 0};
  return faceLook(def, st, slot, active);
}

// Connected via sides (same-colour via below only = top of a stack, above only = bottom, both =
// mid, none = single; `<via>_side` when a pack lacks them) and the Redirector split faces.
std::optional<FaceMaterial> LithoKraftAppearance::faceMaterial(const CustomBlock& cell, FaceSlot slot, bool active) const {
  const BlockDef def = cell.def();
  const std::string tex(def.tex);
  auto named = [&](const std::string& n) -> std::optional<FaceMaterial> {
    if (const FaceMaterial* m = cell.materials.named(n)) return *m;
    return std::nullopt;
  };
  if (isVia(cell.id) && slot != FaceSlot::Top && slot != FaceSlot::Bottom) {
    const bool up = cell.neighbor(0, 1, 0) == cell.id, dn = cell.neighbor(0, -1, 0) == cell.id;
    return named(tex + (up && dn ? "_side_mid" : dn ? "_side_top" : up ? "_side_bottom" : "_side_single"));
  }
  if (roleOf(def) == SimRole::Redirector && (stAux(cell.state) & 4)) {  // split mode
    if (slot == FaceSlot::Top) return named(tex + "_top_split");
    if (slot == FaceSlot::Front) return named(tex + (active ? "_front_split_both_on" : "_front_split"));
  }
  return std::nullopt;
}

// Redirector: transparent direction overlays over the top face (n = front, rotated with the
// block; red and blue lanes can show together). Catchor: 7-segment readout of its match value.
void LithoKraftAppearance::meshExtra(const CustomBlock& cell, MeshWriter& out) const {
  const BlockDef def = cell.def();
  if (cell.blocks.has(cell.neighbor(0, 1, 0), kOpaque)) return;  // top face hidden
  static constexpr int lo[3] = {0, 0, 0}, hi[3] = {16, 16, 16};
  static constexpr const char* kRel = "nesw";
  const int f = stFacing(cell.state);
  const std::string tex(def.tex);
  if (roleOf(def) == SimRole::Redirector) {
    const bool on = stOn(cell.state), split = stAux(cell.state) & 4;
    auto overlay = [&](const char* colour, int rel) {
      const std::string n = tex + "_overlay_" + colour + "_" + kRel[rel] + (on ? "" : "_off");
      if (const FaceMaterial* m = cell.materials.named(n)) out.face(cell, 2, lo, hi, *m, {kShadeCutout, 0}, f);
    };
    overlay("red", 0);
    overlay("blue", split ? ((stAux(cell.state) & 3) - f) & 3 : 0);
  } else if (roleOf(def) == SimRole::Catchor) {
    if (const FaceMaterial* m = cell.materials.named(tex + "_readout_mask"))
      out.face(cell, 2, lo, hi, *m, {kShadeReadout, uint8_t((stValue(cell.state) & 127) | (stOn(cell.state) ? 128 : 0))}, f);
  }
}

// Wire: one flat piece on the cell floor, chosen from its connections and tinted by strength.
void LithoKraftAppearance::meshCustom(const CustomBlock& cell, MeshWriter& out) const {
  const BlockRegistry& reg = cell.blocks;
  const BlockDef def = cell.def();
  if (cell.id == kChipORViewport) {  // ChipOR viewport (workstream 4): transparent top + miniature interior
    chipor::meshViewportCell(cell, out);
    return;
  }
  if (roleOf(def) != SimRole::Wire) return;
  const int ch = def.channel - 1;
  const bool upBlocked = reg.has(cell.neighbor(0, 1, 0), kCutsWire);
  int arms = 0, count = 0;
  for (int d = 0; d < 4; ++d) {
    const BlockDef nd = reg.def(cell.neighbor(kDirX[d], 0, kDirZ[d]));
    bool link = (roleOf(nd) == SimRole::Wire && nd.channel == def.channel) ||
                (roleOf(nd) != SimRole::None && roleOf(nd) != SimRole::Wire && roleOf(nd) != SimRole::Torch);
    const BlockDef up = reg.def(cell.neighbor(kDirX[d], 1, kDirZ[d]));
    const BlockDef dn = reg.def(cell.neighbor(kDirX[d], -1, kDirZ[d]));
    if (!upBlocked && roleOf(up) == SimRole::Wire && up.channel == def.channel) link = true;
    if (!(nd.flags & kCutsWire) && roleOf(dn) == SimRole::Wire && dn.channel == def.channel) link = true;
    if (link) { arms |= 1 << d; ++count; }
  }
  WirePiece piece = WirePiece::Dot;
  int rot = 0;
  if (count == 1) { piece = WirePiece::End; for (int d = 0; d < 4; ++d) if (arms >> d & 1) rot = (d + 2) & 3; }  // canonical arm S
  else if (count == 2 && (arms == 5 || arms == 10)) { piece = WirePiece::Straight; rot = arms == 10 ? 1 : 0; }
  else if (count == 2) { piece = WirePiece::Corner; for (int d = 0; d < 4; ++d) if ((arms >> d & 1) && (arms >> ((d + 1) & 3) & 1)) rot = d; }
  else if (count == 3) { piece = WirePiece::T; for (int d = 0; d < 4; ++d) if (!(arms >> d & 1)) rot = d; }  // canonical missing N
  else if (count == 4) piece = WirePiece::Cross;
  // On a floor: a strip on the cell floor, facing up. Hanging from a ceiling (nothing solid
  // below, solid above): the same strip under the support, facing down; the -Y face mirrors U,
  // so east / west arms swap and the rotation is mirrored to keep the arms on their wires.
  const bool hanging = !reg.has(cell.neighbor(0, -1, 0), kSolid) && reg.has(cell.neighbor(0, 1, 0), kSolid);
  static constexpr int lo[3] = {0, 0, 0}, hi[3] = {16, 1, 16}, clo[3] = {0, 15, 0}, chi[3] = {16, 16, 16};
  const FaceLook fl{uint8_t(ch ? kShadeStripTint1 : kShadeStripTint0), uint8_t(stValue(cell.state))};
  if (hanging) out.face(cell, 3, clo, chi, cell.materials.wirePiece(cell.id, piece), fl, (4 - rot) & 3);
  else out.face(cell, 2, lo, hi, cell.materials.wirePiece(cell.id, piece), fl, rot);
}

void LithoKraftAppearance::bindMaterials(const BlockDef& def, const BlockModel& model, MaterialBindings& b) const {
  const int paletteRow = model.palette.valid() ? int(model.palette.value) : 0;
  switch (roleOf(def)) {
    case SimRole::Wire:
      if (def.shape == Shape::Custom && def.channel >= 1 && def.channel <= 2) {  // vias (cubes) keep the strip palettes
        b.stripPalette[size_t(def.channel - 1)] = paletteRow;
        b.stripRing[size_t(def.channel - 1)] = model.mask("ring").value;
        b.stripDot[size_t(def.channel - 1)] = model.mask("dot").value;
      }
      break;
    case SimRole::ColorLamp: {
      const size_t t = size_t(tierIndex(def.tier));
      b.tierPalette[t] = paletteRow;
      b.tierMask[t] = model.mask("main").value;
      b.tierRim[t] = model.mask("rim").value;
      break;
    }
    case SimRole::Repeater: b.pipMask = model.mask("delay").value; break;
    case SimRole::Memor: b.cellMask = model.mask("value").value; break;
    default: break;
  }
}

// Attached torches and switches: box models (JSON, the glTF-subset format of box_model.h).
// Authored standing on the support; the mesher turns them toward the attach face.
const char* const kTorchModel = R"({"asset": {"version": "ll-box-1"},
  "materials": [{"name": "torch", "slot": "top"}],
  "meshes": [{"name": "stick", "boxes": [{"min": [7, 0, 7], "max": [9, 10, 9], "material": 0}]}],
  "nodes": [{"mesh": 0}]})";
const char* const kSwitchModel = R"({"asset": {"version": "ll-box-1"},
  "materials": [{"name": "base", "slot": "bottom"}, {"name": "lever", "slot": "top"}],
  "meshes": [{"name": "base", "boxes": [{"min": [5, 0, 4], "max": [11, 2, 12], "material": 0}]},
             {"name": "lever", "boxes": [{"min": [7, 2, 7], "max": [9, 9, 9], "material": 1}]}],
  "nodes": [{"mesh": 0}, {"mesh": 1}]})";

const char* const kButtonModel = R"({"asset": {"version": "ll-box-1"},
  "materials": [{"name": "cap", "slot": "top"}],
  "meshes": [{"name": "cap", "boxes": [{"min": [5, 0, 6], "max": [11, 2, 10], "material": 0}]}],
  "nodes": [{"mesh": 0}]})";

const BoxModel* LithoKraftAppearance::boxModel(const BlockDef& def) const {
  static const std::optional<BoxModel> torch = BoxModel::parse(kTorchModel, nullptr);
  static const std::optional<BoxModel> sw = BoxModel::parse(kSwitchModel, nullptr);
  if (roleOf(def) == SimRole::Torch) return torch ? &*torch : nullptr;
  static const std::optional<BoxModel> button = BoxModel::parse(kButtonModel, nullptr);
  if (roleOf(def) == SimRole::Switch) return sw ? &*sw : nullptr;
  if (roleOf(def) == SimRole::Button) return button ? &*button : nullptr;
  return nullptr;
}

}  // namespace lithokraft
