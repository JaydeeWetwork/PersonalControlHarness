// Basic controls: Panel, Label, Button, Image (icon), Checkbox / Toggle, ProgressBar, Slider,
// NumberField (integer, e.g. signal 0..64), Slot (an inventory cell) and Divider (separator line).
// Events: Button "click"; Checkbox, Slider, NumberField "change"; Slot "click" / "rightclick".
#pragma once
#include <string>
#include "litholattice/ui/widget.h"
#include "litholattice/ui/containers.h"

namespace litholattice::ui {

/// Background + border container (style "panel"); children by anchors.
class Panel : public Widget {
 public:
  explicit Panel(std::string styleClass = "panel") : Widget(std::move(styleClass)) {}
};

/// Single- or multi-line text ('\n' breaks lines). Not interactive.
class Label : public Widget {
 public:
  explicit Label(std::string text = {}, std::string styleClass = "label");
  const std::string& text() const { return text_; }
  Label& setText(std::string t) { text_ = std::move(t); return *this; }
  Label& setColor(Color c) { color_ = c; hasColor_ = true; return *this; }
  Label& setCentered(bool c) { centered_ = c; return *this; }
  Vec2 measure(const Theme& theme) const override;
  Widget* hitTest(Vec2 p) override;  ///< only when it has a tooltip

 protected:
  void paint(Painter& p) const override;

 private:
  std::string text_;
  Color color_;
  bool hasColor_ = false, centered_ = false;
};

/// Clickable text button: "click" on release inside, or Enter / Space while focused.
class Button : public Widget {
 public:
  explicit Button(std::string text = {}, std::string styleClass = "button");
  const std::string& text() const { return text_; }
  Button& setText(std::string t) { text_ = std::move(t); return *this; }
  Vec2 measure(const Theme& theme) const override;
  bool onPointerDown(const UiEvent& e) override;
  void onPointerUp(const UiEvent& e) override;
  bool onKey(const UiEvent& e) override;

 protected:
  void paint(Painter& p) const override;

 private:
  std::string text_;
};

/// A named image (block icon, texture) drawn through IUiRenderer::drawImage.
class Image : public Widget {
 public:
  explicit Image(std::string image = {}, Vec2 size = {32, 32});
  const std::string& image() const { return image_; }
  Image& setImage(std::string name) { image_ = std::move(name); return *this; }
  Image& setTint(Color c) { tint_ = c; return *this; }
  Vec2 measure(const Theme&) const override { return size_; }

 protected:
  void paint(Painter& p) const override;

 private:
  std::string image_;
  Vec2 size_;
  Color tint_;
};

/// Check box with a label; `toggle` style draws a switch instead of a box.
class Checkbox : public Widget {
 public:
  explicit Checkbox(std::string text = {}, bool checked = false, bool toggle = false);
  bool checked() const { return checked_; }
  /// Sets the state; emits "change" when it differs and `notify`.
  void setChecked(bool c, bool notify = false);
  Vec2 measure(const Theme& theme) const override;
  bool onPointerDown(const UiEvent& e) override;
  void onPointerUp(const UiEvent& e) override;
  bool onKey(const UiEvent& e) override;

 protected:
  void paint(Painter& p) const override;

 private:
  std::string text_;
  bool checked_, toggle_;
};

/// Horizontal bar filled to value() in [0, 1], with optional centred text.
class ProgressBar : public Widget {
 public:
  ProgressBar() : Widget("progress") {}
  float value() const { return value_; }
  ProgressBar& setValue(float v);
  ProgressBar& setText(std::string t) { text_ = std::move(t); return *this; }
  Vec2 measure(const Theme& theme) const override;

 protected:
  void paint(Painter& p) const override;

 private:
  float value_ = 0;
  std::string text_;
};

/// Horizontal slider over [min, max] with an optional step; drag, wheel or arrow keys.
class Slider : public Widget {
 public:
  Slider(float min = 0, float max = 1, float value = 0, float step = 0);
  float value() const { return value_; }
  /// Clamps and snaps; emits "change" when it differs and `notify`.
  void setValue(float v, bool notify = false);
  Vec2 measure(const Theme& theme) const override;
  bool onPointerDown(const UiEvent& e) override;
  void onPointerMove(const UiEvent& e) override;
  bool onWheel(const UiEvent& e) override;
  bool onKey(const UiEvent& e) override;

 protected:
  void paint(Painter& p) const override;

 private:
  void setFromX(float x);
  float min_, max_, value_, step_;
};

/// Integer field with - / + buttons: typing digits, Up / Down, wheel, clamped to [min, max].
class NumberField : public Widget {
 public:
  NumberField(int min = 0, int max = 64, int value = 0);
  int value() const { return value_; }
  /// Clamps; emits "change" when it differs and `notify`.
  void setValue(int v, bool notify = false);
  int min() const { return min_; }
  int max() const { return max_; }
  Vec2 measure(const Theme& theme) const override;
  bool onPointerDown(const UiEvent& e) override;
  bool onWheel(const UiEvent& e) override;
  bool onKey(const UiEvent& e) override;
  bool onText(char32_t c) override;
  void onFocusChanged(bool focused) override;

 protected:
  void paint(Painter& p) const override;

 private:
  Rect minusRect() const;
  Rect plusRect() const;
  void commitTyping();
  int min_, max_, value_;
  std::string typing_;  ///< digits typed since focus (committed on Enter / focus loss)
};

/// Inventory / hotbar cell: an icon, an optional count or caption, selection highlight.
/// Left click emits "click", right click "rightclick".
class Slot : public Widget {
 public:
  explicit Slot(Vec2 size = {40, 40});
  Slot& setIcon(std::string image, Color tint = {}) { icon_ = std::move(image); tint_ = tint; return *this; }
  const std::string& icon() const { return icon_; }
  Slot& setCaption(std::string c) { caption_ = std::move(c); return *this; }
  Slot& setSelected(bool s) { selected_ = s; return *this; }
  bool selected() const { return selected_; }
  /// Any per-slot payload (block id, item index); not interpreted by the UI.
  Slot& setData(int d) { data_ = d; return *this; }
  int data() const { return data_; }
  Vec2 measure(const Theme&) const override { return size_; }
  bool onPointerDown(const UiEvent& e) override;
  bool onKey(const UiEvent& e) override;

 protected:
  void paint(Painter& p) const override;

 private:
  Vec2 size_;
  std::string icon_, caption_;
  Color tint_;
  bool selected_ = false;
  int data_ = 0;
};

/// Thin separator line (horizontal or vertical) drawn in the style's accent color.
/// Non-interactive; visually groups menu sections. Style class "divider".
class Divider : public Widget {
 public:
  explicit Divider(Orientation o = Orientation::Horizontal, float thickness = 1.f);
  Orientation orientation() const { return orientation_; }
  Divider& setThickness(float t) { thickness_ = t < 1.f ? 1.f : t; return *this; }
  float thickness() const { return thickness_; }
  Vec2 measure(const Theme& theme) const override;
  Widget* hitTest(Vec2) override { return nullptr; }

 protected:
  void paint(Painter& p) const override;

 private:
  Orientation orientation_;
  float thickness_;
};

}  // namespace litholattice::ui
