#version 330 core
// Sprites: atlas texel x vertex tint. uAlphaTest is 0.5 for the cutout pass
// (kShadeCutout: alpha-tested, depth-writing) and 0 for the blended passes
// (kShadeAlphaBlend / kShadeAdditive, whose GL blend state the backend sets
// per run); the discard keeps cutout edges hard under either state.
in vec2 vUV; in vec4 vCol;
uniform sampler2D uAtlas;
uniform float uAlphaTest;
out vec4 frag;
void main() {
  vec4 c = texture(uAtlas, vUV) * vCol;
  if (c.a < uAlphaTest) discard;
  frag = c;
}
