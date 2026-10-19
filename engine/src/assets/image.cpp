#include "litholattice/assets/image.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iterator>
#include "stb_image.h"
#include "vendor/nanosvg.h"
#include "vendor/nanosvgrast.h"

namespace litholattice {

Image::Image(int width, int height, std::vector<uint8_t> rgba) : w_(width), h_(height), px_(std::move(rgba)) {
  assert(px_.size() == size_t(w_) * size_t(h_) * 4);
}

std::optional<Image> Image::decode(std::span<const uint8_t> bytes, std::string* err) {
  if (bytes.empty() || bytes.size() > size_t(64) << 20) {  // stb takes an int length; textures are far below 64 MB
    if (err) *err = bytes.empty() ? "empty file" : "file too large";
    return std::nullopt;
  }
  int w = 0, h = 0, n = 0;
  // Decompression-bomb guard: probe dimensions BEFORE decoding. A gigapixel
  // PNG compresses to kilobytes but decodes to gigabytes. 8192px matches the
  // largest sane texture atlas; the SVG path already clamps at 2048.
  if (!stbi_info_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n)) {
    if (err) *err = stbi_failure_reason() ? stbi_failure_reason() : "decode failed";
    return std::nullopt;
  }
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192) {
    if (err) *err = "image dimensions out of bounds";
    return std::nullopt;
  }
  unsigned char* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
  if (!px) {
    if (err) *err = stbi_failure_reason() ? stbi_failure_reason() : "decode failed";
    return std::nullopt;
  }
  Image img(w, h, std::vector<uint8_t>(px, px + size_t(w) * size_t(h) * 4));
  stbi_image_free(px);
  return img;
}

namespace {

/// Case-insensitive ".svg" extension check (Litho SVG routing).
bool hasSvgExtension(const std::filesystem::path& p) {
  std::string ext = p.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return ext == ".svg";
}

}  // namespace

std::optional<Image> Image::decodeSvg(std::span<const uint8_t> bytes, int width, int height, std::string* err) {
  auto fail = [&](const char* why) -> std::optional<Image> {
    if (err) *err = why;
    return std::nullopt;
  };
  if (bytes.empty() || bytes.size() > size_t(64) << 20)
    return fail(bytes.empty() ? "empty file" : "file too large");
  std::string src(bytes.begin(), bytes.end());  // nsvgParse parses in place: private NUL-terminated copy
  src.push_back('\0');
  NSVGimage* svg = nsvgParse(src.data(), "px", 96.0f);
  if (!svg) return fail("SVG parse failed");
  int w = width, h = height;
  if (w <= 0 || h <= 0) {  // intrinsic size
    // Clamp in FLOAT before converting (P3-3, Appendix E M5): converting a
    // raw NanoSVG dimension like 1e30 to int first is out-of-range UB; the
    // old code converted first and clamped the (already-UB) result.
    w = int(std::ceil(std::clamp(svg->width, 1.0f, 2048.0f)));
    h = int(std::ceil(std::clamp(svg->height, 1.0f, 2048.0f)));
    if (w <= 0 || h <= 0) { nsvgDelete(svg); return fail("SVG has no drawable size"); }
  }
  w = std::clamp(w, 1, 2048);
  h = std::clamp(h, 1, 2048);
  if (svg->width <= 0 || svg->height <= 0) { nsvgDelete(svg); return fail("SVG has no drawable size"); }
  const float scale = std::min(float(w) / svg->width, float(h) / svg->height);  // fit, aspect kept
  const float tx = (float(w) - svg->width * scale) * 0.5f;  // centered; padding stays transparent
  const float ty = (float(h) - svg->height * scale) * 0.5f;
  std::vector<uint8_t> px(size_t(w) * size_t(h) * 4, 0);
  NSVGrasterizer* rast = nsvgCreateRasterizer();
  if (!rast) { nsvgDelete(svg); return fail("SVG rasterizer init failed"); }
  nsvgRasterize(rast, svg, tx, ty, scale, px.data(), w, h, w * 4);  // straight-alpha RGBA8
  nsvgDeleteRasterizer(rast);
  nsvgDelete(svg);
  return Image(w, h, std::move(px));
}

namespace {

/// Read a whole file with the decoder's 64 MiB cap enforced DURING the
/// read (P3-3, Appendix E M3): the old code slurped unboundedly and let
/// decode()/decodeSvg() reject oversize input only after a multi-GB file
/// was already resident. Chunked reads stop at cap+1 bytes.
std::optional<std::vector<uint8_t>> readFileCapped(const std::filesystem::path& file, std::string* err) {
  constexpr size_t kCap = size_t(64) << 20;
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    if (err) *err = "cannot open " + file.generic_string();
    return std::nullopt;
  }
  std::vector<uint8_t> bytes;
  std::array<char, 64 << 10> chunk;
  while (in) {
    in.read(chunk.data(), std::streamsize(chunk.size()));
    const std::streamsize n = in.gcount();
    if (n > 0) bytes.insert(bytes.end(), chunk.data(), chunk.data() + n);
    if (bytes.size() > kCap) {
      if (err) *err = "file too large";
      return std::nullopt;
    }
  }
  if (in.bad()) {
    if (err) *err = "read error: " + file.generic_string();
    return std::nullopt;
  }
  return bytes;
}

}  // namespace

