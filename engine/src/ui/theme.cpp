#include "litholattice/ui/theme.h"
#include <cstdlib>

namespace litholattice::ui {
namespace {

bool parseColor(const Json& v, Color& out) {
  if (v.type == Json::Type::Array && (v.arr.size() == 3 || v.arr.size() == 4)) {
    out = {float(v.arr[0].num), float(v.arr[1].num), float(v.arr[2].num), v.arr.size() == 4 ? float(v.arr[3].num) : 1.f};
    return true;
  }
  if (v.type == Json::Type::String && v.str.size() >= 7 && v.str[0] == '#') {
    const unsigned long bits = std::strtoul(v.str.c_str() + 1, nullptr, 16);
    const bool hasAlpha = v.str.size() == 9;
    const auto byte = [&](int i) { return float((bits >> (8 * i)) & 0xFF) / 255.f; };
    out = hasAlpha ? Color{byte(3), byte(2), byte(1), byte(0)} : Color{byte(2), byte(1), byte(0), 1.f};
    return v.str.size() == 7 || hasAlpha;
  }
  return false;
}

bool parseEdges(const Json& v, Edges& out) {
  if (v.type == Json::Type::Number) {
    out = Edges::all(float(v.num));
    return true;
  }
  if (v.type == Json::Type::Array && v.arr.size() == 4) {
    out = {float(v.arr[0].num), float(v.arr[1].num), float(v.arr[2].num), float(v.arr[3].num)};
    return true;
  }
  return false;
}

/// Applies the members present in `j` on top of `s`.
bool parseStyle(const Json& j, Style& s, const std::string& name, std::string& err) {
  if (j.type != Json::Type::Object) {
    err = "style '" + name + "' must be an object";
    return false;
  }
  const std::pair<const char*, Color*> colors[] = {
      {"background", &s.background}, {"hover", &s.hover},   {"pressed", &s.pressed},
      {"disabled", &s.disabled},     {"text", &s.text},     {"text_disabled", &s.textDisabled},
      {"accent", &s.accent},         {"border", &s.border}};
  for (const auto& [key, value] : j.obj) {
    bool ok = true, known = false;
    for (const auto& [k, c] : colors)
      if (key == k) {
        known = true;
        ok = parseColor(value, *c);
      }
    if (key == "border_width") {
      known = true;
      s.borderWidth = float(value.num);
    } else if (key == "text_scale") {
      known = true;
      s.textScale = float(value.num);
    } else if (key == "padding") {
      known = true;
      ok = parseEdges(value, s.padding);
    }
    if (!known) {
      err = "style '" + name + "': unknown key '" + key + "'";
      return false;
    }
    if (!ok) {
      err = "style '" + name + "': bad value for '" + key + "'";
      return false;
    }
  }
  return true;
}

}  // namespace

Theme Theme::defaults() {
  Theme t;
  Style base;
  t.styles_["default"] = base;
  Style panel = base;
  panel.background = {0.f, 0.f, 0.f, 0.72f};
  panel.padding = Edges::all(8.f);
  t.styles_["panel"] = panel;
  Style bare = base;  // layout-only containers: draw nothing, no padding
  bare.background = {0, 0, 0, 0};
  bare.borderWidth = 0;
  bare.padding = Edges::all(0.f);
  for (const char* name : {"none", "root", "stack", "scrollview", "splitter", "image", "label"}) t.styles_[name] = bare;
  Style title = bare;  // launch-menu title: big flat text, no background
  title.textScale = 4.5f;
  t.styles_["title"] = title;
  Style edit = base;
  edit.background = {0.05f, 0.05f, 0.07f, 0.95f};
  t.styles_["textbox"] = edit;
  t.styles_["editor"] = edit;
  Style menu = base;
  menu.background = {0.16f, 0.16f, 0.19f, 0.98f};
  t.styles_["menu"] = menu;
  // Mod-content frame treatment (mod hardening item 5): visually distinct
  // from the system dialog look — amber-tinted border on a dark panel.
  // Mods ship no theme packs, so they cannot restyle this class; the
  // MOD badge itself is painted with fixed colours (see ui/mod_ui.h).
  Style modPanel = base;
  modPanel.background = {0.09f, 0.07f, 0.04f, 0.96f};
  modPanel.border = {1.f, 0.62f, 0.f, 1.f};
  modPanel.borderWidth = 2.f;
  modPanel.padding = Edges::all(8.f);
  t.styles_["mod_panel"] = modPanel;
  Style tooltip = base;
  tooltip.background = {0.02f, 0.02f, 0.03f, 0.95f};
  tooltip.text = {1.f, 1.f, 0.85f, 1.f};
  t.styles_["tooltip"] = tooltip;
  Style slot = base;
  slot.background = {0.12f, 0.12f, 0.14f, 0.9f};
  slot.hover = {0.45f, 0.45f, 0.5f, 0.95f};
  slot.accent = {1.f, 1.f, 1.f, 0.95f};
  t.styles_["slot"] = slot;
  return t;
}

bool Theme::parse(const Json& json, Theme& out, std::string& err) {
  Theme t = defaults();
  if (json.type != Json::Type::Object) {
    err = "theme must be an object";
    return false;
  }
  if (const Json* g = json.get("glyph")) {
    if (g->type != Json::Type::Array || g->arr.size() != 2) {
      err = "'glyph' must be [width, height]";
      return false;
    }
    t.glyph_ = {float(g->arr[0].num), float(g->arr[1].num)};
  }
  t.scale_ = float(json.getNum("scale", 1));
  if (const Json* styles = json.get("styles")) {
    Style base = t.styles_["default"];
    if (const Json* d = styles->get("default"); d && !parseStyle(*d, base, "default", err)) return false;
    // Built-in classes keep their own look unless the pack overrides them; new classes start
    // from the (possibly overridden) default.
    t.styles_["default"] = base;
    for (const auto& [name, value] : styles->obj) {
      if (name == "default") continue;
      auto it = t.styles_.find(name);
      Style s = it != t.styles_.end() ? it->second : base;
      if (!parseStyle(value, s, name, err)) return false;
      t.styles_[name] = s;
    }
  }
  out = std::move(t);
  return true;
}

bool Theme::parse(const std::string& text, Theme& out, std::string& err) {
  Json j;
  if (!Json::parse(text, j, &err)) return false;
  return parse(j, out, err);
}

const Style& Theme::style(std::string_view widgetClass) const {
  const auto it = styles_.find(widgetClass);
  return it != styles_.end() ? it->second : styles_.find("default")->second;
}

}  // namespace litholattice::ui
