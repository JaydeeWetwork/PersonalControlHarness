#version 330 core
// Sprites: world-space textured quads (billboards / particles) from the
// sprite pass. Positions are world blocks; uVP is the frame's view-projection
// (same matrix the chunk pass uses). UVs are atlas image space (v down from
// the page's top-left); the atlas pages are uploaded row-first, so the
// emitted corner/UV pairing holds as-is.
layout(location = 0) in vec3 aPos; layout(location = 1) in vec2 aUV; layout(location = 2) in vec4 aCol;
uniform mat4 uVP;
out vec2 vUV; out vec4 vCol;
void main() {
  vUV = aUV; vCol = aCol;
  gl_Position = uVP * vec4(aPos, 1.0);
}
