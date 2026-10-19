// RGBA8 image in CPU memory (no GPU types). Decoding uses stb_image (PNG) and
// NanoSVG (SVG) privately.
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace litholattice {

class Image {
 public:
  Image() = default;
  /// `rgba` must hold width * height * 4 bytes (straight alpha).
  Image(int width, int height, std::vector<uint8_t> rgba);

  /// Decodes PNG bytes to RGBA8 (stb_image built PNG-only). nullopt + `err` on failure.
  static std::optional<Image> decode(std::span<const uint8_t> bytes, std::string* err);
  /// Decodes SVG bytes to RGBA8 by rasterizing at width x height (Litho SVG / NanoSVG).
  /// width/height <= 0 selects the SVG's intrinsic size. The SVG is fit (aspect kept)
  /// and centered; padding is transparent. nullopt + `err` on failure (malformed SVG).
  static std::optional<Image> decodeSvg(std::span<const uint8_t> bytes, int width, int height, std::string* err);
  /// Loads by file extension: `.svg` (case-insensitive) routes to decodeSvg at intrinsic
  /// size, everything else to decode (PNG). nullopt + `err` on failure.
  static std::optional<Image> load(const std::filesystem::path& file, std::string* err);
  /// Loads an SVG file rasterized at width x height (width/height <= 0: intrinsic size).
  static std::optional<Image> loadSvg(const std::filesystem::path& file, int width, int height, std::string* err);
  /// The magenta / black "missing texture" checker (4 x 4 cells).
  static Image checker(int size);

  int width() const { return w_; }
  int height() const { return h_; }
  bool empty() const { return px_.empty(); }
  const std::vector<uint8_t>& pixels() const { return px_; }
  /// Pointer to the RGBA of pixel (x, y); no bounds check beyond an assert.
  const uint8_t* pixel(int x, int y) const;

  /// Rows [y0, y0 + count) as a new image (one frame of a vertical animation strip).
  Image rows(int y0, int count) const;
  /// Alpha-weighted mean colour, 0..1 (HUD swatches). Magenta if fully transparent.
  std::array<float, 3> averageColor() const;
  /// FNV-1a over size and pixels; used to report which textures changed on hot reload.
  uint64_t hash() const;

  /// Encodes as an RGBA8 PNG. Uncompressed (zlib "stored" blocks): simple and dependency-free,
  /// meant for tools, tests and debug output rather than shipping assets.
  std::vector<uint8_t> encodePng() const;
  /// encodePng() written to `file`. False + `err` on I/O failure.
  bool savePng(const std::filesystem::path& file, std::string* err) const;

  bool operator==(const Image&) const = default;

 private:
  int w_ = 0, h_ = 0;
  std::vector<uint8_t> px_;
};

}  // namespace litholattice
