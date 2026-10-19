// Vulkan backend for IRenderer: one graphics queue, kFramesInFlight frames in flight (fence +
// acquire semaphore per frame, render-done semaphore per swapchain image), swapchain rebuilt on
// resize / out-of-date. Chunk vertices live in a VertexArena; replaced meshes are freed once no
// frame in flight can still read them. Main/render thread only.
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>
#include "vk_device.h"
#include "vk_frame_uniforms.h"
#include "vk_pipelines.h"
#include "vk_resources.h"
#include "vk_swapchain.h"
#include "vk_texture_array.h"
#include "vk_vertex_arena.h"
#include "litholattice/assets/shader_library.h"
#include "litholattice/core/log.h"
#include "litholattice/render/block_materials.h"
#include "litholattice/render/overlay_geometry.h"
#include "litholattice/render_vk/sprite_vertices.h"
#include "litholattice/render_vk/vulkan_renderer.h"

namespace litholattice {
namespace {

using namespace vk;

constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kMaxQuads = kMaxQuadsPerMesh;  // shared quad index buffer size (renderer.h)
constexpr VkDeviceSize kOverlayBytes = 4 << 20;  // per frame: lines + HUD vertices (~130k: a full-screen editor)
constexpr VkDeviceSize kStagingBytes = VkDeviceSize(8) << 20;  // per frame slot (device-local vertex uploads)
constexpr VkDeviceSize kHudOffset = 512;         // HUD vertices start here (lines before)

static_assert(sizeof(vk_sprite::SpriteVertex) == 40, "sprite vertex layout (vk_pipelines attributes)");

/// Linear, clamp-to-edge sampler for the sprite atlas pages (GL33 uses
/// GL_LINEAR for its sprite page textures; the block texture array's
/// nearest-mag filtering is the chunk path's choice, not the sprites').
UniqueSampler createLinearSampler(const VulkanDevice& dev) {
  auto si = make<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler s = VK_NULL_HANDLE;
  check(vkCreateSampler(dev.device(), &si, nullptr, &s), "vkCreateSampler (sprite atlas)");
  return UniqueSampler(dev.device(), s);
}

int64_t meshKey(int cx, int sy, int cz) {
  // Fix 2026-10-11 (audit): left-shifting negative int64_t is UB; cast through
  // uint64_t first, then back (bit-preserving).
  return int64_t((uint64_t(uint32_t(cx)) << 40) ^ (uint64_t(uint32_t(sy)) << 32) ^ uint64_t(uint32_t(cz)));
}

class VulkanRenderer final : public IRenderer {
 public:
  ~VulkanRenderer() override {
    if (dev_) vkDeviceWaitIdle(dev_->device());
  }

  bool init(const VulkanRendererDesc& desc, std::string* error) {
    dev_ = VulkanDevice::create(desc, error);
    if (!dev_) return false;
    const VkDevice d = dev_->device();
    auto pi = make<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pi.queueFamilyIndex = dev_->queueFamily();
    VkCommandPool pool = VK_NULL_HANDLE;
    if (!check(vkCreateCommandPool(d, &pi, nullptr, &pool), "vkCreateCommandPool")) return fail(error, "command pool");
    pool_ = UniqueCommandPool(d, pool);

    const auto surface = chooseSurfaceConfig(*dev_, desc.vsync, desc.presentMode);
    if (!surface) return fail(error, "no usable surface format / depth format");
    surface_ = *surface;
    pipelines_ = RenderPipelines::create(*dev_, surface_.color.format, surface_.depth, error);
    if (!pipelines_) return false;
    VertexArena::Kind kind = VertexArena::Kind::Device;
    if (!VertexArena::parseKind(desc.vertexArena, kind)) logError("vulkan: unknown vertex arena '%s'; using device", desc.vertexArena.c_str());
    arena_ = std::make_unique<VertexArena>(*dev_, kind);
    logInfo("vulkan: vertex arena %s, quad index buffer %s", VertexArena::kindName(kind), kind == VertexArena::Kind::Host ? "host-visible" : "device-local");
    if (!createStaticBuffers() || !createFrames() || !createDescriptors()) return fail(error, "frame resources");
    textures_ = std::make_unique<VkTextureArray>(*dev_, pool_.get(), [this] { writeDescriptors(); });
    if (!textures_->upload(TextureRegistry{})) return fail(error, "initial texture upload");
    logInfo("vulkan: using %s", dev_->description().c_str());
    return true;
  }

  const char* name() const override { return "Vulkan"; }
  std::string presentMode() const override { return presentModeName(surface_.presentMode); }
  std::string deviceInfo() const override { return dev_->description(); }
  ITextureArray& textureArray() override { return *textures_; }

