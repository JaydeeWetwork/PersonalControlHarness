// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

#include "litholattice/procgen/litho_formula_gallery.h"

#include <cctype>
#include <fstream>
#include <sstream>

#include "litholattice/core/json.h"

namespace litholattice::procgen::litho_formula {
namespace {

bool readFile(const std::string& path, std::string& text, std::string& err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    err = "cannot open " + path;
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  text = ss.str();
  return true;
}

bool parseParams(const litholattice::Json& obj, LithoParams& p, std::string& err) {
  static const char* keys[6] = {"a", "b", "m", "n1", "n2", "n3"};
  double* slots[6] = {&p.a, &p.b, &p.m, &p.n1, &p.n2, &p.n3};
  for (int i = 0; i < 6; ++i) {
    const litholattice::Json* v = obj.get(keys[i]);
    if (!v || v->type != litholattice::Json::Type::Number) {
      err = std::string("params missing numeric \"") + keys[i] + "\"";
      return false;
    }
    *slots[i] = v->num;
  }
  return true;
}

bool validName(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  for (char c : name)
    if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_')
      return false;
  return true;
}

}  // namespace

bool loadLithoFormulaGallery(const std::string& jsonPath,
                             std::vector<LithoPreset>& out,
                             std::string* err) {
  std::string errLocal;
  std::string text;
  if (!readFile(jsonPath, text, errLocal)) {
    if (err) *err = errLocal;
    return false;
  }
  litholattice::Json root;
  if (!litholattice::Json::parse(text, root, &errLocal)) {
    if (err) *err = jsonPath + ": JSON syntax error: " + errLocal;
    return false;
  }
  const litholattice::Json* presets = root.get("presets");
  if (!presets || presets->type != litholattice::Json::Type::Array) {
    if (err) *err = jsonPath + ": missing \"presets\" array";
    return false;
  }
  std::vector<LithoPreset> loaded;
  for (size_t i = 0; i < presets->arr.size(); ++i) {
    const litholattice::Json& e = presets->arr[i];
    LithoPreset p;
    p.name = e.getStr("name");
    if (!validName(p.name)) {
      if (err) *err = jsonPath + ": preset #" + std::to_string(i) +
                      " has an illegal name (want [a-z0-9_], <=64 chars)";
      return false;
    }
    for (const auto& q : loaded)
      if (q.name == p.name) {
        if (err) *err = jsonPath + ": duplicate preset name \"" + p.name + "\"";
        return false;
      }
    const litholattice::Json* theta = e.get("theta");
    const litholattice::Json* phi = e.get("phi");
    if (!theta || theta->type != litholattice::Json::Type::Object ||
        !phi || phi->type != litholattice::Json::Type::Object) {
      if (err) *err = jsonPath + ": preset \"" + p.name + "\" misses theta/phi objects";
      return false;
    }
    if (!parseParams(*theta, p.theta, errLocal) ||
        !parseParams(*phi, p.phi, errLocal)) {
      if (err) *err = jsonPath + ": preset \"" + p.name + "\": " + errLocal;
      return false;
    }
    p.resolution = static_cast<int>(e.getNum("resolution", 32));
    if (p.resolution < 8 || p.resolution > 64) {
      if (err) *err = jsonPath + ": preset \"" + p.name + "\": resolution " +
                      std::to_string(p.resolution) + " outside [8, 64]";
      return false;
    }
    const int mat = static_cast<int>(e.getNum("material", 1));
    if (mat < 1 || mat > 255) {
      if (err) *err = jsonPath + ": preset \"" + p.name + "\": illegal material id";
      return false;
    }
    p.material = static_cast<uint8_t>(mat);
    p.description = e.getStr("description");
    if (p.description.empty()) {
      if (err) *err = jsonPath + ": preset \"" + p.name + "\" needs a description";
      return false;
    }
    loaded.push_back(std::move(p));
  }
  if (loaded.empty()) {
    if (err) *err = jsonPath + ": gallery holds no presets";
    return false;
  }
  out = std::move(loaded);
  return true;
}

}  // namespace litholattice::procgen::litho_formula
