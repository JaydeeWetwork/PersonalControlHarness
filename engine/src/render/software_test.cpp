// Software renderer test: renders a REAL quad through renderMesh() and
// verifies the rasterizer's output pixels, then saves a PPM for eyeballing.
// (P3-3, Appendix D L8: the old body built no geometry at all — the
// rasterizer calls were commented out — and "passed" by writing a PPM of
// a cleared buffer, proving only framebuffer allocation. Real coverage
// also lives in engine_tests (software sections) and the sprite host
// selftest; this TU is the standalone visual smoke test.)
#include "litholattice/render/software_test.h"
#include "litholattice/render/model_dither.h"
#include "litholattice/render/software_aa.h"
#include "litholattice/render/software_ascii.h"
#include "litholattice/render/software_renderer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace litholattice {

// Minimal PPM writer (P6, viewable anywhere; the historical name says PNG
// but the file was always a PPM — kept honest now).
static bool writePPM(const char* path, const SoftwareFramebuffer& fb) {
  FILE* ppm = std::fopen(path, "wb");
  if (!ppm) return false;
  std::fprintf(ppm, "P6\n%d %d\n255\n", fb.width, fb.height);
  for (uint32_t y = 0; y < fb.height; ++y) {
    for (uint32_t x = 0; x < fb.width; ++x) {
      uint32_t p = fb.pixels[size_t(y) * fb.width + x];
      uint8_t rgb[3] = {uint8_t((p >> 16) & 0xFF), uint8_t((p >> 8) & 0xFF), uint8_t(p & 0xFF)};
      std::fwrite(rgb, 1, 3, ppm);
    }
  }
  std::fclose(ppm);
  return true;
}

