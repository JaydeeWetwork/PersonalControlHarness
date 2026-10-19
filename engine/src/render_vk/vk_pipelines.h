// The render pass (colour + depth, cleared each frame), the one descriptor-set layout and
// pipeline layout every pipeline shares, and the graphics pipelines built from the embedded
// SPIR-V: chunks, depth-tested lines (block outline), overlay lines (crosshair) and the HUD.
// Private to render_vk.
#pragma once
#include <memory>
#include <string>
#include "vk_device.h"
#include "vk_handles.h"

namespace litholattice::vk {

/// Descriptor bindings of set 0 (see assets/shaders/vk/*).
enum Binding : uint32_t { kBindingFrame = 0, kBindingTextureArray = 1, kBindingPalette = 2, kBindingFont = 3,
                          kBindingSpriteAtlas = 4 };

class RenderPipelines {
 public:
  /// Construction token: only RenderPipelines's own factory can make one, so it can use make_unique / make_shared.
  class Passkey {
    friend class RenderPipelines;
    Passkey() = default;
  };
  explicit RenderPipelines(Passkey) {}
  /// Builds the render pass for the given formats and every pipeline. nullptr + `error` on failure.
  static std::unique_ptr<RenderPipelines> create(const VulkanDevice& dev, VkFormat color, VkFormat depth,
                                                 std::string* error);

  VkRenderPass renderPass() const { return renderPass_.get(); }
  VkDescriptorSetLayout setLayout() const { return setLayout_.get(); }
  VkPipelineLayout layout() const { return layout_.get(); }
  VkPipeline chunk() const { return chunk_.get(); }
  VkPipeline lineDepth() const { return lineDepth_.get(); }
  VkPipeline lineOverlay() const { return lineOverlay_.get(); }
  VkPipeline hud() const { return hud_.get(); }
  /// Sprite pass pipelines (JD wiring 2026-10-13): same vertex layout and
  /// shader pair; cutout = depth-write + alpha-test discard, blend /
  /// additive = depth-test only, per-shade blend factors.
  VkPipeline spriteCutout() const { return spriteCutout_.get(); }
  VkPipeline spriteBlend() const { return spriteBlend_.get(); }
  VkPipeline spriteAdditive() const { return spriteAdditive_.get(); }

 private:
  UniqueRenderPass renderPass_;
  UniqueDescriptorSetLayout setLayout_;
  UniquePipelineLayout layout_;
  UniquePipeline chunk_, lineDepth_, lineOverlay_, hud_;
  UniquePipeline spriteCutout_, spriteBlend_, spriteAdditive_;
};

}  // namespace litholattice::vk
