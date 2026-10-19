#version 450
#extension GL_GOOGLE_include_directive : require
// Vulkan chunk vertices: same inputs and outputs as ../chunk.vert (12-byte PackedVertex), with the
// frame values in a uniform block and the sub-chunk offset as a push constant. Clip space is
// converted from OpenGL conventions (y up, z in -w..w) to Vulkan (y down, z in 0..w).
// The 16-bit position is read as xy + z (R16G16 + R16 are guaranteed vertex formats; R16G16B16
// is optional in Vulkan).
#include "frame.glsl"
layout(push_constant) uniform PulsePush { vec3 uOffset; vec4 uPulseP0; vec4 uPulseP1; vec4 uTraceP0; vec4 uTraceP1; };
layout(location = 0) in uvec2 aPosXY;  // 1/16 block units, local to the sub-chunk
layout(location = 3) in uint aPosZ;
layout(location = 1) in uvec4 aInfo;   // u, v (1/16), face | ao<<3 | rot<<5, layer
layout(location = 2) in uvec2 aShade;  // value, mode
layout(location = 0) out vec3 vWorld; layout(location = 1) out vec3 vNormal;
layout(location = 2) out float vAO; layout(location = 3) out vec2 vUV;
layout(location = 4) flat out int vLayer; layout(location = 5) flat out int vVal;
layout(location = 6) flat out int vMode; layout(location = 7) flat out int vRot;
const vec3 N[6] = vec3[6](vec3(1,0,0), vec3(-1,0,0), vec3(0,1,0), vec3(0,-1,0), vec3(0,0,1), vec3(0,0,-1));
void main() {
  vWorld = vec3(uvec3(aPosXY, aPosZ)) / 16.0 + uOffset;
  vNormal = N[aInfo.z & 7u];
  vAO = float((aInfo.z >> 3u) & 3u) / 3.0;
  vRot = int((aInfo.z >> 5u) & 3u);
  vUV = vec2(aInfo.xy) / 16.0;
  vLayer = int(aInfo.w); vVal = int(aShade.x); vMode = int(aShade.y);
  vec4 p = uVP * vec4(vWorld, 1.0);
  gl_Position = vec4(p.x, -p.y, (p.z + p.w) * 0.5, p.w);
}
