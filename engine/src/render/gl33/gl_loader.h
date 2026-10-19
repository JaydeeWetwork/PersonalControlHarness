// Tiny vendored OpenGL 3.3 core loader: only the entry points this prototype uses.
// Resolved at runtime through a loader supplied by the platform layer (GLFW today).
// Private to the gl33 backend.
#pragma once
#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#define LITHOLATTICE_APIENTRY __stdcall
#else
#define LITHOLATTICE_APIENTRY
#endif

using GLenum = unsigned int;   using GLuint = unsigned int;  using GLint = int;
using GLsizei = int;           using GLboolean = unsigned char; using GLbitfield = unsigned int;
using GLfloat = float;         using GLchar = char;          using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_LINES 0x0001
#define GL_TRIANGLES 0x0004
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_TEST 0x0B71
#define GL_CULL_FACE 0x0B44
#define GL_BACK 0x0405
#define GL_CCW 0x0901
#define GL_LEQUAL 0x0203
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
#define GL_STATIC_DRAW 0x88E4
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_COPY_READ_BUFFER 0x8F36
#define GL_COPY_WRITE_BUFFER 0x8F37
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#define GL_RGBA8 0x8058
#define GL_RGBA 0x1908
#define GL_BGRA 0x80E1
#define GL_RGB 0x1907
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_LINEAR 0x2601
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE1 0x84C1
#define GL_RED 0x1903
#define GL_R8 0x8229
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_NEAREST 0x2600
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_BLEND 0x0BE2
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 0x0001
#define GL_TEXTURE_BUFFER 0x8C2A
#define GL_R32F 0x822E

#define LITHOLATTICE_GL_FUNCS(X) \
  X(void, Clear, (GLbitfield)) \
  X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
  X(void, Enable, (GLenum)) \
  X(void, Disable, (GLenum)) \
  X(void, GenTextures, (GLsizei, GLuint*)) \
  X(void, TexImage3D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
  X(void, TexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
  X(void, GenerateMipmap, (GLenum)) \
  X(void, ActiveTexture, (GLenum)) \
  X(GLenum, GetError, ()) \
  X(void, DeleteTextures, (GLsizei, const GLuint*)) \
  X(void, BindTexture, (GLenum, GLuint)) \
  X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
  X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
  X(void, CopyTexSubImage2D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei)) \
  X(void, TexParameteri, (GLenum, GLenum, GLint)) \
  X(void, PixelStorei, (GLenum, GLint)) \
  X(void, BlendFunc, (GLenum, GLenum)) \
  X(void, Uniform2f, (GLint, GLfloat, GLfloat)) \
  X(void, Uniform1i, (GLint, GLint)) \
  X(void, DrawArrays, (GLenum, GLint, GLsizei)) \
  X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) \
  X(void, CullFace, (GLenum)) \
  X(void, FrontFace, (GLenum)) \
  X(void, DepthFunc, (GLenum)) \
  X(void, DepthMask, (GLboolean)) \
  X(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
  X(const unsigned char*, GetString, (GLenum)) \
  X(void, DrawElements, (GLenum, GLsizei, GLenum, const void*)) \
  X(GLuint, CreateShader, (GLenum)) \
  X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) \
  X(void, CompileShader, (GLuint)) \
  X(void, GetShaderiv, (GLuint, GLenum, GLint*)) \
  X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
  X(void, DeleteShader, (GLuint)) \
  X(GLuint, CreateProgram, ()) \
  X(void, AttachShader, (GLuint, GLuint)) \
  X(void, LinkProgram, (GLuint)) \
  X(void, GetProgramiv, (GLuint, GLenum, GLint*)) \
  X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) \
  X(void, UseProgram, (GLuint)) \
  X(void, DeleteProgram, (GLuint)) \
  X(GLint, GetUniformLocation, (GLuint, const GLchar*)) \
  X(void, UniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat*)) \
  X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat)) \
  X(void, Uniform1f, (GLint, GLfloat)) \
  X(void, GenVertexArrays, (GLsizei, GLuint*)) \
  X(void, DeleteVertexArrays, (GLsizei, const GLuint*)) \
  X(void, BindVertexArray, (GLuint)) \
  X(void, GenBuffers, (GLsizei, GLuint*)) \
  X(void, DeleteBuffers, (GLsizei, const GLuint*)) \
  X(void, BindBuffer, (GLenum, GLuint)) \
  X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum)) \
  X(void, BufferSubData, (GLenum, GLintptr, GLsizeiptr, const void*)) \
  X(void, CopyBufferSubData, (GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr)) \
  X(void, DrawElementsBaseVertex, (GLenum, GLsizei, GLenum, const void*, GLint)) \
  X(void, EnableVertexAttribArray, (GLuint)) \
  X(void, VertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void*)) \
  X(void, TexBuffer, (GLenum, GLenum, GLuint)) \
  X(void, Uniform3fv, (GLint, GLsizei, const GLfloat*)) \
  X(void, Uniform4fv, (GLint, GLsizei, const GLfloat*)) \
  X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))

