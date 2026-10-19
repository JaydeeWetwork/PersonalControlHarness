// PBR material registry (phase 1): data-driven physical material parameters.
//
// WHAT: MaterialDef (albedo/roughness/metalness/emissive/IOR/transparency) plus
//   MaterialRegistry (string ID <-> compact 16-bit global ID) and ChunkPalette
//   (per-chunk 8-bit voxel index -> 16-bit global ID). The mesher and shaders never
//   know material properties: voxels carry only an 8-bit palette index
//   (BinaryQuad::material is that index, passthrough); the chunk palette resolves it
//   to a global registry ID, and the registry holds the physical parameters.
// WHY: JD's locked rule - materials are DATA, never hardcoded. String IDs
//   ("lithokraft:iron") are the stable identity: mods register via data files, the
//   engine ships LithoKraft defaults as a data file, and saves persist string IDs.
//   Compact 16-bit IDs keep per-chunk palettes (256 entries x 2 bytes = 512 B)
//   and any ID-indexed uniform/shader tables cheap.
// HOW: register() assigns dense compact IDs in registration order. Re-registering
//   the same string ID with identical params is idempotent (returns the existing
//   ID); conflicting params are a caller error and return kInvalidMaterialId.
//
// Relationship to BlockMaterials (block_materials.h): SEPARATE concerns. BlockMaterials
//   is texture bindings (texture-array layer + UV rect per block face, animations,
//   palette rows); this registry is physical PBR parameters. A block may reference
//   both: BlockMaterials says *what it looks like on the texture array*, the
//   MaterialRegistry says *how light interacts with it*.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "litholattice/render/transparency_method.h"

namespace litholattice {

/// Physical (PBR) parameters for one material, as loaded from data files.
struct MaterialDef {
  std::array<float, 3> albedo{1.f, 1.f, 1.f};  ///< linear-space base colour
  std::string albedoTexture;                   ///< texture/layer ref into the texture registry (empty = flat colour)
  float roughness = 0.9f;                      ///< 0 = mirror, 1 = fully diffuse
  float metalness = 0.0f;                      ///< 0 = dielectric, 1 = metal
  std::array<float, 3> emissive{0.f, 0.f, 0.f};  ///< linear-space emission colour
  float emissiveIntensity = 0.0f;              ///< emission multiplier (0 = no emission)
  float ior = 1.5f;                            ///< index of refraction (dielectrics; ~1.5 glass-adjacent default)
  float alpha = 1.0f;                          ///< 0..1; < 1 means transparent, rendered per `transparency`
  TransparencyMethod transparency = TransparencyMethod::ScreenDoor;  ///< blend strategy when not opaque
  /// Helper: opaque materials skip all transparency paths (sorting, dither, OIT, blending).
  bool opaque() const { return alpha >= 1.0f; }

  bool operator==(const MaterialDef& o) const {
    return albedo == o.albedo && albedoTexture == o.albedoTexture && roughness == o.roughness &&
           metalness == o.metalness && emissive == o.emissive &&
           emissiveIntensity == o.emissiveIntensity && ior == o.ior && alpha == o.alpha &&
           transparency == o.transparency;
  }
  bool operator!=(const MaterialDef& o) const { return !(*this == o); }
};

/// Data-driven registry: string ID <-> compact 16-bit global material ID.
/// Not thread-safe; build during startup (engine loads the defaults data file,
/// mods register from their data files) then share read-only.
class MaterialRegistry {
 public:
  static constexpr uint16_t kInvalidMaterialId = 0xFFFF;  ///< returned for unknown IDs / failed registration
  static constexpr size_t kMaxMaterials = kInvalidMaterialId;  ///< 0xFFFF is reserved as the invalid sentinel

  /// Registers `id` with `def`, returning the compact ID. Idempotent: re-registering an
  /// identical def returns the existing ID. Conflicting params are an error: returns
  /// kInvalidMaterialId (the original registration is kept). IDs must be non-empty.
  uint16_t registerMaterial(std::string id, const MaterialDef& def);
  /// Compact ID for a string ID, or kInvalidMaterialId if unregistered.
  uint16_t idFor(std::string_view id) const;
  /// Def for a compact ID, or nullptr if unregistered/out of range.
  const MaterialDef* defFor(uint16_t id) const;
  /// String ID for a compact ID (for saves, which persist string IDs), or nullptr.
  const std::string* stringIdFor(uint16_t id) const;
  size_t size() const { return ids_.size(); }

