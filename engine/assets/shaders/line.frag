#version 330 core
uniform vec3 uC; out vec4 frag;
void main() { frag = vec4(uC, 1.0); }
