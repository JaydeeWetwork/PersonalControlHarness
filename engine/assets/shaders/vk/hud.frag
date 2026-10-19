#version 450
layout(set = 0, binding = 3) uniform sampler2D uFont;
layout(location = 0) in vec2 vUV; layout(location = 1) in vec4 vCol;
layout(location = 0) out vec4 frag;
void main() {
  float a = vUV.x < 0.0 ? 1.0 : texture(uFont, vUV).r;   // u < 0: solid rectangle
  frag = vec4(vCol.rgb, vCol.a * a);
}
