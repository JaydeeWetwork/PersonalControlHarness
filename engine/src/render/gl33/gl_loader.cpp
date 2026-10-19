#include "gl_loader.h"
#include <cstdio>

#define LITHOLATTICE_DEFINE(ret, name, args) PFN_litholattice_gl##name litholattice_gl##name = nullptr;
LITHOLATTICE_GL_FUNCS(LITHOLATTICE_DEFINE)
#undef LITHOLATTICE_DEFINE

bool vcLoadGL(void* (*loader)(const char*)) {
  bool ok = true;
#define LITHOLATTICE_LOAD(ret, name, args)                                                     \
  litholattice_gl##name = reinterpret_cast<PFN_litholattice_gl##name>(loader("gl" #name)); \
  if (!litholattice_gl##name) { std::fprintf(stderr, "GL loader: missing gl%s\n", #name); ok = false; }
  LITHOLATTICE_GL_FUNCS(LITHOLATTICE_LOAD)
#undef LITHOLATTICE_LOAD
  return ok;
}
