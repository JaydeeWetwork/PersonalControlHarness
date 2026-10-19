#include "vk_pipelines.h"
#include <cstdint>
#include <span>
#include <vector>
#include "vk_frame_uniforms.h"
#include "litholattice/render/overlay_geometry.h"
#include "litholattice/render/renderer.h"
// SPIR-V generated at build time from assets/shaders/vk (glslang --vn): const uint32_t arrays.
#include "spv/chunk_frag.h"
#include "spv/chunk_vert.h"
#include "spv/hud_frag.h"
#include "spv/hud_vert.h"
#include "spv/line_frag.h"
#include "spv/line_vert.h"
#include "spv/sprite_frag.h"
#include "spv/sprite_vert.h"

namespace litholattice::vk {
namespace {

UniqueShaderModule shaderModule(const VulkanDevice& dev, std::span<const uint32_t> code) {
  auto ci = make<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
  ci.codeSize = code.size_bytes();
  ci.pCode = code.data();
  VkShaderModule m = VK_NULL_HANDLE;
  check(vkCreateShaderModule(dev.device(), &ci, nullptr, &m), "vkCreateShaderModule");
  return UniqueShaderModule(dev.device(), m);
}

/// The fixed-function choices that differ between the pipelines.
struct PipelineDesc {
  std::span<const uint32_t> vertexCode, fragmentCode;
  uint32_t stride = 0;
  std::vector<VkVertexInputAttributeDescription> attributes;
  VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  bool depthTest = true, depthWrite = true, cullBack = false, alphaBlend = false;
  // Blend factors used when alphaBlend is set (defaults = the HUD's
  // SRC_ALPHA / ONE_MINUS_SRC_ALPHA). The sprite additive pipeline sets
  // dst = ONE (GL33's glBlendFunc(GL_SRC_ALPHA, GL_ONE)).
  VkBlendFactor srcBlend = VK_BLEND_FACTOR_SRC_ALPHA, dstBlend = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
};

UniquePipeline buildPipeline(const VulkanDevice& dev, VkRenderPass pass, VkPipelineLayout layout,
                             const PipelineDesc& d) {
  const UniqueShaderModule vs = shaderModule(dev, d.vertexCode), fs = shaderModule(dev, d.fragmentCode);
  if (!vs || !fs) return {};
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs.get(), "main", nullptr};
  stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs.get(), "main", nullptr};

  const VkVertexInputBindingDescription binding{0, d.stride, VK_VERTEX_INPUT_RATE_VERTEX};
  auto vi = make<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &binding;
  vi.vertexAttributeDescriptionCount = uint32_t(d.attributes.size());
  vi.pVertexAttributeDescriptions = d.attributes.data();
  auto ia = make<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
  ia.topology = d.topology;
  auto vp = make<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  auto rs = make<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = d.cullBack ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
  // The shaders negate y (OpenGL-style matrices), which keeps OpenGL's counter-clockwise front faces.
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.f;
  auto ms = make<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  auto ds = make<VkPipelineDepthStencilStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
  ds.depthTestEnable = d.depthTest;
  ds.depthWriteEnable = d.depthWrite;
  ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  if (d.alphaBlend) {
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = d.srcBlend;
    blend.dstColorBlendFactor = d.dstBlend;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
  }
  auto cb = make<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;
  const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  auto dyn = make<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dynamic;

  auto ci = make<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
  ci.stageCount = 2;
  ci.pStages = stages;
  ci.pVertexInputState = &vi;
  ci.pInputAssemblyState = &ia;
  ci.pViewportState = &vp;
  ci.pRasterizationState = &rs;
  ci.pMultisampleState = &ms;
  ci.pDepthStencilState = &ds;
  ci.pColorBlendState = &cb;
  ci.pDynamicState = &dyn;
  ci.layout = layout;
  ci.renderPass = pass;
  VkPipeline p = VK_NULL_HANDLE;
  if (!check(vkCreateGraphicsPipelines(dev.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &p), "vkCreateGraphicsPipelines"))
    return {};
  return UniquePipeline(dev.device(), p);
}

UniqueRenderPass buildRenderPass(const VulkanDevice& dev, VkFormat color, VkFormat depth) {
  VkAttachmentDescription att[2]{};
  att[0].format = color;
  att[0].samples = VK_SAMPLE_COUNT_1_BIT;
  att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  att[1] = att[0];
  att[1].format = depth;
  att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  const VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  const VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &colorRef;
  sub.pDepthStencilAttachment = &depthRef;
  // Wait for the previous user of the swapchain image / depth buffer before writing them.
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
  dep.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  auto ci = make<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
  ci.attachmentCount = 2;
  ci.pAttachments = att;
  ci.subpassCount = 1;
  ci.pSubpasses = &sub;
  ci.dependencyCount = 1;
  ci.pDependencies = &dep;
  VkRenderPass rp = VK_NULL_HANDLE;
  check(vkCreateRenderPass(dev.device(), &ci, nullptr, &rp), "vkCreateRenderPass");
  return UniqueRenderPass(dev.device(), rp);
}

}  // namespace

