// Material-table upload packing for the GPU PBR path (PBR phase 2, Worker 3).
//
// WHAT: the CPU side of the std430 `PbrMaterial` row documented in
//   assets/shaders/pbr_shading.glsl. `packPbrMaterialTable` turns a MaterialRegistry
//   into the dense row array the GL backend uploads (indexed by compact registry ID).
// WHY: the shader's texelFetch math assumes the exact offsets below; this file is
//   the single CPU definition of that layout, shared by the GL backend and the
//   headless packing tests (no GL context needed to verify it).
// HOW: header-only, mirrors the pbr_lights.h pattern. The TRUE std430 layout of the
//   struct (verified against glslang's SPIR-V Offset decorations, 2026-10-11 - the
//   phase-1 comment claiming 64-byte rows was wrong; vec3 is 12 bytes in std430,
//   only its *alignment* is 16):
//
//     floats  0..2 : albedo (linear rgb)       <- vec3 albedo @ byte 0
//     float   3    : roughness                  <- float roughness @ byte 12
//     floats  4..6 : emissive (linear rgb)      <- vec3 emissive @ byte 16
//     float   7    : emissiveIntensity          <- @ byte 28
//     float   8    : metalness                  <- @ byte 32
//     float   9    : ior                        <- @ byte 36
//     float   10   : alpha                      <- @ byte 40
//     float   11   : (pad; struct stride is 48 bytes)
//
// TRANSPORT NOTE (locked small decision): on the GL 3.3 backend the rows travel in a
//   buffer texture (GL 3.1+ core; SSBOs need 4.3 and the vendored loader is 3.3-only).
//   The shader reads row r, float k as texelFetch(uPbrMaterials, r * 12 + k) - the
//   SAME 48-byte rows a Vulkan backend would bind as a real std430 SSBO, so the
//   layout contract in pbr_shading.glsl holds for both transports.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "litholattice/render/pbr_materials.h"

namespace litholattice {

/// One packed material row: the true 48-byte std430 `PbrMaterial` from pbr_shading.glsl.
struct PbrMaterialRow {
  float v[12]{};
};
static_assert(sizeof(PbrMaterialRow) == 48, "PbrMaterialRow is the 48-byte std430 row");
static_assert(offsetof(PbrMaterialRow, v) == 0, "row starts at the float array");

/// Float index of each field inside the row (texelFetch(uPbrMaterials, id * 12 + kPbr*)).
constexpr int kPbrRowFloats = 12;          ///< floats per row (48 bytes)
constexpr int kPbrAlbedo = 0;             ///< v[0..2]
constexpr int kPbrRoughness = 3;          ///< v[3]
constexpr int kPbrEmissive = 4;           ///< v[4..6]
constexpr int kPbrEmissiveIntensity = 7;  ///< v[7]
constexpr int kPbrMetalness = 8;          ///< v[8]
constexpr int kPbrIor = 9;               ///< v[9]
constexpr int kPbrAlpha = 10;            ///< v[10]

/// Packs one registry def into a row. Pure function (no registry needed).
inline void packPbrMaterialRow(const MaterialDef& def, PbrMaterialRow& row) {
  row.v[kPbrAlbedo + 0] = def.albedo[0];
  row.v[kPbrAlbedo + 1] = def.albedo[1];
  row.v[kPbrAlbedo + 2] = def.albedo[2];
  row.v[kPbrRoughness] = def.roughness;
  row.v[kPbrEmissive + 0] = def.emissive[0];
  row.v[kPbrEmissive + 1] = def.emissive[1];
  row.v[kPbrEmissive + 2] = def.emissive[2];
  row.v[kPbrEmissiveIntensity] = def.emissiveIntensity;
  row.v[kPbrMetalness] = def.metalness;
  row.v[kPbrIor] = def.ior;
  row.v[kPbrAlpha] = def.alpha;
}

/// Packs the whole registry: row[i] is the def for compact ID i (dense, like the
/// registry). The GL backend uploads rows.data() as the material table; shader index
/// `vMaterialId` (the compact ID) addresses it directly.
inline void packPbrMaterialTable(const MaterialRegistry& reg, std::vector<PbrMaterialRow>& rows) {
  rows.resize(reg.size());
  for (size_t i = 0; i < reg.size(); ++i) {
    const MaterialDef* def = reg.defFor(static_cast<uint16_t>(i));
    if (def) packPbrMaterialRow(*def, rows[i]);
  }
}

}  // namespace litholattice
