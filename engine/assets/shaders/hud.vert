#version 330 core
// HUD: pixel-space quads (rects and font glyphs).
layout(location = 0) in vec2 aPos; layout(location = 1) in vec2 aUV; layout(location = 2) in vec4 aCol;
uniform vec2 uScreen;
out vec2 vUV; out vec4 vCol;
void main() {
  vUV = aUV; vCol = aCol;
  gl_Position = vec4(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0, 0.0, 1.0);
}
