#version 330 core
// Chunk vertices: 12-byte PackedVertex (see render/renderer.h). Base pack shader; packs may
// override it with assets/packs/<pack>/shaders/chunk.vert.
layout(location = 0) in uvec3 aPos;    // 1/16 block units, local to the sub-chunk
layout(location = 1) in uvec4 aInfo;   // u, v (1/16), face | ao<<3 | rot<<5, layer
layout(location = 2) in uvec2 aShade;  // value, mode
uniform mat4 uVP;
uniform vec3 uOffset;
out vec3 vWorld; out vec3 vNormal; out float vAO; out vec2 vUV;
flat out int vLayer; flat out int vVal; flat out int vMode; flat out int vRot;
const vec3 N[6] = vec3[6](vec3(1,0,0), vec3(-1,0,0), vec3(0,1,0), vec3(0,-1,0), vec3(0,0,1), vec3(0,0,-1));
void main() {
  vWorld = vec3(aPos) / 16.0 + uOffset;
  vNormal = N[aInfo.z & 7u];
  vAO = float((aInfo.z >> 3u) & 3u) / 3.0;
  vRot = int((aInfo.z >> 5u) & 3u);
  vUV = vec2(aInfo.xy) / 16.0;
  vLayer = int(aInfo.w); vVal = int(aShade.x); vMode = int(aShade.y);
  gl_Position = uVP * vec4(vWorld, 1.0);
}
