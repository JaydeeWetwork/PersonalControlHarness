// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

/// Litho Formula core sampler (ProcGen workstream, authorized 2026-10-12).
///
/// The 2D Litho Formula — radius as a function of angle:
///   r(phi) = (|cos(m*phi/4)/a|^n2 + |sin(m*phi/4)/b|^n3|)^(-1/n1)
///
/// The 3D form is the spherical product of two 2D evaluations r1(theta), r2(phi):
///   x = r1(theta)*cos(theta) * r2(phi)*cos(phi)
///   y = r1(theta)*sin(theta) * r2(phi)*cos(phi)
///   z = r2(phi)*sin(phi)
/// with theta in [-pi, pi] (longitude) and phi in [-pi/2, pi/2] (latitude).
///
/// Closed-form anchor: a=b=1, m=4, n1=n2=n3=2 gives r(phi)=1 for all phi
/// (the unit circle); the 3D spherical product of two such evaluators is the
/// unit sphere.
///
/// Pure functions, no engine dependencies. Deterministic: identical inputs
/// produce bitwise-identical outputs on every platform.
///
/// DEGENERATE RAILS — graceful, never NaN/inf, never throws:
///   - |n1| is clamped to >= 1e-6 before the -1/n1 exponent.
///   - |r| is clamped to <= 1e6 (finite cap).
///   - |a| and |b| are floored at 1e-9 so cos/0 can never produce 0/0.
///   - A zero base (both powered terms vanish) would be 0^(-1/n1) = +inf;
///     it returns the finite cap instead.
///   - Any non-finite input (angle or parameter) collapses the radius to 0
///     and a 3D point to the origin.
/// m == 0 needs no special case: cos(0)=1, sin(0)=0, so it yields a constant
/// radius for every phi (covered by the self-test).
#pragma once

namespace litholattice::procgen::litho_formula {

/// One 2D Litho Formula evaluator's parameters.
struct LithoParams {
  double a = 1.0;
  double b = 1.0;
  double m = 0.0;
  double n1 = 1.0;
  double n2 = 1.0;
  double n3 = 1.0;
};

/// Plain 3D point (the sampler has no engine vector dependency).
struct Vec3 {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

/// 2D Litho Formula radius at angle phi. Always finite; see DEGENERATE RAILS.
double lithoR(double phi, const LithoParams& p);

/// 3D point via the spherical product of two 2D evaluations: pTheta drives
/// the longitude factor r1(theta), pPhi the latitude factor r2(phi).
/// Components are always finite; see DEGENERATE RAILS.
Vec3 lithoPoint(double theta, double phi, const LithoParams& pTheta, const LithoParams& pPhi);

/// Self-test entry point (litho_formula_selftest.cpp). Returns failure count.
int runLithoFormulaSelfTest();
/// Self-test for the `lithoform` graph node (litho_formula_node_selftest.cpp).
int runLithoFormulaNodeSelfTest();
/// Self-test for the "lithoform_stamp" worldgen feature (litho_stamp_feature_selftest.cpp).
int runLithoStampFeatureSelfTest();

}  // namespace litholattice::procgen::litho_formula
