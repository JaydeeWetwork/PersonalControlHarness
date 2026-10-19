// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

/// Self-test for the `lithoform` procgen graph node (wired 2026-10-12, JD-locked).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "litholattice/procgen/graph.h"
#include "litholattice/procgen/litho_formula.h"
#include "litholattice/procgen/litho_formula_voxelize.h"
#include "litholattice/procgen/procgen_selftest.h"

namespace litholattice::procgen::litho_formula {
namespace {

/// Minimal check helper (same pattern as procgen_selftest.cpp's local Checker).
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

constexpr const char* kSphereGraph = R"({
  "nodes": {
    "ball": {"type": "lithoform",
             "theta": {"a": 1, "b": 1, "m": 4, "n1": 2, "n2": 2, "n3": 2},
             "phi": {"a": 1, "b": 1, "m": 4, "n1": 2, "n2": 2, "n3": 2},
             "radius": 8, "falloff": 1, "center": [0, 0, 0]}
  },
  "outputs": {"d": "ball"}
})";

// Unit-sphere params: every direction has surface radius exactly 1
// (a=b=1, m=4, n1=n2=n3=2, as used in the graphs below).

void nodeChecks(Checker& check) {
  std::string err;
  GraphDesc desc;
  check(GraphDesc::parse(kSphereGraph, desc, err), "lithoform: sphere graph parses (" + err + ")");
  auto g = NoiseGraph::compile(desc, 1234, err);
  check(g != nullptr, "lithoform: sphere graph compiles (" + err + ")");
  if (!g) return;
  const int out = g->outputIndex("d");
  check(out >= 0, "lithoform: output 'd' resolves");

  // Center of the sphere: deep inside -> 1.
  check(g->evaluateAt(out, 0.f, 0.f, 0.f) == 1.f, "lithoform: center evaluates to 1");
  // Halfway to the surface along x: rn=0.5, d=(1-0.5)/(1/8)=4 -> smoothstep clamps to 1.
  check(g->evaluateAt(out, 4.f, 0.f, 0.f) == 1.f, "lithoform: deep interior evaluates to 1");
  // Exactly on the surface: d=0 -> smoothstep(-1,1,0)=0.5.
  const float surf = g->evaluateAt(out, 8.f, 0.f, 0.f);
  check(std::fabs(surf - 0.5f) < 1e-6f,
        "lithoform: surface point evaluates to 0.5 (got " + std::to_string(surf) + ")");
  // Far outside -> 0.
  check(g->evaluateAt(out, 80.f, 0.f, 0.f) == 0.f, "lithoform: far outside evaluates to 0");
  // Just outside the surface: rn slightly > 1 -> small negative d -> slightly below 0.5.
  const float near = g->evaluateAt(out, 8.4f, 0.f, 0.f);
  check(near < 0.5f && near > 0.f,
        "lithoform: falloff band is smooth (got " + std::to_string(near) + ")");

  // Spot-check against the sampler directly: starfish-like longitude shape at a point
  // off the axes. Node and sampler must agree to float precision.
  GraphDesc desc2;
  const char* starGraph = R"({
    "nodes": {
      "s": {"type": "lithoform",
            "theta": {"a": 1, "b": 1, "m": 5, "n1": 0.9, "n2": 1, "n3": 1},
            "phi": {"a": 1, "b": 1, "m": 2, "n1": 2, "n2": 2, "n3": 2},
            "radius": 6, "falloff": 2, "center": [10, -3, 7]}
    },
    "outputs": {"d": "s"}
  })";
  check(GraphDesc::parse(starGraph, desc2, err), "lithoform: star graph parses (" + err + ")");
  auto g2 = NoiseGraph::compile(desc2, 99, err);
  check(g2 != nullptr, "lithoform: star graph compiles (" + err + ")");
  if (g2) {
    const int o2 = g2->outputIndex("d");
    // Point in world space; replicate the node's math against the sampler.
    const double px = 13.0, py = -1.0, pz = 9.0;  // q = p - center = (3, 2, 2)
    const double qx = 3.0, qy = 2.0, qz = 2.0;
    const double qlen = std::sqrt(qx * qx + qy * qy + qz * qz);
    // Polar axis is world Y (see litho_formula_node.cpp / voxelizer mapping).
    const double phiA = std::asin(qy / qlen), thetaA = std::atan2(qz, qx);
    const LithoParams th{1.0, 1.0, 5.0, 0.9, 1.0, 1.0};
    const LithoParams ph{1.0, 1.0, 2.0, 2.0, 2.0, 2.0};
    const auto pt = lithoPoint(thetaA, phiA, th, ph);
    const double R = std::sqrt(pt.x * pt.x + pt.y * pt.y + pt.z * pt.z);
    const double d = (R - qlen / 6.0) / (2.0 / 6.0);
    const double t = std::clamp((d + 1.0) * 0.5, 0.0, 1.0);
    const float expected = float(t * t * (3.0 - 2.0 * t));
    const float got = g2->evaluateAt(o2, float(px), float(py), float(pz));
    check(std::fabs(got - expected) < 1e-5f,
          "lithoform: node agrees with the sampler (got " + std::to_string(got) + ", want " +
              std::to_string(expected) + ")");
  }

  // Composition with a modifier node: clamp the field.
  GraphDesc desc3;
  const char* compGraph = R"({
    "nodes": {
      "ball": {"type": "lithoform",
               "theta": {"a": 1, "b": 1, "m": 4, "n1": 2, "n2": 2, "n3": 2},
               "phi": {"a": 1, "b": 1, "m": 4, "n1": 2, "n2": 2, "n3": 2},
               "radius": 8, "falloff": 1},
      "c": {"type": "clamp", "input": "ball", "min": 0.25, "max": 0.75}
    },
    "outputs": {"d": "c"}
  })";
  check(GraphDesc::parse(compGraph, desc3, err), "lithoform: composed graph parses (" + err + ")");
  auto g3 = NoiseGraph::compile(desc3, 7, err);
  check(g3 != nullptr, "lithoform: composed graph compiles (" + err + ")");
  if (g3) {
    const int o3 = g3->outputIndex("d");
    check(g3->evaluateAt(o3, 0.f, 0.f, 0.f) == 0.75f, "lithoform: clamp caps the interior at 0.75");
    check(g3->evaluateAt(o3, 80.f, 0.f, 0.f) == 0.25f,
          "lithoform: clamp floors the exterior at 0.25");
  }

  // Determinism: no seed anywhere; two evaluations agree bitwise.
  const float a1 = g->evaluateAt(out, 5.f, 2.f, -3.f);
  const float a2 = g->evaluateAt(out, 5.f, 2.f, -3.f);
  check(a1 == a2, "lithoform: deterministic across evaluations");

  // Hard mask: falloff = 0.
  GraphDesc desc4;
  const char* maskGraph = R"({
    "nodes": {"m": {"type": "lithoform",
                    "theta": {"a": 1, "b": 1, "m": 4, "n1": 2, "n2": 2, "n3": 2},
                    "phi": {"a": 1, "b": 1, "m": 4, "n1": 2, "n2": 2, "n3": 2},
                    "radius": 8, "falloff": 0}},
    "outputs": {"d": "m"}
  })";
  auto g4 = GraphDesc::parse(maskGraph, desc4, err) ? NoiseGraph::compile(desc4, 1, err) : nullptr;
  check(g4 != nullptr, "lithoform: hard-mask graph compiles (" + err + ")");
  if (g4) {
    const int o4 = g4->outputIndex("d");
    check(g4->evaluateAt(o4, 4.f, 0.f, 0.f) == 1.f, "lithoform: hard mask inside -> 1");
    check(g4->evaluateAt(o4, 80.f, 0.f, 0.f) == 0.f, "lithoform: hard mask outside -> 0");
  }

  // Rotation: 90 degrees about z maps the x-axis evaluation onto y.
  GraphDesc desc5;
  const char* rotGraph = R"({
    "nodes": {"r": {"type": "lithoform",
                    "theta": {"a": 1, "b": 2, "m": 2, "n1": 2, "n2": 2, "n3": 2},
                    "phi": {"a": 1, "b": 1, "m": 2, "n1": 2, "n2": 2, "n3": 2},
                    "radius": 8, "falloff": 1, "rotation": [0, 0, 1.57079632679]}},
    "outputs": {"d": "r"}
  })";
  auto g5 = GraphDesc::parse(rotGraph, desc5, err) ? NoiseGraph::compile(desc5, 1, err) : nullptr;
  check(g5 != nullptr, "lithoform: rotated graph compiles (" + err + ")");
  if (g5) {
    const int o5 = g5->outputIndex("d");
    // Unrotated twin for comparison.
    GraphDesc desc6;
    const char* plainGraph = R"({
      "nodes": {"r": {"type": "lithoform",
                      "theta": {"a": 1, "b": 2, "m": 2, "n1": 2, "n2": 2, "n3": 2},
                      "phi": {"a": 1, "b": 1, "m": 2, "n1": 2, "n2": 2, "n3": 2},
                      "radius": 8, "falloff": 1}},
      "outputs": {"d": "r"}
    })";
    auto g6 = GraphDesc::parse(plainGraph, desc6, err) ? NoiseGraph::compile(desc6, 1, err) : nullptr;
    check(g6 != nullptr, "lithoform: unrotated twin compiles (" + err + ")");
    if (g6) {
      const int o6 = g6->outputIndex("d");
      // Rotating the shape +90deg about z moves the +x lobe to +y: value at (0,6,0)
      // rotated == value at (6,0,0) unrotated.
      const float vr = g5->evaluateAt(o5, 0.f, 6.f, 0.f);
      const float vu = g6->evaluateAt(o6, 6.f, 0.f, 0.f);
      check(std::fabs(vr - vu) < 1e-5f, "lithoform: rotation reorients the field (got " +
                                            std::to_string(vr) + ", want " + std::to_string(vu) + ")");
    }
  }

  // Validation failures go through the err channel.
  GraphDesc bad;
  check(!GraphDesc::parse(
            R"({"nodes": {"b": {"type": "lithoform", "radius": 0}}, "outputs": {"d": "b"}})", bad,
            err) ||
            !NoiseGraph::compile(bad, 1, err),
        "lithoform: radius 0 rejected");
  GraphDesc bad2;
  check(!GraphDesc::parse(
            R"({"nodes": {"b": {"type": "lithoform", "falloff": -1}}, "outputs": {"d": "b"}})",
            bad2, err) ||
            !NoiseGraph::compile(bad2, 1, err),
        "lithoform: negative falloff rejected");
  GraphDesc bad3;
  check(!GraphDesc::parse(
            R"({"nodes": {"b": {"type": "lithoform", "theta": {"a": 1}}}, "outputs": {"d": "b"}})",
            bad3, err) ||
            !NoiseGraph::compile(bad3, 1, err),
        "lithoform: incomplete theta object rejected");
  GraphDesc bad4;
  check(!GraphDesc::parse(
            R"({"nodes": {"b": {"type": "lithoform", "rotation": [0, 0]}}, "outputs": {"d": "b"}})",
            bad4, err) ||
            !NoiseGraph::compile(bad4, 1, err),
        "lithoform: malformed rotation rejected");
}

