// Software-renderer anti-aliasing: FXAA-class post filter, conservative
// morphological coverage restoration (CMAA lineage), SSAA resolve.
// Clean-room implementations from the published technique descriptions
// (see Software-Renderer-AA-2026-10-13.md for the survey + citations);
// no third-party code. All passes gather from a source copy and are
// single-threaded like the rasterizer they post-process.
#include "litholattice/render/software_aa.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace litholattice {
namespace {

inline float lumaOf(uint32_t p) {
  const float r = float((p >> 16) & 0xFF), g = float((p >> 8) & 0xFF),
              b = float(p & 0xFF);
  return (0.299f * r + 0.587f * g + 0.114f * b) * (1.f / 255.f);
}

inline uint32_t packPx(float r, float g, float b) {
  auto ch = [](float v) { return uint32_t(std::clamp(v, 0.f, 255.f) + 0.5f); };
  return 0xFF000000u | (ch(r) << 16) | (ch(g) << 8) | ch(b);
}

void computeLuma(const SoftwareFramebuffer& fb, SoftwareAaScratch& s) {
  const size_t n = size_t(fb.width) * fb.height;
  s.luma.resize(n);
  for (size_t i = 0; i < n; ++i) s.luma[i] = lumaOf(fb.pixels[i]);
}

// Bilinear sample in pixel-index space (centers on integers), clamped.
uint32_t bilinearAt(const uint32_t* src, int w, int h, float fx, float fy) {
  fx = std::clamp(fx, 0.f, float(w - 1));
  fy = std::clamp(fy, 0.f, float(h - 1));
  const int x0 = int(fx), y0 = int(fy);
  const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
  const float tx = fx - float(x0), ty = fy - float(y0);
  float r = 0, g = 0, b = 0;
  const int xs[2] = {x0, x1}, ys[2] = {y0, y1};
  const float wx[2] = {1.f - tx, tx}, wy[2] = {1.f - ty, ty};
  for (int j = 0; j < 2; ++j)
    for (int i = 0; i < 2; ++i) {
      const uint32_t p = src[size_t(ys[j]) * size_t(w) + size_t(xs[i])];
      const float wgt = wx[i] * wy[j];
      r += float((p >> 16) & 0xFF) * wgt;
      g += float((p >> 8) & 0xFF) * wgt;
      b += float(p & 0xFF) * wgt;
    }
  return packPx(r, g, b);
}

}  // namespace

const char* softwareAaModeName(SoftwareAaMode mode) {
  switch (mode) {
    case SoftwareAaMode::Fxaa: return "fxaa";
    case SoftwareAaMode::Cmaa: return "cmaa";
    case SoftwareAaMode::Ssaa2x: return "ssaa2x";
    default: return "off";
  }
}

bool softwareAaModeFromName(const char* name, SoftwareAaMode& out) {
  if (!name) return false;
  const std::string_view sv(name);
  if (sv == "off") { out = SoftwareAaMode::Off; return true; }
  if (sv == "fxaa") { out = SoftwareAaMode::Fxaa; return true; }
  if (sv == "cmaa") { out = SoftwareAaMode::Cmaa; return true; }
  if (sv == "ssaa2x" || sv == "ssaa") { out = SoftwareAaMode::Ssaa2x; return true; }
  return false;
}

