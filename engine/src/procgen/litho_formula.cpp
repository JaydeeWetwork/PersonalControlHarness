// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

// Litho Formula core sampler implementation. Pure functions; only <cmath> and
// <algorithm> — no engine dependencies. Derived solely from the mathematical
// definition in litho_formula.h; no third-party code referenced.
#include "litholattice/procgen/litho_formula.h"

#include <algorithm>
#include <cmath>

namespace litholattice::procgen::litho_formula {
namespace {

// |n1| floor applied before the -1/n1 exponent (never divide by ~zero).
constexpr double kN1Floor = 1e-6;
// |a|/|b| floor: keeps the cos/a, sin/b divisions away from 0/0.
constexpr double kDenomFloor = 1e-9;
// Finite output cap: |r| never exceeds this.
constexpr double kRCap = 1e6;

bool allFinite(double phi, const LithoParams& p) {
  return std::isfinite(phi) && std::isfinite(p.a) && std::isfinite(p.b) &&
         std::isfinite(p.m) && std::isfinite(p.n1) && std::isfinite(p.n2) &&
         std::isfinite(p.n3);
}

}  // namespace

double lithoR(double phi, const LithoParams& p) {
  // Non-finite input: collapse to a point. Graceful, never NaN.
  if (!allFinite(phi, p)) return 0.0;

  const double aSafe = std::max(std::fabs(p.a), kDenomFloor);
  const double bSafe = std::max(std::fabs(p.b), kDenomFloor);
  const double angle = p.m * phi * 0.25;  // m*phi/4
  const double t1 = std::fabs(std::cos(angle)) / aSafe;  // |cos(m*phi/4)/a|
  const double t2 = std::fabs(std::sin(angle)) / bSafe;  // |sin(m*phi/4)/b|
  // t1, t2 >= 0, so the pows are >= 0 (pow(0, negative) -> +inf is fine here).
  const double base = std::pow(t1, p.n2) + std::pow(t2, p.n3);
  if (!(base > 0.0)) {
    // Both terms vanished: base^(-1/n1) would be 0^(negative) = +inf.
    // Return the finite cap instead.
    return kRCap;
  }
  // Clamp |n1| before the exponent; keep its sign (negative n1 is meaningful).
  const double n1c = std::copysign(std::max(std::fabs(p.n1), kN1Floor), p.n1);
  double r = std::pow(base, -1.0 / n1c);
  if (!std::isfinite(r)) r = kRCap;  // e.g. base<1 raised to a huge negative power
  return r > kRCap ? kRCap : r;      // r >= 0 by construction
}

Vec3 lithoPoint(double theta, double phi, const LithoParams& pTheta, const LithoParams& pPhi) {
  // Non-finite angles: collapse to the origin (lithoR would return 0 for them
  // anyway, but cos/sin of NaN is NaN — guard explicitly).
  if (!std::isfinite(theta) || !std::isfinite(phi)) return Vec3{};
  const double r1 = lithoR(theta, pTheta);
  const double r2 = lithoR(phi, pPhi);
  const double cosT = std::cos(theta), sinT = std::sin(theta);
  const double cosP = std::cos(phi), sinP = std::sin(phi);
  // r1, r2 are in [0, 1e6]; products stay finite (<= 1e12).
  Vec3 out;
  out.x = r1 * cosT * r2 * cosP;
  out.y = r1 * sinT * r2 * cosP;
  out.z = r2 * sinP;
  return out;
}

}  // namespace litholattice::procgen::litho_formula
