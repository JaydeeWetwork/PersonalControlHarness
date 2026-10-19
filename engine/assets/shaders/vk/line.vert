#version 450
// Vulkan lines (block highlight outline, crosshair): OpenGL-convention matrix, converted clip space.
layout(push_constant) uniform Push { mat4 uM; vec4 uC; };
layout(location = 0) in vec3 aP;
void main() {
  vec4 p = uM * vec4(aP, 1.0);
  gl_Position = vec4(p.x, -p.y, (p.z + p.w) * 0.5, p.w);
}
