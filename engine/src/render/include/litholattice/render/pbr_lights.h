// Many-lights hybrid (PBR phase 3): K-nearest analytic lights for specular.
//
// WHAT: AnalyticLight (position + linear RGB emission + intensity + distance) plus
//   LightCollector, which reduces an arbitrary set of emissive sources to the K nearest
//   for one shaded position, sorted nearest-first. Also an irradiance helper mapping
//   the existing 1-64 flood-fill light level + sky to normalized diffuse/ambient
//   irradiance, and an attenuated-radiance helper producing the lightColor argument
//   CookTorrance::shade expects.
// WHY: JD's locked many-lights hybrid: the 1-64 light-level flood fill handles diffuse
//   for unlimited torches; the K nearest emissive blocks become analytic lights for the
//   PBR specular term. NO full global illumination.
// HOW: renderer-decoupled, pure data + logic, no renderer dependency. The collector
//   takes a generic forEachEmissive callable (position + rgb + intensity per source),
//   so the software renderer, a future Vulkan path, or tests can all supply emissive
//   sources without depending on world internals. The convenience adapter
//   filterCandidatesViaLighting reads from ILighting where that data exists.
//
// RESOLVED (PBR phase 2): ChunkEmissiveProvider (pbr_emissive_provider.h) is the
//   world-side forEachEmissive - a cached chunk-material scan with explicit refresh
//   (scan-vs-retained decision documented there). The adapter above now has a real
//   candidate source to filter; the "caller supplies positions" fallback remains valid
//   for tools/tests that enumerate sources themselves.
// OPEN QUESTION (no engine changes invented here): neither ILighting nor LightEngine
//   can enumerate emissive sources. LightEngine::addSource feeds the BFS and retains
//   no source list (there is no enumeration API), and ILighting::lightAt returns the
//   post-flood-fill level, not source identity. So today the caller must supply
//   candidate positions (e.g. the software renderer scanning emissive chunks, or the
//   game tracking placed torches); the adapter only *filters* those candidates through
//   the fill. A world-side forEachEmissive (chunk scan or retained source list) is
//   needed before the adapter can run without caller-supplied positions.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "litholattice/world/lighting.h"

namespace litholattice {

/// One emissive block as an analytic point light. The distance field is filled in by
/// LightCollector (Euclidean distance from the shaded position, in blocks).
struct AnalyticLight {
  float x = 0.f, y = 0.f, z = 0.f;  ///< world-space position (caller-supplied convention)
  float r = 0.f, g = 0.f, b = 0.f;  ///< linear-space emission colour
  float intensity = 0.f;            ///< emission multiplier (e.g. torch level scaling)
  float distance = 0.f;             ///< set by collect(); blocks
};

/// K-nearest emissive-source reduction for one shaded position. Stateless except K.
class LightCollector {
 public:
  /// maxLights = K: how many nearest emissive blocks become analytic lights.
  /// Default 8 per JD's locked decision; configurable per renderer.
  explicit LightCollector(size_t maxLights = 8) : maxLights_(maxLights) {}

  size_t maxLights() const { return maxLights_; }
  void setMaxLights(size_t k) { maxLights_ = k; }

  /// Reduce emissive sources to the K nearest to (px, py, pz), sorted nearest-first.
  /// Ties resolve in enumeration order (first-enumerated wins: stable).
  ///
  /// forEachEmissive is invoked as
  ///   sink(float x, float y, float z, float r, float g, float b, float intensity)
  /// once per candidate source. Sources with intensity <= 0 are ignored.
  /// Fill `out` with the K nearest sources (no allocation if capacity suffices).
  /// Fix 2026-10-11 (audit): the per-pixel PBR path was allocating a vector per
  /// fragment; callers now pass a scratch vector (e.g. a mutable context member).
  template <typename ForEachEmissive>
  void collectInto(float px, float py, float pz, ForEachEmissive&& forEachEmissive,
                   std::vector<AnalyticLight>& out) const {
    out.clear();
    if (maxLights_ == 0) return;
    out.reserve(maxLights_ + 1);
    std::vector<AnalyticLight>& heap = out;  // max-heap on squared distance; front is worst
    // push_heap/pop_heap put the "greatest" element at front per this comparator, so
    // define "greatest" as the worst candidate: largest squared distance. Strictly
    // greater only, so equal-distance incumbents (enumerated earlier) always survive
    // newcomers: that is what makes ties stable.
    auto worse = [](const AnalyticLight& a, const AnalyticLight& b) {
      return a.distance < b.distance;
    };
    forEachEmissive([&](float x, float y, float z, float r, float g, float b,
                        float intensity) {
      if (intensity <= 0.f) return;
      const float dx = x - px, dy = y - py, dz = z - pz;
      AnalyticLight cand{x, y, z, r, g, b, intensity, dx * dx + dy * dy + dz * dz};
      if (heap.size() < maxLights_) {
        heap.push_back(cand);
        std::push_heap(heap.begin(), heap.end(), worse);
      } else if (cand.distance < heap.front().distance) {
        std::pop_heap(heap.begin(), heap.end(), worse);
        heap.back() = cand;
        std::push_heap(heap.begin(), heap.end(), worse);
      }
    });
    for (auto& l : heap) l.distance = std::sqrt(l.distance);
    std::stable_sort(heap.begin(), heap.end(),
                     [](const AnalyticLight& a, const AnalyticLight& b) {
                       return a.distance < b.distance;
                     });
  }

