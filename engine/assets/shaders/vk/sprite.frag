#version 450
// Vulkan sprites: atlas texel x vertex tint — the vk twin of
// ../sprite.frag (GL33). The atlas pages live in a texture array at
// set 0, binding 4 (layer = the quad's page). uAlphaTest (push constant)
// is 0.5 for the cutout pass and 0 for the blended passes; the discard
// keeps cutout edges hard under either blend state, as on GL33.
layout(set = 0, binding = 4) uniform sampler2DArray uAtlas;
layout(push_constant) uniform SpritePush { float uAlphaTest; float pad[3]; };
layout(location = 0) in vec2 vUV;
layout(location = 1) in vec4 vCol;
layout(location = 2) flat in int vPage;
layout(location = 0) out vec4 frag;
void main() {
  vec4 c = texture(uAtlas, vec3(vUV, float(vPage))) * vCol;
  if (c.a < uAlphaTest) discard;
  frag = c;
}
