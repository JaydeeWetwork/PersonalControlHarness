// OpenGL 3.3 core backend for IRenderer. Owns all GL objects; call only from the thread
// that owns the GL context (the main/render thread).
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <iterator>
#include <map>
#include <optional>
#include <unordered_map>
#include <string_view>
#include "builtin_shaders.h"
#include "gl_loader.h"
#include "gl_program.h"
#include "gl_texture_array.h"
#include "litholattice/assets/shader_library.h"
#include "litholattice/core/log.h"
#include "litholattice/render/block_materials.h"
#include "litholattice/render/overlay_geometry.h"
#include "litholattice/render/pbr_gl_upload.h"
#include "litholattice/render/pbr_quad_mesh.h"
#include "litholattice/render/renderer.h"

namespace litholattice {
namespace {

using gl33::GlProgram;
using gl33::GlTextureArray;

/// Programs the backend draws with; names match assets/shaders/<name>.vert/.frag.
/// "chunk_pbr" is the PBR path (BinaryQuad meshes); the classic "chunk" program keeps
/// serving PackedVertex meshes - selection follows the uploaded meshes, not a flag.
constexpr std::string_view kProgramNames[] = {"chunk", "hud", "line", "chunk_pbr", "sprite"};
constexpr size_t kChunkPbrProgram = 3;
constexpr size_t kSpriteProgram = 4;  // sprite pass (FrameParams::spriteQuads); non-fatal like chunk_pbr

/// Interleaved sprite-pass vertex (pos / atlas uv / tint), streamed per frame.
struct SpriteVert { float x, y, z, u, v, r, g, b, a; };

/// Built-in copy of a base-pack shader file (generated from assets/shaders at build time).
std::optional<std::string> builtinFile(std::string_view file) {
  for (const BuiltinShaderFile& f : kBuiltinShaderFiles)
    if (file == f.name) return std::string(f.source);
  return std::nullopt;
}

/// Built-in shader stage with its includes expanded from the other built-in files.
std::string builtinSource(std::string_view file, std::string* error) {
  const std::optional<std::string> src = builtinFile(file);
  if (!src) { *error = "missing built-in file " + std::string(file); return {}; }
  return expandShaderIncludes(*src, builtinFile, error).value_or(std::string{});
}

constexpr uint32_t kMaxQuads = kMaxQuadsPerMesh;  // shared quad index buffer size (renderer.h)

class GL33Renderer final : public IRenderer {
 public:
  explicit GL33Renderer(bool arena) : useArena_(arena) {}
  bool init() {
    logInfo("gl33: chunk meshes in %s", useArena_ ? "one arena VBO + VAO (base-vertex draws)" : "one VBO + VAO per sub-chunk");
    if (loadShaders(ShaderLibrary{}) < 0) return false;  // built-ins until the app loads pack shaders
    std::vector<uint32_t> idx(kMaxQuads * 6);  // shared quad index buffer
    for (uint32_t q = 0; q < kMaxQuads; ++q) {
      const uint32_t b = q * 4; uint32_t* o = &idx[q * 6];
      o[0] = b; o[1] = b + 1; o[2] = b + 2; o[3] = b; o[4] = b + 2; o[5] = b + 3;
    }
    glGenBuffers(1, &ebo_); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(idx.size() * 4), idx.data(), GL_STATIC_DRAW);
    glGenVertexArrays(1, &lineVao_); glBindVertexArray(lineVao_);
    glGenBuffers(1, &lineVbo_); glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    glBufferData(GL_ARRAY_BUFFER, 24 * 3 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    initHud();
    // Sprite pass VAO/VBO (streamed per frame by drawSprites).
    glGenVertexArrays(1, &spriteVao_); glBindVertexArray(spriteVao_);
    glGenBuffers(1, &spriteVbo_); glBindBuffer(GL_ARRAY_BUFFER, spriteVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(sizeof(SpriteVert) * 6144), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), reinterpret_cast<const void*>(offsetof(SpriteVert, u)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(SpriteVert), reinterpret_cast<const void*>(offsetof(SpriteVert, r)));
    glBindVertexArray(0);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LEQUAL);
    glEnable(GL_CULL_FACE); glCullFace(GL_BACK); glFrontFace(GL_CCW);
    return true;
  }

