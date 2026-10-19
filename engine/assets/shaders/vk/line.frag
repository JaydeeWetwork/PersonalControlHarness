#version 450
layout(push_constant) uniform Push { mat4 uM; vec4 uC; };
layout(location = 0) out vec4 frag;
void main() { frag = vec4(uC.rgb, 1.0); }