  void setMaterials(const BlockMaterials& m) override {
    const MaterialBindings& b = m.bindings();
    FrameUniforms& u = materials_;
    for (size_t c = 0; c < 2; ++c) { u.stripPalette[c].value = b.stripPalette[c]; u.stripRing[c].value = b.stripRing[c]; }
    for (size_t t = 0; t < 3; ++t) {
      u.tierPalette[t].value = b.tierPalette[t];
      u.tierMask[t].value = b.tierMask[t];
      u.tierRim[t].value = b.tierRim[t];
    }
    u.pipMask = b.pipMask;
    u.cellMask = b.cellMask;
    for (size_t c = 0; c < 2; ++c) u.stripDot[c].value = b.stripDot[c];
    u.digits = b.debugDigits;
    const auto anims = m.animations();
    for (size_t i = 0; i < std::size(u.animStart); ++i) {
      const ShaderAnimation a = i < anims.size() ? anims[i] : ShaderAnimation{};
      u.animStart[i].value = a.sequenceStart;
      u.animLength[i].value = a.sequenceLength;
      u.animFrameMs[i].value = a.frameMs;
    }
    const auto frames = m.animationFrames();
    for (size_t i = 0; i < std::size(u.animSequence); ++i) u.animSequence[i].value = i < frames.size() ? frames[i] : 0;
  }

  // Vulkan shaders are SPIR-V built in at compile time; pack shader files are OpenGL GLSL.
  int loadShaders(const ShaderLibrary& lib) override {
    if (lib.size() > 0)
      logInfo("vulkan: pack shader files are OpenGL-only; using the built-in SPIR-V (assets/shaders/vk)");
    return 0;
  }

  void uploadEntityMesh(const std::vector<PackedVertex>&) override {
    // TODO: VK entity mesh — entities/items are invisible on Vulkan until this lands.
    // Loud, not silent (fix 2026-10-11): rate-limit the warning to once per process.
    static bool warned = false;
    if (!warned) { warned = true; logWarn("vulkan: uploadEntityMesh not implemented — entities invisible"); }
  }
  void uploadChunkMesh(const ChunkMesh& m) override {
    const int64_t key = meshKey(m.cx, m.sy, m.cz);
    const auto it = meshes_.find(key);
    if (it != meshes_.end()) {
      retire(it->second.slice);
      meshes_.erase(it);
    }
    if (m.verts.empty()) return;
    const VkDeviceSize bytes = m.verts.size() * sizeof(PackedVertex);
    const auto slice = arena_->allocate(bytes);
    if (!slice) {
      logError("vulkan: out of vertex memory; sub-chunk (%d,%d,%d) not drawn", m.cx, m.sy, m.cz);
      return;
    }
    if (uint8_t* dst = arena_->data(*slice)) std::memcpy(dst, m.verts.data(), size_t(bytes));  // host / ReBAR: in place
    else stage(arena_->buffer(*slice), slice->offset, m.verts.data(), bytes);
    const uint32_t quads = std::min<uint32_t>(uint32_t(m.verts.size() / 4), kMaxQuads);
    Mesh gm{*slice, quads * 6, m.cx, m.sy, m.cz};
    for (int i = 0; i < 4; ++i) { gm.pulseP0[i] = m.pulseP0[i]; gm.pulseP1[i] = m.pulseP1[i]; gm.traceP0[i] = m.traceP0[i]; gm.traceP1[i] = m.traceP1[i]; }
    meshes_[key] = gm;
  }

  // PBR (BinaryQuad) path: Vulkan has no chunk_pbr pipeline. Loud once, not
  // silent (P1-RENDER H4, mirroring the uploadEntityMesh pattern above):
  // uploads and material tables are dropped, never quietly.
  void uploadPbrChunkMesh(const PbrChunkMesh&) override {
    static bool warned = false;
    if (!warned) { warned = true; logWarn("vulkan: uploadPbrChunkMesh not implemented — PBR (BinaryQuad) meshes invisible on Vulkan"); }
  }
  void setPbrMaterials(const MaterialRegistry&) override {
    static bool warned = false;
    if (!warned) { warned = true; logWarn("vulkan: setPbrMaterials not implemented — no PBR material table on Vulkan"); }
  }

  void clearChunkMeshes() override {
    vkDeviceWaitIdle(dev_->device());
    for (FrameSlot& f : frames_) { f.copies.clear(); f.stagingUsed = 0; f.stagingSubmitted = false; }
    for (const auto& [k, mesh] : meshes_) arena_->free(mesh.slice);
    for (const Retired& r : retired_) arena_->free(r.slice);
    meshes_.clear();
    retired_.clear();
  }

