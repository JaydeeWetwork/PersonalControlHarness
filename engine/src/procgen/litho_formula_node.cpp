// Litho Formula — Wet Wafer Works' clean-room approximation of the shape-generation
// concept originated by Johan Gielis (2003; cf. Genicap). Independent implementation
// from the mathematical definition; no third-party code referenced. Underlying patent
// expired 2020-05-10. Sources of inspiration: Gielis's concept and the procedural
// ambition of No Man's Sky (Hello Games).

/// `lithoform` procgen graph node (wired 2026-10-12, JD-locked for alpha).
///
/// Generator-style node evaluating the Litho Formula analytically per sample point and
/// writing a density-like value, composing with modifier nodes exactly like `noise`.
/// See engine/src/procgen/litho_formula_node_spec.md for the full design.
#include "graph_nodes.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "litholattice/procgen/litho_formula.h"

namespace litholattice::procgen::detail {
namespace {

using litho_formula::LithoParams;

bool readVec3Local(const Json& params, const char* key, std::array<double, 3>& out,
                   std::string& err) {
  const Json* v = params.get(key);
  if (!v) {
    out = {0.0, 0.0, 0.0};
    return true;
  }
  if (v->type == Json::Type::Number) {
    out.fill(v->num);
    return true;
  }
  if (v->type != Json::Type::Array || v->arr.size() != 3) {
    err = std::string("'") + key + "' must be a number or [x, y, z]";
    return false;
  }
  for (size_t i = 0; i < 3; ++i) {
    if (v->arr[i].type != Json::Type::Number) {
      err = std::string("'") + key + "' must be a number or [x, y, z]";
      return false;
    }
    out[i] = v->arr[i].num;
  }
  return true;
}

/// Reads one of the theta/phi param objects. Absent object -> defaults; a present
/// object must carry all six numbers (per the node spec).
bool readLithoParams(const Json& p, const char* key, LithoParams& out, std::string& err) {
  static const LithoParams kDefault{1.0, 1.0, 2.0, 2.0, 2.0, 2.0};
  const Json* v = p.get(key);
  if (!v) {
    out = kDefault;
    return true;
  }
  if (v->type != Json::Type::Object) {
    err = std::string("'") + key + "' must be an object {a,b,m,n1,n2,n3}";
    return false;
  }
  const char* names[6] = {"a", "b", "m", "n1", "n2", "n3"};
  double* slots[6] = {&out.a, &out.b, &out.m, &out.n1, &out.n2, &out.n3};
  for (int i = 0; i < 6; ++i) {
    const Json* f = v->get(names[i]);
    if (!f || f->type != Json::Type::Number) {
      err = std::string("'") + key + "." + names[i] + "' must be a number";
      return false;
    }
    *slots[i] = f->num;
  }
  return true;
}

/// Inverse Euler rotation R^T for R = Rz(rz)*Ry(ry)*Rx(rx), as row-major 3x3.
std::array<std::array<double, 3>, 3> unrotateMatrix(double rx, double ry, double rz) {
  const double cx = std::cos(rx), sx = std::sin(rx);
  const double cy = std::cos(ry), sy = std::sin(ry);
  const double cz = std::cos(rz), sz = std::sin(rz);
  // Rx, Ry, Rz rows:
  const double Rx[3][3] = {{1, 0, 0}, {0, cx, -sx}, {0, sx, cx}};
  const double Ry[3][3] = {{cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy}};
  const double Rz[3][3] = {{cz, -sz, 0}, {sz, cz, 0}, {0, 0, 1}};
  double tmp[3][3] = {};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k) tmp[i][j] += Ry[i][k] * Rx[k][j];
  double R[3][3] = {};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k) R[i][j] += Rz[i][k] * tmp[k][j];
  // Transpose: unrot[i][j] = R[j][i].
  std::array<std::array<double, 3>, 3> u{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) u[i][j] = R[j][i];
  return u;
}

