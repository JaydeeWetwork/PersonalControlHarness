// ASCII renderer pipeline (EXPERIMENT TIER, JD 2026-10-13).
//
// A post stage on the software renderer's finished frame: the frame is
// summarized into a glyph grid — each cell's average luminance selects a
// character from a 10-step ramp, and its average colour tints the glyph.
//
// JD-RULED AMENDMENT (2026-10-13 15:52 EDT): the value resolution behind
// this stage spans the FULL 0-255 range (256 steps), NOT a 64-step ramp.
// This is a deliberate, JD-named EXCEPTION to the game's 64-scale
// conventions, so there is more variance in the texturing and look of
// blocks: the luminance -> level quantization runs at 256-step resolution
// (level feeds the ramp-glyph selection AND the tint), and the Systerian
// mapping feeds that full-range level (0..255) straight into the codec's
// composedGlyph — values are the script's native encoding, so a cell's
// glyph now carries units/tens/hundreds strokes instead of only the
// 32..64 code-point subset the ASCII layer produced. The span is a named
// setting (render.ascii_value_span, default 255 = full range).
//
// Cell colour: each cell's tint is the average of the FINISHED
// FRAMEBUFFER's pixels over the cell (full 0-255 RGB per channel) — the
// framebuffer itself carries the engine's SVG-texture-generator variance
// (block textures are SVG-generated and rasterized into the frame), so
// the stage samples rendered texture variance directly; no flat
// per-block colour is substituted anywhere in this stage.
// Two products:
//   - the raw text grid (the point for machine consumers: an agent / the
//     Daemon reading the world as text — orders of magnitude cheaper per
//     frame than pixels; see Ascii-Renderer-2026-10-13.md for the
//     bytes/frame table), and
//   - an image visualization of that grid (glyphs drawn with a tiny
//     built-in 5x7 bitmap font, no external font dependency), which is
//     what the software presenter shows when render.ascii is on.
// Purely presentational: reads the framebuffer, never touches sim state.
#pragma once

#include <cstdint>
#include <string>
#include "litholattice/render/software_renderer.h"  // SoftwareFramebuffer

namespace litholattice {

struct AsciiGridInfo {
  int cols = 0;
  int rows = 0;
  size_t textBytes = 0;  ///< bytes of the text grid, newlines included
};

/// The 10-step luminance ramp, darkest to brightest.
inline const char* asciiRamp() { return " .:-=+*#%@"; }

/// JD-RULED AMENDMENT (2026-10-13): the value span for this stage's
/// luminance -> level mapping. The shipped default is the FULL 0-255
/// range (256 steps) — a deliberate exception to the game's 64-scale
/// conventions (see the file-header note). Parameterized via the
/// render.ascii_value_span setting; clamped to [1, kAsciiMaxValueSpan].
inline constexpr int kAsciiMaxValueSpan = 255;
inline constexpr int kAsciiDefaultValueSpan = 255;

/// Quantize a luminance in [0,1] to a level in [0, span] at (span+1)-step
/// resolution. At the default span (255) a mid-gray luminance resolves to
/// level ~128 — far beyond the 64th step a 64-step mapping could reach.
inline int asciiValueLevel(float luma, int span) {
  span = span < 1 ? 1 : (span > kAsciiMaxValueSpan ? kAsciiMaxValueSpan : span);
  const float c = luma < 0.f ? 0.f : (luma > 1.f ? 1.f : luma);
  const int level = int(c * float(span + 1));
  return level > span ? span : level;
}

/// Ramp-glyph index (0..9) for a level at the given span: the visible
/// ASCII glyphs remain the 10 ramp characters, but the level that selects
/// among them (and the tint beside it) is computed at full span resolution.
inline int asciiRampIndex(int level, int span) {
  span = span < 1 ? 1 : (span > kAsciiMaxValueSpan ? kAsciiMaxValueSpan : span);
  const int idx = level * 10 / (span + 1);
  return idx > 9 ? 9 : idx;
}

/// Glyph set for the grid (JD 2026-10-13): plain ASCII ramp characters,
/// or SYSTERIAN — the engine's certificate-signing script, drawn from
/// the codec's generative stroke geometry (composedGlyph) — no glyph
/// assets. AMENDED (JD ruling 2026-10-13): the Systerian value is the
/// cell's full-range level (0..span, default 0..255), NOT the ramp
/// character's code point via the ASCII layer (the experiment build's
/// 32..64 subset). The ASCII layer in isysterian.h remains the codec's
/// certificate-text mapping; this renderer maps luminance -> value.
enum class AsciiGlyphSet : int { Ascii = 0, Systerian = 1 };

/// Stable cvar spellings for render.ascii_set ("ascii" | "systerian").
const char* asciiGlyphSetName(AsciiGlyphSet set);
bool asciiGlyphSetFromName(const char* name, AsciiGlyphSet& out);

/// Summarize `fb` into a text glyph grid with `cols` columns (clamped to
/// [8, fb.width]); rows follow the terminal character aspect (~1:2).
/// Returns the grid (one '\n' per row, including the last). In Systerian
/// mode the "text" grid is the glyph-VALUE grid: each cell is the
/// THREE-digit value of its full-range level (000..span, zero-padded,
/// fixed width), since Systerian has no Unicode block — the faithful
/// machine-readable form of the script, whose values are its native
/// encoding. (Amended 2026-10-13: the experiment build emitted the
/// two-digit code points of the 10 ramp characters, a 32..64 subset;
/// JD's ruling replaces that with the full 0-255 level span.)
/// `span` is the value span (render.ascii_value_span; default
/// kAsciiDefaultValueSpan = 255, the full range).
std::string framebufferToAsciiText(const SoftwareFramebuffer& fb, int cols,
                                   AsciiGridInfo* info = nullptr);
std::string framebufferToAsciiText(const SoftwareFramebuffer& fb, int cols,
                                   AsciiGlyphSet set, AsciiGridInfo* info = nullptr);
std::string framebufferToAsciiText(const SoftwareFramebuffer& fb, int cols,
                                   AsciiGlyphSet set, AsciiGridInfo* info,
                                   int span);

/// Replace `fb`'s pixels with the glyph visualization of its own grid
/// (average-colour glyphs on black) at `cols` columns: 5x7 bitmap glyphs
/// for Ascii, real Systerian stroke geometry for Systerian — drawn from
/// composedGlyph(level) with the cell's full-range level, so Systerian
/// cells show units/tens/hundreds strokes across the 0..span range.
/// The text grid itself is returned through `textOut` when non-null.
void framebufferToAsciiImage(SoftwareFramebuffer& fb, int cols,
                             std::string* textOut = nullptr,
                             AsciiGridInfo* info = nullptr);
void framebufferToAsciiImage(SoftwareFramebuffer& fb, int cols, AsciiGlyphSet set,
                             std::string* textOut = nullptr,
                             AsciiGridInfo* info = nullptr);
void framebufferToAsciiImage(SoftwareFramebuffer& fb, int cols, AsciiGlyphSet set,
                             std::string* textOut, AsciiGridInfo* info, int span);

}  // namespace litholattice