  /// Generation counter: bumped on every successful new registration, never on the
  /// idempotent or conflicting paths (those change nothing). Render backends compare
  /// this to decide when the uploaded material table is stale (phase 2: the GL
  /// backend's setPbrMaterials re-uploads only when the generation moved) - a cheap
  /// dirty flag with no caller bookkeeping, sufficient because registered defs are
  /// never mutated in place (conflicting re-registration is an error, original kept).
  uint64_t generation() const { return generation_; }

  /// Loads a materials data file (JSON; see assets/materials/lithokraft.materials for the
  /// documented format). Registers every material; returns false (and sets err) if the file
  /// cannot be read/parsed or any registration conflicts.
  bool loadFile(const std::string& path, std::string* err = nullptr);
  /// Same as loadFile, from already-read text (used by tests and by callers that embed the file).
  bool loadText(const std::string& text, std::string* err = nullptr);

 private:
  std::vector<std::string> ids_;        // compact ID -> string ID (dense, registration order)
  std::vector<MaterialDef> defs_;       // compact ID -> def
  uint64_t generation_ = 0;             // bumped on every successful new registration
};

/// Built-in fallback material (phase 2): `initMaterialRegistry()` registers this FIRST
/// (compact ID 0), so "unknown string ID on load" and "empty column palette" both have a
/// defined answer instead of garbage. Engine-scoped ID so it never collides with a game's
/// own namespace. Plain matte dielectric.
constexpr std::string_view kDefaultMaterialStringId = "litholattice:default";

/// The built-in default's parameters (see kDefaultMaterialStringId).
MaterialDef defaultMaterialDef();

/// Startup wiring (phase 2, resolves phase-1 open question #1): fills `reg` from the game's
/// material data file. The GAME owns asset-path resolution and passes the resolved path —
/// LithoKraft's main.cpp passes <engine assets>/materials/lithokraft.materials, mirroring how
/// it resolves config/settings.json (GameSettings::load). Call on a fresh registry: the
/// built-in default is registered first, so it lands on compact ID 0.
/// Returns true when the file loaded. On a missing/unreadable/invalid path it returns false
/// and sets `err`, BUT the registry still holds the built-in default, so the caller logs and
/// continues — the engine's init-error convention (degrade loudly, keep running; cf. audio's
/// "no output device; continuing muted"). Tests and headless builds use exactly this path.
bool initMaterialRegistry(MaterialRegistry& reg, const std::string& path, std::string* err = nullptr);

/// Per-chunk palette: the 8-bit voxel palette index stored per voxel (and passthrough in
/// BinaryQuad::material) resolves to a 16-bit global registry ID through this table.
/// A chunk's voxels may reference at most 256 distinct materials, which is the whole point:
/// the palette is built from the chunk's actual material set at mesh time.
class ChunkPalette {
 public:
  static constexpr size_t kCapacity = 256;  ///< one entry per possible 8-bit voxel index

  /// Builds the palette from the chunk's material set in voxel-index order:
  /// voxel index i maps to the global ID of stringIds[i]. Fails (returns false) if there are
  /// more than 256 entries or a string ID is unknown to the registry.
  /// Overflow is an error, never silent first-255-wins (locked 2026-10-10): the 8-bit voxel
  /// index addresses exactly 0..255, so the caller must shrink the chunk's material set or
  /// fail the mesh job rather than silently re-material the overflow voxels.
  bool build(const MaterialRegistry& reg, const std::vector<std::string>& stringIds, std::string* err = nullptr);

  /// Global registry ID for an 8-bit voxel index (i.e. for BinaryQuad::material).
  uint16_t globalId(uint8_t voxelIndex) const { return entries_[voxelIndex]; }
  /// Number of populated entries (voxel indices 0 .. size()-1 are valid).
  size_t size() const { return size_; }
  /// Raw table (for upload to GPU / serialisation helpers).
  const uint16_t* entries() const { return entries_; }

  /// Round-trip helper: the string IDs in voxel-index order, for save files
  /// (saves persist string IDs per JD's locked rule, not compact IDs).
  std::vector<std::string> stringIds(const MaterialRegistry& reg) const;

 private:
  uint16_t entries_[kCapacity]{};
  size_t size_ = 0;
};

}  // namespace litholattice
