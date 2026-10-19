// PBR material registry: implementation. Data-file format is JSON via the engine's
// minimal Json helper (core/json.h); see assets/materials/lithokraft.materials.
#include "litholattice/render/pbr_materials.h"
#include "litholattice/core/json.h"
#include <fstream>
#include <sstream>

namespace litholattice {
namespace {

bool parseTransparency(const std::string& s, TransparencyMethod* out, std::string* err) {
  if (s == "screendoor") *out = TransparencyMethod::ScreenDoor;
  else if (s == "alphablend") *out = TransparencyMethod::AlphaBlend;
  else if (s == "weightedoit") *out = TransparencyMethod::WeightedOIT;
  else if (s == "additive") *out = TransparencyMethod::Additive;
  else {
    if (err) *err = "unknown transparency method '" + s + "'";
    return false;
  }
  return true;
}

bool parseRgb(const Json& j, const std::string& key, std::array<float, 3>& out, std::string* err) {
  const Json* v = j.get(key);
  if (!v) return true;  // absent: keep the caller's default
  if (v->type != Json::Type::Array || v->arr.size() != 3) {
    if (err) *err = "'" + key + "' must be a 3-element array";
    return false;
  }
  for (size_t i = 0; i < 3; ++i) {
    if (v->arr[i].type != Json::Type::Number) {
      if (err) *err = "'" + key + "' elements must be numbers";
      return false;
    }
    out[i] = static_cast<float>(v->arr[i].num);
  }
  return true;
}

}  // namespace

uint16_t MaterialRegistry::registerMaterial(std::string id, const MaterialDef& def) {
  if (id.empty() || ids_.size() >= kMaxMaterials) return kInvalidMaterialId;
  for (size_t i = 0; i < ids_.size(); ++i) {
    if (ids_[i] == id) {
      if (defs_[i] == def) return static_cast<uint16_t>(i);  // idempotent
      return kInvalidMaterialId;  // conflicting re-registration: original wins
    }
  }
  const uint16_t assigned = static_cast<uint16_t>(ids_.size());
  ids_.push_back(std::move(id));
  defs_.push_back(def);
  ++generation_;
  return assigned;
}

uint16_t MaterialRegistry::idFor(std::string_view id) const {
  for (size_t i = 0; i < ids_.size(); ++i)
    if (ids_[i] == id) return static_cast<uint16_t>(i);
  return kInvalidMaterialId;
}

const MaterialDef* MaterialRegistry::defFor(uint16_t id) const {
  return id < defs_.size() ? &defs_[id] : nullptr;
}

const std::string* MaterialRegistry::stringIdFor(uint16_t id) const {
  return id < ids_.size() ? &ids_[id] : nullptr;
}

bool MaterialRegistry::loadText(const std::string& text, std::string* err) {
  // Data files may carry full-line `//` comments (the format doc lives at the top of the file);
  // the minimal Json parser does not understand them, so strip them before parsing.
  std::string stripped;
  stripped.reserve(text.size());
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t eol = text.find('\n', pos);
    const std::string line = text.substr(pos, eol == std::string::npos ? eol : eol - pos);
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string::npos || line.compare(first, 2, "//") != 0) {
      stripped.append(line);
      stripped.push_back('\n');
    }
    pos = (eol == std::string::npos) ? text.size() : eol + 1;
  }
  Json root;
  if (!Json::parse(stripped, root, err)) return false;
  const Json* mats = root.get("materials");
  if (!mats || mats->type != Json::Type::Array) {
    if (err) *err = "materials file must contain a 'materials' array";
    return false;
  }
  for (const Json& m : mats->arr) {
    const std::string id = m.getStr("id");
    if (id.empty()) {
      if (err) *err = "material entry without an 'id'";
      return false;
    }
    MaterialDef def;
    if (!parseRgb(m, "albedo", def.albedo, err)) return false;
    def.albedoTexture = m.getStr("albedo_texture");
    def.roughness = static_cast<float>(m.getNum("roughness", def.roughness));
    def.metalness = static_cast<float>(m.getNum("metalness", def.metalness));
    if (!parseRgb(m, "emissive", def.emissive, err)) return false;
    def.emissiveIntensity = static_cast<float>(m.getNum("emissive_intensity", def.emissiveIntensity));
    def.ior = static_cast<float>(m.getNum("ior", def.ior));
    def.alpha = static_cast<float>(m.getNum("alpha", def.alpha));
    // Fix 2026-10-11 (audit): range-validate numeric fields; garbage used to load
    // silently (roughness 5, alpha -1, ior 0). Out-of-range -> error, fail closed.
    auto rangeErr = [&](const char* field, float v, float lo, float hi) {
      if (!(v >= lo && v <= hi)) {
        if (err) *err = std::string("material '") + id + "': '" + field + "' out of range";
        return true;
      }
      return false;
    };
    if (rangeErr("roughness", def.roughness, 0.f, 1.f)) return false;
    if (rangeErr("metalness", def.metalness, 0.f, 1.f)) return false;
    if (rangeErr("alpha", def.alpha, 0.f, 1.f)) return false;
    if (rangeErr("ior", def.ior, 1.f, 3.f)) return false;
    if (rangeErr("emissive_intensity", def.emissiveIntensity, 0.f, 1e6f)) return false;
    const std::string tm = m.getStr("transparency", "screendoor");
    if (!parseTransparency(tm, &def.transparency, err)) return false;
    if (registerMaterial(id, def) == kInvalidMaterialId) {
      if (err) *err = "conflicting re-registration of '" + id + "'";
      return false;
    }
  }
  return true;
}

