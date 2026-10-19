#version 450
#extension GL_GOOGLE_include_directive : require
// Vulkan chunk shading: Vulkan-style declarations, then the shading shared with the OpenGL
// program (../chunk_shading.glsl, compiled in at build time).
#include "frame.glsl"
// Wafer ground pulse: LITHO_PULSE_PUSH tells ../chunk_shading.glsl that the
// pulse uniforms come from the push-constant block (below) and the Frame
// block (uGroundPulseTime/uGroundPulseOn in frame.glsl), not from plain uniforms.
#define LITHO_PULSE_PUSH 1
#define LITHO_TRACE_PUSH 1
// The shared shading reads uGroundPulseTime/uGroundPulseOn and
// uGroundTraceTime/uGroundTraceOn; on Vulkan they live in the Frame
// block's uPulse/uTrace vec4s (frame.glsl), so map the names onto them.
#define uGroundPulseTime uPulse.x
#define uGroundPulseOn uPulse.y
#define uGroundTraceTime uTrace.x
#define uGroundTraceOn uTrace.y
layout(push_constant) uniform PulsePush { vec3 uOffset; vec4 uPulseP0; vec4 uPulseP1; vec4 uTraceP0; vec4 uTraceP1; };
layout(set = 0, binding = 1) uniform sampler2DArray uTex;
layout(set = 0, binding = 2) uniform sampler2D uPal;
layout(location = 0) in vec3 vWorld; layout(location = 1) in vec3 vNormal;
layout(location = 2) in float vAO; layout(location = 3) in vec2 vUV;
layout(location = 4) flat in int vLayer; layout(location = 5) flat in int vVal;
layout(location = 6) flat in int vMode; layout(location = 7) flat in int vRot;
layout(location = 0) out vec4 frag;
#include "chunk_shading.glsl"