// ---------------------------------------------------------------------------
// FXAA-class post filter (Lottes lineage, clean-room): local-contrast gate,
// subpixel term from the cross neighbourhood, edge orientation by gradient,
// endpoint search along the edge (the pair straddling the edge is followed
// until its average luma breaks), blend offset from the distance to the
// nearer end + the subpixel term. Operates on LDR framebuffer values.
// ---------------------------------------------------------------------------
void applyFxaaPost(SoftwareFramebuffer& fb, SoftwareAaScratch& s) {
  const int w = int(fb.width), h = int(fb.height);
  if (w < 3 || h < 3) return;
  s.src = fb.pixels;
  computeLuma(fb, s);
  const uint32_t* src = s.src.data();
  const float* L = s.luma.data();
  auto lum = [&](int x, int y) {
    x = std::clamp(x, 0, w - 1);
    y = std::clamp(y, 0, h - 1);
    return L[size_t(y) * size_t(w) + size_t(x)];
  };
  constexpr float kEdgeRel = 0.125f;   // FXAA quality edge threshold (relative)
  constexpr float kEdgeAbs = 0.0625f;  // FXAA quality edge threshold (absolute floor)
  constexpr int kSearch = 12;          // endpoint search radius (steps of 1 px)

  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const float lM = lum(x, y);
      const float lN = lum(x, y - 1), lS = lum(x, y + 1);
      const float lW = lum(x - 1, y), lE = lum(x + 1, y);
      const float lNW = lum(x - 1, y - 1), lNE = lum(x + 1, y - 1);
      const float lSW = lum(x - 1, y + 1), lSE = lum(x + 1, y + 1);
      float rMax = lM, rMin = lM;
      for (float v : {lN, lS, lE, lW, lNW, lNE, lSW, lSE}) {
        rMax = std::max(rMax, v);
        rMin = std::min(rMin, v);
      }
      const float range = rMax - rMin;
      if (range < std::max(kEdgeAbs, kEdgeRel * rMax)) continue;  // not an edge

      // Subpixel term: how far the cross average sits from the center,
      // normalized by the local range, smoothstepped (FXAA's subpix curve).
      const float crossAvg = (lN + lS + lE + lW) * 0.25f;
      float sub = std::clamp(std::fabs(crossAvg - lM) / range, 0.f, 1.f);
      sub = sub * sub * (3.f - 2.f * sub);
      const float subOff = sub * 0.375f;  // <= 3/8 px from the subpixel term

      // Orientation: gradient magnitude across each axis (Sobel).
      const float gx = std::fabs((lNE + 2.f * lE + lSE) - (lNW + 2.f * lW + lSW));
      const float gy = std::fabs((lSW + 2.f * lS + lSE) - (lNW + 2.f * lN + lNE));
      const bool horzEdge = gy >= gx;  // edge runs horizontally

      // Endpoint search along the edge: follow the average of the pair
      // straddling the edge; the end is where that average breaks by
      // >= 1/4 of the across-edge gradient (FXAA's end-of-edge test).
      float edgeOff = 0.f;
      bool towardNeg = false;  // blend direction: toward the steeper side
      if (horzEdge) {
        const float stepN = std::fabs(lN - lM), stepS = std::fabs(lS - lM);
        towardNeg = stepN >= stepS;
        const float grad = std::max(stepN, stepS);
        const float avg0 = 0.5f * (lN + lS);
        auto search = [&](int dir) {
          for (int t = 1; t <= kSearch; ++t) {
            const float a = 0.5f * (lum(x + dir * t, y - 1) + lum(x + dir * t, y + 1));
            if (std::fabs(a - avg0) >= 0.25f * grad) return t;
          }
          return kSearch + 1;  // no end inside the search radius
        };
        const int dPos = search(+1), dNeg = search(-1);
        const float len = float(dPos + dNeg);
        edgeOff = std::clamp(0.5f - float(std::min(dPos, dNeg)) / len, 0.f, 0.5f);
        const float off = std::max(edgeOff, subOff);
        if (off > 0.f)
          fb.pixels[size_t(y) * size_t(w) + size_t(x)] =
              bilinearAt(src, w, h, float(x), float(y) + (towardNeg ? -off : off));
      } else {
        const float stepW = std::fabs(lW - lM), stepE = std::fabs(lE - lM);
        towardNeg = stepW >= stepE;
        const float grad = std::max(stepW, stepE);
        const float avg0 = 0.5f * (lW + lE);
        auto search = [&](int dir) {
          for (int t = 1; t <= kSearch; ++t) {
            const float a = 0.5f * (lum(x - 1, y + dir * t) + lum(x + 1, y + dir * t));
            if (std::fabs(a - avg0) >= 0.25f * grad) return t;
          }
          return kSearch + 1;
        };
        const int dPos = search(+1), dNeg = search(-1);
        const float len = float(dPos + dNeg);
        edgeOff = std::clamp(0.5f - float(std::min(dPos, dNeg)) / len, 0.f, 0.5f);
        const float off = std::max(edgeOff, subOff);
        if (off > 0.f)
          fb.pixels[size_t(y) * size_t(w) + size_t(x)] =
              bilinearAt(src, w, h, float(x) + (towardNeg ? -off : off), float(y));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Conservative morphological coverage AA (MLAA -> SMAA -> CMAA2 lineage,
// clean-room variant). Pass 1 marks edges between pixels (luma steps and
// depth steps; monotone-ramp texture gradients are rejected unless the
// depth buffer vouches for a geometry step). Pass 2 walks each edge run,
// reconstructs the edge line's subpixel offset at the run ends from the
// adjacent-column/row edges (the turn signal), and integrates the line's
// coverage over each touched pixel analytically — the restored coverage
// IS the blend weight. Pass 3 composites from the source copy.
// Deviations from full CMAA2 (documented in the AA report): the shape
// library is reduced to long-edge runs + end turns (no Z-pattern special
// cases), and weights are analytic instead of SMAA's area texture.
// ---------------------------------------------------------------------------
void applyCmaaPost(SoftwareFramebuffer& fb, const float* depth, SoftwareAaScratch& s) {
  const int w = int(fb.width), h = int(fb.height);
  if (w < 2 || h < 2) return;
  const size_t n = size_t(w) * size_t(h);
  s.src = fb.pixels;
  computeLuma(fb, s);
  const float* L = s.luma.data();
  s.edgeR.assign(n, 0);
  s.edgeB.assign(n, 0);
  auto lum = [&](int x, int y) {
    x = std::clamp(x, 0, w - 1);
    y = std::clamp(y, 0, h - 1);
    return L[size_t(y) * size_t(w) + size_t(x)];
  };
  auto dep = [&](int x, int y) {
    x = std::clamp(x, 0, w - 1);
    y = std::clamp(y, 0, h - 1);
    return depth[size_t(y) * size_t(w) + size_t(x)];
  };
  constexpr float kLumaEdge = 0.08f;    // luma step that counts as an edge
  constexpr float kDepthEdge = 0.0025f; // NDC-z step that counts as geometry
  // A monotone ramp on BOTH sides of a boundary = texture gradient, not a
  // geometry step (CMAA2's conservatism): steps continue in the same
  // direction with comparable size on both columns/rows.
  auto rampV = [&](int x, int y, float step) {  // vertical ramp in column x at row y
    const float a = lum(x, y) - lum(x, y - 1), b = lum(x, y + 1) - lum(x, y);
    return (a > 0.35f * step && b > 0.35f * step) || (a < -0.35f * step && b < -0.35f * step);
  };
  auto rampH = [&](int x, int y, float step) {  // horizontal ramp in row y at column x
    const float a = lum(x, y) - lum(x - 1, y), b = lum(x + 1, y) - lum(x, y);
    return (a > 0.35f * step && b > 0.35f * step) || (a < -0.35f * step && b < -0.35f * step);
  };
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const size_t i = size_t(y) * size_t(w) + size_t(x);
      if (x + 1 < w) {
        const float dL = std::fabs(L[i + 1] - L[i]);
        const bool dEdge = depth && std::fabs(dep(x + 1, y) - dep(x, y)) > kDepthEdge;
        bool e = dEdge || dL > kLumaEdge;
        if (e && !dEdge && rampV(x, y, dL) && rampV(x + 1, y, dL)) e = false;
        s.edgeR[i] = e ? 1 : 0;
      }
      if (y + 1 < h) {
        const float dL = std::fabs(L[i + size_t(w)] - L[i]);
        const bool dEdge = depth && std::fabs(dep(x, y + 1) - dep(x, y)) > kDepthEdge;
        bool e = dEdge || dL > kLumaEdge;
        if (e && !dEdge && rampH(x, y, dL) && rampH(x, y + 1, dL)) e = false;
        s.edgeB[i] = e ? 1 : 0;
      }
    }
  }

  s.acc.assign(n * 4, 0.f);
  auto addBlend = [&](int px, int py, int qx, int qy, float cov) {
    if (cov <= 0.004f) return;  // sub-1/255 coverage is invisible
    float* a = &s.acc[(size_t(py) * size_t(w) + size_t(px)) * 4];
    const uint32_t c = s.src[size_t(qy) * size_t(w) + size_t(qx)];
    a[0] += cov * float((c >> 16) & 0xFF);
    a[1] += cov * float((c >> 8) & 0xFF);
    a[2] += cov * float(c & 0xFF);
    a[3] += cov;
  };
  // Integral of clamp(v(t), 0, 1) for v linear from v0 to v1 over t in [0,1]
  // (the positive part of the line offset across one pixel's extent).
  auto coverageIntegral = [](float v0, float v1) {
    const float a = std::clamp(v0, -1.f, 1.f), b = std::clamp(v1, -1.f, 1.f);
    if (a <= 0.f && b <= 0.f) return 0.f;
    if (a >= 0.f && b >= 0.f) return 0.5f * (std::min(a, 1.f) + std::min(b, 1.f));
    const float t = a / (a - b);  // zero crossing in (0,1)
    return a > 0.f ? 0.5f * t * a : 0.5f * (1.f - t) * b;
  };
  constexpr int kMaxRun = 32;

  // Vertical edges: edgeR(x,y) separates A=(x,y) from B=(x+1,y). The line
  // offset o is measured in pixels from the stored boundary (+ = toward B).
  for (int x = 0; x + 1 < w; ++x) {
    int y = 0;
    while (y < h) {
      if (!s.edgeR[size_t(y) * size_t(w) + size_t(x)]) { ++y; continue; }
      const int yTop = y;
      while (y + 1 < h && s.edgeR[size_t(y + 1) * size_t(w) + size_t(x)] &&
             y - yTop + 1 < kMaxRun)
        ++y;
      const int yBot = y;
      ++y;
      // End offsets from the adjacent columns' edges in the rows just
      // outside the run: the boundary stepped toward B (+0.5) or A (-0.5).
      float oTop = 0.f, oBot = 0.f;
      if (yTop > 0) {
        if (x + 2 < w && s.edgeR[size_t(yTop - 1) * size_t(w) + size_t(x + 1)]) oTop = 0.5f;
        else if (x > 0 && s.edgeR[size_t(yTop - 1) * size_t(w) + size_t(x - 1)]) oTop = -0.5f;
      }
      if (yBot + 1 < h) {
        if (x + 2 < w && s.edgeR[size_t(yBot + 1) * size_t(w) + size_t(x + 1)]) oBot = 0.5f;
        else if (x > 0 && s.edgeR[size_t(yBot + 1) * size_t(w) + size_t(x - 1)]) oBot = -0.5f;
      }
      const float runLen = float(yBot - yTop + 1);
      for (int yy = yTop; yy <= yBot; ++yy) {
        const float u0 = float(yy - yTop) / runLen, u1 = float(yy - yTop + 1) / runLen;
        const float oA = oTop + (oBot - oTop) * u0;  // line offset at pixel top
        const float oB = oTop + (oBot - oTop) * u1;  // ... and at pixel bottom
        const float covA = coverageIntegral(-oA, -oB);  // A covered by B's side
        const float covB = coverageIntegral(oA, oB);    // B covered by A's side
        addBlend(x, yy, x + 1, yy, covA);
        addBlend(x + 1, yy, x, yy, covB);
      }
    }
  }
  // Horizontal edges: edgeB(x,y) separates A=(x,y) from B=(x,y+1); the
  // offset o is measured from the stored boundary (+ = toward B, down).
  for (int y = 0; y + 1 < h; ++y) {
    int x = 0;
    while (x < w) {
      if (!s.edgeB[size_t(y) * size_t(w) + size_t(x)]) { ++x; continue; }
      const int xLeft = x;
      while (x + 1 < w && s.edgeB[size_t(y) * size_t(w) + size_t(x + 1)] &&
             x - xLeft + 1 < kMaxRun)
        ++x;
      const int xRight = x;
      ++x;
      float oLeft = 0.f, oRight = 0.f;
      if (xLeft > 0) {
        if (y + 2 < h && s.edgeB[size_t(y + 1) * size_t(w) + size_t(xLeft - 1)]) oLeft = 0.5f;
        else if (y > 0 && s.edgeB[size_t(y - 1) * size_t(w) + size_t(xLeft - 1)]) oLeft = -0.5f;
      }
      if (xRight + 1 < w) {
        if (y + 2 < h && s.edgeB[size_t(y + 1) * size_t(w) + size_t(xRight + 1)]) oRight = 0.5f;
        else if (y > 0 && s.edgeB[size_t(y - 1) * size_t(w) + size_t(xRight + 1)]) oRight = -0.5f;
      }
      const float runLen = float(xRight - xLeft + 1);
      for (int xx = xLeft; xx <= xRight; ++xx) {
        const float u0 = float(xx - xLeft) / runLen, u1 = float(xx - xLeft + 1) / runLen;
        const float oA = oLeft + (oRight - oLeft) * u0;
        const float oB = oLeft + (oRight - oLeft) * u1;
        const float covA = coverageIntegral(-oA, -oB);
        const float covB = coverageIntegral(oA, oB);
        addBlend(xx, y, xx, y + 1, covA);
        addBlend(xx, y + 1, xx, y, covB);
      }
    }
  }

  // Composite: out = src * (1 - min(sum cov, 1)) + accumulated blends.
  for (size_t i = 0; i < n; ++i) {
    const float* a = &s.acc[i * 4];
    if (a[3] <= 0.f) continue;
    const uint32_t c = s.src[i];
    float r = float((c >> 16) & 0xFF), g = float((c >> 8) & 0xFF), b = float(c & 0xFF);
    if (a[3] >= 1.f) {
      const float inv = 1.f / a[3];
      r = a[0] * inv;
      g = a[1] * inv;
      b = a[2] * inv;
    } else {
      const float keep = 1.f - a[3];
      r = r * keep + a[0];
      g = g * keep + a[1];
      b = b * keep + a[2];
    }
    fb.pixels[i] = packPx(r, g, b);
  }
}

void resolveSsaa2x(const SoftwareFramebuffer& hi, SoftwareFramebuffer& out) {
  const uint32_t w = out.width, h = out.height;
  if (hi.width != w * 2 || hi.height != h * 2) return;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint32_t r = 0, g = 0, b = 0;
      for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
          const uint32_t p = hi.pixels[size_t(2 * y + dy) * hi.width + (2 * x + dx)];
          r += (p >> 16) & 0xFF;
          g += (p >> 8) & 0xFF;
          b += p & 0xFF;
        }
      out.pixels[size_t(y) * w + x] = packPx(r * 0.25f, g * 0.25f, b * 0.25f);
    }
  }
}

}  // namespace litholattice
