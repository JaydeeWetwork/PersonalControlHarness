# Litho SVG

SVG as a first-class texture source for the LithoLattice engine. Any texture in a
resource pack (blocks, items, UI, particles) may be authored as `.svg`; the engine
rasterizes it into the texture pipeline at the pack's tile size, so one vector asset
ships crisply at every pack resolution (pack_32, pack_64, …).

## How it works

- `Image::decodeSvg(bytes, width, height, err)` rasterizes SVG bytes to an RGBA8
  `Image` (straight alpha, same contract as PNG decoding). `width`/`height` ≤ 0 selects
  the SVG's intrinsic size.
- `Image::load(path, …)` routes `.svg` files (case-insensitive extension) to the SVG
  decoder at intrinsic size; `Image::loadSvg(path, width, height, …)` rasterizes at an
  explicit size.
- The resource loader detects `.svg` texture paths and rasterizes at the pack's
  `tile_size` (tile × tile), so registry size validation passes unchanged. No manifest
  format change — extension-driven.
- Raster cache (`SvgRasterCache`, see `resource_loader.h`): one rasterization per
  (absolute path, tile size, file size, mtime) — repeated pack loads and UI reuse don't
  re-rasterize, and hot-reloaded files re-rasterize because the mtime key changes.

## Rasterizer: NanoSVG (vendored)

`vendor/nanosvg.h` + `vendor/nanosvgrast.h` (Mikko Mononen, zlib/libpng license —
header kept intact, see `THIRD_PARTY_LICENSES.md` at the repo root). Built as
`litholattice_nanosvg` via `svg/nanosvg_impl.c`, following the `stb_image_impl.c`
pattern (third-party TU, engine warning flags off).

## Documented limits

Litho SVG covers the vector subset our art needs: paths, basic shapes
(`rect`/`circle`/`ellipse`/`line`/`polyline`/`polygon`), flat fills and gradients,
transforms, viewBox. Not supported:

- SMIL/CSS animation — textures are stills; use the animation-strip system instead.
- Filters (`feGaussianBlur` etc.) — bake them into the asset before shipping.
- `<text>` layout — convert text to paths before shipping (no font stack in-engine).
- External references (`<use href="…">` across files, external images/stylesheets).

Undecodable SVG → `nullopt` + `err` → the magenta missing-texture path, exactly like a
bad PNG. Failure semantics are unchanged everywhere else.
