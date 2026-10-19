// Software present path: framebuffer texture upload + fullscreen blit.
// See software_presenter.h for the design and layer rationale.
#include "litholattice/render/software_presenter.h"

#include <optional>
#include <string>
#include <utility>

#include "gl_loader.h"
#include "gl_program.h"
#include "litholattice/core/log.h"

namespace litholattice {
namespace {

// Fullscreen triangle from gl_VertexID (no vertex buffers): ids 0,1,2 map
// to NDC (-1,-1), (3,-1), (-1,3) with uvs (0,0), (2,0), (0,2).
constexpr const char* kPresentVert = R"(#version 330 core
out vec2 vUv;
void main() {
  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  vUv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// The framebuffer is top-row-first and texture row 0 is the first uploaded
// row, so sample with a flipped v: the frame's top row lands at the top of
// the window. (Orientation is proven end-to-end by the app capture check.)
constexpr const char* kPresentFrag = R"(#version 330 core
in vec2 vUv;
uniform sampler2D uTex;
out vec4 oColor;
void main() {
  oColor = texture(uTex, vec2(vUv.x, 1.0 - vUv.y));
}
)";

class SoftwarePresenterImpl final : public SoftwarePresenter {
 public:
  ~SoftwarePresenterImpl() override {
    if (tex_) litholattice_glDeleteTextures(1, &tex_);
    if (vao_) litholattice_glDeleteVertexArrays(1, &vao_);
  }

  bool init() {
    std::string log;
    std::optional<gl33::GlProgram> prog = gl33::GlProgram::build(kPresentVert, kPresentFrag, &log);
    if (!prog) {
      logError("software present: blit program failed: %s", log.c_str());
      return false;
    }
    prog_ = std::move(*prog);
    texLoc_ = prog_.uniform("uTex");
    litholattice_glGenTextures(1, &tex_);
    litholattice_glGenVertexArrays(1, &vao_);
    if (!tex_ || !vao_) {
      logError("software present: GL object creation failed");
      return false;
    }
    litholattice_glBindTexture(GL_TEXTURE_2D, tex_);
    litholattice_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    litholattice_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    litholattice_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    litholattice_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    litholattice_glBindTexture(GL_TEXTURE_2D, 0);
    return true;
  }

  void present(const uint32_t* pixels, int w, int h, int fbW, int fbH) override {
    if (!pixels || w <= 0 || h <= 0 || fbW <= 0 || fbH <= 0) return;
    litholattice_glViewport(0, 0, fbW, fbH);
    litholattice_glDisable(GL_DEPTH_TEST);
    litholattice_glDisable(GL_BLEND);
    litholattice_glActiveTexture(GL_TEXTURE0);
    litholattice_glBindTexture(GL_TEXTURE_2D, tex_);
    // 0xAARRGGBB texels are B,G,R,A bytes in memory: GL_BGRA/UNSIGNED_BYTE
    // reads them with no swizzle pass on the CPU.
    if (w != texW_ || h != texH_) {
      litholattice_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
      texW_ = w;
      texH_ = h;
    } else {
      litholattice_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
    }
    prog_.use();
    if (texLoc_ >= 0) litholattice_glUniform1i(texLoc_, 0);
    litholattice_glBindVertexArray(vao_);
    litholattice_glDrawArrays(GL_TRIANGLES, 0, 3);
    litholattice_glBindVertexArray(0);
    litholattice_glUseProgram(0);
    litholattice_glBindTexture(GL_TEXTURE_2D, 0);
  }

 private:
  gl33::GlProgram prog_;
  GLint texLoc_ = -1;
  GLuint tex_ = 0, vao_ = 0;
  int texW_ = 0, texH_ = 0;
};

}  // namespace

std::unique_ptr<SoftwarePresenter> createSoftwarePresenter(GLProcLoader loader) {
  if (!vcLoadGL(loader)) return nullptr;
  auto p = std::make_unique<SoftwarePresenterImpl>();
  if (!p->init()) return nullptr;
  return p;
}

}  // namespace litholattice
