// Senses: occluded sight (a voxel ray; no wallhacks), hearing with occlusion falloff, and
// game-sampled fields (signal strength, heat). Deterministic, read-only on the world.
#pragma once
#include <functional>
#include <vector>
#include "litholattice/ai/nav.h"

namespace litholattice::ai {

struct Vec { float x = 0, y = 0, z = 0; };

struct SightParams { float range = 24.f; float fovCos = 0.f; };  ///< fovCos 0 = 180 degree cone
/// Can an eye at `eye` facing `dir` (unit) see `target`? Blocked by any cell `opaque` says is
/// opaque on the segment (DDA through every crossed cell, endpoints excluded).
bool canSee(const ChunkMap& w, const std::function<bool(BlockId)>& opaque, Vec eye, Vec dir, Vec target, SightParams p = {});
/// Count of opaque cells between a and b (0 = clear line).
int occluders(const ChunkMap& w, const std::function<bool(BlockId)>& opaque, Vec a, Vec b, int stopAt = 8);

struct Sound { Vec pos; float loudness = 16.f; uint32_t kind = 0; };
/// Perceived loudness at `ear` (0 = inaudible): linear falloff over `loudness` blocks, halved
/// for each occluding block.
float hear(const ChunkMap& w, const std::function<bool(BlockId)>& opaque, Vec ear, const Sound& s);

/// Field sense (signal / heat / anything the game samples): strongest value within `radius`
/// cells, sampled on an integer grid; returns value and where.
struct FieldHit { float value = 0; Cell at; };
FieldHit senseField(const std::function<float(int, int, int)>& field, Cell at, int radius);

}  // namespace litholattice::ai