class LithoformNode final : public Node {
 public:
  LithoformNode(LithoParams theta, LithoParams phi, double radius, double falloff,
                std::array<std::array<double, 3>, 3> unrot, std::array<double, 3> center)
      : theta_(theta),
        phi_(phi),
        radius_(radius),
        falloff_(falloff),
        unrot_(unrot),
        center_(center) {}

  void eval(const PointsView& p, std::span<const std::span<const float>> /*inputs*/,
            std::span<float> out) const override {
    for (size_t i = 0; i < out.size(); ++i) {
      const double dx = double(p.x[i]) - center_[0];
      const double dy = double(p.y[i]) - center_[1];
      const double dz = double(p.z[i]) - center_[2];
      const double qx = unrot_[0][0] * dx + unrot_[0][1] * dy + unrot_[0][2] * dz;
      const double qy = unrot_[1][0] * dx + unrot_[1][1] * dy + unrot_[1][2] * dz;
      const double qz = unrot_[2][0] * dx + unrot_[2][1] * dy + unrot_[2][2] * dz;
      const double qlen = std::sqrt(qx * qx + qy * qy + qz * qz);
      if (!(qlen > 0.0)) {
        out[i] = 1.0f;  // exactly at the center: deep inside
        continue;
      }
      const double rn = qlen / radius_;
      // Polar axis is world Y (game up), matching the voxelizer/stamp path
      // (formula (x, y, z) -> world (x, z, y)): phi is the latitude off the
      // world XZ plane and theta the longitude in that plane. Mapping phi to
      // world Z instead rotated every non-spherical shape by 90 degrees
      // relative to its baked stamp (review H3).
      const double phiA = std::asin(std::clamp(qy / qlen, -1.0, 1.0));
      const double thetaA = std::atan2(qz, qx);
      const auto pt = litho_formula::lithoPoint(thetaA, phiA, theta_, phi_);
      const double R = std::sqrt(pt.x * pt.x + pt.y * pt.y + pt.z * pt.z);
      if (!(R > 0.0)) {
        out[i] = 0.0f;  // degenerate direction: outside
        continue;
      }
      const double d = (R - rn) / (falloff_ / radius_);
      float v;
      if (falloff_ == 0.0) {
        v = d >= 0.0 ? 1.0f : 0.0f;
      } else {
        const double t = std::clamp((d + 1.0) * 0.5, 0.0, 1.0);
        v = float(t * t * (3.0 - 2.0 * t));
      }
      out[i] = v;
    }
  }

 private:
  LithoParams theta_, phi_;
  double radius_, falloff_;
  std::array<std::array<double, 3>, 3> unrot_;
  std::array<double, 3> center_;
};

}  // namespace

std::unique_ptr<Node> makeLithoform(const Json& p, uint64_t /*seed*/, std::string& err) {
  // No seed: the shape is a deterministic analytic function; variety comes from scatter.
  LithoParams theta, phi;
  if (!readLithoParams(p, "theta", theta, err)) return nullptr;
  if (!readLithoParams(p, "phi", phi, err)) return nullptr;
  const double radius = p.getNum("radius", 8.0);
  if (!(radius > 0.0)) {
    err = "'radius' must be > 0";
    return nullptr;
  }
  const double falloff = p.getNum("falloff", 1.0);
  if (!(falloff >= 0.0)) {
    err = "'falloff' must be >= 0";
    return nullptr;
  }
  std::array<double, 3> rot{0.0, 0.0, 0.0}, center{0.0, 0.0, 0.0};
  if (!readVec3Local(p, "rotation", rot, err)) return nullptr;
  if (!readVec3Local(p, "center", center, err)) return nullptr;
  return std::make_unique<LithoformNode>(theta, phi, radius, falloff,
                                         unrotateMatrix(rot[0], rot[1], rot[2]), center);
}

}  // namespace litholattice::procgen::detail
