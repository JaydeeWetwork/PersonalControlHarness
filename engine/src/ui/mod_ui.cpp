// Mod UI confinement + badging (mod hardening item 5, 2026-10-11).
#include "litholattice/ui/mod_ui.h"

#include <vector>

#include "litholattice/ui/ui_root.h"

namespace litholattice::ui {
namespace {

// The MOD tag colours are FIXED (not theme lookups): no theme pack — and no
// mod, which ships no theme packs — can restyle or hide the badge.
constexpr Color kBadgeStripBg{0.10f, 0.07f, 0.02f, 1.f};
constexpr Color kBadgeTagBg{1.f, 0.62f, 0.f, 1.f};
constexpr Color kBadgeTagText{0.f, 0.f, 0.f, 1.f};
constexpr Color kBadgeText{1.f, 0.85f, 0.55f, 1.f};

bool isIdChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// Iterative pre-scan of mod JSON: nesting depth + node count, so the
// recursive widget factory below can never be stack-overflowed by a hostile
// layout. Counts every value node; depth counts object/array nesting.
bool scanJson(const Json& root, std::string& err) {
  struct Frame {
    const Json* j;
    size_t child = 0;
  };
  std::vector<Frame> stack;
  stack.push_back({&root, 0});
  size_t nodes = 0;
  size_t maxDepth = 1;
  while (!stack.empty()) {
    Frame& f = stack.back();
    if (f.child == 0) {
      ++nodes;
      if (nodes > kModUiMaxJsonNodes) {
        err = "mod UI: layout exceeds the node budget";
        return false;
      }
      maxDepth = std::max(maxDepth, stack.size());
      if (maxDepth > kModUiMaxJsonDepth) {
        err = "mod UI: layout nests too deep";
        return false;
      }
    }
    const Json* next = nullptr;
    if (f.j->type == Json::Type::Object && f.child < f.j->obj.size()) {
      next = &f.j->obj[f.child].second;
      ++f.child;
    } else if (f.j->type == Json::Type::Array && f.child < f.j->arr.size()) {
      next = &f.j->arr[f.child];
      ++f.child;
    }
    if (next) {
      stack.push_back({next, 0});
    } else {
      stack.pop_back();
    }
  }
  return true;
}

// Walks a built widget tree (iterative): counts widgets and rejects reserved
// style classes anywhere in the subtree, including on the content root.
bool validateTree(const Widget& root, std::string& err) {
  std::vector<const Widget*> stack{&root};
  size_t count = 0;
  while (!stack.empty()) {
    const Widget* w = stack.back();
    stack.pop_back();
    if (++count > kModUiMaxWidgets) {
      err = "mod UI: panel exceeds the widget budget";
      return false;
    }
    if (ModUiFactory::isReservedStyle(w->styleClass())) {
      err = std::string("mod UI: reserved style class '") + w->styleClass() +
            "' is not available to mods";
      return false;
    }
    for (const auto& child : w->children()) stack.push_back(child.get());
  }
  return true;
}

}  // namespace

ModPanel::ModPanel(std::string modId, std::string title)
    : Panel(kModPanelStyleClass), modId_(std::move(modId)), title_(std::move(title)) {
  body_ = &add<StackPanel>(Orientation::Vertical, 6.f);
}

float ModPanel::badgeHeight() const {
  return theme().glyphSize(style().textScale).y + 8.f;
}

Rect ModPanel::badgeRect() const {
  const float h = badgeHeight() + style().padding.top;
  return {rect().x, rect().y, rect().w, h};
}

void ModPanel::arrange(const Theme& theme) {
  const Rect c = contentRect();
  const float strip = badgeHeight() + style().padding.top;
  body_->place({c.x, c.y + strip, c.w, std::max(0.f, c.h - strip)}, theme);
}

void ModPanel::paint(Painter& p) const {
  Widget::paint(p);   // background + border of the "mod_panel" frame treatment
  paintChildren(p);   // the mod's content, confined below the badge strip
  paintBadge(p);      // LAST: nothing the mod owns can cover or remove it
}

void ModPanel::paintBadge(Painter& p) const {
  const Rect bar = badgeRect();
  if (bar.empty()) return;
  p.fill(bar, kBadgeStripBg);
  const float tagW = theme().textWidth("MOD", style().textScale) + 12.f;
  const Rect tag{bar.x + 6.f, bar.y + 4.f, tagW, std::max(0.f, bar.h - 8.f)};
  p.fill(tag, kBadgeTagBg);
  p.text({tag.x + 6.f, tag.y + 2.f}, "MOD", kBadgeTagText, style().textScale);
  const std::string who = modId_ + " - " + title_;
  p.text({tag.right() + 8.f, bar.y + 4.f}, who, kBadgeText, style().textScale);
  // A fixed amber rule under the strip: the frame treatment survives even a
  // pack that recolours "mod_panel" into invisibility.
  p.fill({bar.x, bar.bottom() - 1.f, bar.w, 1.f}, kBadgeTagBg);
}

bool ModUiFactory::isReservedStyle(std::string_view styleClass) {
  for (const char* r : kModReservedStyles)
    if (styleClass == r) return true;
  return false;
}

bool ModUiFactory::isValidModId(std::string_view id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id)
    if (!isIdChar(c)) return false;
  return true;
}

std::unique_ptr<ModPanel> ModUiFactory::createPanel(const std::string& modId,
                                                   const std::string& panelId,
                                                   const std::string& title,
                                                   const std::string& bodyJson,
                                                   std::string& err) {
  if (!isValidModId(modId)) {
    err = "mod UI: invalid mod id (want [a-z0-9_]{1,64})";
    return nullptr;
  }
  if (!isValidModId(panelId)) {
    err = "mod UI: invalid panel id (want [a-z0-9_]{1,64})";
    return nullptr;
  }
  if (title.empty() || title.size() > kModUiMaxTitleLen) {
    err = "mod UI: title must be 1..128 characters";
    return nullptr;
  }

  auto panel = std::make_unique<ModPanel>(modId, title);
  panel->setId(panelId);

  if (!bodyJson.empty()) {
    Json json;
    if (!Json::parse(bodyJson, json, &err)) {
      err = "mod UI: bad layout JSON: " + err;
      return nullptr;
    }
    if (!scanJson(json, err)) return nullptr;  // depth / node DoS guard
    std::string buildErr;
    std::unique_ptr<Widget> content = buildWidget(json, buildErr);
    if (!content) {
      err = "mod UI: " + buildErr;
      return nullptr;
    }
    // The factory's type table is closed (no dialog/modal/popup types exist),
    // and mod handlers are script functions only; the remaining check is the
    // reserved style classes (no system-look spoofing).
    if (!validateTree(*content, err)) return nullptr;
    panel->body().addChild(std::move(content));
  }
  return panel;
}

ModPanel& ModUiFactory::attachToRoot(UiRoot& ui, std::unique_ptr<ModPanel> panel) {
  // Main layer only. Modals and popups stay engine-exclusive: a mod can never
  // seize the screen with a fake system prompt.
  Widget& w = ui.root().addChild(std::move(panel));
  return static_cast<ModPanel&>(w);
}

}  // namespace litholattice::ui
