// Interfaces the UI needs from the outside, injected by the game: drawing (IUiRenderer, one
// implementation in render serves both the gl33 and the Vulkan backend) and the clipboard.
#pragma once
#include <string>
#include <string_view>
#include "litholattice/ui/geometry.h"

namespace litholattice::ui {

/// Immediate drawing in paint order. The UI owns nothing GPU-side.
class IUiRenderer {
 public:
  virtual ~IUiRenderer() = default;
  virtual void fillRect(const Rect& r, Color c) = 0;
  /// Single-line ASCII text at `pos` (top-left), glyph size = theme glyph size * `scale`.
  virtual void drawText(Vec2 pos, std::string_view text, Color c, float scale) = 0;
  /// A named image (block icon, texture) stretched into `r`, multiplied by `tint`.
  virtual void drawImage(const Rect& r, std::string_view image, Color tint) = 0;
  /// Restricts drawing to `r` intersected with the current clip until popClip().
  virtual void pushClip(const Rect& r) = 0;
  virtual void popClip() = 0;
};

class IClipboard {
 public:
  virtual ~IClipboard() = default;
  virtual std::string text() const = 0;
  virtual void setText(const std::string& text) = 0;
};

/// Process-local clipboard (tests, or when no OS clipboard is wired).
class MemoryClipboard final : public IClipboard {
 public:
  std::string text() const override { return text_; }
  void setText(const std::string& text) override { text_ = text; }

 private:
  std::string text_;
};

}  // namespace litholattice::ui
