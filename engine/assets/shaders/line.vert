#version 330 core
// Block highlight outline.
layout(location = 0) in vec3 aP;
uniform mat4 uM;
void main() { gl_Position = uM * vec4(aP, 1.0); }