bool MaterialRegistry::loadFile(const std::string& path, std::string* err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot open materials file '" + path + "'";
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return loadText(ss.str(), err);
}

bool ChunkPalette::build(const MaterialRegistry& reg, const std::vector<std::string>& stringIds,
                         std::string* err) {
  if (stringIds.size() > kCapacity) {
    if (err) *err = "chunk palette holds at most 256 materials";
    return false;
  }
  for (size_t i = 0; i < stringIds.size(); ++i) {
    const uint16_t gid = reg.idFor(stringIds[i]);
    if (gid == MaterialRegistry::kInvalidMaterialId) {
      if (err) *err = "unknown material '" + stringIds[i] + "'";
      return false;
    }
    entries_[i] = gid;
  }
  size_ = stringIds.size();
  return true;
}

std::vector<std::string> ChunkPalette::stringIds(const MaterialRegistry& reg) const {
  std::vector<std::string> out;
  out.reserve(size_);
  for (size_t i = 0; i < size_; ++i) {
    const std::string* s = reg.stringIdFor(entries_[i]);
    out.push_back(s ? *s : std::string{});
  }
  return out;
}

MaterialDef defaultMaterialDef() {
  MaterialDef d;
  d.albedo = {0.8f, 0.8f, 0.8f};  // neutral matte grey: shades sanely under any light
  d.roughness = 0.9f;
  d.metalness = 0.0f;
  return d;
}

bool initMaterialRegistry(MaterialRegistry& reg, const std::string& path, std::string* err) {
  // The built-in default first: on a fresh registry this is compact ID 0, the defined
  // fallback for unknown string IDs and empty palettes.
  if (reg.registerMaterial(std::string(kDefaultMaterialStringId), defaultMaterialDef()) ==
      MaterialRegistry::kInvalidMaterialId) {
    if (err) *err = "could not register the built-in default material (conflicting prior registration)";
    return false;
  }
  if (path.empty()) {
    if (err) *err = "no materials data file path (engine assets not found)";
    return false;  // graceful: the caller logs and continues with the built-in default
  }
  return reg.loadFile(path, err);  // err already set on failure; the default stays registered
}

}  // namespace litholattice
