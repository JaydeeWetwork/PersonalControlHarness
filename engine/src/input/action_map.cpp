#include "litholattice/input/action_map.h"
#include <fstream>
#include <sstream>
#include "litholattice/core/json.h"

namespace litholattice {
std::vector<std::pair<ActionId, ActionId>> ActionMap::conflicts() const {
  std::vector<std::pair<ActionId, ActionId>> out;
  for (size_t i = 0; i < binds_.size(); ++i)
    for (size_t j = i + 1; j < binds_.size(); ++j) {
      const ActionId a = ActionId(i), b = ActionId(j);
      if (actions_->mayShareBinding(a, b)) continue;
      for (const Binding& x : binds_[i])
        for (const Binding& y : binds_[j])
          if (x == y) out.emplace_back(a, b);
    }
  return out;
}

std::string ActionMap::toJson() const {
  std::ostringstream o;
  const AnalogSettings& s = analog_;
  o << "{\n  \"version\": 1,\n  \"analog\": {\n"
    << "    \"stickDeadzone\": " << s.stickDeadzone << ",\n"
    << "    \"triggerDeadzone\": " << s.triggerDeadzone << ",\n"
    << "    \"stickCurve\": " << s.stickCurve << ",\n"
    << "    \"mouseSensitivity\": " << s.mouseSensitivity << ",\n"
    << "    \"stickLookSpeed\": " << s.stickLookSpeed << ",\n"
    << "    \"invertY\": " << (s.invertY ? "true" : "false") << ",\n"
    << "    \"pressThreshold\": " << s.pressThreshold << "\n  },\n  \"bindings\": {\n";
  for (size_t i = 0; i < binds_.size(); ++i) {
    o << "    \"" << actions_->name(ActionId(i)) << "\": [";
    for (size_t k = 0; k < binds_[i].size(); ++k) o << (k ? ", " : "") << '"' << binds_[i][k].toString() << '"';
    o << "]" << (i + 1 < binds_.size() ? "," : "") << "\n";
  }
  o << "  }\n}\n";
  return o.str();
}

std::optional<ActionMap> ActionMap::fromJson(const ActionMap& defaults, const std::string& text, std::string* err,
                                             std::vector<std::string>* warnings) {
  Json root;
  if (!Json::parse(text, root, err)) return std::nullopt;
  if (root.type != Json::Type::Object) {
    if (err) *err = "top level is not an object";
    return std::nullopt;
  }
  auto warn = [&](std::string w) { if (warnings) warnings->push_back(std::move(w)); };
  ActionMap m = defaults;
  if (const Json* a = root.get("analog")) {
    AnalogSettings& s = m.analog_;
    auto num = [&](const char* key, float& v) { v = float(a->getNum(key, v)); };
    num("stickDeadzone", s.stickDeadzone);
    num("triggerDeadzone", s.triggerDeadzone);
    num("stickCurve", s.stickCurve);
    num("mouseSensitivity", s.mouseSensitivity);
    num("stickLookSpeed", s.stickLookSpeed);
    num("pressThreshold", s.pressThreshold);
    if (const Json* inv = a->get("invertY"); inv && inv->type == Json::Type::Bool) s.invertY = inv->b;
  }
  if (const Json* b = root.get("bindings"); b && b->type == Json::Type::Object) {
    for (const auto& [name, list] : b->obj) {
      const auto action = m.actions_->find(name);
      if (!action) { warn("unknown action '" + name + "'"); continue; }
      if (list.type != Json::Type::Array) { warn("bindings of '" + name + "' are not a list"); continue; }
      std::vector<Binding> parsed;
      for (const Json& item : list.arr) {
        const auto bnd = item.type == Json::Type::String ? Binding::parse(item.str) : std::nullopt;
        if (bnd) parsed.push_back(*bnd);
        else warn("bad binding '" + item.str + "' for " + name);
      }
      m.setBindings(*action, std::move(parsed));
    }
  }
  return m;
}

BindingsFile loadOrCreateBindings(const std::filesystem::path& path, const ActionMap& defaults) {
  BindingsFile f{defaults};
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    std::string err;
    f.status = BindingsFile::Status::Created;
    f.message = saveBindings(f.map, path, &err) ? "created from defaults" : "could not create (" + err + "); using defaults";
    return f;
  }
  std::ifstream in(path, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  std::string err;
  if (auto m = ActionMap::fromJson(defaults, ss.str(), &err, &f.warnings)) {
    f.map = std::move(*m);
    f.message = "loaded";
  } else {
    f.status = BindingsFile::Status::Invalid;
    f.message = "invalid (" + err + "); using defaults, file left untouched";
  }
  return f;
}

bool saveBindings(const ActionMap& map, const std::filesystem::path& path, std::string* err) {
  std::error_code ec;
  if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (err) *err = "cannot write " + path.string();
    return false;
  }
  out << map.toJson();
  return bool(out);
}

}  // namespace litholattice
