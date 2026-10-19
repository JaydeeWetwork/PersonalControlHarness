// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

// Self-test for the Litho Formula core sampler. Conventions follow
// procgen_selftest.cpp: a local Checker printing "  [PASS]/[FAIL]" lines.
// All expectations are derived by hand from the mathematical definition;
// no third-party reference values are used.
#include "litholattice/procgen/litho_formula.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>

namespace litholattice::procgen::litho_formula {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

constexpr double kPi = 3.14159265358979323846;

/// The closed-form anchor: a=b=1, m=4, n1=n2=n3=2 -> r(phi)=1 for all phi.
LithoParams circleParams() {
  LithoParams p;
  p.a = 1.0;
  p.b = 1.0;
  p.m = 4.0;
  p.n1 = 2.0;
  p.n2 = 2.0;
  p.n3 = 2.0;
  return p;
}

bool finiteVec3(const Vec3& v) {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

}  // namespace

int runLithoFormulaSelfTest() {
  Checker check;

  // --- 1. Circle anchor: r(phi) == 1 within 1e-9 across [0, 2pi) -------------
  {
    const LithoParams c = circleParams();
    double maxDev = 0.0;
    for (int i = 0; i < 720; ++i) {
      const double phi = (2.0 * kPi * i) / 720.0;
      maxDev = std::max(maxDev, std::fabs(lithoR(phi, c) - 1.0));
    }
    std::printf("    circle: max |r-1| over 720 angles = %.3e\n", maxDev);
    check(maxDev <= 1e-9, "circle anchor: a=b=1,m=4,n1=n2=n3=2 -> r(phi)=1 within 1e-9");
  }

  // --- 2. m-fold rotational symmetry -----------------------------------------
  // r(phi + 2*pi*k/m) re-evaluates the formula at argument m*phi/4 + k*pi/2;
  // odd k swaps the |cos|/|sin| terms, so the test params use a==b and n2==n3
  // (the swap then preserves the sum). Even k is exact for any params.
  {
    const double phis[] = {0.0, 0.3, 0.7, 1.1, 2.2, 3.7, 5.9};
    double maxDev = 0.0;
    for (const double m : {3.0, 5.0, 6.0}) {
      LithoParams p;
      p.a = 1.5;
      p.b = 1.5;
      p.m = m;
      p.n1 = 0.7;
      p.n2 = 1.3;
      p.n3 = 1.3;
      for (const double phi : phis) {
        const double r0 = lithoR(phi, p);
        for (int k = 1; k <= 3; ++k)
          maxDev = std::max(maxDev, std::fabs(lithoR(phi + 2.0 * kPi * k / m, p) - r0));
      }
    }
    std::printf("    symmetry: max |r(phi+2*pi*k/m)-r(phi)| = %.3e\n", maxDev);
    check(maxDev <= 1e-9, "m-fold symmetry: m in {3,5,6}, k in 1..3 within 1e-9");
  }

  // --- 3. Hand-computed spot checks ------------------------------------------
  {
    const LithoParams c = circleParams();
    const double r0 = lithoR(0.0, c);        // |1|^2 + |0|^2 = 1 -> 1^(-1/2) = 1
    const double rq = lithoR(kPi / 4.0, c);  // m*phi/4 = pi/4: cos=sin=sqrt(2)/2
                                            // -> (0.5+0.5)^(-1/2) = 1
    const double rh = lithoR(kPi / 2.0, c);  // |~0|^2 + |1|^2 = 1 -> 1
    std::printf("    spots: r(0)=%.17g r(pi/4)=%.17g r(pi/2)=%.17g\n", r0, rq, rh);
    check(std::fabs(r0 - 1.0) <= 1e-12, "spot: lithoR(0, circle) == 1");
    check(std::fabs(rq - 1.0) <= 1e-12, "spot: lithoR(pi/4, circle) == 1 (hand: (0.5+0.5)^(-1/2))");
    check(std::fabs(rh - 1.0) <= 1e-9, "spot: lithoR(pi/2, circle) == 1");
    // 3D axis pins for the sphere case (r1 = r2 = 1).
    const Vec3 px = lithoPoint(0.0, 0.0, c, c);
    const Vec3 py = lithoPoint(kPi / 2.0, 0.0, c, c);
    const Vec3 pz = lithoPoint(0.0, kPi / 2.0, c, c);
    const auto near = [](double a, double b) { return std::fabs(a - b) <= 1e-9; };
    check(near(px.x, 1.0) && near(px.y, 0.0) && near(px.z, 0.0), "spot: lithoPoint(0,0) == (1,0,0)");
    check(near(py.x, 0.0) && near(py.y, 1.0) && near(py.z, 0.0), "spot: lithoPoint(pi/2,0) == (0,1,0)");
    check(near(pz.x, 0.0) && near(pz.y, 0.0) && near(pz.z, 1.0), "spot: lithoPoint(0,pi/2) == (0,0,1)");
  }

  // --- 4. 3D sphere case: |lithoPoint| == 1 over a (theta, phi) grid ---------
  {
    const LithoParams c = circleParams();
    double maxDev = 0.0;
    for (int i = 0; i < 13; ++i) {
      const double theta = -kPi + (2.0 * kPi * i) / 12.0;  // [-pi, pi]
      for (int j = 0; j < 9; ++j) {
        const double phi = -kPi / 2.0 + (kPi * j) / 8.0;  // [-pi/2, pi/2]
        const Vec3 v = lithoPoint(theta, phi, c, c);
        const double n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        maxDev = std::max(maxDev, std::fabs(n - 1.0));
      }
    }
    std::printf("    sphere: max ||p|-1| over 13x9 grid = %.3e\n", maxDev);
    check(maxDev <= 1e-9, "3D sphere: circle params for both factors -> |p| == 1 within 1e-9");
  }

  // --- 5. Degenerate sweep: finite output, no NaN/inf --------------------------
  {
    const double phis[] = {0.0, 0.7, -2.1, 3.14159};
    const double n1s[] = {1e-9, -1e-9, 0.0};
    const double exps[] = {1e9, -1e9};
    bool ok = true;
    for (const double n1 : n1s)
      for (const double m : {0.0, 4.0})
        for (const double phi : phis) {
          LithoParams p;
          p.m = m;
          p.n1 = n1;
          ok = ok && std::isfinite(lithoR(phi, p));
        }
    for (const double n1 : n1s)
      for (const double e2 : exps)
        for (const double e3 : exps) {
          LithoParams p;
          p.m = 4.0;
          p.n1 = n1;
          p.n2 = e2;
          p.n3 = e3;
          ok = ok && std::isfinite(lithoR(0.7, p));
        }
    {  // zero denominators must not produce 0/0
      LithoParams p;
      p.a = 0.0;
      p.b = 0.0;
      p.m = 4.0;
      for (const double phi : phis) ok = ok && std::isfinite(lithoR(phi, p));
    }
    {  // non-finite inputs collapse gracefully
      const LithoParams p;
      ok = ok && std::isfinite(lithoR(std::numeric_limits<double>::quiet_NaN(), p));
      ok = ok && std::isfinite(lithoR(std::numeric_limits<double>::infinity(), p));
      LithoParams bad;
      bad.m = std::numeric_limits<double>::quiet_NaN();
      ok = ok && std::isfinite(lithoR(0.5, bad));
      bad = LithoParams();
      bad.n1 = std::numeric_limits<double>::quiet_NaN();
      ok = ok && std::isfinite(lithoR(0.5, bad));
    }
    check(ok, "degenerate: n1 in {1e-9,-1e-9,0}, m=0, extreme exponents, a=b=0, NaN/Inf -> finite");

    bool ok3 = true;
    LithoParams d;
    d.m = 4.0;
    d.n1 = 0.0;
    for (const double th : phis)
      for (const double ph : phis) ok3 = ok3 && finiteVec3(lithoPoint(th, ph, d, d));
    const Vec3 o1 = lithoPoint(std::numeric_limits<double>::quiet_NaN(), 0.5, d, d);
    const Vec3 o2 = lithoPoint(0.1, std::numeric_limits<double>::infinity(), d, d);
    ok3 = ok3 && o1.x == 0.0 && o1.y == 0.0 && o1.z == 0.0 && o2.x == 0.0 && o2.y == 0.0 &&
          o2.z == 0.0;
    check(ok3, "degenerate 3D: finite components; NaN/Inf angles -> origin");

    // m=0 yields a constant radius: a=2,b=3,m=0,n1=1,n2=n3=2
    // -> angle 0 -> (|1/2|^2 + 0)^(-1/1) = 4 for every phi.
    LithoParams z;
    z.a = 2.0;
    z.b = 3.0;
    z.m = 0.0;
    z.n1 = 1.0;
    z.n2 = 2.0;
    z.n3 = 2.0;
    bool const0 = true;
    for (const double phi : phis) const0 = const0 && std::fabs(lithoR(phi, z) - 4.0) <= 1e-12;
    check(const0, "m=0: constant radius 4 for all phi (hand: (1/4)^(-1))");

    // Determinism: pure functions, bitwise identical on repeat calls.
    const LithoParams q = circleParams();
    check(lithoR(1.234, q) == lithoR(1.234, q), "determinism: lithoR bitwise identical");
    const Vec3 v1 = lithoPoint(1.1, -0.4, q, q), v2 = lithoPoint(1.1, -0.4, q, q);
    check(v1.x == v2.x && v1.y == v2.y && v1.z == v2.z, "determinism: lithoPoint bitwise identical");
  }

  return check.fails();
}

}  // namespace litholattice::procgen::litho_formula