/// Regression test for review H3: the node and the voxelizer must agree on
/// where a NON-SPHERICAL shape sits. A sphere cannot catch a polar-axis
/// swap (it looks the same rotated), and the spot-check above cannot either
/// (it replicates the node's own convention), so this bakes the gallery
/// "starfish" preset with voxelizeLithoShape and compares occupancy against
/// the analytic node field cell by cell, plus the surface radius along
/// each world axis. With the pre-fix mapping (polar axis = world Z) the
/// starfish is rotated 90 degrees: its +Y surface radius is 0.743 instead
/// of 1.000 and occupancy agreement collapses (both asserted below).
void voxelizerAgreementChecks(Checker& check) {
  // Gallery preset "starfish" (game/LithoKraft/assets/models/
  // litho_formula_gallery.json): five-armed, pinched five-fold longitude.
  const LithoParams th{1.0, 1.0, 5.0, 0.9, 1.0, 1.0};
  const LithoParams ph{1.0, 1.0, 2.0, 2.0, 2.0, 2.0};
  const int n = 32;
  const VoxelResult vr = voxelizeLithoShape(th, ph, n);
  check(vr.resolution == n && !vr.filled.empty(),
        "lithoform-vs-voxelizer: starfish bakes non-empty at 32");

  // Reproduce the voxelizer's normalization (litho_formula_voxelize.cpp
  // step 1): the sampled bounding radius maps to (n/2 - 1) grid cells.
  const double pi = std::acos(-1.0);
  double maxR = 0.0;
  for (int i = 0; i < 8 * n; ++i) {
    const double theta = -pi + 2.0 * pi * (i + 0.5) / (8 * n);
    for (int j = 0; j < 4 * n; ++j) {
      const double phi = -pi / 2.0 + pi * (j + 0.5) / (4 * n);
      const auto pt = lithoPoint(theta, phi, th, ph);
      if (!std::isfinite(pt.x + pt.y + pt.z)) continue;
      maxR = std::max({maxR, std::fabs(pt.x), std::fabs(pt.y), std::fabs(pt.z)});
    }
  }
  check(maxR > 0.0, "lithoform-vs-voxelizer: sampled bounding radius positive");
  if (!(maxR > 0.0) || vr.filled.empty()) return;
  const double scale = (n / 2.0 - 1.0) / maxR;  // grid cells per formula unit
  const double center = (n - 1) / 2.0;

  std::vector<char> baked(static_cast<size_t>(n) * n * n, 0);
  for (const auto& cell : vr.filled)
    baked[static_cast<size_t>((cell[2] * n + cell[1]) * n + cell[0])] = 1;

  // The node for the same preset: radius 1 (world units are formula units),
  // hard mask so eval is a pure inside/outside test.
  std::string err;
  GraphDesc desc;
  const char* graphJson = R"({
    "nodes": {
      "s": {"type": "lithoform",
            "theta": {"a": 1, "b": 1, "m": 5, "n1": 0.9, "n2": 1, "n3": 1},
            "phi": {"a": 1, "b": 1, "m": 2, "n1": 2, "n2": 2, "n3": 2},
            "radius": 1, "falloff": 0, "center": [0, 0, 0]}
    },
    "outputs": {"d": "s"}
  })";
  check(GraphDesc::parse(graphJson, desc, err),
        "lithoform-vs-voxelizer: starfish graph parses (" + err + ")");
  auto g = NoiseGraph::compile(desc, 1, err);
  check(g != nullptr, "lithoform-vs-voxelizer: starfish graph compiles (" + err + ")");
  if (!g) return;
  const int out = g->outputIndex("d");

  // Occupancy agreement at grid cell centers. Grid -> formula is the
  // voxelizer's mapping (formula (x, y, z) -> grid (x, z, y)); formula ->
  // world is its inverse (world (x, y, z) = formula (x, z, y)), because
  // the node's polar axis is world Y.
  size_t agree = 0;
  const size_t total = static_cast<size_t>(n) * n * n;
  for (int gz = 0; gz < n; ++gz)
    for (int gy = 0; gy < n; ++gy)
      for (int gx = 0; gx < n; ++gx) {
        const double fx = ((gx + 0.5) - center) / scale;   // formula x
        const double fp = ((gy + 0.5) - center) / scale;   // formula z (polar)
        const double fy = ((gz + 0.5) - center) / scale;   // formula y
        const bool nodeIn =
            g->evaluateAt(out, float(fx), float(fp), float(fy)) >= 0.5f;
        const bool bakedIn = baked[static_cast<size_t>((gz * n + gy) * n + gx)] != 0;
        if (nodeIn == bakedIn) ++agree;
      }
  const double agreement = double(agree) / double(total);
  char abuf[64];
  std::snprintf(abuf, sizeof(abuf), "%.4f", agreement);
  // Measured on this preset: the correct mapping agrees on 0.967 of cells
  // (the residual is voxelization error -- the voxelizer fills sampled
  // column spans, not the analytic surface, so boundary cells differ);
  // the pre-fix mapping agrees on only 0.876. The 0.95 bar sits between
  // the two with margin on both sides.
  check(agreement >= 0.95,
        std::string("lithoform-vs-voxelizer: starfish occupancy agreement >= 0.95 (got ") +
            abuf + ")");

  // Surface radius along each world axis: node radius by bisection on the
  // hard mask vs the baked shape's farthest filled cell on that axis
  // (grid axis line through the center cells, index 15 == exactly 0 in
  // formula units because center is 15.5 and cell centers are at +0.5).
  auto nodeRadiusAlong = [&](double dx, double dy, double dz) {
    double lo = 0.0, hi = 3.0;  // inside at lo, outside at hi
    for (int it = 0; it < 48; ++it) {
      const double mid = 0.5 * (lo + hi);
      if (g->evaluateAt(out, float(dx * mid), float(dy * mid), float(dz * mid)) >= 0.5f)
        lo = mid;
      else
        hi = mid;
    }
    return 0.5 * (lo + hi);
  };
  auto bakedRadiusAlong = [&](int axis) {
    int best = -1;
    for (const auto& cell : vr.filled) {
      // Axis lines: +X is (gy, gz) == (15, 15); +Y is (gx, gz) == (15, 15);
      // +Z is (gx, gy) == (15, 15).
      if (axis == 0 && cell[1] == 15 && cell[2] == 15) best = std::max(best, cell[0]);
      if (axis == 1 && cell[0] == 15 && cell[2] == 15) best = std::max(best, cell[1]);
      if (axis == 2 && cell[0] == 15 && cell[1] == 15) best = std::max(best, cell[2]);
    }
    // Center of the farthest filled cell, in formula units.
    return best < 0 ? -1.0 : ((best + 0.5) - center) / scale;
  };
  const double tol = 1.5 / scale;  // 1.5 cells of voxelization slack
  const char* axisName[3] = {"+X", "+Y", "+Z"};
  const double dirs[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (int axis = 0; axis < 3; ++axis) {
    const double rn = nodeRadiusAlong(dirs[axis][0], dirs[axis][1], dirs[axis][2]);
    const double rb = bakedRadiusAlong(axis);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "node %.3f, baked %.3f, tol %.3f", rn, rb, tol);
    check(rb > 0.0 && std::fabs(rn - rb) <= tol,
          std::string("lithoform-vs-voxelizer: starfish surface radius along ") +
              axisName[axis] + " agrees (" + buf + ")");
  }
}

}  // namespace

int runLithoFormulaNodeSelfTest() {
  Checker check;
  nodeChecks(check);
  voxelizerAgreementChecks(check);
  return check.fails();
}

}  // namespace litholattice::procgen::litho_formula