  void renderFrame(const FrameParams& p) override {
    if (p.width <= 0 || p.height <= 0) return;
    // Sprite atlas (JD wiring 2026-10-13): (re)upload the pages when the
    // game's registry version moved — the GL33 contract, mirrored. The
    // upload waits for the device to go idle (same discipline as the
    // block texture array), so it happens here, before this frame's slot
    // is waited on and recorded.
    if (p.spriteQuads && p.spriteQuadCount > 0 && !p.spriteAtlasPages.empty() &&
        p.spriteAtlasVersion != spriteAtlasUploaded_)
      uploadSpriteAtlas(p);
    // Culling-cache prototypes (JD 2026-10-11): arm per-frame state. All default off.
    cullProto_.beginFrame(p.viewProj, p.yawRad, p.pitchRad, p.timeSec);
    FrameSlot& f = frames_[frameNumber_ % kFramesInFlight];
    const VkDevice d = dev_->device();
    const VkFence fence = f.inFlight.get();
    waitSlot(f);
    releaseRetired();
    if (!swapchain_ || swapchainStale_ || requested_.width != uint32_t(p.width) || requested_.height != uint32_t(p.height))
      if (!recreateSwapchain(uint32_t(p.width), uint32_t(p.height))) return;

    uint32_t image = 0;
    VkResult r = vkAcquireNextImageKHR(d, swapchain_->handle(), UINT64_MAX, f.imageAcquired.get(), VK_NULL_HANDLE, &image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { swapchainStale_ = true; return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { check(r, "vkAcquireNextImageKHR"); return; }
    vkResetFences(d, 1, &fence);
    record(f, image, p);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkSemaphore acquired = f.imageAcquired.get(), done = renderDone_[image].get();
    auto si = make<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &acquired;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &f.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &done;
    if (!check(vkQueueSubmit(dev_->queue(), 1, &si, fence), "vkQueueSubmit")) return;
    f.stagingSubmitted = true;  // its staging bytes are free again once this fence signals
    const VkSwapchainKHR sc = swapchain_->handle();
    auto pi = make<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc;
    pi.pImageIndices = &image;
    r = vkQueuePresentKHR(dev_->queue(), &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchainStale_ = true;  // rebuilt next frame
    else if (r != VK_SUCCESS) check(r, "vkQueuePresentKHR");
    ++frameNumber_;
  }

  RenderStats stats() const override { return stats_; }
  void requestGrab() override { grabArmed_ = true; }
  bool grabFramebuffer(std::vector<uint8_t>& rgb, int& w, int& h) override {
    if (!grabReady_ || !grabStaging_) return false;
    FrameSlot& f = frames_[grabSlot_ % kFramesInFlight];
    // Wait for the armed frame (which carried the copy) to finish on the GPU.
    const VkFence fence = f.inFlight.get();
    if (vkWaitForFences(dev_->device(), 1, &fence, VK_TRUE, 1000000000ULL) != VK_SUCCESS)
      return false;
    w = int(grabExtent_.width);
    h = int(grabExtent_.height);
    const bool bgra = grabFormat_ == VK_FORMAT_B8G8R8A8_UNORM;
    rgb.resize(size_t(w) * size_t(h) * 3);
    const uint8_t* src = grabStaging_->mapped();
    // Vulkan images are top-left origin: rows are already top-first for PNG.
    for (int y = 0; y < h; ++y) {
      const uint8_t* srow = src + size_t(y) * size_t(w) * 4;
      uint8_t* drow = rgb.data() + size_t(y) * size_t(w) * 3;
      for (int x = 0; x < w; ++x) {
        drow[0] = srow[bgra ? 2 : 0];
        drow[1] = srow[1];
        drow[2] = srow[bgra ? 0 : 2];
        srow += 4;
        drow += 3;
      }
    }
    return true;
  }
  void setFrustumMarginDeg(float degrees) override {
    frustumMarginDeg_ = std::clamp(degrees, 0.f, 15.f);  // JD-locked 2026-10-11
  }
  void setCullProtoConfig(const CullProtoConfig& cfg) override { cullProto_.configure(cfg); }
  CullProtoStats cullProtoStats() const override { return cullProto_.stats(); }

 private:
  struct FrameSlot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;  // freed with the pool
    UniqueFence inFlight;
    UniqueSemaphore imageAcquired;
    std::optional<Buffer> uniforms, overlay;
    // Sprite pass vertex stream (JD wiring 2026-10-13): per-slot so a frame
    // in flight never races the next frame's upload; grow-only like GL33.
    std::optional<Buffer> sprites;
    VkDeviceSize spriteCapacityVerts = 0;
    // Staging ring for device-local vertex uploads made before this slot's next frame; the
    // copies are recorded at the start of its command buffer. Reusable once its fence signals.
    std::optional<Buffer> staging;
    VkDeviceSize stagingUsed = 0;
    bool stagingSubmitted = false;
    std::vector<std::pair<VkBuffer, VkBufferCopy>> copies;
    VkDescriptorSet set = VK_NULL_HANDLE;  // freed with the descriptor pool
  };
  struct Mesh {
    VertexArena::Slice slice;
    uint32_t indexCount = 0;
    int cx = 0, sy = 0, cz = 0;
    float pulseP0[4]{};  ///< wafer ground-pulse blended params (from ChunkMesh)
    float pulseP1[4]{};
    float traceP0[4]{};  ///< wafer ground-trace blended params (from ChunkMesh)
    float traceP1[4]{};
  };
  struct Retired {
    uint64_t retiredAt = 0;  ///< frameNumber_ when replaced: frames before it may still read it
    VertexArena::Slice slice;
  };

  static bool fail(std::string* error, const char* what) {
    *error = std::string("Vulkan init failed: ") + what;
    return false;
  }

  bool createStaticBuffers() {
    const auto hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkDeviceSize idxBytes = VkDeviceSize(kMaxQuads) * 6 * sizeof(uint32_t);
    auto host = Buffer::create(*dev_, idxBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostVisible);
    if (!host) return false;
    auto* idx = reinterpret_cast<uint32_t*>(host->mapped());
    for (uint32_t q = 0; q < kMaxQuads; ++q) {
      const uint32_t b = q * 4;
      const uint32_t quad[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
      std::memcpy(idx + q * 6, quad, sizeof quad);
    }
    if (arena_->kind() == VertexArena::Kind::Host) {
      quadIndices_ = std::move(host);
    } else {  // device-local: one copy at init
      quadIndices_ = Buffer::create(*dev_, idxBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      if (!quadIndices_) return false;
      const VkBuffer src = host->handle(), dst = quadIndices_->handle();
      if (!submitOnce(*dev_, pool_.get(), [&](VkCommandBuffer c) {
            const VkBufferCopy region{0, 0, idxBytes};
            vkCmdCopyBuffer(c, src, dst, 1, &region);
          }))
        return false;
    }
    const HudFontAtlas atlas = buildHudFontAtlas();
    font_ = uploadImage2D(*dev_, pool_.get(), VK_FORMAT_R8_UNORM, uint32_t(atlas.width), uint32_t(atlas.height),
                          atlas.pixels.data(), 1);
    fontSampler_ = createNearestSampler(*dev_);
    spriteSampler_ = createLinearSampler(*dev_);
    // Sprite atlas starts as a 1x1x1 dummy array so descriptor binding 4 is
    // always valid; real pages replace it on the first sprite frame whose
    // atlas version differs (uploadSpriteAtlas).
    if (!uploadSpritePages(std::span<const ::litholattice::Image>{}, 0)) return false;
    return font_.has_value() && fontSampler_ && spriteSampler_;
  }

  bool createFrames() {
    const VkDevice d = dev_->device();
    const auto hostVisible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (FrameSlot& f : frames_) {
      auto ai = make<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
      ai.commandPool = pool_.get();
      ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ai.commandBufferCount = 1;
      auto fi = make<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
      fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // the first wait returns at once
      VkFence fence = VK_NULL_HANDLE;
      VkSemaphore sem = VK_NULL_HANDLE;
      if (!check(vkAllocateCommandBuffers(d, &ai, &f.cmd), "vkAllocateCommandBuffers") ||
          !check(vkCreateFence(d, &fi, nullptr, &fence), "vkCreateFence"))
        return false;
      f.inFlight = UniqueFence(d, fence);
      if (!createSemaphore(sem)) return false;
      f.imageAcquired = UniqueSemaphore(d, sem);
      f.uniforms = Buffer::create(*dev_, sizeof(FrameUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, hostVisible);
      f.overlay = Buffer::create(*dev_, kOverlayBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, hostVisible);
      if (!f.uniforms || !f.overlay) return false;
      if (arena_->kind() != VertexArena::Kind::Host) {
        f.staging = Buffer::create(*dev_, kStagingBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, hostVisible);
        if (!f.staging) return false;
      }
    }
    return true;
  }

  bool createSemaphore(VkSemaphore& out) const {
    auto si = make<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
    return check(vkCreateSemaphore(dev_->device(), &si, nullptr, &out), "vkCreateSemaphore");
  }

  bool createDescriptors() {
    const VkDevice d = dev_->device();
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kFramesInFlight},
                                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * kFramesInFlight}};
    auto ci = make<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
    ci.maxSets = kFramesInFlight;
    ci.poolSizeCount = uint32_t(std::size(sizes));
    ci.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (!check(vkCreateDescriptorPool(d, &ci, nullptr, &pool), "vkCreateDescriptorPool")) return false;
    descriptorPool_ = UniqueDescriptorPool(d, pool);
    std::array<VkDescriptorSetLayout, kFramesInFlight> layouts;
    layouts.fill(pipelines_->setLayout());
    auto ai = make<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
    ai.descriptorPool = pool;
    ai.descriptorSetCount = kFramesInFlight;
    ai.pSetLayouts = layouts.data();
    std::array<VkDescriptorSet, kFramesInFlight> sets{};
    if (!check(vkAllocateDescriptorSets(d, &ai, sets.data()), "vkAllocateDescriptorSets")) return false;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) frames_[i].set = sets[i];
    return true;
  }

  // Points every frame's set at the current images (device idle: after init or a texture upload).
  void writeDescriptors() {
    const VkDescriptorImageInfo array = textures_->arrayDescriptor(), palette = textures_->paletteDescriptor();
    const VkDescriptorImageInfo font{fontSampler_.get(), font_->view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo spriteAtlas{spriteSampler_.get(),
                                            spriteAtlas_ ? spriteAtlas_->view() : VK_NULL_HANDLE,
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (const FrameSlot& f : frames_) {
      const VkDescriptorBufferInfo ubo{f.uniforms->handle(), 0, sizeof(FrameUniforms)};
      VkWriteDescriptorSet w[5]{};
      for (uint32_t i = 0; i < 5; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = f.set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      }
      w[kBindingFrame].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      w[kBindingFrame].pBufferInfo = &ubo;
      w[kBindingTextureArray].pImageInfo = &array;
      w[kBindingPalette].pImageInfo = &palette;
      w[kBindingFont].pImageInfo = &font;
      w[kBindingSpriteAtlas].pImageInfo = &spriteAtlas;
      vkUpdateDescriptorSets(dev_->device(), 5, w, 0, nullptr);
    }
  }

  bool recreateSwapchain(uint32_t width, uint32_t height) {
    vkDeviceWaitIdle(dev_->device());
    auto next = Swapchain::create(*dev_, surface_, pipelines_->renderPass(), width, height, swapchain_.get());
    swapchain_ = std::move(next);
    if (!swapchain_) return false;  // minimized or lost: try again next frame
    requested_ = {width, height};
    swapchainStale_ = false;
    renderDone_.clear();
    for (uint32_t i = 0; i < swapchain_->imageCount(); ++i) {
      VkSemaphore s = VK_NULL_HANDLE;
      if (!createSemaphore(s)) return false;
      renderDone_.emplace_back(dev_->device(), s);
    }
    return true;
  }

  void retire(const VertexArena::Slice& s) { retired_.push_back({frameNumber_, s}); }

  // Waits until the GPU is done with `f`; recycles its staging ring if its copies were submitted.
  void waitSlot(FrameSlot& f) {
    const VkFence fence = f.inFlight.get();
    vkWaitForFences(dev_->device(), 1, &fence, VK_TRUE, UINT64_MAX);
    if (f.stagingSubmitted) { f.stagingUsed = 0; f.stagingSubmitted = false; }
  }

  // Device-local upload: bytes go into the next frame's staging ring, the copy is recorded at the
  // start of that frame (then a transfer -> vertex-input barrier). A ring that is full is flushed
  // with a one-off submit; a mesh larger than the ring goes through a temporary buffer.
  void stage(VkBuffer dst, VkDeviceSize dstOffset, const void* src, VkDeviceSize bytes) {
    FrameSlot& f = frames_[frameNumber_ % kFramesInFlight];
    waitSlot(f);  // the slot's previous frame may still read this ring (renderFrame waits on it anyway)
    if (f.stagingUsed + bytes > kStagingBytes) flushCopiesNow(f);
    if (bytes > kStagingBytes) {
      auto tmp = Buffer::create(*dev_, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (!tmp) return;
      std::memcpy(tmp->mapped(), src, size_t(bytes));
      const VkBuffer s = tmp->handle();
      submitOnce(*dev_, pool_.get(), [&](VkCommandBuffer c) {
        const VkBufferCopy region{0, dstOffset, bytes};
        vkCmdCopyBuffer(c, s, dst, 1, &region);
      });
      return;
    }
    std::memcpy(f.staging->mapped() + f.stagingUsed, src, size_t(bytes));
    f.copies.push_back({dst, VkBufferCopy{f.stagingUsed, dstOffset, bytes}});
    f.stagingUsed += (bytes + 15) & ~VkDeviceSize(15);
  }

  void recordCopies(VkCommandBuffer cmd, FrameSlot& f) {
    if (f.copies.empty()) return;
    for (const auto& [dst, region] : f.copies) vkCmdCopyBuffer(cmd, f.staging->handle(), dst, 1, &region);
    auto mb = make<VkMemoryBarrier>(VK_STRUCTURE_TYPE_MEMORY_BARRIER);
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    f.copies.clear();
  }

  // Staging ring full (many uploads before one frame, e.g. a dimension load): copy now and wait.
  void flushCopiesNow(FrameSlot& f) {
    if (!f.copies.empty()) submitOnce(*dev_, pool_.get(), [&](VkCommandBuffer c) { recordCopies(c, f); });
    f.copies.clear();
    f.stagingUsed = 0;
  }

  // Called after waiting for this slot's fence: every frame before frameNumber_ + 1 - kFramesInFlight is done.
  void releaseRetired() {
    std::erase_if(retired_, [&](const Retired& r) {
      if (frameNumber_ + 1 < kFramesInFlight + r.retiredAt) return false;
      arena_->free(r.slice);
      return true;
    });
  }

  void writeUniforms(const FrameSlot& f, const FrameParams& p) {
    FrameUniforms u = materials_;
    std::memcpy(u.viewProj, p.viewProj.m, sizeof u.viewProj);
    u.cam[0] = p.camPos.x; u.cam[1] = p.camPos.y; u.cam[2] = p.camPos.z;
    u.sky[0] = p.sky.x; u.sky[1] = p.sky.y; u.sky[2] = p.sky.z;
    u.fogEnd = p.fogEnd;
    u.time = p.timeSec; u.debug = p.debugOverlay ? 1 : 0;
    u.pulseTime = p.groundPulseTimeSec; u.pulseOn = p.groundPulseOn ? 1.f : 0.f;  // wafer ground pulse
    u.traceTime = p.groundTraceTimeSec; u.traceOn = p.groundTraceOn ? 1.f : 0.f;  // wafer ground traces
    u.tile = textures_->tileSize();
    std::memcpy(f.uniforms->mapped(), &u, sizeof u);
  }

  void record(FrameSlot& f, uint32_t image, const FrameParams& p) {
    writeUniforms(f, p);
    const VkCommandBuffer cmd = f.cmd;
    auto bi = make<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    recordCopies(cmd, f);  // this frame's vertex uploads land before any draw reads them
    VkClearValue clear[2]{};
    clear[0].color = {{p.sky.x, p.sky.y, p.sky.z, 1.f}};
    clear[1].depthStencil = {1.f, 0};
    const VkExtent2D extent = swapchain_->extent();
    auto rp = make<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
    rp.renderPass = pipelines_->renderPass();
    rp.framebuffer = swapchain_->framebuffer(image);
    rp.renderArea = {{0, 0}, extent};
    rp.clearValueCount = 2;
    rp.pClearValues = clear;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0, 0, float(extent.width), float(extent.height), 0.f, 1.f};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->layout(), 0, 1, &f.set, 0, nullptr);
    drawChunks(cmd, p);
    drawSprites(cmd, f, p);  // sprite pass: after world geometry, before lines/HUD (GL33 order)
    drawOverlay(cmd, f, p);
    vkCmdEndRenderPass(cmd);
    // Screenshot grab (lithoscript `screenshot`): the game arms the grab during
    // update, before the frame is drawn. Copy the just-rendered swapchain image
    // to a host staging buffer before present (image-to-host copy).
    if (grabArmed_) {
      grabArmed_ = false;
      recordGrab(cmd, uint32_t(frameNumber_ % kFramesInFlight), image);
    }
    vkEndCommandBuffer(cmd);
  }

  /// Records the swapchain-image -> host-staging copy for a screenshot grab.
  /// Runs inside the armed frame's command buffer, before present.
  void recordGrab(VkCommandBuffer cmd, uint32_t slot, uint32_t image) {
    grabReady_ = false;
    if (!swapchain_ || !swapchain_->supportsTransferSrc()) return;
    const VkExtent2D extent = swapchain_->extent();
    const VkFormat fmt = surface_.color.format;
    // chooseSurfaceConfig() only ever picks one of these two UNORM formats.
    if (fmt != VK_FORMAT_B8G8R8A8_UNORM && fmt != VK_FORMAT_R8G8B8A8_UNORM) return;
    const VkDeviceSize need = VkDeviceSize(extent.width) * VkDeviceSize(extent.height) * 4;
    if (!grabStaging_ || grabStaging_->size() < need || grabExtent_.width != extent.width ||
        grabExtent_.height != extent.height) {
      const auto hostVisible =
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      grabStaging_ =
          Buffer::create(*dev_, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, hostVisible);
      if (!grabStaging_) return;
      grabExtent_ = extent;
    }
    grabFormat_ = fmt;
    grabSlot_ = slot;
    const VkImage img = swapchain_->image(image);
    auto bar = make<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
    bar.srcAccessMask = 0;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = img;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &bar);
    VkBufferImageCopy cp{};  // no sType: plain copy description
    cp.bufferOffset = 0;
    cp.bufferRowLength = 0;  // tightly packed
    cp.bufferImageHeight = 0;
    cp.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.imageOffset = {0, 0, 0};
    cp.imageExtent = {extent.width, extent.height, 1};
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           grabStaging_->handle(), 1, &cp);
    bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    bar.dstAccessMask = 0;
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr,
                         1, &bar);
    grabReady_ = true;  // the slot's in-flight fence tells grabFramebuffer() when done
  }

