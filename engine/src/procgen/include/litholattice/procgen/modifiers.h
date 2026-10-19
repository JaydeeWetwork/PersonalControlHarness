// Noise modifiers: fractal sums (FBm, ridged multifractal, billow, turbulence) and domain warp.
// Each wraps other noises and is itself an INoise, so they compose (warped FBm of Worley, ...).
#pragma once
#include <memory>
#include <optional>
#include <string_view>
#include "litholattice/procgen/noise.h"

namespace litholattice::procgen {

enum class FractalType { FBm, Ridged, Billow, Turbulence, PingPong };

struct FractalSettings {
  FractalType type = FractalType::FBm;
  int octaves = 4;           ///< 1..16
  float lacunarity = 2.f;    ///< frequency multiplier per octave
  float gain = 0.5f;         ///< amplitude multiplier per octave
};

/// Sum of `octaves` copies of `source` at rising frequency, each octave offset by a fixed
/// seed-derived shift so octaves do not line up at the origin. Output normalized by the summed
/// amplitudes: FBm and billow stay in [-1, 1]; ridged, turbulence and ping-pong in
/// [0, 1]. Expects a [-1, 1] source.
///
/// Ping-Pong (2026-10-13, clean-room, implementation-defined): each octave's sample n
/// is folded by a triangular ping-pong wave, pp(n) = tri((n + 1) * 1.5) where tri(v)
/// is the period-2 triangle wave tri(v) = 1 - |1 - (v mod 2)| in [0, 1] -- the range
/// is folded TWICE across [-1, 1] (pp(-1) = 0, pp(-1/3) = 1, pp(1/3) = 0, pp(1) = 1),
/// so successive octaves interleave ridges instead of stacking them like Ridged's
/// single fold.
std::unique_ptr<INoise> createFractal(std::shared_ptr<const INoise> source, const FractalSettings& settings,
                                      uint64_t seed);

/// Domain warp: samples `source` at p + amplitude * (warpX(p * frequency), warpY(...), ...), each
/// axis offset taken from `warp` at a different fixed shift. Range = source's range.
std::unique_ptr<INoise> createDomainWarp(std::shared_ptr<const INoise> source, std::shared_ptr<const INoise> warp,
                                         float amplitude, float frequency);

/// Curl noise (2026-10-13, clean-room): a divergence-free vector field derived from
/// noise potentials by finite differences -- the field has no sinks or sources, so it
/// advects without clumping (carver steering, drift, flow effects). 2D: for a scalar
/// potential psi, curl = (d psi/dy, -d psi/dx). 3D: for a vector potential
/// Psi = (px, py, pz), curl = nabla x Psi. All differences are central with step eps;
/// the result is a pure function of the potentials and the position.
struct CurlVec2 { float x, y; };
struct CurlVec3 { float x, y, z; };
CurlVec2 curlNoise2(const INoise& potential, float x, float y, float eps = 0.01f);
CurlVec3 curlNoise3(const INoise& potX, const INoise& potY, const INoise& potZ, float x, float y, float z,
                    float eps = 0.01f);

std::string_view toString(FractalType t);
std::optional<FractalType> parseFractalType(std::string_view s);

}  // namespace litholattice::procgen
