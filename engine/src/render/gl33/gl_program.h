// Private to the gl33 backend: a linked GLSL program (RAII, move-only).
#pragma once
#include <optional>
#include <string>
#include "gl_loader.h"

namespace litholattice::gl33 {

class GlProgram {
 public:
  GlProgram() = default;
  ~GlProgram();
  GlProgram(GlProgram&& o) noexcept : id_(o.id_) { o.id_ = 0; }
  GlProgram& operator=(GlProgram&& o) noexcept;
  GlProgram(const GlProgram&) = delete;
  GlProgram& operator=(const GlProgram&) = delete;

  /// Compiles and links; nullopt with the driver's log in `log` on failure.
  static std::optional<GlProgram> build(const std::string& vertex, const std::string& fragment, std::string* log);

  GLuint id() const { return id_; }
  GLint uniform(const char* name) const { return glGetUniformLocation(id_, name); }
  void use() const { glUseProgram(id_); }

 private:
  explicit GlProgram(GLuint id) : id_(id) {}
  GLuint id_ = 0;
};

}  // namespace litholattice::gl33
