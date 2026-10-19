// Minimal JSON reader and writer (objects, arrays, strings, numbers, true/false/null) for pack
// manifests and generator graphs. No \u escapes beyond ASCII. Enough for data files we ship; not
// a general library.
#pragma once
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace litholattice {

struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  const Json* get(const std::string& key) const {  // object member or nullptr
    for (auto& [k, v] : obj) if (k == key) return &v;
    return nullptr;
  }
  std::string getStr(const std::string& key, const std::string& def = {}) const {
    const Json* v = get(key); return v && v->type == Type::String ? v->str : def;
  }
  double getNum(const std::string& key, double def = 0) const {
    const Json* v = get(key); return v && v->type == Type::Number ? v->num : def;
  }
  bool getBool(const std::string& key, bool def = false) const {
    const Json* v = get(key); return v && v->type == Type::Bool ? v->b : def;
  }
  // Parses text; returns false (and sets err) on syntax errors.
  static bool parse(const std::string& text, Json& out, std::string* err = nullptr);

  /// Serializes to JSON text. Object members keep their order; numbers use the shortest form that
  /// reads back to the same double. `indent` < 0: one line; otherwise pretty-printed.
  std::string dump(int indent = 2) const;

  static Json number(double v) { Json j; j.type = Type::Number; j.num = v; return j; }
  static Json string(std::string v) { Json j; j.type = Type::String; j.str = std::move(v); return j; }
  static Json boolean(bool v) { Json j; j.type = Type::Bool; j.b = v; return j; }
  static Json array() { Json j; j.type = Type::Array; return j; }
  static Json object() { Json j; j.type = Type::Object; return j; }
  /// Appends (object) member `key`; returns it.
  Json& add(std::string key, Json value) { obj.emplace_back(std::move(key), std::move(value)); return obj.back().second; }
};

}  // namespace litholattice
