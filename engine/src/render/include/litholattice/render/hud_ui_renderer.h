// ui::IUiRenderer on top of the HUD layer: UI draws become ordered, CPU-clipped HudDraws that
// both backends (gl33 and Vulkan) already render through buildHudVertices, so the UI needs no
// backend-specific code. Images resolve to a colour swatch through a callback (the HUD has no
// texture path yet).
#pragma once
#include <functional>
#include <optional>
#include <string_view>
#include <vector>
#include "litholattice/assets/image.h"
#include "litholattice/render/renderer.h"
#include "litholattice/ui/ui_renderer.h"

namespace litholattice {

class HudUiRenderer final : public ui::IUiRenderer {
 public:
  using ImageResolver = std::function<std::optional<ui::Color>(std::string_view image)>;

  /// Appends to `layer.draws` (the caller clears the layer each frame).
  /// Pixel-art icons: a small RGBA image drawn as one rect per opaque pixel (alpha >= 50%),
  /// so the HUD needs no textures. Keep icons small (16 x 16, see downsampleIcon).
  using IconResolver = std::function<const Image*(std::string_view image)>;
  explicit HudUiRenderer(HudLayer& layer, ImageResolver images = {}, IconResolver icons = {})
      : layer_(layer), images_(std::move(images)), icons_(std::move(icons)) {}

  void fillRect(const ui::Rect& r, ui::Color c) override;
  void drawText(ui::Vec2 pos, std::string_view text, ui::Color c, float scale) override;
  void drawImage(const ui::Rect& r, std::string_view image, ui::Color tint) override;
  void pushClip(const ui::Rect& r) override;
  void popClip() override;

 private:
  HudClip clip() const { return clips_.empty() ? HudClip{} : clips_.back(); }
  HudLayer& layer_;
  ImageResolver images_;
  IconResolver icons_;
  std::vector<HudClip> clips_;
};

/// Box-filters `src` down to `size` x `size` (alpha-weighted colour), for HUD icons.
Image downsampleIcon(const Image& src, int size = 16);

}  // namespace litholattice
