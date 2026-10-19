#version 450
#extension GL_GOOGLE_include_directive : require
// Vulkan sprites: world-space textured quads (billboards / particles) from
// the sprite pass — the vk twin of ../sprite.vert (GL33). Positions are
// world blocks; the frame's view-projection comes from the shared Frame
// uniform block (set 0, binding 0, frame.glsl), converted from OpenGL to
// Vulkan clip conventions exactly like vk/chunk.vert and vk/line.vert.
// UVs are atlas image space (v down from the page's top-left); the atlas
// pages upload row-first as a texture array, layer = the quad's page.
#include "frame.glsl"
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aCol;
layout(location = 3) in float aPage;
layout(location = 0) out vec2 vUV;
layout(location = 1) out vec4 vCol;
layout(location = 2) flat out int vPage;
void main() {
  vUV = aUV; vCol = aCol; vPage = int(aPage + 0.5);
  vec4 p = uVP * vec4(aPos, 1.0);
  gl_Position = vec4(p.x, -p.y, (p.z + p.w) * 0.5, p.w);
}