  template <typename ForEachEmissive>
  std::vector<AnalyticLight> collect(float px, float py, float pz,
                                     ForEachEmissive&& forEachEmissive) const {
    std::vector<AnalyticLight> out;
    collectInto(px, py, pz, std::forward<ForEachEmissive>(forEachEmissive), out);
    return out;
  }

 private:
  size_t maxLights_;
};

/// Normalized irradiance from the flood-fill light level (0-64) and sky (0-64).
struct Irradiance {
  float diffuse = 0.f;  ///< flood-fill-driven term, 0..1
  float ambient = 0.f;  ///< sky-driven hemisphere term, 0..1
};

/// Ambient computation mode (JD-locked 2026-10-11, cvar `render.ambient_mode`).
/// Fraction is the default: one multiply per shaded point. NormalizedSun is the
/// direction-aware hemisphere variant (more accurate, costs a normalize).
enum class AmbientMode : uint8_t {
  Fraction = 0,      ///< ambient = skyLevel / 64 (pure sky constant, no direction)
  NormalizedSun = 1  ///< ambient = sky/64 * (0.5 + 0.5*dot(n, sunDir/|sunDir|))
};

/// Map the 1-64 flood-fill world onto normalized irradiance.
///   diffuse = max(blockLevel, skyLevel) / 64
///   ambient = skyLevel / 64            (Fraction)
///   ambient = sky/64 * hemisphere(n)   (NormalizedSun)
/// Rationale: the block channel of the fill already carries propagated sky light, but
/// taking the max guards against partial/stale fills so a pixel in full daylight is
/// always exactly 1.0; ambient is a pure sky constant (hemisphere fill, no direction).
/// Sanity: level 0 -> 0.0, level 64 -> 1.0, sky-only (block 0, sky 64) -> 1.0/1.0.
inline Irradiance irradianceFromLightLevel(uint8_t blockLevel, uint8_t skyLevel) {
  Irradiance out;
  out.diffuse = static_cast<float>(blockLevel > skyLevel ? blockLevel : skyLevel) /
                static_cast<float>(kMaxLight);
  out.ambient = static_cast<float>(skyLevel) / static_cast<float>(kMaxLight);
  return out;
}

/// Mode-aware overload. For NormalizedSun the caller passes the (not necessarily
/// unit) sun direction and the surface normal; the hemisphere term keeps the
/// 0..1 contract (dot in [-1,1] -> 0.5+0.5*dot in [0,1]).
inline Irradiance irradianceFromLightLevel(uint8_t blockLevel, uint8_t skyLevel,
                                           AmbientMode mode, float nx, float ny, float nz,
                                           float sunX, float sunY, float sunZ) {
  Irradiance out = irradianceFromLightLevel(blockLevel, skyLevel);
  if (mode == AmbientMode::NormalizedSun) {
    const float len = std::sqrt(sunX * sunX + sunY * sunY + sunZ * sunZ);
    if (len > 1e-6f) {
      const float d = (nx * sunX + ny * sunY + nz * sunZ) / len;
      const float hemi = 0.5f + 0.5f * (d < -1.f ? -1.f : (d > 1.f ? 1.f : d));
      out.ambient *= hemi;
    }
  }
  return out;
}

/// Incoming radiance of one analytic light for CookTorrance::shade's lightColor
/// argument: rgb * intensity * (1 / (1 + d^2)). Inverse-square falloff with a +1
/// softener so the light never blows up at distance 0; an approximation documented
/// as such (JD's locked design: these lights drive specular only, never diffuse).
inline std::array<float, 3> attenuatedRadiance(const AnalyticLight& light) {
  const float att = 1.f / (1.f + light.distance * light.distance);
  const float s = light.intensity * att;
  return {light.r * s, light.g * s, light.b * s};
}

/// Convenience adapter: wrap a caller-supplied candidate enumerator with an ILighting
/// visibility filter. Candidates are block-space positions with a colour and intensity;
/// a candidate survives only if the flood fill reports light at its block
/// (lightAt > 0). Block coordinates are floor(x), floor(y), floor(z). Usage:
///   collector.collect(px, py, pz, filterCandidatesViaLighting(lighting, dim, candidates));
///
/// This does NOT discover emissive sources: the caller enumerates them. See the
/// header's OPEN QUESTION.
template <typename ForEachCandidate>
auto filterCandidatesViaLighting(const ILighting& lighting, uint8_t dim,
                                 ForEachCandidate&& candidates) {
  return [&](auto&& sink) {
    candidates([&](float x, float y, float z, float r, float g, float b,
                   float intensity) {
      const int bx = static_cast<int>(std::floor(x));
      const int by = static_cast<int>(std::floor(y));
      const int bz = static_cast<int>(std::floor(z));
      if (lighting.lightAt(bx, by, bz, dim) > 0) {
        sink(x, y, z, r, g, b, intensity);
      }
    });
  };
}

}  // namespace litholattice