std::optional<Image> Image::loadSvg(const std::filesystem::path& file, int width, int height, std::string* err) {
  const auto bytes = readFileCapped(file, err);
  if (!bytes) return std::nullopt;
  return decodeSvg(*bytes, width, height, err);
}

std::optional<Image> Image::load(const std::filesystem::path& file, std::string* err) {
  if (hasSvgExtension(file)) return loadSvg(file, 0, 0, err);  // intrinsic size
  const auto bytes = readFileCapped(file, err);
  if (!bytes) return std::nullopt;
  return decode(*bytes, err);
}

Image Image::checker(int size) {
  std::vector<uint8_t> px(size_t(size) * size_t(size) * 4);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      const bool m = ((x * 4 / size) + (y * 4 / size)) & 1;
      uint8_t* p = &px[(size_t(y) * size_t(size) + size_t(x)) * 4];
      p[0] = m ? 255 : 0; p[1] = 0; p[2] = m ? 255 : 0; p[3] = 255;
    }
  return Image(size, size, std::move(px));
}

const uint8_t* Image::pixel(int x, int y) const {
  assert(x >= 0 && x < w_ && y >= 0 && y < h_);
  return &px_[(size_t(y) * size_t(w_) + size_t(x)) * 4];
}

Image Image::rows(int y0, int count) const {
  assert(y0 >= 0 && count >= 0 && y0 + count <= h_);
  const size_t stride = size_t(w_) * 4;
  return Image(w_, count, std::vector<uint8_t>(px_.begin() + ptrdiff_t(stride * size_t(y0)),
                                                px_.begin() + ptrdiff_t(stride * size_t(y0 + count))));
}

std::array<float, 3> Image::averageColor() const {
  double s[3] = {0, 0, 0}, wsum = 0;
  for (size_t i = 0; i + 3 < px_.size(); i += 4) {
    const double a = px_[i + 3] / 255.0;
    for (int c = 0; c < 3; ++c) s[c] += a * px_[i + size_t(c)] / 255.0;
    wsum += a;
  }
  if (wsum <= 0) return {1.f, 0.f, 1.f};
  return {float(s[0] / wsum), float(s[1] / wsum), float(s[2] / wsum)};
}

uint64_t Image::hash() const {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&](uint8_t b) { h = (h ^ b) * 1099511628211ull; };
  for (int i = 0; i < 4; ++i) { mix(uint8_t(w_ >> (8 * i))); mix(uint8_t(h_ >> (8 * i))); }
  for (const uint8_t b : px_) mix(b);
  return h;
}

namespace {

void put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 3; i >= 0; --i) out.push_back(uint8_t(v >> (8 * i)));
}

uint32_t crc32(std::span<const uint8_t> bytes) {
  uint32_t c = 0xFFFFFFFFu;
  for (const uint8_t b : bytes) {
    c ^= b;
    for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

void pngChunk(std::vector<uint8_t>& png, const char (&type)[5], std::span<const uint8_t> data) {
  put32(png, uint32_t(data.size()));
  const size_t start = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  put32(png, crc32(std::span(png).subspan(start)));
}

}  // namespace

std::vector<uint8_t> Image::encodePng() const {
  const size_t rowBytes = size_t(w_) * 4;
  std::vector<uint8_t> raw;
  raw.reserve((rowBytes + 1) * size_t(h_));
  for (int y = 0; y < h_; ++y) {
    raw.push_back(0);  // filter: none
    const auto row = px_.begin() + std::ptrdiff_t(size_t(y) * rowBytes);
    raw.insert(raw.end(), row, row + std::ptrdiff_t(rowBytes));
  }
  std::vector<uint8_t> z{0x78, 0x01};
  uint32_t a = 1, b = 0;  // Adler-32
  for (const uint8_t ch : raw) {
    a = (a + ch) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t off = 0; off < raw.size() || off == 0; off += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - off);
    z.push_back(off + n >= raw.size() ? 1 : 0);
    z.insert(z.end(), {uint8_t(n & 255), uint8_t(n >> 8), uint8_t(~n & 255), uint8_t((~n >> 8) & 255)});
    z.insert(z.end(), raw.begin() + std::ptrdiff_t(off), raw.begin() + std::ptrdiff_t(off + n));
  }
  put32(z, (b << 16) | a);
  std::vector<uint8_t> ihdr;
  put32(ihdr, uint32_t(w_));
  put32(ihdr, uint32_t(h_));
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});  // 8-bit RGBA
  std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  pngChunk(png, "IHDR", ihdr);
  pngChunk(png, "IDAT", z);
  pngChunk(png, "IEND", {});
  return png;
}

bool Image::savePng(const std::filesystem::path& file, std::string* err) const {
  const std::vector<uint8_t> png = encodePng();
  std::ofstream f(file, std::ios::binary);
  f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
  if (!f) {
    if (err) *err = "cannot write " + file.string();
    return false;
  }
  return true;
}

}  // namespace litholattice