#define LITHOLATTICE_DECLARE(ret, name, args) \
  using PFN_litholattice_gl##name = ret(LITHOLATTICE_APIENTRY*) args; extern PFN_litholattice_gl##name litholattice_gl##name;
LITHOLATTICE_GL_FUNCS(LITHOLATTICE_DECLARE)
#undef LITHOLATTICE_DECLARE

#define glClear litholattice_glClear
#define glDeleteProgram litholattice_glDeleteProgram
#define glClearColor litholattice_glClearColor
#define glEnable litholattice_glEnable
#define glDisable litholattice_glDisable
#define glGenTextures litholattice_glGenTextures
#define glTexImage3D litholattice_glTexImage3D
#define glTexSubImage3D litholattice_glTexSubImage3D
#define glGenerateMipmap litholattice_glGenerateMipmap
#define glActiveTexture litholattice_glActiveTexture
#define glGetError litholattice_glGetError
#define glDeleteTextures litholattice_glDeleteTextures
#define glBindTexture litholattice_glBindTexture
#define glTexImage2D litholattice_glTexImage2D
#define glCopyTexSubImage2D litholattice_glCopyTexSubImage2D
#define glTexParameteri litholattice_glTexParameteri
#define glPixelStorei litholattice_glPixelStorei
#define glBlendFunc litholattice_glBlendFunc
#define glUniform2f litholattice_glUniform2f
#define glUniform1i litholattice_glUniform1i
#define glDrawArrays litholattice_glDrawArrays
#define glVertexAttribPointer litholattice_glVertexAttribPointer
#define glCullFace litholattice_glCullFace
#define glFrontFace litholattice_glFrontFace
#define glDepthFunc litholattice_glDepthFunc
#define glDepthMask litholattice_glDepthMask
#define glViewport litholattice_glViewport
#define glGetString litholattice_glGetString
#define glDrawElements litholattice_glDrawElements
#define glCreateShader litholattice_glCreateShader
#define glShaderSource litholattice_glShaderSource
#define glCompileShader litholattice_glCompileShader
#define glGetShaderiv litholattice_glGetShaderiv
#define glGetShaderInfoLog litholattice_glGetShaderInfoLog
#define glDeleteShader litholattice_glDeleteShader
#define glCreateProgram litholattice_glCreateProgram
#define glAttachShader litholattice_glAttachShader
#define glLinkProgram litholattice_glLinkProgram
#define glGetProgramiv litholattice_glGetProgramiv
#define glGetProgramInfoLog litholattice_glGetProgramInfoLog
#define glUseProgram litholattice_glUseProgram
#define glGetUniformLocation litholattice_glGetUniformLocation
#define glUniformMatrix4fv litholattice_glUniformMatrix4fv
#define glUniform3f litholattice_glUniform3f
#define glUniform1f litholattice_glUniform1f
#define glGenVertexArrays litholattice_glGenVertexArrays
#define glDeleteVertexArrays litholattice_glDeleteVertexArrays
#define glBindVertexArray litholattice_glBindVertexArray
#define glGenBuffers litholattice_glGenBuffers
#define glDeleteBuffers litholattice_glDeleteBuffers
#define glBindBuffer litholattice_glBindBuffer
#define glBufferData litholattice_glBufferData
#define glBufferSubData litholattice_glBufferSubData
#define glCopyBufferSubData litholattice_glCopyBufferSubData
#define glDrawElementsBaseVertex litholattice_glDrawElementsBaseVertex
#define glEnableVertexAttribArray litholattice_glEnableVertexAttribArray
#define glVertexAttribIPointer litholattice_glVertexAttribIPointer
#define glTexBuffer litholattice_glTexBuffer
#define glUniform3fv litholattice_glUniform3fv
#define glUniform4fv litholattice_glUniform4fv
#define glReadPixels litholattice_glReadPixels

// Returns false (and prints the missing symbol) if any entry point can't be resolved.
// Call with a current GL context.
bool vcLoadGL(void* (*loader)(const char*));
