#version 330 core
// Chunk shading: texture array, palette tints and masks (wire C4 ring, colour lamps, repeater
// pips, memory cells) and animated faces. Base pack shader; a pack may override it.
// The shading itself is shared with the Vulkan program: chunk_shading.glsl.
in vec3 vWorld; in vec3 vNormal; in float vAO; in vec2 vUV;
flat in int vLayer; flat in int vVal; flat in int vMode; flat in int vRot;
uniform sampler2DArray uTex; uniform sampler2D uPal;
uniform vec3 uCam; uniform vec3 uSky; uniform float uFogEnd; uniform float uTime; uniform int uDebug;
// Near-camera model dithering (JD 2026-10-13): enables the guarded block
// at the top of chunk_shading.glsl's main(); band = (near, far) blocks.
#define LITHO_MODEL_DITHER
uniform float uModelDither;     // 1 = on, 0 = off
uniform vec2 uModelDitherBand;  // (near, far) view depth in blocks
// Material bindings come from the block models (resources.json): layer / palette-row indices.
uniform int uTile;
uniform int uWirePal[2]; uniform int uRing[2];
uniform int uLampPal[3]; uniform int uLampMask[3]; uniform int uLampRim[3];
uniform int uRepMask; uniform int uMemMask;
// Animations (mode 10, value = animation index): frame sequence slice + frame time each.
const int MAX_ANIMS = 8;
uniform int uAnimStart[MAX_ANIMS]; uniform int uAnimLen[MAX_ANIMS]; uniform float uAnimMs[MAX_ANIMS];
uniform int uAnimSeq[64];
uniform int uDot[2]; uniform int uDigits;
out vec4 frag;
#include "chunk_shading.glsl"
