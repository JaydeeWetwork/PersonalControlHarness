// Glue header: maps the sprite module's shade onto the render module's
// ShadeMode. This is the ONLY sprite header that includes renderer.h —
// the sprite core stays free of render dependencies (the renderer links
// half the engine and will consume SpriteQuads itself), so consumers who
// speak render types include this where they need the translation.
#pragma once

#include "litholattice/render/renderer.h"
#include "litholattice/sprite/sprite.h"

namespace litholattice::sprite {

/// The render ShadeMode a sprite shade draws with:
///   Cutout     -> kShadeCutout     (alpha test; the renderer's sprite mode)
///   AlphaBlend -> kShadeAlphaBlend (src_alpha, 1-src_alpha; sorted)
///   Additive   -> kShadeAdditive   (src_alpha, 1; particles/glow)
inline ShadeMode shadeModeFor(SpriteShade shade) {
  switch (shade) {
    case SpriteShade::Cutout: return kShadeCutout;
    case SpriteShade::AlphaBlend: return kShadeAlphaBlend;
    case SpriteShade::Additive: return kShadeAdditive;
  }
  return kShadeCutout;
}

}  // namespace litholattice::sprite