  void push(VkCommandBuffer cmd, const void* data, uint32_t bytes) const {
    vkCmdPushConstants(cmd, pipelines_->layout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, bytes, data);
  }

  void drawChunks(VkCommandBuffer cmd, const FrameParams& p) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->chunk());
    vkCmdBindIndexBuffer(cmd, quadIndices_->handle(), 0, VK_INDEX_TYPE_UINT32);
    stats_.drawn = 0;
    stats_.meshes = uint32_t(meshes_.size());
    for (const auto& [k, mesh] : meshes_) {
      const Vec3 mn{mesh.cx * 16.f, mesh.sy * 16.f, mesh.cz * 16.f};
      // Prototype pipeline: Full draws; MeshOnly/Cull skip (mesh stays uploaded).
      if (cullProto_.decide(p.viewProj, mn, mn + Vec3{16, 16, 16}, p.camPos, frustumMarginDeg_) !=
          CullDecision::Full)
        continue;
      const ChunkPush pc{{mn.x, mn.y, mn.z}, 0.f,
                         {mesh.pulseP0[0], mesh.pulseP0[1], mesh.pulseP0[2], mesh.pulseP0[3]},
                         {mesh.pulseP1[0], mesh.pulseP1[1], mesh.pulseP1[2], mesh.pulseP1[3]},
                         {mesh.traceP0[0], mesh.traceP0[1], mesh.traceP0[2], mesh.traceP0[3]},
                         {mesh.traceP1[0], mesh.traceP1[1], mesh.traceP1[2], mesh.traceP1[3]}};
      push(cmd, &pc, sizeof pc);
      const VkBuffer buffer = arena_->buffer(mesh.slice);
      const VkDeviceSize offset = mesh.slice.offset;
      vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
      vkCmdDrawIndexed(cmd, mesh.indexCount, 1, 0, 0, 0);
      ++stats_.drawn;
    }
  }

  // (Re)uploads the sprite atlas pages as one texture array (layer = page
  // index, pages upload row-first exactly as GL33 uploads them). All pages
  // must share one size — the sprite registry's pages always do (square,
  // power-of-two, one page size per registry); a mixed-size span is the
  // one genuinely unsupported sub-case and is named, warned once, and the
  // frame's sprites are skipped rather than misdrawn.
  bool uploadSpritePages(std::span<const ::litholattice::Image> pages, uint64_t version) {
    uint32_t w = 1, h = 1;
    size_t layers = 1;
    if (!pages.empty()) {
      w = uint32_t(pages[0].width());
      h = uint32_t(pages[0].height());
      layers = pages.size();
      for (const ::litholattice::Image& page : pages) {
        if (page.width() != int(w) || page.height() != int(h) || w == 0 || h == 0) {
          static bool warnedMixed = false;
          if (!warnedMixed) {
            warnedMixed = true;
            logWarn("vulkan: sprite atlas pages have mixed sizes — sprite pass skipped for this atlas (pages must share one size)");
          }
          spriteAtlasPages_ = 0;
          spriteAtlasUploaded_ = version;
          return false;
        }
      }
    }
    vkDeviceWaitIdle(dev_->device());
    const VkDeviceSize layerBytes = VkDeviceSize(w) * h * 4;
    auto staging = Buffer::create(*dev_, layerBytes * layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!staging) return false;
    if (pages.empty()) {
      std::memset(staging->mapped(), 0, size_t(layerBytes));  // dummy: transparent black
    } else {
      for (size_t i = 0; i < layers; ++i)
        std::memcpy(staging->mapped() + layerBytes * i, pages[i].pixels().data(), size_t(layerBytes));
    }
    auto ci = make<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = uint32_t(layers);
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    auto image = vk::Image::create(*dev_, ci, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!image) return false;
    const VkImage img = image->handle();
    const VkBuffer src = staging->handle();
    const bool ok = submitOnce(*dev_, pool_.get(), [&](VkCommandBuffer cmd) {
      transitionImage(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, uint32_t(layers)};
      region.imageExtent = {w, h, 1};
      vkCmdCopyBufferToImage(cmd, src, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      transitionImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });
    if (!ok) return false;
    spriteAtlas_ = std::move(image);
    spriteAtlasPages_ = pages.size();
    spriteAtlasUploaded_ = version;
    if (frames_[0].set != VK_NULL_HANDLE) writeDescriptors();
    return true;
  }

  void uploadSpriteAtlas(const FrameParams& p) {
    if (!uploadSpritePages(p.spriteAtlasPages, p.spriteAtlasVersion))
      logError("vulkan: sprite atlas upload failed; sprites skipped for atlas version %llu",
               (unsigned long long)p.spriteAtlasVersion);
  }

  // Sprite pass (JD wiring 2026-10-13), at parity with GL33: the frame's
  // quads expand to vertices in INPUT ORDER (blend/additive arrive
  // pre-sorted far-to-near; never re-sorted), stream into this slot's
  // grow-only vertex buffer, and draw in runs of equal (shade, page):
  // cutout = alpha test 0.5 + depth write, blend = SRC_ALPHA /
  // ONE_MINUS_SRC_ALPHA, additive = SRC_ALPHA / ONE — the three sprite
  // pipelines. Double-sided, like GL33 (sprites read from both sides).
  void drawSprites(VkCommandBuffer cmd, FrameSlot& f, const FrameParams& p) {
    if (!p.spriteQuads || p.spriteQuadCount == 0 || spriteAtlasPages_ == 0) return;
    vk_sprite::buildSpriteVertices(p.spriteQuads, p.spriteQuadCount, spriteVerts_);
    if (spriteVerts_.empty()) return;
    if (!f.sprites || f.spriteCapacityVerts < spriteVerts_.size()) {
      const VkDeviceSize verts = std::max<VkDeviceSize>(spriteVerts_.size(), 6144);
      auto buf = Buffer::create(*dev_, verts * sizeof(vk_sprite::SpriteVertex),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (!buf) {
        logError("vulkan: sprite vertex buffer allocation failed; sprites skipped this frame");
        return;
      }
      f.sprites = std::move(buf);
      f.spriteCapacityVerts = verts;
    }
    std::memcpy(f.sprites->mapped(), spriteVerts_.data(),
                spriteVerts_.size() * sizeof(vk_sprite::SpriteVertex));
    const VkBuffer buffer = f.sprites->handle();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
    const VkPipeline pipelines[3] = {pipelines_->spriteCutout(), pipelines_->spriteBlend(),
                                     pipelines_->spriteAdditive()};
    bool drew = false;
    size_t skippedQuads = 0;
    for (const vk_sprite::SpriteRun& run :
         vk_sprite::spriteRuns(p.spriteQuads, p.spriteQuadCount, spriteAtlasPages_)) {
      if (run.pipeline < 0) {  // non-sprite shade or page out of range: skip, never misdraw
        skippedQuads += run.vertexCount / 6;
        continue;
      }
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines[run.pipeline]);
      const SpritePush pc{vk_sprite::spriteAlphaTestForShade(run.shade), {0.f, 0.f, 0.f}};
      vkCmdPushConstants(cmd, pipelines_->layout(),
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
      vkCmdDraw(cmd, run.vertexCount, 1, run.firstVertex, 0);
      drew = true;
    }
    if (skippedQuads > 0) {
      static bool warnedSkip = false;
      if (!warnedSkip) {
        warnedSkip = true;
        logWarn("vulkan: sprite pass skipped %zu quads (non-sprite shade or atlas page out of range)", skippedQuads);
      }
    }
    if (drew) ++stats_.drawn;
  }

  // Block outline (depth tested), crosshair (always on top), then the HUD, from this frame's
  // overlay buffer: lines at offset 0, HUD vertices at kHudOffset.
  void drawOverlay(VkCommandBuffer cmd, const FrameSlot& f, const FrameParams& p) {
    const VkBuffer buffer = f.overlay->handle();
    uint8_t* const base = f.overlay->mapped();
    VkDeviceSize offset = 0;
    auto drawLines = [&](VkPipeline pipeline, std::span<const Vec3> pts, const Mat4& m, Vec3 color) {
      std::memcpy(base + offset, pts.data(), pts.size_bytes());
      vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
      LinePush pc{};
      std::memcpy(pc.matrix, m.m, sizeof pc.matrix);
      pc.color[0] = color.x; pc.color[1] = color.y; pc.color[2] = color.z; pc.color[3] = 1.f;
      push(cmd, &pc, sizeof pc);
      vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &offset);
      vkCmdDraw(cmd, uint32_t(pts.size()), 1, 0, 0);
      offset += pts.size_bytes();
    };
    if (p.highlight)
      drawLines(pipelines_->lineDepth(), blockOutlineLines(p.hx, p.hy, p.hz), p.viewProj, {0.05f, 0.05f, 0.08f});
    if (p.crosshair) drawLines(pipelines_->lineOverlay(), crosshairLines(p.width, p.height), Mat4::identity(), {1, 1, 1});

    buildHudVertices(p.hud, hudVerts_);
    const size_t maxVerts = size_t((kOverlayBytes - kHudOffset) / sizeof(HudVertex));
    const size_t count = std::min(hudVerts_.size(), maxVerts / 6 * 6);
    // D-M6: the overlay buffer is fixed-size; excess HUD verts are dropped.
    // Loud once instead of silent truncation.
    if (count < hudVerts_.size()) {
      static bool warnedHudTrunc = false;
      if (!warnedHudTrunc) { warnedHudTrunc = true; logWarn("vulkan: HUD overlay buffer full — %zu of %zu HUD vertices dropped", hudVerts_.size() - count, hudVerts_.size()); }
    }
    if (count == 0) return;
    std::memcpy(base + kHudOffset, hudVerts_.data(), count * sizeof(HudVertex));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_->hud());
    const HudPush pc{{float(p.width), float(p.height)}};
    push(cmd, &pc, sizeof pc);
    const VkDeviceSize hudOffset = kHudOffset;
    vkCmdBindVertexBuffers(cmd, 0, 1, &buffer, &hudOffset);
    vkCmdDraw(cmd, uint32_t(count), 1, 0, 0);
  }

  // Declaration order is destruction order reversed: the device goes last.
  std::unique_ptr<VulkanDevice> dev_;
  UniqueCommandPool pool_;
  SurfaceConfig surface_;
  std::unique_ptr<RenderPipelines> pipelines_;
  std::unique_ptr<VertexArena> arena_;
  std::optional<Buffer> quadIndices_;
  std::optional<vk::Image> font_;
  UniqueSampler fontSampler_;
  // Sprite pass state (JD wiring 2026-10-13): atlas pages as one texture
  // array + its sampler; spriteAtlasUploaded_ is the registry version on
  // the GPU (UINT64_MAX = nothing real uploaded yet, dummy array bound).
  std::optional<vk::Image> spriteAtlas_;
  UniqueSampler spriteSampler_;
  uint64_t spriteAtlasUploaded_ = UINT64_MAX;
  size_t spriteAtlasPages_ = 0;
  std::vector<vk_sprite::SpriteVertex> spriteVerts_;
  std::array<FrameSlot, kFramesInFlight> frames_;
  UniqueDescriptorPool descriptorPool_;
  std::unique_ptr<VkTextureArray> textures_;
  std::unique_ptr<Swapchain> swapchain_;
  std::vector<UniqueSemaphore> renderDone_;  // per swapchain image
  VkExtent2D requested_{};    // window size the swapchain was built for
  bool swapchainStale_ = false;  // out of date / suboptimal: rebuild before the next frame
  float frustumMarginDeg_ = 5.f;  ///< FOV culling margin, degrees (JD 2026-10-11); game sets it
  CullProtoState cullProto_;  ///< culling-cache prototypes (JD 2026-10-11); all default off
  std::unordered_map<int64_t, Mesh> meshes_;
  std::vector<Retired> retired_;
  uint64_t frameNumber_ = 0;
  // Screenshot grab state (lithoscript `screenshot`): armed by requestGrab()
  // during update; recordGrab() copies the armed frame's swapchain image to
  // grabStaging_ before present; grabFramebuffer() waits on the armed slot's
  // fence and swizzles to top-first RGB.
  bool grabArmed_ = false;
  bool grabReady_ = false;
  uint32_t grabSlot_ = 0;
  VkFormat grabFormat_ = VK_FORMAT_UNDEFINED;
  VkExtent2D grabExtent_{0, 0};
  std::optional<Buffer> grabStaging_;
  FrameUniforms materials_{};
  std::vector<HudVertex> hudVerts_;
  RenderStats stats_;
};

}  // namespace

std::unique_ptr<IRenderer> createVulkanRenderer(const VulkanRendererDesc& desc, std::string* error) {
  auto r = std::make_unique<VulkanRenderer>();
  std::string err;
  if (!r->init(desc, &err)) {
    if (error) *error = err;
    return nullptr;
  }
  return r;
}

}  // namespace litholattice
