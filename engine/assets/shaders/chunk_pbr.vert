#version 330 core
// Chunk PBR vertex program (PBR phase 2 - ACTIVE: built as "chunk_pbr" by the gl33
// backend alongside the classic "chunk" program. Selection follows the meshes, not a
// mode flag: this program draws PbrChunkMesh uploads (the BinaryQuad path), the
// classic program draws PackedVertex uploads (see renderer.h).
//
// Vertex layout: PbrQuadVertex (render/pbr_quad_mesh.h), 20 bytes - the lean contract
// the BinaryQuad path can actually provide. aMaterial/aLight are flat per quad
// (merged quads share one material and one light level by construction).
layout(location = 0) in uvec3 aPos;      // 1/16 block units, local to the sub-chunk
layout(location = 1) in uint aFace;      // 0..5: +X,-X,+Y,-Y,+Z,-Z (== BinaryQuad::dir)
layout(location = 2) in uint aMaterial;  // compact MaterialRegistry ID (flat)
layout(location = 3) in uvec2 aLight;    // flood-fill block/sky level 0..64 (flat)
uniform mat4 uVP;
uniform vec3 uOffset;
uniform vec3 uCam;
out vec3 vWorld; out vec3 vNormal; out vec3 vViewDir;
flat out uint vMaterialId; flat out float vBlockLight; flat out float vSkyLight;
const vec3 N[6] = vec3[6](vec3(1,0,0), vec3(-1,0,0), vec3(0,1,0),
                          vec3(0,-1,0), vec3(0,0,1), vec3(0,0,-1));
void main() {
  vWorld = vec3(aPos) / 16.0 + uOffset;
  vNormal = N[aFace & 7u];
  vViewDir = uCam - vWorld;
  vMaterialId = aMaterial;
  vBlockLight = float(aLight.x);
  vSkyLight = float(aLight.y);
  gl_Position = uVP * vec4(vWorld, 1.0);
}
