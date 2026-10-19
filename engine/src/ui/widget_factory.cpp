#include "litholattice/ui/widget_factory.h"
#include <functional>
#include <map>
#include <set>
#include <vector>
#include "litholattice/ui/containers.h"
#include "litholattice/ui/controls.h"
#include "litholattice/ui/lists.h"
#include "litholattice/ui/text_edit.h"

namespace litholattice::ui {
namespace {

const std::set<std::string, std::less<>> kCommonKeys = {"type", "id", "style", "tooltip", "visible", "enabled",
                                                        "layout", "on", "children"};

Vec2 vec2(const Json* j, Vec2 def) {
  if (!j || j->type != Json::Type::Array || j->arr.size() != 2) return def;
  return {float(j->arr[0].num), float(j->arr[1].num)};
}

Edges edges(const Json* j) {
  if (!j) return {};
  if (j->type == Json::Type::Number) return Edges::all(float(j->num));
  if (j->type == Json::Type::Array && j->arr.size() == 4)
    return {float(j->arr[0].num), float(j->arr[1].num), float(j->arr[2].num), float(j->arr[3].num)};
  return {};
}

std::vector<std::string> strings(const Json* j) {
  std::vector<std::string> out;
  if (j && j->type == Json::Type::Array)
    for (const Json& v : j->arr) out.push_back(v.str);
  return out;
}

std::string text(const Json& j, const char* key) {
  const Json* v = j.get(key);
  return v && v->type == Json::Type::String ? v->str : std::string();
}

Orientation orientation(const Json& j, Orientation def) {
  const std::string o = text(j, "orientation");
  if (o == "horizontal") return Orientation::Horizontal;
  if (o == "vertical") return Orientation::Vertical;
  return def;
}

struct TypeInfo {
  std::set<std::string, std::less<>> keys;  // type-specific keys
  std::function<std::unique_ptr<Widget>(const Json&)> make;
  bool takesChildren = true;
};

const std::map<std::string, TypeInfo, std::less<>>& types() {
  static const std::map<std::string, TypeInfo, std::less<>> kTypes = {
      {"widget", {{}, [](const Json&) { return std::make_unique<Widget>(); }}},
      {"panel", {{}, [](const Json&) { return std::make_unique<Panel>(); }}},
      {"label", {{"text"}, [](const Json& j) { return std::make_unique<Label>(text(j, "text")); }, false}},
      {"button", {{"text"}, [](const Json& j) { return std::make_unique<Button>(text(j, "text")); }, false}},
      {"image",
       {{"image", "size"}, [](const Json& j) { return std::make_unique<Image>(text(j, "image"), vec2(j.get("size"), {32, 32})); }, false}},
      {"checkbox",
       {{"text", "checked", "toggle"},
        [](const Json& j) { return std::make_unique<Checkbox>(text(j, "text"), j.getBool("checked", false), j.getBool("toggle", false)); },
        false}},
      {"progress", {{"value"}, [](const Json& j) {
                      auto p = std::make_unique<ProgressBar>();
                      p->setValue(float(j.getNum("value", 0)));
                      return p;
                    }, false}},
      {"slider",
       {{"min", "max", "value", "step"},
        [](const Json& j) {
          return std::make_unique<Slider>(float(j.getNum("min", 0)), float(j.getNum("max", 1)), float(j.getNum("value", 0)),
                                          float(j.getNum("step", 0)));
        },
        false}},
      {"number",
       {{"min", "max", "value"},
        [](const Json& j) {
          return std::make_unique<NumberField>(int(j.getNum("min", 0)), int(j.getNum("max", 64)), int(j.getNum("value", 0)));
        },
        false}},
      {"slot", {{"size", "image"}, [](const Json& j) {
                  auto s = std::make_unique<Slot>(vec2(j.get("size"), {40, 40}));
                  s->setIcon(text(j, "image"));
                  return s;
                }, false}},
      {"divider", {{"orientation", "thickness"}, [](const Json& j) {
                    return std::make_unique<Divider>(orientation(j, Orientation::Horizontal), float(j.getNum("thickness", 1)));
                  }, false}},
      {"stack",
       {{"orientation", "spacing"},
        [](const Json& j) { return std::make_unique<StackPanel>(orientation(j, Orientation::Vertical), float(j.getNum("spacing", 4))); }}},
      {"grid",
       {{"columns", "cell", "spacing"},
        [](const Json& j) {
          return std::make_unique<GridPanel>(int(j.getNum("columns", 4)), vec2(j.get("cell"), {40, 40}), float(j.getNum("spacing", 4)));
        }}},
      {"textbox", {{"text", "placeholder"}, [](const Json& j) {
                     auto t = std::make_unique<TextBox>(text(j, "text"));
                     t->setPlaceholder(text(j, "placeholder"));
                     return t;
                   }, false}},
      {"editor", {{"text", "line_numbers"}, [](const Json& j) {
                    auto t = std::make_unique<TextEditor>();
                    t->setText(text(j, "text"));
                    t->setLineNumbers(j.getBool("line_numbers", true));
                    return t;
                  }, false}},
      {"list", {{"items", "selected"}, [](const Json& j) {
                  auto l = std::make_unique<ListView>();
                  l->setItems(strings(j.get("items")));
                  l->setSelected(int(j.getNum("selected", -1)));
                  return l;
                }, false}},
      {"dropdown",
       {{"items", "selected"},
        [](const Json& j) { return std::make_unique<Dropdown>(strings(j.get("items")), int(j.getNum("selected", 0))); },
        false}},
  };
  return kTypes;
}

std::unique_ptr<Widget> build(const Json& j, const std::string& path, std::string& err) {
  if (j.type != Json::Type::Object) {
    err = path + ": widget must be an object";
    return nullptr;
  }
  const std::string type = text(j, "type");
  const auto& table = types();
  // Containers with special child handling.
  const bool scroll = type == "scroll", split = type == "splitter", tabs = type == "tabs";
  const auto it = table.find(type);
  if (it == table.end() && !scroll && !split && !tabs) {
    err = path + ": unknown widget type '" + type + "'";
    return nullptr;
  }
  static const std::set<std::string, std::less<>> kSplitKeys = {"orientation", "ratio"}, kTabKeys = {"titles"}, kNone;
  const auto& extra = scroll ? kNone : split ? kSplitKeys : tabs ? kTabKeys : it->second.keys;
  for (const auto& [key, value] : j.obj)
    if (!kCommonKeys.count(key) && !extra.count(key)) {
      err = path + ": unknown key '" + key + "' for type '" + type + "'";
      return nullptr;
    }

  // Children first (so scroll / splitter / tabs can adopt them).
  std::vector<std::unique_ptr<Widget>> kids;
  if (const Json* c = j.get("children")) {
    if (it != table.end() && !it->second.takesChildren) {
      err = path + ": '" + type + "' takes no children";
      return nullptr;
    }
    for (size_t i = 0; i < c->arr.size(); ++i) {
      auto k = build(c->arr[i], path + "/" + type + "[" + std::to_string(i) + "]", err);
      if (!k) return nullptr;
      kids.push_back(std::move(k));
    }
  }

  std::unique_ptr<Widget> w;
  if (scroll) {
    if (kids.size() != 1) {
      err = path + ": 'scroll' needs exactly one child";
      return nullptr;
    }
    auto sv = std::make_unique<ScrollView>();
    sv->setContent(std::move(kids[0]));
    w = std::move(sv);
  } else if (split) {
    if (kids.size() != 2) {
      err = path + ": 'splitter' needs exactly two children";
      return nullptr;
    }
    auto sp = std::make_unique<Splitter>(orientation(j, Orientation::Horizontal), float(j.getNum("ratio", 0.3)));
    sp->setPane(0, std::move(kids[0]));
    sp->setPane(1, std::move(kids[1]));
    w = std::move(sp);
  } else if (tabs) {
    const std::vector<std::string> titles = strings(j.get("titles"));
    if (titles.size() != kids.size()) {
      err = path + ": 'tabs' needs one title per child";
      return nullptr;
    }
    auto tv = std::make_unique<TabView>();
    for (size_t i = 0; i < kids.size(); ++i) tv->addTab(titles[i], std::move(kids[i]));
    w = std::move(tv);
  } else {
    w = it->second.make(j);
    for (auto& k : kids) w->addChild(std::move(k));
  }

  w->setId(text(j, "id"));
  if (const std::string s = text(j, "style"); !s.empty()) w->setStyleClass(s);
  w->setTooltip(text(j, "tooltip"));
  w->setVisible(j.getBool("visible", true));
  w->setEnabled(j.getBool("enabled", true));
  if (const Json* l = j.get("layout")) {
    Layout& lay = w->layout();
    lay.anchorMin = vec2(l->get("anchor_min"), lay.anchorMin);
    lay.anchorMax = vec2(l->get("anchor_max"), lay.anchorMax);
    lay.insets = edges(l->get("insets"));
    lay.offset = vec2(l->get("offset"), {});
    lay.pivot = vec2(l->get("pivot"), {});
    lay.size = vec2(l->get("size"), {});
    lay.minSize = vec2(l->get("min_size"), {});
    lay.margin = edges(l->get("margin"));
    lay.flex = float(l->getNum("flex", 0));
  }
  if (const Json* on = j.get("on"))
    for (const auto& [event, decl] : on->obj) w->onScript(event, decl.str);
  return w;
}

}  // namespace

std::unique_ptr<Widget> buildWidget(const Json& json, std::string& err) { return build(json, "ui", err); }

std::unique_ptr<Widget> buildWidget(const std::string& text, std::string& err) {
  Json j;
  if (!Json::parse(text, j, &err)) return nullptr;
  return buildWidget(j, err);
}

}  // namespace litholattice::ui
