# Systerian reference masters (test fixture)

These 39 SVGs are reference output from `generate.py`, the original Python
glyph generator for the LithoKraft certificate-signing script. The script is
based on 13th-century Cistercian numerals. `engine_systerian` parses their
stroke endpoints and rivet centres and checks that the C++ codec reproduces
them glyph for glyph.

- `svg/violet/on_dark/` holds the 36 component glyphs (digits 1-9 in the
  units, tens, hundreds and thousands places).
- `composed/on_dark/` holds three composed values: 1111, 2026 and 9999.

Only the files the self-test reads are bundled. Running `python generate.py`
regenerates the full set (both colourways, PNGs and specimen sheets) next to
the script. Set the `SYSTERIAN_REFERENCE_DIR` environment variable to point
the test at another copy.

Author: JaydeeWetwork / Wet Wafer Works Games Incorporated. MIT licensed,
like the rest of this repository.