int testSoftwareRenderer() {
  SoftwareRenderer renderer;
  renderer.resize(400, 300);
  // The legacy pixel assertions below predate model dithering (JD
  // 2026-10-13) and use an ortho MVP whose clip w = 1 sits inside the
  // default fade band; disable the fade for them (the dither-specific
  // checks at the end of this test exercise it explicitly).
  renderer.setModelDither(false);

  // One quad through the real path: PackedVertex data + MVP, exactly as
  // renderFrame feeds stored chunk meshes. MVP maps block units [0,16]
  // to NDC [-1,1] on x/y, z passthrough, w = 1. The quad spans the full
  // 16x16 blocks at z = 0.5, top face (face 2 = faceColor's unshaded
  // face), wound in the mesher convention; value 200 -> flat gray 200.
  float mvp[16] = {};
  mvp[0] = 0.125f; mvp[5] = 0.125f; mvp[10] = 1.f;
  mvp[12] = -1.f; mvp[13] = -1.f; mvp[15] = 1.f;
  PackedVertex q[4] = {};
  const uint16_t c[4][3] = {{0, 0, 8}, {256, 0, 8}, {256, 256, 8}, {0, 256, 8}};
  for (int i = 0; i < 4; ++i) {
    q[i].x = c[i][0]; q[i].y = c[i][1]; q[i].z = c[i][2];
    q[i].faceAo = 2;    // top face
    q[i].value = 200;   // flat luminance
    q[i].mode = kShadeTex;
  }
  const uint32_t kClear = 0xFF1a1a2e;  // dark blue background
  renderer.clear(kClear);
  renderer.renderMesh(q, 4, mvp);

  const SoftwareFramebuffer& fb = renderer.framebuffer();
  size_t covered = 0;
  for (uint32_t p : fb.pixels)
    if (p != kClear) ++covered;
  const uint32_t center = fb.pixels[size_t(fb.height / 2) * fb.width + fb.width / 2];
  std::printf("Software renderer test: %dx%d framebuffer, %zu covered pixels, center 0x%08x\n",
              fb.width, fb.height, covered, center);

  // The quad covers the whole frame: nearly every pixel is the flat gray,
  // and the center pixel is exactly faceColor(2, 200, kShadeTex).
  if (covered < size_t(fb.width) * fb.height * 9 / 10) {
    std::printf("FAIL: quad covered only %zu pixels\n", covered);
    return 1;
  }
  if (center != 0xFFC8C8C8u) {
    std::printf("FAIL: center pixel 0x%08x, expected 0xffc8c8c8\n", center);
    return 1;
  }

  // --- Model dithering (JD 2026-10-13) -------------------------------------
  // Math: fade endpoints + IGN determinism/range/distribution.
  if (modelDitherFade(2.f, 2.f, 8.f) != 0.f || modelDitherFade(8.f, 2.f, 8.f) != 1.f ||
      modelDitherFade(5.f, 2.f, 8.f) != 0.5f) {
    std::printf("FAIL: modelDitherFade endpoints/midpoint wrong\n");
    return 1;
  }
  {
    double sum = 0;
    float mn = 1.f, mx = 0.f;
    for (int y = 0; y < 64; ++y)
      for (int x = 0; x < 64; ++x) {
        const float v = modelDitherIgn(x, y);
        if (v != modelDitherIgn(x, y)) {  // same pixel, same value, always
          std::printf("FAIL: modelDitherIgn not deterministic at (%d,%d)\n", x, y);
          return 1;
        }
        if (v < 0.f || v >= 1.f) {
          std::printf("FAIL: modelDitherIgn out of [0,1): %f\n", v);
          return 1;
        }
        sum += v;
        mn = std::min(mn, v);
        mx = std::max(mx, v);
      }
    const double mean = sum / (64.0 * 64.0);
    if (mean < 0.4 || mean > 0.6 || mn > 0.05f || mx < 0.95f) {
      std::printf("FAIL: modelDitherIgn distribution off (mean %.3f, min %.3f, max %.3f)\n",
                  mean, mn, mx);
      return 1;
    }
  }
  // Rasterizer: same quad, MVP w = 4 (constant view depth 4 blocks).
  float mvpFar[16];
  std::memcpy(mvpFar, mvp, sizeof mvp);
  mvpFar[15] = 4.f;
  auto renderCount = [&](bool on, float nearB, float farB) {
    renderer.setModelDither(on, nearB, farB);
    renderer.clear(kClear);
    renderer.renderMesh(q, 4, mvpFar);
    size_t n = 0;
    for (uint32_t p : renderer.framebuffer().pixels)
      if (p != kClear) ++n;
    return n;
  };
  const size_t fullCount = renderCount(false, 2.f, 8.f);
  if (fullCount == 0) {
    std::printf("FAIL: dither test setup: quad vanished with dither off\n");
    return 1;
  }
  if (renderCount(true, 8.f, 16.f) != 0) {  // depth 4 <= near: fully faded
    std::printf("FAIL: dither: quad at depth 4 survived band 8..16\n");
    return 1;
  }
  if (renderCount(true, 0.5f, 2.f) != fullCount) {  // depth 4 >= far: untouched
    std::printf("FAIL: dither: quad at depth 4 changed under band 0.5..2\n");
    return 1;
  }
  const size_t midCount = renderCount(true, 2.f, 8.f);  // fade(4) = 1/3 kept
  if (midCount == 0 || midCount >= fullCount ||
      midCount < fullCount / 5 || midCount > fullCount / 2) {
    std::printf("FAIL: dither: mid-band coverage %zu of %zu (expected ~1/3)\n",
                midCount, fullCount);
    return 1;
  }
  renderer.setModelDither(false);
  std::printf("Model dither checks: fade/IGN OK, mid-band coverage %zu/%zu\n",
              midCount, fullCount);

  // --- AA ladder (JD 2026-10-13) -------------------------------------------
  // Mode name roundtrip + unknown names rejected to a stable state.
  {
    SoftwareAaMode m;
    if (!softwareAaModeFromName("cmaa", m) || m != SoftwareAaMode::Cmaa ||
        !softwareAaModeFromName("fxaa", m) || m != SoftwareAaMode::Fxaa ||
        !softwareAaModeFromName("ssaa2x", m) || m != SoftwareAaMode::Ssaa2x ||
        !softwareAaModeFromName("off", m) || m != SoftwareAaMode::Off ||
        softwareAaModeFromName("banana", m)) {
      std::printf("FAIL: AA mode name roundtrip\n");
      return 1;
    }
    if (std::string(softwareAaModeName(SoftwareAaMode::Cmaa)) != "cmaa") {
      std::printf("FAIL: AA mode name for Cmaa\n");
      return 1;
    }
  }
  // Synthetic edge scene: left half white, right half black, one step edge;
  // a post pass must (a) leave flat interiors bit-untouched, (b) soften the
  // step (produce at least one intermediate value at the boundary).
  auto edgeScene = []() {
    SoftwareFramebuffer f;
    f.resize(32, 16);
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 32; ++x)
        f.pixels[size_t(y) * 32 + size_t(x)] = x < 16 ? 0xFFFFFFFFu : 0xFF000000u;
    return f;
  };
  // Diagonal scene for CMAA: a boundary marching one pixel every two rows
  // forces run-end turns, which is where restored coverage blends pixels.
  // (A perfectly axis-aligned edge has exact 0/1 coverage and correctly
  // blends NOTHING — checked explicitly below.)
  auto checkPost = [&](const char* name, bool useCmaa) {
    SoftwareFramebuffer f;
    f.resize(32, 16);
    std::vector<float> depth(size_t(32) * 16, 0.4f);
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 32; ++x) {
        const bool white = useCmaa ? (x < 8 + (y >> 1)) : (x < 16);
        f.pixels[size_t(y) * 32 + size_t(x)] = white ? 0xFFFFFFFFu : 0xFF000000u;
        if (!white) depth[size_t(y) * 32 + size_t(x)] = 0.6f;
      }
    const auto before = f.pixels;
    SoftwareAaScratch scratch;
    if (useCmaa)
      applyCmaaPost(f, depth.data(), scratch);
    else
      applyFxaaPost(f, scratch);
    // Flat interiors untouched (columns far from the boundary).
    for (int y = 0; y < 16; ++y)
      for (int x : {0, 2, 29, 31})
        if (f.pixels[size_t(y) * 32 + size_t(x)] != before[size_t(y) * 32 + size_t(x)]) {
          std::printf("FAIL: %s post changed flat interior at (%d,%d)\n", name, x, y);
          return false;
        }
    // At least one intermediate tone near the boundary.
    bool intermediate = false;
    for (size_t i = 0; i < f.pixels.size() && !intermediate; ++i) {
      const uint32_t p = f.pixels[i];
      if (p != 0xFFFFFFFFu && p != 0xFF000000u) intermediate = true;
    }
    if (!intermediate) {
      std::printf("FAIL: %s post left the edge binary\n", name);
      return false;
    }
    return true;
  };
  // CMAA conservatism: an axis-aligned edge is exact — no blend at all.
  {
    SoftwareFramebuffer f = edgeScene();
    const auto before = f.pixels;
    SoftwareAaScratch scratch;
    std::vector<float> depth(size_t(32) * 16, 0.5f);
    applyCmaaPost(f, depth.data(), scratch);
    if (f.pixels != before) {
      std::printf("FAIL: cmaa blended an exactly-aligned edge (not conservative)\n");
      return 1;
    }
  }
  if (!checkPost("fxaa", false)) return 1;
  if (!checkPost("cmaa", true)) return 1;
  // SSAA resolve: exact 2x2 average.
  {
    SoftwareFramebuffer hi, out;
    hi.resize(4, 4);
    out.resize(2, 2);
    for (auto& p : hi.pixels) p = 0xFF000000u;
    hi.pixels[0] = 0xFFFFFFFFu;  // one white in the first 2x2 -> 25% gray
    resolveSsaa2x(hi, out);
    const uint32_t expect = 0xFF000000u | (64u << 16) | (64u << 8) | 64u;
    if (out.pixels[0] != expect || out.pixels[1] != 0xFF000000u) {
      std::printf("FAIL: ssaa2x resolve average wrong (%08x)\n", out.pixels[0]);
      return 1;
    }
  }
  std::printf("AA ladder checks: names OK, fxaa/cmaa soften edges + keep flats, ssaa resolve OK\n");

  // --- ASCII pipeline (JD 2026-10-13) ---------------------------------------
  {
    SoftwareFramebuffer f;
    f.resize(64, 32);
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 64; ++x)
        f.pixels[size_t(y) * 64 + size_t(x)] = x < 32 ? 0xFF000000u : 0xFFFFFFFFu;
    AsciiGridInfo info;
    const std::string text = framebufferToAsciiText(f, 32, &info);
    if (info.cols != 32 || info.rows < 4 || info.textBytes != text.size()) {
      std::printf("FAIL: ascii grid geometry (%d cols, %d rows)\n", info.cols, info.rows);
      return 1;
    }
    // Left half dark -> ' ', right half bright -> '@', per row.
    const size_t rowLen = size_t(info.cols) + 1;
    for (int y = 0; y < info.rows; ++y) {
      if (text[size_t(y) * rowLen + 2] != ' ' || text[size_t(y) * rowLen + 29] != '@') {
        std::printf("FAIL: ascii ramp mapping wrong at row %d\n", y);
        return 1;
      }
    }
    framebufferToAsciiImage(f, 32);
    bool anyLit = false;
    for (uint32_t p : f.pixels)
      if (p != 0xFF000000u) anyLit = true;
    if (!anyLit) {
      std::printf("FAIL: ascii image drew nothing for the bright half\n");
      return 1;
    }
  }
  std::printf("ASCII checks: grid geometry + ramp mapping + image output OK\n");

  // --- Systerian glyph set (JD 2026-10-13; AMENDED by JD ruling 2026-10-13
  // 15:52 EDT: full 0-255 value span) --------------------------------------
  // The experiment build mapped a cell through the ASCII layer: ramp char
  // -> code point (' ' 32 .. '@' 64), so its pin read "mid-gray -> 58,
  // white -> 64". That 58 does NOT survive the ruling: it encoded the
  // 32..64 code-point subset, not the luminance. Under the full-range
  // mapping the value IS the luminance level (0..span, default 255):
  // 0xFF808080 (luma 128/255) -> level 128, white -> level 255 — both
  // beyond anything a 64-step mapping can produce (its ceiling is 64).
  {
    if (std::string(asciiGlyphSetName(AsciiGlyphSet::Systerian)) != "systerian") {
      std::printf("FAIL: systerian set name\n");
      return 1;
    }
    // Amendment pins: span default + level resolution + parameterization.
    if (kAsciiDefaultValueSpan != 255 || asciiValueLevel(0.f, 255) != 0 ||
        asciiValueLevel(1.f, 255) != 255 || asciiValueLevel(128.f / 255.f, 255) != 128 ||
        asciiValueLevel(128.f / 255.f, 255) <= 64 ||  // beyond the 64th step
        asciiValueLevel(128.f / 255.f, 64) != 32) {   // span is a real parameter
      std::printf("FAIL: ascii value span/level (default %d, mid %d, span64 %d)\n",
                  kAsciiDefaultValueSpan, asciiValueLevel(128.f / 255.f, 255),
                  asciiValueLevel(128.f / 255.f, 64));
      return 1;
    }
    SoftwareFramebuffer f;
    f.resize(64, 32);
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 64; ++x)
        f.pixels[size_t(y) * 64 + size_t(x)] = x < 32 ? 0xFF808080u : 0xFFFFFFFFu;
    AsciiGridInfo info;
    const std::string text =
        framebufferToAsciiText(f, 32, AsciiGlyphSet::Systerian, &info);
    if (info.textBytes != text.size() || text.size() != size_t(info.rows) * (32 * 3 + 1)) {
      std::printf("FAIL: systerian value grid size (%zu bytes)\n", text.size());
      return 1;
    }
    const size_t stride = 32 * 3 + 1;
    for (int y = 0; y < info.rows; ++y) {
      if (text.compare(size_t(y) * stride + 6, 3, "128") != 0 ||
          text.compare(size_t(y) * stride + 87, 3, "255") != 0) {
        std::printf("FAIL: systerian value mapping wrong at row %d\n", y);
        return 1;
      }
    }
    SoftwareFramebuffer fa = f, fs = f;
    framebufferToAsciiImage(fa, 32, AsciiGlyphSet::Ascii);
    framebufferToAsciiImage(fs, 32, AsciiGlyphSet::Systerian);
    if (fa.pixels == fs.pixels) {
      std::printf("FAIL: systerian image identical to ascii image\n");
      return 1;
    }
    // Glyph 128 (hundreds+tens+units strokes) has strokes: the (tinted)
    // left half must show lit pixels, where the ASCII set's '+' shows far
    // fewer.
    size_t fsLit = 0, faLit = 0;
    for (int y = 0; y < 32; ++y)
      for (int x = 0; x < 32; ++x) {
        if (fs.pixels[size_t(y) * 64 + size_t(x)] != 0xFF000000u) ++fsLit;
        if (fa.pixels[size_t(y) * 64 + size_t(x)] != 0xFF000000u) ++faLit;
      }
    if (fsLit == 0 || fsLit <= faLit) {
      std::printf("FAIL: systerian glyph strokes missing (lit %zu vs ascii %zu)\n", fsLit,
                  faLit);
      return 1;
    }
    std::printf("Systerian checks: value grid 128/255 OK (full 0-255 span, JD "
                "2026-10-13), glyph strokes drawn (lit %zu)\n",
                fsLit);
  }

  if (writePPM("/tmp/sw_test.ppm", fb)) {
    std::printf("Wrote /tmp/sw_test.ppm\n");
    return 0;
  }
  std::printf("Failed to write output\n");
  return 1;
}

}  // namespace litholattice
