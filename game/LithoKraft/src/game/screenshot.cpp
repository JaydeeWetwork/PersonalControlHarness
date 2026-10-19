// Screenshot capture pipeline for the lithoscript `screenshot` command.
// LF line endings.
#include "lithokraft/game/screenshot.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

#include "litholattice/mcp/mcp_senses.h"  // encodePngRgb (clean-room PNG writer)
#include "litholattice/render/renderer.h"

namespace lithokraft {
namespace {

constexpr int kPngMaxDim = 4096;  // matches encodePngRgb's sanity cap

}  // namespace

void downscaleBoxAverage(const uint8_t* src, int srcW, int srcH,
                         std::vector<uint8_t>& dst, int dstW, int dstH) {
  dst.assign(size_t(dstW) * size_t(dstH) * 3, 0);
  if (!src || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;
  for (int dy = 0; dy < dstH; ++dy) {
    // Source box for this row: [y0, y1), at least 1 px tall.
    int y0 = int(int64_t(dy) * srcH / dstH);
    int y1 = int(int64_t(dy + 1) * srcH / dstH);
    if (y1 <= y0) y1 = y0 + 1;
    if (y0 >= srcH) y0 = srcH - 1;
    if (y1 > srcH) y1 = srcH;
    for (int dx = 0; dx < dstW; ++dx) {
      int x0 = int(int64_t(dx) * srcW / dstW);
      int x1 = int(int64_t(dx + 1) * srcW / dstW);
      if (x1 <= x0) x1 = x0 + 1;
      if (x0 >= srcW) x0 = srcW - 1;
      if (x1 > srcW) x1 = srcW;
      uint64_t sum[3] = {0, 0, 0};
      for (int sy = y0; sy < y1; ++sy) {
        const uint8_t* row = src + size_t(sy) * size_t(srcW) * 3;
        for (int sx = x0; sx < x1; ++sx) {
          sum[0] += row[size_t(sx) * 3 + 0];
          sum[1] += row[size_t(sx) * 3 + 1];
          sum[2] += row[size_t(sx) * 3 + 2];
        }
      }
      const uint64_t n = uint64_t(y1 - y0) * uint64_t(x1 - x0);
      uint8_t* d = dst.data() + (size_t(dy) * size_t(dstW) + size_t(dx)) * 3;
      d[0] = uint8_t(sum[0] / n);
      d[1] = uint8_t(sum[1] / n);
      d[2] = uint8_t(sum[2] / n);
    }
  }
}

bool captureScreenshot(litholattice::IRenderer& r, bool lowres, int visionW,
                       int visionH, CapturedShot& out, std::string* err) {
  std::vector<uint8_t> rgb;
  int w = 0, h = 0;
  if (!r.grabFramebuffer(rgb, w, h)) {
    if (err)
      *err = "framebuffer grab not available (renderer unsupported, or no "
             "armed frame rendered yet)";
    return false;
  }
  if (w > kPngMaxDim || h > kPngMaxDim) {
    if (err)
      *err = "framebuffer too large for PNG (" + std::to_string(w) + "x" +
             std::to_string(h) + ")";
    return false;
  }
  const uint8_t* px = rgb.data();
  int pw = w, ph = h;
  std::vector<uint8_t> small;
  if (lowres && visionW > 0 && visionH > 0 && visionW <= kPngMaxDim &&
      visionH <= kPngMaxDim && (visionW != w || visionH != h)) {
    downscaleBoxAverage(rgb.data(), w, h, small, visionW, visionH);
    px = small.data();
    pw = visionW;
    ph = visionH;
  }
  std::vector<uint8_t> png;
  if (!litholattice::mcp::encodePngRgb(pw, ph, px, png)) {
    if (err) *err = "PNG encode failed";
    return false;
  }
  out.width = pw;
  out.height = ph;
  out.png = std::move(png);
  return true;
}

std::string autoScreenshotPath(bool lowres) {
  const std::time_t t =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[64];
  std::snprintf(buf, sizeof buf, "screenshots/lithokraft-%04d%02d%02d-%02d%02d%02d%s.png",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, lowres ? "-lowres" : "");
  return buf;
}

ScreenshotPathResolution resolveScreenshotPath(const std::filesystem::path& root,
                                                 const std::string& requested,
                                                 bool lowres) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path canonRoot = fs::weakly_canonical(root, ec);
  const fs::path rootPath = ec ? root.lexically_normal() : canonRoot;
  // The confined default: the auto-name's filename directly under the root.
  const fs::path fallback =
      rootPath / fs::path(autoScreenshotPath(lowres)).filename();
  if (requested.empty()) return {fallback, false};

  const fs::path req(requested);
  if (req.is_absolute()) return {fallback, true};  // absolute: refused
  const fs::path joined = fs::weakly_canonical(rootPath / req, ec);
  if (ec) return {fallback, true};
  // Component-wise containment (mirrors ResourcePack::has): the resolved
  // path must have the root as a proper prefix and name a file below it.
  auto ri = rootPath.begin();
  auto ji = joined.begin();
  for (; ri != rootPath.end(); ++ri, ++ji) {
    if (ji == joined.end() || *ji != *ri) return {fallback, true};
  }
  if (ji == joined.end()) return {fallback, true};  // the root itself, no file
  return {joined, false};
}

bool writeFileBytes(const std::string& path,
                    const std::vector<uint8_t>& bytes, std::string* err) {
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  if (ec) {
    if (err) *err = "cannot create directory: " + ec.message();
    return false;
  }
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    if (err) *err = "cannot open '" + path + "' for writing";
    return false;
  }
  const size_t n =
      std::fwrite(bytes.data(), 1, bytes.size(), f);
  const bool ok = n == bytes.size() && std::fclose(f) == 0;
  if (!ok) {
    std::fclose(f);
    if (err) *err = "write failed for '" + path + "'";
    return false;
  }
  return true;
}

}  // namespace lithokraft
