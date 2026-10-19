// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

// Self-tests for the Litho Formula voxelizer (litho_formula_voxelize.h).
// Acceptance checks: sphere volume, watertight interior fill, resolution
// handling, star-smoke, determinism. Wired into runProcgenSelfTest.

#include "litholattice/procgen/litho_formula_voxelize_selftest.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "litholattice/procgen/litho_formula_voxelize.h"
#include "litholattice/subdiv/octree.h"

namespace litholattice::procgen::litho_formula {
namespace {

const double kPi = std::acos(-1.0);

struct Checker {
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails_ = 0;
};

std::string fmt(const char* fmtStr, double v) {
  char buf[128];
  std::snprintf(buf, sizeof(buf), fmtStr, v);
  return buf;
}

// Closed-form anchor params: a=b=1, m=4, n1=n2=n3=2 -> r(phi) = 1, the unit
// circle; the spherical product of two such evaluators is the unit sphere.
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

void samplerAnchorCheck(Checker& check) {
  // Validates the binding to the real sampler header: the closed-form anchor
  // must reproduce r = 1 (within float epsilon) at every angle.
  const LithoParams c = circleParams();
  bool ok = true;
  for (int i = 0; i <= 32; ++i) {
    const double phi = -kPi + 2.0 * kPi * i / 32.0;
    ok = ok && std::fabs(lithoR(phi, c) - 1.0) < 1e-12;
  }
  check(ok, "sampler anchor: lithoR(phi, circle) == 1 for all phi");
}

void sphereVolumeCheck(Checker& check) {
  // The unit sphere voxelized at 64^3: the voxelizer maps the analytic
  // radius 1 to 31 grid cells (the sampled bounding radius lands on
  // [0.5, 63.5]; the sampled-vs-analytic maxR differs by < 0.01%, negligible
  // against the 6% bar). Filled count * voxel volume ~= 4/3 pi r^3.
  const LithoParams c = circleParams();
  const VoxelResult vr = voxelizeLithoShape(c, c, 64);
  const double cellsPerUnit = 31.0;
  const double voxelVol = 1.0 / (cellsPerUnit * cellsPerUnit * cellsPerUnit);
  const double vol = static_cast<double>(vr.filled.size()) * voxelVol;
  const double expect = 4.0 / 3.0 * kPi;  // r = 1
  const double rel = std::fabs(vol - expect) / expect;
  check(vr.resolution == 64 && !vr.filled.empty() && rel <= 0.06,
        "sphere volume at 64 within 6% (N=" + std::to_string(vr.filled.size()) +
            ", rel err " + fmt("%.4f", rel) + ")");
}

void watertightCheck(Checker& check) {
  // On the convex sphere, every cell whose center is within
  // r - sqrt(3) * cellSize of the shape center must be filled: no interior
  // holes. Shape center maps to 31.5 per axis; cell size is 1 grid cell;
  // analytic radius is 31 cells (sampled radius is a hair larger, so 31 is
  // the conservative bound).
  const LithoParams c = circleParams();
  const VoxelResult vr = voxelizeLithoShape(c, c, 64);
  const int n = 64;
  std::vector<char> filled(static_cast<size_t>(n) * n * n, 0);
  for (const auto& cell : vr.filled)
    filled[static_cast<size_t>((cell[2] * n + cell[1]) * n + cell[0])] = 1;
  const double rIn = 31.0 - std::sqrt(3.0);
  bool ok = true;
  size_t interiorCells = 0;
  for (int z = 0; z < n && ok; ++z)
    for (int y = 0; y < n && ok; ++y)
      for (int x = 0; x < n && ok; ++x) {
        const double dx = x + 0.5 - 31.5;
        const double dy = y + 0.5 - 31.5;
        const double dz = z + 0.5 - 31.5;
        if (dx * dx + dy * dy + dz * dz <= rIn * rIn) {
          ++interiorCells;
          if (!filled[static_cast<size_t>((z * n + y) * n + x)]) ok = false;
        }
      }
  check(ok, "watertight fill: all " + std::to_string(interiorCells) +
                " deep-interior sphere cells filled (no holes)");
}

void resolutionCheck(Checker& check) {
  const LithoParams c = circleParams();
  bool ok = true;
  for (const int res : {16, 32, 64}) {
    const VoxelResult vr = voxelizeLithoShape(c, c, res);
    if (vr.resolution != res || vr.filled.empty()) {
      ok = false;
      continue;
    }
    for (const auto& cell : vr.filled) {
      if (cell[0] < 0 || cell[0] >= res || cell[1] < 0 || cell[1] >= res ||
          cell[2] < 0 || cell[2] >= res) {
        ok = false;
        break;
      }
    }
    // emitToOctree lands at level log2(resolution): 16->4, 32->5, 64->6.
    int level = 0;
    for (int p = res; p > 1; p >>= 1) ++level;
    litholattice::subdiv::SparseOctree tree;
    emitToOctree(vr, tree, 7);
    if (tree.leafCount() != vr.filled.size()) ok = false;
    for (size_t i = 0; i < vr.filled.size() && ok; i += vr.filled.size() / 7 + 1) {
      const auto& cell = vr.filled[i];
      if (tree.get(level, cell[0], cell[1], cell[2]) != 7) ok = false;
    }
  }
  check(ok, "resolutions 16/32/64: coords in [0,res)^3, octree at level log2(res)");

  // Invalid resolutions are rejected cleanly: empty result, resolution 0.
  bool rejected = true;
  for (const int bad : {24, 0, 7, 100, -8}) {
    const VoxelResult vr = voxelizeLithoShape(c, c, bad);
    if (vr.resolution != 0 || !vr.filled.empty()) rejected = false;
    litholattice::subdiv::SparseOctree tree;
    emitToOctree(vr, tree);
    if (!tree.empty()) rejected = false;
  }
  check(rejected, "invalid resolutions (24, 0, 7, 100, -8) rejected: empty result");
}

void degenerateCheck(Checker& check) {
  // Non-finite parameters collapse the sampler to the origin (per its
  // degenerate rails); the voxelizer must return an empty result, not
  // crash or emit garbage. (Both factors degenerate so x=y=z=0.)
  LithoParams bad;
  bad.a = std::numeric_limits<double>::quiet_NaN();
  const VoxelResult vr = voxelizeLithoShape(bad, bad, 32);
  check(vr.filled.empty(), "degenerate (NaN) params: empty result, no crash");
}

void starSmokeCheck(Checker& check) {
  // Star-like params (m = 5): must voxelize to a non-empty, bounded,
  // deterministic result.
  LithoParams star = circleParams();
  star.m = 5.0;
  star.n1 = 1.0;
  star.n2 = 1.0;
  star.n3 = 1.0;
  const LithoParams c = circleParams();
  const VoxelResult a = voxelizeLithoShape(star, c, 32);
  const VoxelResult b = voxelizeLithoShape(star, c, 32);
  bool bounded = !a.filled.empty();
  for (const auto& cell : a.filled) {
    if (cell[0] < 0 || cell[0] >= 32 || cell[1] < 0 || cell[1] >= 32 ||
        cell[2] < 0 || cell[2] >= 32) {
      bounded = false;
      break;
    }
  }
  check(bounded, "star (m=5) smoke: non-empty, bounded in [0,32)^3 (N=" +
                     std::to_string(a.filled.size()) + ")");
  check(a.filled == b.filled, "star (m=5): deterministic across runs");
}

}  // namespace

int runLithoFormulaVoxelizeSelfTest() {
  std::printf("Litho Formula voxelizer self-test\n");
  Checker check;
  samplerAnchorCheck(check);
  sphereVolumeCheck(check);
  watertightCheck(check);
  resolutionCheck(check);
  degenerateCheck(check);
  starSmokeCheck(check);
  std::printf(check.fails_ ? "LITHO VOXELIZER SELFTEST FAILED (%d)\n"
                           : "LITHO VOXELIZER SELFTEST OK\n",
              check.fails_);
  return check.fails_;
}

}  // namespace litholattice::procgen::litho_formula
