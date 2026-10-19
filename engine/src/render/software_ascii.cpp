// ASCII renderer pipeline — implementation (EXPERIMENT TIER, JD
// 2026-10-13). See software_ascii.h for the design and
// Ascii-Renderer-2026-10-13.md for the survey + economics.
#include "litholattice/render/software_ascii.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "litholattice/systerian/isysterian.h"

namespace litholattice {

const char* asciiGlyphSetName(AsciiGlyphSet set) {
  return set == AsciiGlyphSet::Systerian ? "systerian" : "ascii";
}

bool asciiGlyphSetFromName(const char* name, AsciiGlyphSet& out) {
  if (!name) return false;
  const std::string_view v(name);
  if (v == "ascii") { out = AsciiGlyphSet::Ascii; return true; }
  if (v == "systerian") { out = AsciiGlyphSet::Systerian; return true; }
  return false;
}

namespace {

// 5x7 bitmap font for the ten ramp glyphs, rows top-to-bottom, '#' = lit.
// Hand-drawn for this module; covers exactly asciiRamp()'s characters.
const char* glyphRows(char c, int row) {
  static const char* kSpace[7] = {".....", ".....", ".....", ".....",
                                  ".....", ".....", "....."};
  static const char* kDot[7] = {".....", ".....", ".....", ".....",
                                ".....", ".##..", ".##.."};
  static const char* kColon[7] = {".....", ".##..", ".##..", ".....",
                                  ".....", ".##..", ".##.."};
  static const char* kDash[7] = {".....", ".....", ".....", ".####",
                                 ".....", ".....", "....."};
  static const char* kEq[7] = {".....", ".....", ".####", ".....",
                               ".####", ".....", "....."};
  static const char* kPlus[7] = {".....", "..#..", "..#..", ".###.",
                                 "..#..", "..#..", "....."};
  static const char* kStar[7] = {".....", ".#.#.", "..#..", ".###.",
                                 "..#..", ".#.#.", "....."};
  static const char* kHash[7] = {".#.#.", ".#.#.", "#####", ".#.#.",
                                 "#####", ".#.#.", ".#.#."};
  static const char* kPct[7] = {"##..#", "##.#.", "...#.", "..#..",
                                ".#...", ".#.##", "#..##"};
  static const char* kAt[7] = {".###.", "#...#", "#.###", "#.#.#",
                               "#....", "#....", ".###."};
  switch (c) {
    case '.': return kDot[row];
    case ':': return kColon[row];
    case '-': return kDash[row];
    case '=': return kEq[row];
    case '+': return kPlus[row];
    case '*': return kStar[row];
    case '#': return kHash[row];
    case '%': return kPct[row];
    case '@': return kAt[row];
    default: return kSpace[row];
  }
}

struct CellAvg {
  float r = 0, g = 0, b = 0, luma = 0;
  char glyph = ' ';
  int level = 0;  ///< full-range value level in [0, span] (JD amendment 2026-10-13)
};

// Grid geometry shared by the text and image forms: cells tile the frame
// with a ~1:2 (w:h) character aspect. Returns false for degenerate input.
bool gridGeometry(const SoftwareFramebuffer& fb, int cols, int& outCols,
                  int& outRows, float& cellW, float& cellH) {
  if (fb.width < 8 || fb.height < 8) return false;
  outCols = std::clamp(cols, 8, int(fb.width));
  cellW = float(fb.width) / float(outCols);
  cellH = cellW * 2.f;
  outRows = std::max(1, int(std::lround(float(fb.height) / cellH)));
  cellH = float(fb.height) / float(outRows);  // exact tiling
  return true;
}

std::vector<CellAvg> summarize(const SoftwareFramebuffer& fb, int cols, int span,
                               int& outCols, int& outRows) {
  float cellW, cellH;
  if (!gridGeometry(fb, cols, outCols, outRows, cellW, cellH)) return {};
  const char* ramp = asciiRamp();
  std::vector<CellAvg> cells(size_t(outCols) * size_t(outRows));
  for (int cy = 0; cy < outRows; ++cy) {
    const int y0 = int(cy * cellH), y1 = std::max(y0 + 1, int((cy + 1) * cellH));
    for (int cx = 0; cx < outCols; ++cx) {
      const int x0 = int(cx * cellW), x1 = std::max(x0 + 1, int((cx + 1) * cellW));
      double r = 0, g = 0, b = 0;
      int n = 0;
      for (int y = y0; y < y1 && y < int(fb.height); ++y)
        for (int x = x0; x < x1 && x < int(fb.width); ++x) {
          const uint32_t p = fb.pixels[size_t(y) * fb.width + size_t(x)];
          r += (p >> 16) & 0xFF;
          g += (p >> 8) & 0xFF;
          b += p & 0xFF;
          ++n;
        }
      CellAvg c;
      if (n > 0) {
        c.r = float(r / n);
        c.g = float(g / n);
        c.b = float(b / n);
      }
      c.luma = (0.299f * c.r + 0.587f * c.g + 0.114f * c.b) * (1.f / 255.f);
      // JD-RULED AMENDMENT (2026-10-13): level at FULL span resolution
      // (default span 255 = 256 steps) — a deliberate exception to the
      // game's 64-scale conventions. The level selects the ramp glyph
      // AND is the Systerian glyph value; the tint beside it is the
      // cell's average framebuffer colour (the rendered, SVG-textured
      // variance), never a flat per-block colour.
      c.level = asciiValueLevel(c.luma, span);
      c.glyph = ramp[asciiRampIndex(c.level, span)];
      cells[size_t(cy) * size_t(outCols) + size_t(cx)] = c;
    }
  }
  return cells;
}

}  // namespace

std::string framebufferToAsciiText(const SoftwareFramebuffer& fb, int cols,
                                   AsciiGridInfo* info) {
  return framebufferToAsciiText(fb, cols, AsciiGlyphSet::Ascii, info);
}

std::string framebufferToAsciiText(const SoftwareFramebuffer& fb, int cols,
                                   AsciiGlyphSet set, AsciiGridInfo* info) {
  return framebufferToAsciiText(fb, cols, set, info, kAsciiDefaultValueSpan);
}

std::string framebufferToAsciiText(const SoftwareFramebuffer& fb, int cols,
                                   AsciiGlyphSet set, AsciiGridInfo* info, int span) {
  int gc = 0, gr = 0;
  const auto cells = summarize(fb, cols, span, gc, gr);
  std::string out;
  if (cells.empty()) return out;
  if (set == AsciiGlyphSet::Systerian) {
    // Glyph-value grid (JD amendment 2026-10-13): each cell is its
    // full-range LEVEL (0..span, default 0..255) — the value fed to
    // composedGlyph, i.e. the script's native encoding — as three
    // zero-padded fixed digits, exact and parseable. (The experiment
    // build emitted the ramp characters' code points, 32..64, two
    // digits; JD's ruling supersedes that subset.)
    out.reserve(size_t(gc * 3 + 1) * size_t(gr));
    for (int y = 0; y < gr; ++y) {
      for (int x = 0; x < gc; ++x) {
        const unsigned v = unsigned(
            cells[size_t(y) * size_t(gc) + size_t(x)].level);
        out += char('0' + (v / 100) % 10);
        out += char('0' + (v / 10) % 10);
        out += char('0' + v % 10);
      }
      out += '\n';
    }
  } else {
    out.reserve(size_t(gc + 1) * size_t(gr));
    for (int y = 0; y < gr; ++y) {
      for (int x = 0; x < gc; ++x) out += cells[size_t(y) * size_t(gc) + size_t(x)].glyph;
      out += '\n';
    }
  }
  if (info) {
    info->cols = gc;
    info->rows = gr;
    info->textBytes = out.size();
  }
  return out;
}

void framebufferToAsciiImage(SoftwareFramebuffer& fb, int cols,
                             std::string* textOut, AsciiGridInfo* info) {
  framebufferToAsciiImage(fb, cols, AsciiGlyphSet::Ascii, textOut, info);
}

void framebufferToAsciiImage(SoftwareFramebuffer& fb, int cols, AsciiGlyphSet set,
                             std::string* textOut, AsciiGridInfo* info) {
  framebufferToAsciiImage(fb, cols, set, textOut, info, kAsciiDefaultValueSpan);
}

void framebufferToAsciiImage(SoftwareFramebuffer& fb, int cols, AsciiGlyphSet set,
                             std::string* textOut, AsciiGridInfo* info, int span) {
  int gc = 0, gr = 0;
  const auto cells = summarize(fb, cols, span, gc, gr);
  if (cells.empty()) return;
  if (textOut) {
    *textOut = framebufferToAsciiText(fb, cols, set, info, span);
  } else if (info) {
    info->cols = gc;
    info->rows = gr;
    info->textBytes = (set == AsciiGlyphSet::Systerian ? size_t(gc * 3 + 1) : size_t(gc + 1)) *
                       size_t(gr);
  }
  if (set == AsciiGlyphSet::Systerian) {
    // Draw the real glyphs: the codec's generative stroke geometry
    // (100x140 grid) scaled into each cell, tinted by the cell's average
    // colour; rivet dots at every graph vertex (JD's rivet rule).
    const float cellW = float(fb.width) / float(gc);
    const float cellH = float(fb.height) / float(gr);
    std::vector<uint32_t> img(size_t(fb.width) * fb.height, 0xFF000000u);
    auto plot = [&](int x, int y, uint32_t tint) {
      if (x >= 0 && y >= 0 && x < int(fb.width) && y < int(fb.height))
        img[size_t(y) * fb.width + size_t(x)] = tint;
    };
    for (int cy = 0; cy < gr; ++cy) {
      for (int cx = 0; cx < gc; ++cx) {
        const CellAvg& c = cells[size_t(cy) * size_t(gc) + size_t(cx)];
        const uint32_t tint = 0xFF000000u |
                              (uint32_t(std::clamp(c.r, 0.f, 255.f)) << 16) |
                              (uint32_t(std::clamp(c.g, 0.f, 255.f)) << 8) |
                              uint32_t(std::clamp(c.b, 0.f, 255.f));
        // JD amendment (2026-10-13): the glyph IS the cell's full-range
        // level (0..span, default 0..255), composed directly by the
        // codec — not the ramp character's code point (32..64 subset).
        const systerian::SysterianGlyph g =
            systerian::composedGlyph(uint32_t(c.level));
        const float ox = cx * cellW, oy = cy * cellH;
        const float sx = cellW / float(systerian::kGridWidth);
        const float sy = cellH / float(systerian::kGridHeight);
        const int thick = std::max(1, int(std::lround(std::min(sx, sy) * 3.0)));
        for (const auto& seg : g.segments) {
          const float ax = ox + float(seg.a.x) * sx, ay = oy + float(seg.a.y) * sy;
          const float bx = ox + float(seg.b.x) * sx, by = oy + float(seg.b.y) * sy;
          const int steps = std::max(1, int(std::ceil(
              std::max(std::fabs(bx - ax), std::fabs(by - ay)))));
          for (int s = 0; s <= steps; ++s) {
            const int px = int(std::lround(ax + (bx - ax) * float(s) / float(steps)));
            const int py = int(std::lround(ay + (by - ay) * float(s) / float(steps)));
            for (int dy = 0; dy < thick; ++dy)
              for (int dx = 0; dx < thick; ++dx) plot(px + dx, py + dy, tint);
          }
        }
        const int rr = std::max(1, int(std::lround(5.4 * std::min(sx, sy))));
        for (const auto& p : g.rivets) {
          const int px = int(std::lround(ox + float(p.x) * sx));
          const int py = int(std::lround(oy + float(p.y) * sy));
          for (int dy = -rr; dy <= rr; ++dy)
            for (int dx = -rr; dx <= rr; ++dx)
              if (dx * dx + dy * dy <= rr * rr) plot(px + dx, py + dy, tint);
        }
      }
    }
    fb.pixels = std::move(img);
    return;
  }
  const float cellW = float(fb.width) / float(gc);
  const float cellH = float(fb.height) / float(gr);
  std::vector<uint32_t> img(size_t(fb.width) * fb.height, 0xFF000000u);
  for (int cy = 0; cy < gr; ++cy) {
    for (int cx = 0; cx < gc; ++cx) {
      const CellAvg& c = cells[size_t(cy) * size_t(gc) + size_t(cx)];
      if (c.glyph == ' ') continue;
      const uint32_t tint = 0xFF000000u | (uint32_t(std::clamp(c.r, 0.f, 255.f)) << 16) |
                            (uint32_t(std::clamp(c.g, 0.f, 255.f)) << 8) |
                            uint32_t(std::clamp(c.b, 0.f, 255.f));
      const int x0 = int(cx * cellW), x1 = int((cx + 1) * cellW);
      const int y0 = int(cy * cellH), y1 = int((cy + 1) * cellH);
      for (int y = y0; y < y1 && y < int(fb.height); ++y) {
        const int grow = std::min(6, int(float(y - y0) * 7.f / std::max(1.f, cellH)));
        const char* bits = glyphRows(c.glyph, grow);
        for (int x = x0; x < x1 && x < int(fb.width); ++x) {
          const int gcol = std::min(4, int(float(x - x0) * 5.f / std::max(1.f, cellW)));
          if (bits[gcol] == '#') img[size_t(y) * fb.width + size_t(x)] = tint;
        }
      }
    }
  }
  fb.pixels = std::move(img);
}

}  // namespace litholattice