  void initHud() {
    const HudFontAtlas atlas = buildHudFontAtlas();
    glGenTextures(1, &fontTex_); glBindTexture(GL_TEXTURE_2D, fontTex_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, atlas.width, atlas.height, 0, GL_RED, GL_UNSIGNED_BYTE, atlas.pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenVertexArrays(1, &hudVao_); glBindVertexArray(hudVao_);
    glGenBuffers(1, &hudVbo_); glBindBuffer(GL_ARRAY_BUFFER, hudVbo_);
    const GLsizei stride = sizeof(HudVertex);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(HudVertex, u)));
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(HudVertex, r)));
  }

  void drawHud(const FrameParams& p) {
    buildHudVertices(p.hud, hudVerts_);
    if (hudVerts_.empty()) return;
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);  // pixel-space quads are clockwise in NDC
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    hudProg_.use();
    glUniform2f(uScreen_, float(p.width), float(p.height));
    glBindTexture(GL_TEXTURE_2D, fontTex_);
    glBindVertexArray(hudVao_); glBindBuffer(GL_ARRAY_BUFFER, hudVbo_);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(hudVerts_.size() * sizeof(HudVertex)), hudVerts_.data(), GL_DYNAMIC_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(hudVerts_.size()));
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST); glEnable(GL_CULL_FACE);
  }

  // Sprite atlas pages: (re)upload when the game's registry version moved.
  // Pages upload row-first (Image row order), which pairs with the emitted
  // image-space UVs exactly (see renderer.h SpriteDrawQuad).
  void uploadSpriteAtlas(const FrameParams& p) {
    for (GLuint t : spriteTex_) glDeleteTextures(1, &t);
    spriteTex_.clear();
    glActiveTexture(GL_TEXTURE0);
    for (const Image& page : p.spriteAtlasPages) {
      GLuint t = 0;
      glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
      glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, page.width(), page.height(), 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, page.pixels().data());
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      spriteTex_.push_back(t);
    }
    spriteAtlasUploaded_ = p.spriteAtlasVersion;
  }

  // Sprite pass: FrameParams::spriteQuads after the world + entities, before
  // lines/HUD. Quads arrive pre-ordered (cutout, then blended back-to-front),
  // so this is a single streamed VBO drawn in runs of equal (shade, page),
  // switching blend/depth state per run. Sprites are double-sided (a
  // FlatFacing decal reads from both sides), so culling is off for the pass.
  void drawSprites(const FrameParams& p) {
    if (!spriteProg_.id() || !p.spriteQuads || p.spriteQuadCount == 0) return;
    if (p.spriteAtlasPages.empty()) return;
    if (p.spriteAtlasVersion != spriteAtlasUploaded_) uploadSpriteAtlas(p);
    if (spriteTex_.empty()) return;
    spriteVerts_.clear();
    spriteVerts_.reserve(p.spriteQuadCount * 6);
    for (size_t q = 0; q < p.spriteQuadCount; ++q) {
      const SpriteDrawQuad& s = p.spriteQuads[q];
      static constexpr int kOrder[6] = {0, 1, 2, 0, 2, 3};
      for (int vi : kOrder) {
        const Vec3& c = s.corners[vi];
        spriteVerts_.push_back(SpriteVert{c.x, c.y, c.z, s.uv[vi][0], s.uv[vi][1], s.r, s.g, s.b, s.a});
      }
    }
    glActiveTexture(GL_TEXTURE0);
    glBindVertexArray(spriteVao_); glBindBuffer(GL_ARRAY_BUFFER, spriteVbo_);
    // Grow-only streaming (P3-3, Appendix D M5): the old code called
    // glBufferData with the exact frame size EVERY frame, orphaning and
    // reallocating the store each time (the 6144-vert init allocation was
    // discarded on the first frame). Now: glBufferData only when the
    // frame outgrows capacity; glBufferSubData into the existing store
    // otherwise.
    if (spriteVerts_.size() > spriteVboCapacity_) {
      spriteVboCapacity_ = spriteVerts_.size();
      glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(spriteVboCapacity_ * sizeof(SpriteVert)),
                   spriteVerts_.data(), GL_DYNAMIC_DRAW);
    } else if (!spriteVerts_.empty()) {
      glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(spriteVerts_.size() * sizeof(SpriteVert)),
                      spriteVerts_.data());
    }
    spriteProg_.use();
    glUniformMatrix4fv(uSpriteVP_, 1, GL_FALSE, p.viewProj.m);
    glDisable(GL_CULL_FACE);
    size_t i = 0;
    while (i < p.spriteQuadCount) {
      const SpriteDrawQuad& s = p.spriteQuads[i];
      size_t j = i + 1;
      while (j < p.spriteQuadCount && p.spriteQuads[j].shade == s.shade &&
             p.spriteQuads[j].page == s.page)
        ++j;
      if (s.page >= 0 && size_t(s.page) < spriteTex_.size()) {
        if (s.shade == kShadeCutout) {
          glDisable(GL_BLEND); glDepthMask(GL_TRUE);
          glUniform1f(uSpriteAlphaTest_, 0.5f);
        } else if (s.shade == kShadeAdditive) {
          glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE); glDepthMask(GL_FALSE);
          glUniform1f(uSpriteAlphaTest_, 0.f);
        } else {  // kShadeAlphaBlend
          glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
          glDepthMask(GL_FALSE);
          glUniform1f(uSpriteAlphaTest_, 0.f);
        }
        glBindTexture(GL_TEXTURE_2D, spriteTex_[size_t(s.page)]);
        glDrawArrays(GL_TRIANGLES, GLsizei(i * 6), GLsizei((j - i) * 6));
      }
      i = j;
    }
    glDepthMask(GL_TRUE); glDisable(GL_BLEND); glEnable(GL_CULL_FACE);
    ++stats_.drawn;
  }

  ~GL33Renderer() override {
    clearChunkMeshes();
    glDeleteTextures(1, &fontTex_);
    if (pbrMatTex_) glDeleteTextures(1, &pbrMatTex_);
    if (pbrMatBuf_) glDeleteBuffers(1, &pbrMatBuf_);
    if (sceneColorTex_) glDeleteTextures(1, &sceneColorTex_);
    // Fix 2026-10-11 (audit): these 7 were never deleted.
    if (ebo_) glDeleteBuffers(1, &ebo_);
    if (lineVao_) glDeleteVertexArrays(1, &lineVao_);
    if (lineVbo_) glDeleteBuffers(1, &lineVbo_);
    if (entityVao_) glDeleteVertexArrays(1, &entityVao_);
    if (entityVbo_) glDeleteBuffers(1, &entityVbo_);
    if (hudVao_) glDeleteVertexArrays(1, &hudVao_);
    if (hudVbo_) glDeleteBuffers(1, &hudVbo_);
    if (spriteVao_) glDeleteVertexArrays(1, &spriteVao_);
    if (spriteVbo_) glDeleteBuffers(1, &spriteVbo_);
    for (GLuint t : spriteTex_) glDeleteTextures(1, &t);
  }

  const char* name() const override { return "OpenGL 3.3 core"; }
  ITextureArray& textureArray() override { return textures_; }

  void setMaterials(const BlockMaterials& m) override {
    bindings_ = m.bindings();
    animations_.assign(m.animations().begin(), m.animations().end());
    animFrames_.assign(m.animationFrames().begin(), m.animationFrames().end());
    applyMaterialUniforms();
  }

  // ---- PBR (BinaryQuad) path (phase 2, Worker 3) ----
  bool supportsPbrPath() const override { return pbrProg_.id() != 0; }

  void setPbrMaterials(const MaterialRegistry& reg) override {
    if (!supportsPbrPath()) return;
    const uint64_t gen = reg.generation();
    if (gen == pbrMatGeneration_) return;  // table is current; uploads are rare
    std::vector<PbrMaterialRow> rows;
    packPbrMaterialTable(reg, rows);  // 48-byte std430 rows, indexed by compact ID
    if (!pbrMatBuf_) { glGenBuffers(1, &pbrMatBuf_); glGenTextures(1, &pbrMatTex_); }
    // Buffer texture (GL 3.1+ core): the shader unpacks rows via
    // texelFetch(uPbrMaterials, id * 12 + k). SSBOs would need GL 4.3.
    glBindBuffer(GL_TEXTURE_BUFFER, pbrMatBuf_);
    glBufferData(GL_TEXTURE_BUFFER, GLsizeiptr(rows.size() * sizeof(PbrMaterialRow)),
                 rows.empty() ? nullptr : rows.data(), GL_STATIC_DRAW);
    glBindTexture(GL_TEXTURE_BUFFER, pbrMatTex_);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_R32F, pbrMatBuf_);
    pbrMatGeneration_ = gen;
    pbrMatRows_ = int(rows.size());
    logInfo("gl33: PBR material table uploaded (%zu materials, generation %llu)",
            rows.size(), (unsigned long long)gen);
  }

  void setPbrTier(PbrTransparencyTier t) override { pbrTier_ = t; }
  void setAmbientMode(AmbientMode m) override { ambientMode_ = m; }
  void setFrustumMarginDeg(float degrees) override {
    frustumMarginDeg_ = std::clamp(degrees, 0.f, 15.f);  // JD-locked 2026-10-11
  }
  void setModelDither(bool enabled, float nearBlocks, float farBlocks) override {
    ditherEnabled_ = enabled;
    ditherNear_ = std::clamp(nearBlocks, 0.f, 1024.f);
    ditherFar_ = std::clamp(farBlocks, ditherNear_, 2048.f);
  }
  void setCullProtoConfig(const CullProtoConfig& cfg) override { cullProto_.configure(cfg); }
  CullProtoStats cullProtoStats() const override { return cullProto_.stats(); }

  // TrueRefraction (Ultra) scene-color pass (phase 2, Worker 4): copies the opaque
  // composite the frame has drawn so far (classic chunks, PBR opaque pass, entities -
  // but NOT overlay lines, the crosshair, or the HUD, which draw later) into a
  // full-res RGBA8 texture and binds it as uSceneColor for the refractive pass.
  //
  // Decisions (Worker 4's to lock):
  // - Full-res, not half-res: the refraction math needs screen-space UV accuracy
  //   (grazing rays offset by whole texels at half-res); Ultra is the opt-in quality
  //   tier, so the fill cost is the accepted price. Half-res stays a valid future
  //   optimization if Ultra ever needs a perf mode.
  // - Bilinear + mip chain for the roughness blur: one textureLod() sample with
  //   lod = roughness * 4 (1/16-res at roughness 1) is the honest cheap PBR answer -
  //   far cheaper than a multi-tap gather, and the mip build is one
  //   glGenerateMipmap per frame at tier 3 only.
  // - The copy is sRGB-encoded framebuffer output; the shader linearizes the sample
  //   (pbrSrgbToLinear) before the Fresnel split.
  // Requires pbrProg_ to be current on entry (sets its uniforms); leaves the scene
  // texture bound on kSceneColorTexUnit.
  void copySceneColor(const FrameParams& p) {
    const int w = p.width, h = p.height;
    if (w <= 0 || h <= 0) return;
    if (sceneColorTex_ == 0) glGenTextures(1, &sceneColorTex_);
    glActiveTexture(GL_TEXTURE0 + kSceneColorTexUnit);
    glBindTexture(GL_TEXTURE_2D, sceneColorTex_);
    if (w != sceneColorW_ || h != sceneColorH_) {
      sceneColorW_ = w;
      sceneColorH_ = h;
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    }
    // Copy from the default framebuffer's back buffer (everything drawn so far).
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
    glGenerateMipmap(GL_TEXTURE_2D);  // roughness blur source; tier-3-only cost
    pbrProg_.use();
    glUniform1i(uSceneColor_, kSceneColorTexUnit);
    glUniform2f(uSceneSize_, float(w), float(h));
    glUniformMatrix4fv(uSceneVP_, 1, GL_FALSE, p.viewProj.m);
  }

  void uploadPbrChunkMesh(const PbrChunkMesh& m) override {
    const int64_t key = (int64_t(m.cx) << 40) ^ (int64_t(m.sy) << 32) ^ int64_t(uint32_t(m.cz));
    if (m.verts.empty() || !supportsPbrPath()) { erasePbr(key); return; }
    // PBR meshes deliberately stay OUT of the chunk arena: the arena's stride and
    // attribute assumptions are PackedVertex-specific, and PBR meshes are the
    // minority path (opaque cubes only; everything else stays classic).
    PbrGpu& g = pbrMeshes_[key];
    g.cx = m.cx; g.sy = m.sy; g.cz = m.cz;
    g.indexCount = GLsizei(std::min<size_t>(m.verts.size() / 4, kMaxQuadsPerMesh) * 6);
    if (!g.vao) {
      glGenVertexArrays(1, &g.vao); glBindVertexArray(g.vao);
      glGenBuffers(1, &g.vbo); glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
      glEnableVertexAttribArray(0);  // aPos: uvec3, 1/16 block units
      glVertexAttribIPointer(0, 3, GL_UNSIGNED_SHORT, sizeof(PbrQuadVertex), nullptr);
      glEnableVertexAttribArray(1);  // aFace: uint
      glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(PbrQuadVertex), reinterpret_cast<const void*>(8));
      glEnableVertexAttribArray(2);  // aMaterial: uint (compact registry ID)
      glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(PbrQuadVertex), reinterpret_cast<const void*>(12));
      glEnableVertexAttribArray(3);  // aLight: uvec2 (block/sky 0..64)
      glVertexAttribIPointer(3, 2, GL_UNSIGNED_BYTE, sizeof(PbrQuadVertex), reinterpret_cast<const void*>(16));
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);  // shared quad index buffer
    } else {
      glBindVertexArray(g.vao); glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    }
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(m.verts.size() * sizeof(PbrQuadVertex)), m.verts.data(), GL_STATIC_DRAW);
  }

  // Returns how many programs use pack sources, or -1 if even a built-in program failed.
  // chunk_pbr is the exception: it may fail (old driver, pack override broken) without
  // failing init - the PBR path then reports unsupported and the game falls back to
  // the classic PackedVertex path (documented activation order, Worker 3).
  int loadShaders(const ShaderLibrary& lib) override {
    // Build every program first and swap them in only if all succeeded, so a failure never leaves
    // a mix of old and new programs bound.
    int fromPacks = 0;
    std::array<std::optional<GlProgram>, std::size(kProgramNames)> built;
    for (size_t i = 0; i < std::size(kProgramNames); ++i) {
      const std::string_view name = kProgramNames[i];
      const bool pbr = (i == kChunkPbrProgram);
      std::string log;
      std::optional<GlProgram>& prog = built[i];
      if (const ShaderSource* src = lib.find(name)) {
        prog = GlProgram::build(src->vertex, src->fragment, &log);
        if (prog) ++fromPacks;
        else logError("shader '%.*s' from %s failed, using the built-in copy:\n%s", int(name.size()), name.data(), src->origin.c_str(), log.c_str());
      } else if (lib.size() > 0) {
        logInfo("shader '%.*s': not in any pack, using the built-in copy", int(name.size()), name.data());
      }
      if (!prog) {
        const std::string file(name);
        const std::string vs = builtinSource(file + ".vert", &log), fs = builtinSource(file + ".frag", &log);
        if (!vs.empty() && !fs.empty()) prog = GlProgram::build(vs, fs, &log);
        if (!prog) {
          logError("built-in shader '%s' failed:\n%s", file.c_str(), log.c_str());
          if (pbr || i == kSpriteProgram) continue;  // optional path unsupported; the rest still inits
          return -1;
        }
      }
    }
    for (size_t i = 0; i < std::size(kProgramNames); ++i) {
      if (i == kChunkPbrProgram || i == kSpriteProgram) continue;
      const std::string_view name = kProgramNames[i];
      (name == "chunk" ? prog_ : name == "hud" ? hudProg_ : lineProg_) = std::move(*built[i]);
    }
    // PBR program: swap in on success, clear on failure (supportsPbrPath() reads id()).
    if (built[kChunkPbrProgram]) pbrProg_ = std::move(*built[kChunkPbrProgram]);
    else pbrProg_ = GlProgram();
    if (!supportsPbrPath()) logError("gl33: chunk_pbr unavailable - PBR/BinaryQuad path disabled, classic path only");
    // Sprite program: same optional-path treatment (drawSprites() reads id()).
    if (built[kSpriteProgram]) spriteProg_ = std::move(*built[kSpriteProgram]);
    else spriteProg_ = GlProgram();
    if (!spriteProg_.id()) logError("gl33: sprite shader unavailable - sprite pass disabled");
    else {
      uSpriteVP_ = spriteProg_.uniform("uVP");
      uSpriteAtlas_ = spriteProg_.uniform("uAtlas");
      uSpriteAlphaTest_ = spriteProg_.uniform("uAlphaTest");
      spriteProg_.use();
      glUniform1i(uSpriteAtlas_, 0);  // atlas pages live on texture unit 0 (2D target)
    }
    uVP_ = prog_.uniform("uVP"); uOffset_ = prog_.uniform("uOffset");
    uCam_ = prog_.uniform("uCam"); uSky_ = prog_.uniform("uSky");
    uFogEnd_ = prog_.uniform("uFogEnd"); uTime_ = prog_.uniform("uTime"); uDebug_ = prog_.uniform("uDebug");
    uModelDither_ = prog_.uniform("uModelDither"); uModelDitherBand_ = prog_.uniform("uModelDitherBand");
    uPulseP0_ = prog_.uniform("uPulseP0"); uPulseP1_ = prog_.uniform("uPulseP1");
    uGroundPulseTime_ = prog_.uniform("uGroundPulseTime"); uGroundPulseOn_ = prog_.uniform("uGroundPulseOn");
    uTraceP0_ = prog_.uniform("uTraceP0"); uTraceP1_ = prog_.uniform("uTraceP1");  // wafer ground traces (JD 2026-10-11)
    uGroundTraceTime_ = prog_.uniform("uGroundTraceTime"); uGroundTraceOn_ = prog_.uniform("uGroundTraceOn");
    uLineM_ = lineProg_.uniform("uM"); uLineC_ = lineProg_.uniform("uC");
    uScreen_ = hudProg_.uniform("uScreen");
    if (supportsPbrPath()) {
      uPbrVP_ = pbrProg_.uniform("uVP"); uPbrOffset_ = pbrProg_.uniform("uOffset");
      uPbrCam_ = pbrProg_.uniform("uCam");
      uPbrMat_ = pbrProg_.uniform("uPbrMaterials");
      uPbrLightPos_ = pbrProg_.uniform("uPbrLightPos[0]");
      uPbrLightColor_ = pbrProg_.uniform("uPbrLightColor[0]");
      uPbrLightCount_ = pbrProg_.uniform("uPbrLightCount");
      uPbrTier_ = pbrProg_.uniform("uPbrTier");
      uPbrModelDither_ = pbrProg_.uniform("uModelDither");
      uPbrModelDitherBand_ = pbrProg_.uniform("uModelDitherBand");
      uAmbientMode_ = pbrProg_.uniform("uAmbientMode");
      uSunDir_ = pbrProg_.uniform("uSunDir");
      uPbrMaterialRows_ = pbrProg_.uniform("uPbrMaterialRows");
      uSceneColor_ = pbrProg_.uniform("uSceneColor");
      uSceneSize_ = pbrProg_.uniform("uSceneSize");
      uSceneVP_ = pbrProg_.uniform("uSceneVP");
      uRefractionPass_ = pbrProg_.uniform("uRefractionPass");
      pbrProg_.use();
      glUniform1i(uPbrMat_, kPbrMatTexUnit);  // material table lives on unit 2 (0/1 are classic)
    }
    hudProg_.use();
    glUniform1i(hudProg_.uniform("uFont"), 0);
    applyMaterialUniforms();
    return fromPacks;
  }

  std::string deviceInfo() const override {
    auto s = [](GLenum e) { const unsigned char* p = glGetString(e); return std::string(p ? reinterpret_cast<const char*>(p) : "?"); };
    return s(GL_VENDOR) + " | " + s(GL_RENDERER) + " | " + s(GL_VERSION);
  }

  void uploadEntityMesh(const std::vector<PackedVertex>& verts) override {
    // Dynamic VBO for entities (items, etc.). Rebuilt each frame.
    // Binds exactly what chunk.vert declares (drawEntities uses prog_):
    // loc 0 = aPos @0, loc 1 = aInfo @6, loc 2 = aShade @10.
    // (Fix 2026-10-11: was binding slot@6 as uint to loc 1 and u,v,faceAo,layer
    // to loc 2 — neither matched the shader.)
    if (!entityVao_) {
      glGenVertexArrays(1, &entityVao_); glBindVertexArray(entityVao_);
      glGenBuffers(1, &entityVbo_); glBindBuffer(GL_ARRAY_BUFFER, entityVbo_);
      glBufferData(GL_ARRAY_BUFFER, sizeof(PackedVertex) * 4096, nullptr, GL_DYNAMIC_DRAW);
      glEnableVertexAttribArray(0);
      glVertexAttribIPointer(0, 3, GL_UNSIGNED_SHORT, sizeof(PackedVertex), nullptr);
      glEnableVertexAttribArray(1);
      glVertexAttribIPointer(1, 4, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(6));
      glEnableVertexAttribArray(2);
      glVertexAttribIPointer(2, 2, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(10));
      glBindVertexArray(0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, entityVbo_);
    const size_t n = std::min(verts.size(), size_t(4096));
    glBufferSubData(GL_ARRAY_BUFFER, 0, n * sizeof(PackedVertex), verts.data());
    entityVertCount_ = GLsizei(n);
  }
  void uploadChunkMesh(const ChunkMesh& m) override {
    // Fix 2026-10-11 (regression): left-shifting negative int64_t is UB;
    // cast through uint64_t first, then back (bit-preserving). Mirrors vk_renderer.cpp.
    const int64_t key = int64_t((uint64_t(uint32_t(m.cx)) << 40) ^ (uint64_t(uint32_t(m.sy)) << 32) ^ uint64_t(uint32_t(m.cz)));
    if (m.verts.empty()) { erase(key); return; }
    if (useArena_) { uploadToArena(key, m); return; }
    Gpu& g = meshes_[key];
    g.cx = m.cx; g.sy = m.sy; g.cz = m.cz;
    g.indexCount = GLsizei(std::min<size_t>(m.verts.size() / 4, kMaxQuads) * 6);  // the mesher already caps
    for (int i = 0; i < 4; ++i) { g.pulseP0[i] = m.pulseP0[i]; g.pulseP1[i] = m.pulseP1[i]; g.traceP0[i] = m.traceP0[i]; g.traceP1[i] = m.traceP1[i]; }
    if (!g.vao) {
      glGenVertexArrays(1, &g.vao); glBindVertexArray(g.vao);
      glGenBuffers(1, &g.vbo); glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
      // Fix 2026-10-11 (regression): the CRITICAL PackedVertex fix covered the
      // entity VAO and arena VAO but missed this per-chunk non-arena VAO, which
      // still bound the pre-fix layout (slot uint@6). Binds exactly what
      // chunk.vert declares, like the entity VAO above:
      // loc 0 = aPos @0, loc 1 = aInfo @6, loc 2 = aShade @10.
      glEnableVertexAttribArray(0);
      glVertexAttribIPointer(0, 3, GL_UNSIGNED_SHORT, sizeof(PackedVertex), nullptr);
      glEnableVertexAttribArray(1);
      glVertexAttribIPointer(1, 4, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(6));
      glEnableVertexAttribArray(2);
      glVertexAttribIPointer(2, 2, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(10));
      glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    } else {
      glBindVertexArray(g.vao); glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
    }
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(m.verts.size() * sizeof(PackedVertex)), m.verts.data(), GL_STATIC_DRAW);
  }

  void clearChunkMeshes() override {
    for (auto& [k, g] : meshes_) if (g.vao) { glDeleteBuffers(1, &g.vbo); glDeleteVertexArrays(1, &g.vao); }
    meshes_.clear();
    for (auto& [k, g] : pbrMeshes_) if (g.vao) { glDeleteBuffers(1, &g.vbo); glDeleteVertexArrays(1, &g.vao); }
    pbrMeshes_.clear();
    if (arenaVao_) { glDeleteBuffers(1, &arenaVbo_); glDeleteVertexArrays(1, &arenaVao_); }
    arenaVao_ = arenaVbo_ = 0;
    arenaVerts_ = 0;
    arenaFree_.clear();
  }

  void renderFrame(const FrameParams& p) override {
    // Culling-cache prototypes (JD 2026-10-11): arm per-frame state before the
    // chunk loops below. All default off; base margin culling is the foundation.
    cullProto_.beginFrame(p.viewProj, p.yawRad, p.pitchRad, p.timeSec);
    lastFrameW_ = p.width;
    lastFrameH_ = p.height;
    glViewport(0, 0, p.width, p.height);
    glClearColor(p.sky.x, p.sky.y, p.sky.z, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    prog_.use();
    glUniformMatrix4fv(uVP_, 1, GL_FALSE, p.viewProj.m);
    glUniform3f(uCam_, p.camPos.x, p.camPos.y, p.camPos.z);
    glUniform3f(uSky_, p.sky.x, p.sky.y, p.sky.z);
    glUniform1f(uFogEnd_, p.fogEnd);
    glUniform1f(uModelDither_, ditherEnabled_ ? 1.f : 0.f);  // near-camera fade (JD 2026-10-13)
    glUniform2f(uModelDitherBand_, ditherNear_, ditherFar_);
    glUniform1f(uTime_, p.timeSec);
    glUniform1i(uDebug_, p.debugOverlay ? 1 : 0);
    glUniform1f(uGroundPulseTime_, p.groundPulseTimeSec);  // wafer ground pulse: sim time, pausing freezes it
    glUniform1f(uGroundPulseOn_, p.groundPulseOn ? 1.f : 0.f);
    glUniform1f(uGroundTraceTime_, p.groundTraceTimeSec);  // wafer ground traces: same sim clock, independent toggle
    glUniform1f(uGroundTraceOn_, p.groundTraceOn ? 1.f : 0.f);
    textures_.bind();
    stats_.drawn = 0;
    // Fix 2026-10-11 (audit): was classic meshes only; count PBR meshes too.
    stats_.meshes = uint32_t(meshes_.size() + pbrMeshes_.size());
    GLuint boundVao = 0;
    // Cull decisions are made ONCE per chunk per frame (P3-3, Appendix D
    // L5): decide() has stats/state side effects, and the PBR loop below
    // used to re-decide chunks present in both maps — double-counting
    // cullProtoStats and re-running the comet/velocity state. The PBR
    // lambda (and both TrueRefraction passes) reuse this frame's map.
    std::unordered_map<int64_t, CullDecision> frameCull_;
    auto decideOnce = [&](int64_t key, const Vec3& mn) {
      const auto it = frameCull_.find(key);
      if (it != frameCull_.end()) return it->second;
      const CullDecision cd =
          cullProto_.decide(p.viewProj, mn, mn + Vec3{16, 16, 16}, p.camPos, frustumMarginDeg_);
      frameCull_.emplace(key, cd);
      return cd;
    };
    for (const auto& [k, g] : meshes_) {
      const Vec3 mn{g.cx * 16.f, g.sy * 16.f, g.cz * 16.f};
      // Prototype pipeline: Full draws; MeshOnly keeps the uploaded mesh but
      // skips the draw (outer tier); Cull skips. Base margin = foundation.
      const CullDecision cd = decideOnce(k, mn);
      if (cd != CullDecision::Full) continue;
      glUniform3f(uOffset_, mn.x, mn.y, mn.z);
      glUniform4fv(uPulseP0_, 1, g.pulseP0);  // wafer ground pulse: this mesh's blended params
      glUniform4fv(uPulseP1_, 1, g.pulseP1);
      glUniform4fv(uTraceP0_, 1, g.traceP0);  // wafer ground traces: this mesh's blended params
      glUniform4fv(uTraceP1_, 1, g.traceP1);
      if (useArena_) {
        if (boundVao != arenaVao_) { glBindVertexArray(arenaVao_); boundVao = arenaVao_; }
        glDrawElementsBaseVertex(GL_TRIANGLES, g.indexCount, GL_UNSIGNED_INT, nullptr, GLint(g.first));
      } else {
        glBindVertexArray(g.vao);
        glDrawElements(GL_TRIANGLES, g.indexCount, GL_UNSIGNED_INT, nullptr);
      }
      ++stats_.drawn;
    }
    // PBR (BinaryQuad) meshes: drawn with chunk_pbr. The classic meshes above are
    // untouched - program selection follows the uploaded meshes (coexistence).
    // pbrMatTex_ == 0 means no material table was ever uploaded (no registry);
    // without it the shader would read black, so skip the draw (loud, not silent).
    //
    // Draw helpers (lambdas, so the TrueRefraction two-pass path can reuse them):
    // drawPbrMeshes expects pbrProg_ current; drawEntities rebinds the classic prog_.
    auto drawPbrMeshes = [&]() {
      for (const auto& [k, g] : pbrMeshes_) {
        const Vec3 mn{g.cx * 16.f, g.sy * 16.f, g.cz * 16.f};
        if (decideOnce(k, mn) != CullDecision::Full)  // decided once per frame (D-L5)
          continue;
        glUniform3f(uPbrOffset_, mn.x, mn.y, mn.z);
        glBindVertexArray(g.vao);
        glDrawElements(GL_TRIANGLES, g.indexCount, GL_UNSIGNED_INT, nullptr);
        ++stats_.drawn;
      }
    };
    auto drawEntities = [&]() {
      // Entities (items): dynamic VBO, drawn as quads with the classic program.
      if (entityVertCount_ <= 0) return;
      prog_.use();
      glBindVertexArray(entityVao_);
      glUniform3f(uOffset_, 0.f, 0.f, 0.f);
      glDrawArrays(GL_TRIANGLES, 0, entityVertCount_);  // game emits 6 verts per quad
      ++stats_.drawn;
    };
    const bool pbrActive = supportsPbrPath() && !pbrMeshes_.empty() && pbrMatTex_ != 0;
    const bool refraction = pbrActive && pbrTier_ == PbrTransparencyTier::TrueRefraction;
    if (pbrActive) {
      pbrProg_.use();
      glUniformMatrix4fv(uPbrVP_, 1, GL_FALSE, p.viewProj.m);
      glUniform3f(uPbrCam_, p.camPos.x, p.camPos.y, p.camPos.z);
      glActiveTexture(GL_TEXTURE0 + kPbrMatTexUnit);
      glBindTexture(GL_TEXTURE_BUFFER, pbrMatTex_);
      // Analytic lights: the game fills FrameParams::pbrLights per frame from its
      // LightCollector (fed by ChunkEmissiveProvider); empty = specular off.
      const uint32_t nl =
          std::min<uint32_t>(p.pbrLightCount, uint32_t(FrameParams::kMaxPbrFrameLights));
      // Fix 2026-10-11 (audit): skip the array uploads when there are no lights
      // (the common case) — the count uniform alone suffices.
      if (nl > 0) {
        float lightPos[FrameParams::kMaxPbrFrameLights * 4];
        float lightColor[FrameParams::kMaxPbrFrameLights * 3];
        for (uint32_t i = 0; i < nl; ++i) {
          const AnalyticLight& L = p.pbrLights[i];
          lightPos[i * 4 + 0] = L.x; lightPos[i * 4 + 1] = L.y; lightPos[i * 4 + 2] = L.z;
          lightPos[i * 4 + 3] = L.intensity;
          lightColor[i * 3 + 0] = L.r; lightColor[i * 3 + 1] = L.g; lightColor[i * 3 + 2] = L.b;
        }
        glUniform4fv(uPbrLightPos_, GLsizei(nl), lightPos);
        glUniform3fv(uPbrLightColor_, GLsizei(nl), lightColor);
      }
      glUniform1i(uPbrLightCount_, GLint(nl));
      glUniform1i(uPbrTier_, GLint(int(pbrTier_)));
      glUniform1f(uPbrModelDither_, ditherEnabled_ ? 1.f : 0.f);
      glUniform2f(uPbrModelDitherBand_, ditherNear_, ditherFar_);
      glUniform1i(uAmbientMode_, GLint(int(ambientMode_)));
      glUniform3f(uSunDir_, sunDir_[0], sunDir_[1], sunDir_[2]);
      glUniform1i(uPbrMaterialRows_, GLint(pbrMatRows_));
      glUniform1i(uRefractionPass_, 0);  // single-pass tiers ignore it; tier 3 pass 0 builds the copy
      // Blend state follows the tier: Simple and PbrReflections composite by alpha;
      // ScreenDoor discards in-shader (no blending, no sorting); TrueRefraction
      // resolves to an opaque composite in two passes (no blending either pass).
      const bool pbrBlend = pbrTier_ == PbrTransparencyTier::Simple ||
                            pbrTier_ == PbrTransparencyTier::PbrReflections;
      if (pbrBlend) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); }
      if (refraction) {
        // Pass 0: opaque PBR fragments only (the shader discards alpha < 1).
        drawPbrMeshes();
        // Entities join the opaque composite so refractive surfaces show them.
        drawEntities();
        copySceneColor(p);
        // Pass 1: refractive fragments only, sampling the copy (alpha >= 1 discarded).
        pbrProg_.use();
        glUniform1i(uRefractionPass_, 1);
        drawPbrMeshes();
      } else {
        drawPbrMeshes();
        drawEntities();
      }
      if (pbrBlend) glDisable(GL_BLEND);
      glActiveTexture(GL_TEXTURE0);  // leave the classic unit current
    } else {
      drawEntities();
    }
    drawSprites(p);  // sprite pass: after world + entities, before lines/HUD
    lineProg_.use();
    glBindVertexArray(lineVao_); glBindBuffer(GL_ARRAY_BUFFER, lineVbo_);
    if (p.highlight) {
      const auto v = blockOutlineLines(p.hx, p.hy, p.hz);
      glBufferData(GL_ARRAY_BUFFER, sizeof v, v.data(), GL_DYNAMIC_DRAW);
      glUniformMatrix4fv(uLineM_, 1, GL_FALSE, p.viewProj.m);
      glUniform3f(uLineC_, 0.05f, 0.05f, 0.08f);
      glDrawArrays(GL_LINES, 0, GLsizei(v.size()));
    }
    if (p.crosshair && p.height > 0) {
      const auto v = crosshairLines(p.width, p.height);
      const Mat4 id = Mat4::identity();
      glBufferData(GL_ARRAY_BUFFER, sizeof v, v.data(), GL_DYNAMIC_DRAW);
      glUniformMatrix4fv(uLineM_, 1, GL_FALSE, id.m);
      glUniform3f(uLineC_, 1.f, 1.f, 1.f);
      glDisable(GL_DEPTH_TEST);
      glDrawArrays(GL_LINES, 0, GLsizei(v.size()));
      glEnable(GL_DEPTH_TEST);
    }
    drawHud(p);
  }

  RenderStats stats() const override { return stats_; }

  // Screenshot grab (lithoscript `screenshot`): the game arms the grab during
  // update, before the frame is drawn; the capture runs after renderFrame(),
  // before the caller presents, reading the back buffer with glReadPixels.
  void requestGrab() override { grabArmed_ = true; }
  bool grabFramebuffer(std::vector<uint8_t>& rgb, int& w, int& h) override {
    if (!grabArmed_ || lastFrameW_ <= 0 || lastFrameH_ <= 0) return false;
    grabArmed_ = false;
    w = lastFrameW_;
    h = lastFrameH_;
    rgb.resize(size_t(w) * size_t(h) * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    // glReadPixels is bottom-up; PNG wants top-first: flip the rows in place.
    const size_t row = size_t(w) * 3;
    std::vector<uint8_t> tmp(row);
    for (int y = 0; y < h / 2; ++y) {
      uint8_t* a = rgb.data() + size_t(y) * row;
      uint8_t* b = rgb.data() + size_t(h - 1 - y) * row;
      std::memcpy(tmp.data(), a, row);
      std::memcpy(a, b, row);
      std::memcpy(b, tmp.data(), row);
    }
    return true;
  }

 private:
  // Uniforms that come from the block models; re-applied after a shader reload.
  void applyMaterialUniforms() {
    prog_.use();
    auto u1 = [&](const std::string& n, int v) { glUniform1i(prog_.uniform(n.c_str()), v); };
    auto arr = [&](const char* n, size_t i) { return std::string(n) + "[" + std::to_string(i) + "]"; };
    u1("uTex", 0); u1("uPal", 1); u1("uTile", textures_.tileSize());
    for (size_t c = 0; c < 2; ++c) { u1(arr("uWirePal", c), bindings_.stripPalette[c]); u1(arr("uRing", c), bindings_.stripRing[c]); }
    for (size_t t = 0; t < 3; ++t) {
      u1(arr("uLampPal", t), bindings_.tierPalette[t]);
      u1(arr("uLampMask", t), bindings_.tierMask[t]);
      u1(arr("uLampRim", t), bindings_.tierRim[t]);
    }
    u1("uRepMask", bindings_.pipMask); u1("uMemMask", bindings_.cellMask);
    for (size_t c = 0; c < 2; ++c) u1(arr("uDot", c), bindings_.stripDot[c]);
    u1("uDigits", bindings_.debugDigits);
    for (size_t i = 0; i < animations_.size(); ++i) {
      u1(arr("uAnimStart", i), animations_[i].sequenceStart);
      u1(arr("uAnimLen", i), animations_[i].sequenceLength);
      glUniform1f(prog_.uniform(arr("uAnimMs", i).c_str()), animations_[i].frameMs);
    }
    for (size_t i = 0; i < animFrames_.size(); ++i) u1(arr("uAnimSeq", i), animFrames_[i]);
  }

  // vao / vbo: per-mesh path. first / count: the mesh's vertex range in the arena VBO.
  struct Gpu { GLuint vao = 0, vbo = 0; GLsizei indexCount = 0; int cx = 0, sy = 0, cz = 0; size_t first = 0, count = 0;
                 float pulseP0[4]{}; float pulseP1[4]{}; float traceP0[4]{}; float traceP1[4]{}; };  // wafer ground-pulse + trace blended params
  // PBR (BinaryQuad) meshes: one VAO+VBO each (never in the arena - see uploadPbrChunkMesh).
  struct PbrGpu { GLuint vao = 0, vbo = 0; GLsizei indexCount = 0; int cx = 0, sy = 0, cz = 0; };
  void erase(int64_t key) {
    auto it = meshes_.find(key);
    if (it == meshes_.end()) return;
    if (useArena_) freeRange(it->second.first, it->second.count);
    else { glDeleteBuffers(1, &it->second.vbo); glDeleteVertexArrays(1, &it->second.vao); }
    meshes_.erase(it);
  }
  void erasePbr(int64_t key) {
    auto it = pbrMeshes_.find(key);
    if (it == pbrMeshes_.end()) return;
    glDeleteBuffers(1, &it->second.vbo); glDeleteVertexArrays(1, &it->second.vao);
    pbrMeshes_.erase(it);
  }

  // ---- arena: one VBO (grown by doubling with glCopyBufferSubData) and one VAO; first-fit
  // free list in vertex units with coalescing. glBufferSubData into a freed range is ordered
  // by the driver after earlier draws that read it.
  static constexpr size_t kArenaInitialVerts = (size_t(32) << 20) / sizeof(PackedVertex);
  void bindArenaAttribs() {
    glBindVertexArray(arenaVao_);
    glBindBuffer(GL_ARRAY_BUFFER, arenaVbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribIPointer(0, 3, GL_UNSIGNED_SHORT, sizeof(PackedVertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribIPointer(1, 4, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(6));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 2, GL_UNSIGNED_BYTE, sizeof(PackedVertex), reinterpret_cast<const void*>(10));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
  }
  void growArena(size_t minVerts) {
    size_t cap = std::max(arenaVerts_ ? arenaVerts_ * 2 : kArenaInitialVerts, minVerts);
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_COPY_WRITE_BUFFER, vbo);
    glBufferData(GL_COPY_WRITE_BUFFER, GLsizeiptr(cap * sizeof(PackedVertex)), nullptr, GL_STATIC_DRAW);
    if (arenaVbo_) {
      glBindBuffer(GL_COPY_READ_BUFFER, arenaVbo_);
      glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, GLsizeiptr(arenaVerts_ * sizeof(PackedVertex)));
      glDeleteBuffers(1, &arenaVbo_);
    }
    if (!arenaVao_) glGenVertexArrays(1, &arenaVao_);
    freeRange(arenaVerts_, cap - arenaVerts_);
    arenaVbo_ = vbo;
    arenaVerts_ = cap;
    bindArenaAttribs();
  }
  std::optional<size_t> allocRange(size_t n) {
    for (auto it = arenaFree_.begin(); it != arenaFree_.end(); ++it) {
      if (it->second < n) continue;
      const size_t first = it->first, rest = it->second - n;
      arenaFree_.erase(it);
      if (rest) arenaFree_.emplace(first + n, rest);
      return first;
    }
    return std::nullopt;
  }
  void freeRange(size_t first, size_t n) {
    if (!n) return;
    auto it = arenaFree_.emplace(first, n).first;
    if (const auto next = std::next(it); next != arenaFree_.end() && it->first + it->second == next->first) {
      it->second += next->second;
      arenaFree_.erase(next);
    }
    if (it != arenaFree_.begin())
      if (const auto prev = std::prev(it); prev->first + prev->second == it->first) {
        prev->second += it->second;
        arenaFree_.erase(it);
      }
  }
  void uploadToArena(int64_t key, const ChunkMesh& m) {
    const size_t n = m.verts.size();
    auto it = meshes_.find(key);
    if (it != meshes_.end() && it->second.count != n) { freeRange(it->second.first, it->second.count); it->second.count = 0; }
    Gpu& g = meshes_[key];
    if (g.count != n) {
      auto first = allocRange(n);
      if (!first) { growArena(n); first = allocRange(n); }
      // Fix 2026-10-11 (audit): allocation failure after the old range was freed
      // left stale first/indexCount -> draws garbage from the freed range.
      if (!first) { g.indexCount = 0; return; }
      g.first = *first;
      g.count = n;
    }
    g.cx = m.cx; g.sy = m.sy; g.cz = m.cz;
    g.indexCount = GLsizei(std::min<size_t>(n / 4, kMaxQuads) * 6);
    for (int i = 0; i < 4; ++i) { g.pulseP0[i] = m.pulseP0[i]; g.pulseP1[i] = m.pulseP1[i]; g.traceP0[i] = m.traceP0[i]; g.traceP1[i] = m.traceP1[i]; }
    glBindBuffer(GL_ARRAY_BUFFER, arenaVbo_);
    glBufferSubData(GL_ARRAY_BUFFER, GLintptr(g.first * sizeof(PackedVertex)), GLsizeiptr(n * sizeof(PackedVertex)), m.verts.data());
  }
  bool useArena_ = true;
  GLuint arenaVao_ = 0, arenaVbo_ = 0;
  size_t arenaVerts_ = 0;
  std::map<size_t, size_t> arenaFree_;  // first vertex -> count
  std::unordered_map<int64_t, Gpu> meshes_;
  GLuint entityVao_ = 0, entityVbo_ = 0;
  GLsizei entityVertCount_ = 0;
  GlProgram prog_, lineProg_, hudProg_;
  GLuint ebo_ = 0, lineVao_ = 0, lineVbo_ = 0;
  GlTextureArray textures_;
  MaterialBindings bindings_;
  std::vector<ShaderAnimation> animations_;
  std::vector<int> animFrames_;
  GLint uTime_ = -1, uDebug_ = -1;
  GLint uPulseP0_ = -1, uPulseP1_ = -1, uGroundPulseTime_ = -1, uGroundPulseOn_ = -1;  // wafer ground pulse
  GLint uTraceP0_ = -1, uTraceP1_ = -1, uGroundTraceTime_ = -1, uGroundTraceOn_ = -1;  // wafer ground traces (JD 2026-10-11)
  GLint uVP_ = -1, uOffset_ = -1, uCam_ = -1, uSky_ = -1, uFogEnd_ = -1, uLineM_ = -1, uLineC_ = -1;
  GLint uModelDither_ = -1, uModelDitherBand_ = -1;  // near-camera fade (JD 2026-10-13)
  bool ditherEnabled_ = false; float ditherNear_ = 2.f, ditherFar_ = 8.f;  // setting (default on) enables it
  // PBR (BinaryQuad) path state (phase 2, Worker 3).
  GlProgram pbrProg_;
  std::unordered_map<int64_t, PbrGpu> pbrMeshes_;
  GLuint pbrMatBuf_ = 0, pbrMatTex_ = 0;  // material table: buffer texture (GL 3.1+)
  uint64_t pbrMatGeneration_ = UINT64_MAX;  // registry generation of the uploaded table
  PbrTransparencyTier pbrTier_ = PbrTransparencyTier::ScreenDoor;  // user-selectable; game sets it
  static constexpr GLint kPbrMatTexUnit = 2;  // material table texture unit (0/1 are classic)
  GLint uPbrVP_ = -1, uPbrOffset_ = -1, uPbrCam_ = -1, uPbrMat_ = -1;
  GLint uPbrLightPos_ = -1, uPbrLightColor_ = -1, uPbrLightCount_ = -1, uPbrTier_ = -1;
  GLint uPbrModelDither_ = -1, uPbrModelDitherBand_ = -1;
  GLint uAmbientMode_ = -1, uSunDir_ = -1, uPbrMaterialRows_ = -1;
  AmbientMode ambientMode_ = AmbientMode::Fraction;  // JD-locked 2026-10-11; game sets it
  float frustumMarginDeg_ = 5.f;  ///< FOV culling margin, degrees (JD 2026-10-11); game sets it
  CullProtoState cullProto_;  ///< culling-cache prototypes (JD 2026-10-11); all default off
  float sunDir_[3] = {0.4f, 0.85f, 0.3f};  ///< sun direction (need not be unit)
  int pbrMatRows_ = 0;  ///< live PBR material table row count (for the OOB clamp)
  // TrueRefraction (Ultra) scene-color pass state (phase 2, Worker 4).
  GLuint sceneColorTex_ = 0;  // full-res RGBA8 copy of the opaque composite
  int sceneColorW_ = 0, sceneColorH_ = 0;
  static constexpr GLint kSceneColorTexUnit = 3;  // 0/1 classic, 2 = PBR material TBO
  GLint uSceneColor_ = -1, uSceneSize_ = -1, uSceneVP_ = -1, uRefractionPass_ = -1;
  RenderStats stats_;
  GLuint hudVao_ = 0, hudVbo_ = 0, fontTex_ = 0;
  GLint uScreen_ = -1;
  std::vector<HudVertex> hudVerts_;
  // Sprite pass state (2026-10-12): program + streamed VBO + atlas page
  // textures; spriteAtlasUploaded_ is the registry version on the GPU
  // (UINT64_MAX = nothing uploaded yet).
  GlProgram spriteProg_;
  GLuint spriteVao_ = 0, spriteVbo_ = 0;
  std::vector<GLuint> spriteTex_;
  uint64_t spriteAtlasUploaded_ = UINT64_MAX;
  GLint uSpriteVP_ = -1, uSpriteAtlas_ = -1, uSpriteAlphaTest_ = -1;
  std::vector<SpriteVert> spriteVerts_;
  size_t spriteVboCapacity_ = 6144;  ///< verts allocated in spriteVbo_ (grow-only, D-M5)
  // Screenshot grab state: the armed frame's size (renderFrame records it),
  // so grabFramebuffer() knows how much to read back.
  int lastFrameW_ = 0, lastFrameH_ = 0;
  bool grabArmed_ = false;
};

}  // namespace

std::unique_ptr<IRenderer> createGL33Renderer(GLProcLoader loader, bool arena) {
  if (!vcLoadGL(loader)) return nullptr;
  auto r = std::make_unique<GL33Renderer>(arena);
  if (!r->init()) return nullptr;
  return r;
}

}  // namespace litholattice
