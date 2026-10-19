#include "litholattice/assets/box_model.h"
#include <algorithm>
#include <cmath>
#include "litholattice/core/json.h"

namespace litholattice {
namespace {
bool vec3(const Json* j, int out[3]) {
  if (!j || j->type != Json::Type::Array || j->arr.size() != 3) return false;
  for (int i = 0; i < 3; ++i) {
    if (j->arr[size_t(i)].type != Json::Type::Number) return false;
    const double v = j->arr[size_t(i)].num;
    if (v != std::floor(v) || v < -16 || v > 32) return false;
    out[i] = int(v);
  }
  return true;
}
std::optional<FaceSlot> slotNamed(const std::string& s) {
  static const char* names[] = {"top", "bottom", "front", "back", "left", "right"};
  for (int i = 0; i < 6; ++i) if (s == names[i]) return FaceSlot(i);
  return std::nullopt;
}
}  // namespace

std::optional<BoxModel> BoxModel::parse(const std::string& text, std::string* err) {
  auto fail = [&](const std::string& why) -> std::optional<BoxModel> { if (err) *err = "box model: " + why; return std::nullopt; };
  Json j;
  if (!Json::parse(text, j, err)) return std::nullopt;
  const Json* asset = j.get("asset");
  if (!asset || asset->getStr("version") != "ll-box-1") return fail("asset.version must be \"ll-box-1\"");
  BoxModel m;
  if (const Json* mats = j.get("materials"))
    for (const Json& e : mats->arr) {
      const auto slot = slotNamed(e.getStr("slot", "top"));
      if (!slot) return fail("material slot must be top|bottom|front|back|left|right");
      m.materials.push_back({e.getStr("name"), *slot});
    }
  if (m.materials.empty()) m.materials.push_back({"default", FaceSlot::Top});
  const Json* meshes = j.get("meshes");
  if (!meshes || meshes->arr.empty()) return fail("needs at least one mesh");
  for (const Json& e : meshes->arr) {
    Mesh mesh{e.getStr("name"), {}};
    const Json* boxes = e.get("boxes");
    if (!boxes || boxes->arr.empty()) return fail("mesh '" + mesh.name + "' has no boxes");
    for (const Json& b : boxes->arr) {
      Box box;
      if (!vec3(b.get("min"), box.min) || !vec3(b.get("max"), box.max)) return fail("box min/max must be 3 integers in -16..32");
      for (int i = 0; i < 3; ++i) if (box.min[i] >= box.max[i]) return fail("box min must be < max on every axis");
      box.material = int(b.getNum("material", 0));
      if (box.material < 0 || box.material >= int(m.materials.size())) return fail("box material index out of range");
      mesh.boxes.push_back(box);
    }
    m.meshes.push_back(std::move(mesh));
  }
  const Json* nodes = j.get("nodes");
  if (!nodes || nodes->arr.empty()) return fail("needs at least one node");
  for (const Json& e : nodes->arr) {
    Node n;
    n.mesh = int(e.getNum("mesh", -1));
    if (n.mesh < 0 || n.mesh >= int(m.meshes.size())) return fail("node mesh index out of range");
    if (const Json* t = e.get("translation"); t && !vec3(t, n.translation)) return fail("translation must be 3 integers");
    if (const Json* s = e.get("scale")) {
      int sc[3];
      if (!vec3(s, sc) || sc[0] != 1 || sc[1] != 1 || sc[2] != 1) return fail("scale is not supported (must be [1,1,1])");
    }
    if (const Json* r = e.get("rotation")) {  // quaternion [x, y, z, w]: quarter turns about Y only
      if (r->type != Json::Type::Array || r->arr.size() != 4) return fail("rotation must be a quaternion [x,y,z,w]");
      const double qx = r->arr[0].num, qy = r->arr[1].num, qz = r->arr[2].num, qw = r->arr[3].num;
      if (std::abs(qx) > 1e-4 || std::abs(qz) > 1e-4) return fail("rotation must be about Y only");
      const double ang = 2.0 * std::atan2(qy, qw) * 180.0 / 3.14159265358979;
      const double q = ang / 90.0;
      if (std::abs(q - std::round(q)) > 1e-3) return fail("rotation must be a multiple of 90 degrees");
      n.quarterTurnsY = ((int(std::lround(q)) % 4) + 4) % 4;
    }
    m.nodes.push_back(n);
  }
  return m;
}

std::vector<BoxModel::Box> BoxModel::orientedBoxes(int attach) const {
  std::vector<Box> out;
  for (const Node& n : nodes)
    for (Box b : meshes[size_t(n.mesh)].boxes) {
      for (int t = 0; t < n.quarterTurnsY; ++t) {  // (x, z) -> (16 - z, x) about the cell centre
        const Box o = b;
        b.min[0] = 16 - o.max[2]; b.max[0] = 16 - o.min[2];
        b.min[2] = o.min[0]; b.max[2] = o.max[0];
      }
      for (int i = 0; i < 3; ++i) { b.min[i] += n.translation[i]; b.max[i] += n.translation[i]; }
      Box r = b;  // authored with the support below (-Y); y = distance from the support face
      switch (attach) {
        case 2: r.min[1] = 16 - b.max[1]; r.max[1] = 16 - b.min[1]; break;                          // support above
        case 3: r.min[2] = b.min[1]; r.max[2] = b.max[1]; r.min[1] = b.min[2]; r.max[1] = b.max[2]; break;  // -Z
        case 4: r.min[2] = 16 - b.max[1]; r.max[2] = 16 - b.min[1]; r.min[1] = b.min[2]; r.max[1] = b.max[2]; break;
        case 5: r.min[0] = b.min[1]; r.max[0] = b.max[1]; r.min[1] = b.min[0]; r.max[1] = b.max[0]; break;  // -X
        case 6: r.min[0] = 16 - b.max[1]; r.max[0] = 16 - b.min[1]; r.min[1] = b.min[0]; r.max[1] = b.max[0]; break;
        default: break;
      }
      out.push_back(r);
    }
  return out;
}

}  // namespace litholattice
