// Small value-type math for the engine (column-major matrices, OpenGL clip conventions).
#pragma once
#include <cmath>

namespace litholattice {

struct Vec3 { float x = 0, y = 0, z = 0; };
inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 normalize(Vec3 a) {
  float l = std::sqrt(dot(a, a));
  // Zero vector -> NaN without this guard; return a safe default.
  if (!(l > 1e-12f)) return Vec3{0.f, 1.f, 0.f};
  return a * (1.f / l);
}

struct Mat4 {
  float m[16]{};
  static Mat4 identity() { Mat4 r; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f; return r; }
};
inline Mat4 mul(const Mat4& a, const Mat4& b) {
  Mat4 r;
  for (int c = 0; c < 4; ++c)
    for (int rr = 0; rr < 4; ++rr) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
      r.m[c * 4 + rr] = s;
    }
  return r;
}
inline Mat4 perspective(float fovy, float aspect, float zn, float zf) {
  Mat4 r; float f = 1.f / std::tan(fovy * 0.5f);
  r.m[0] = f / aspect; r.m[5] = f; r.m[10] = (zf + zn) / (zn - zf); r.m[11] = -1.f;
  r.m[14] = 2.f * zf * zn / (zn - zf);
  return r;
}
inline Mat4 lookDir(Vec3 eye, Vec3 fwd) {
  Vec3 f = normalize(fwd), s = normalize(cross(f, {0, 1, 0})), u = cross(s, f);
  Mat4 r;
  r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
  r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
  r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
  r.m[12] = -dot(s, eye); r.m[13] = -dot(u, eye); r.m[14] = dot(f, eye); r.m[15] = 1.f;
  return r;
}
// AABB vs the 6 frustum planes of a view-projection matrix.
inline bool aabbInFrustum(const Mat4& vp, Vec3 mn, Vec3 mx) {
  const float* m = vp.m;
  for (int p = 0; p < 6; ++p) {
    const int row = p / 2; const float sgn = (p & 1) ? -1.f : 1.f;
    float a = m[3] + sgn * m[row], b = m[7] + sgn * m[4 + row], c = m[11] + sgn * m[8 + row], d = m[15] + sgn * m[12 + row];
    Vec3 v{a >= 0 ? mx.x : mn.x, b >= 0 ? mx.y : mn.y, c >= 0 ? mx.z : mn.z};
    if (a * v.x + b * v.y + c * v.z + d < 0) return false;
  }
  return true;
}

/// AABB vs frustum with an angular margin (degrees): the box is expanded by
/// d*tan(margin) on every side, where d = distance from camPos to the box
/// center - approximating a frustum widened by `marginDeg` on all sides.
/// The shock absorber against "frustum occlusion tearing" (JD 2026-10-11):
/// fast view rotation reveals already-rendered chunks instead of pop-in.
/// marginDeg <= 0 degenerates to the exact aabbInFrustum test.
inline bool aabbInFrustumMargin(const Mat4& vp, Vec3 mn, Vec3 mx, Vec3 camPos, float marginDeg) {
  if (!(marginDeg > 0.f)) return aabbInFrustum(vp, mn, mx);
  const Vec3 c{(mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f};
  const float dx = c.x - camPos.x, dy = c.y - camPos.y, dz = c.z - camPos.z;
  const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
  constexpr float kDegToRad = 3.14159265358979323846f / 180.f;
  const float expand = d * std::tan(marginDeg * kDegToRad);
  const Vec3 e{expand, expand, expand};
  return aabbInFrustum(vp, {mn.x - e.x, mn.y - e.y, mn.z - e.z},
                       {mx.x + e.x, mx.y + e.y, mx.z + e.z});
}

}  // namespace litholattice
