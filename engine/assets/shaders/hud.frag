#version 330 core
in vec2 vUV; in vec4 vCol; uniform sampler2D uFont; out vec4 frag;
void main() {
  float a = vUV.x < 0.0 ? 1.0 : texture(uFont, vUV).r;   // u < 0: solid rectangle
  frag = vec4(vCol.rgb, vCol.a * a);
}