std::unique_ptr<RenderPipelines> RenderPipelines::create(const VulkanDevice& dev, VkFormat color, VkFormat depth,
                                                         std::string* error) {
  auto p = std::make_unique<RenderPipelines>(Passkey{});
  p->renderPass_ = buildRenderPass(dev, color, depth);

  const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  const VkDescriptorSetLayoutBinding bindings[] = {
      {kBindingFrame, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, both, nullptr},
      {kBindingTextureArray, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {kBindingPalette, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {kBindingFont, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {kBindingSpriteAtlas, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  auto li = make<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
  li.bindingCount = uint32_t(std::size(bindings));
  li.pBindings = bindings;
  VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
  if (check(vkCreateDescriptorSetLayout(dev.device(), &li, nullptr, &setLayout), "vkCreateDescriptorSetLayout"))
    p->setLayout_ = UniqueDescriptorSetLayout(dev.device(), setLayout);

  const VkPushConstantRange push{both, 0, kPushConstantBytes};
  auto pi = make<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
  pi.setLayoutCount = 1;
  pi.pSetLayouts = &setLayout;
  pi.pushConstantRangeCount = 1;
  pi.pPushConstantRanges = &push;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  if (p->renderPass_ && p->setLayout_ && check(vkCreatePipelineLayout(dev.device(), &pi, nullptr, &layout), "vkCreatePipelineLayout"))
    p->layout_ = UniquePipelineLayout(dev.device(), layout);
  if (!p->layout_) {
    *error = "render pass / pipeline layout creation failed";
    return nullptr;
  }

  // Chunks: PackedVertex 12-byte attribute prefix: xy (R16G16) @0 + z (R16) @4 +
  // info (R8G8B8A8) @6 + shade (R8G8) @10. The trailing slot u32 @12 is not bound.
  // (Fix 2026-10-11: slot moved to the end so this prefix is stable.)
  PipelineDesc chunk;
  chunk.vertexCode = kSpvChunkVert;
  chunk.fragmentCode = kSpvChunkFrag;
  chunk.stride = sizeof(PackedVertex);
  chunk.attributes = {{0, 0, VK_FORMAT_R16G16_UINT, 0}, {3, 0, VK_FORMAT_R16_UINT, 4},
                      {1, 0, VK_FORMAT_R8G8B8A8_UINT, 6}, {2, 0, VK_FORMAT_R8G8_UINT, 10}};
  chunk.cullBack = true;
  p->chunk_ = buildPipeline(dev, p->renderPass(), layout, chunk);

  PipelineDesc line;
  line.vertexCode = kSpvLineVert;
  line.fragmentCode = kSpvLineFrag;
  line.stride = sizeof(Vec3);
  line.attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}};
  line.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
  p->lineDepth_ = buildPipeline(dev, p->renderPass(), layout, line);
  line.depthTest = line.depthWrite = false;
  p->lineOverlay_ = buildPipeline(dev, p->renderPass(), layout, line);

  PipelineDesc hud;
  hud.vertexCode = kSpvHudVert;
  hud.fragmentCode = kSpvHudFrag;
  hud.stride = sizeof(HudVertex);
  hud.attributes = {{0, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(HudVertex, x))},
                    {1, 0, VK_FORMAT_R32G32_SFLOAT, uint32_t(offsetof(HudVertex, u))},
                    {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, uint32_t(offsetof(HudVertex, r))}};
  hud.depthTest = hud.depthWrite = false;
  hud.alphaBlend = true;
  p->hud_ = buildPipeline(dev, p->renderPass(), layout, hud);

  // Sprites (JD wiring 2026-10-13): vk_sprite::SpriteVertex layout —
  // pos (vec3) @0, uv (vec2) @12, tint (vec4) @20, page (float) @36.
  // Double-sided like GL33 (cullBack stays false), depth tested against
  // the world; cutout writes depth, the blended passes do not.
  PipelineDesc sprite;
  sprite.vertexCode = kSpvSpriteVert;
  sprite.fragmentCode = kSpvSpriteFrag;
  sprite.stride = 40;  // sizeof(vk_sprite::SpriteVertex), asserted in vk_renderer.cpp
  sprite.attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                       {1, 0, VK_FORMAT_R32G32_SFLOAT, 12},
                       {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 20},
                       {3, 0, VK_FORMAT_R32_SFLOAT, 36}};
  p->spriteCutout_ = buildPipeline(dev, p->renderPass(), layout, sprite);
  sprite.alphaBlend = true;
  sprite.depthWrite = false;
  p->spriteBlend_ = buildPipeline(dev, p->renderPass(), layout, sprite);
  sprite.dstBlend = VK_BLEND_FACTOR_ONE;  // additive: SRC_ALPHA, ONE (as GL33)
  p->spriteAdditive_ = buildPipeline(dev, p->renderPass(), layout, sprite);

  if (!p->chunk_ || !p->lineDepth_ || !p->lineOverlay_ || !p->hud_ || !p->spriteCutout_ ||
      !p->spriteBlend_ || !p->spriteAdditive_) {
    *error = "graphics pipeline creation failed";
    return nullptr;
  }
  return p;
}

}  // namespace litholattice::vk
