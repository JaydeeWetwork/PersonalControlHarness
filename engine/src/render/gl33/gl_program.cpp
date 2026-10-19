#include "gl_program.h"

namespace litholattice::gl33 {
namespace {

GLuint compile(GLenum type, const std::string& src, std::string* log) {
  const GLuint s = glCreateShader(type);
  const char* text = src.c_str();
  glShaderSource(s, 1, &text, nullptr);
  glCompileShader(s);
  GLint ok = 0;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (ok) return s;
  char buf[2048] = {};
  glGetShaderInfoLog(s, sizeof buf, nullptr, buf);
  if (log) *log += std::string(type == GL_VERTEX_SHADER ? "vertex: " : "fragment: ") + buf;
  glDeleteShader(s);
  return 0;
}

}  // namespace

GlProgram::~GlProgram() {
  if (id_) glDeleteProgram(id_);
}

GlProgram& GlProgram::operator=(GlProgram&& o) noexcept {
  if (this != &o) {
    if (id_) glDeleteProgram(id_);
    id_ = o.id_;
    o.id_ = 0;
  }
  return *this;
}

std::optional<GlProgram> GlProgram::build(const std::string& vertex, const std::string& fragment, std::string* log) {
  const GLuint vs = compile(GL_VERTEX_SHADER, vertex, log);
  const GLuint fs = compile(GL_FRAGMENT_SHADER, fragment, log);
  if (!vs || !fs) {
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return std::nullopt;
  }
  GlProgram p(glCreateProgram());
  glAttachShader(p.id_, vs);
  glAttachShader(p.id_, fs);
  glLinkProgram(p.id_);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint ok = 0;
  glGetProgramiv(p.id_, GL_LINK_STATUS, &ok);
  if (!ok) {
    char buf[2048] = {};
    glGetProgramInfoLog(p.id_, sizeof buf, nullptr, buf);
    if (log) *log += std::string("link: ") + buf;
    return std::nullopt;
  }
  return p;
}

}  // namespace litholattice::gl33
