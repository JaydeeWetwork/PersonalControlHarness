// Graphics quality presets: transparency method selection per tier.
// Engine-level, exposed to LithoKraft and mods.
#pragma once
#include "litholattice/render/transparency.h"

namespace litholattice {

/// Graphics quality tier.
enum class GraphicsTier {
  Low,      // Maximum performance (Chromebooks, old hardware)
  Medium,   // Balanced
  High,     // Good GPUs
  Ultra,    // Best visual quality
};

/// Transparency method for glass at a given tier.
inline TransparencyMethod glassMethod(GraphicsTier tier) {
  switch (tier) {
    case GraphicsTier::Low: return TransparencyMethod::ScreenDoor;
    case GraphicsTier::Medium: return TransparencyMethod::ScreenDoor;
    case GraphicsTier::High: return TransparencyMethod::AlphaBlend;
    case GraphicsTier::Ultra: return TransparencyMethod::WeightedOIT;
  }
  return TransparencyMethod::ScreenDoor;
}

/// Transparency method for water at a given tier.
inline TransparencyMethod waterMethod(GraphicsTier tier) {
  switch (tier) {
    case GraphicsTier::Low: return TransparencyMethod::ScreenDoor;
    case GraphicsTier::Medium: return TransparencyMethod::AlphaBlend;
    case GraphicsTier::High: return TransparencyMethod::AlphaBlend;
    case GraphicsTier::Ultra: return TransparencyMethod::WeightedOIT;
  }
  return TransparencyMethod::ScreenDoor;
}

/// Transparency method for particles at a given tier.
inline TransparencyMethod particleMethod(GraphicsTier tier) {
  // Particles are always additive — it's the fastest and looks best.
  (void)tier;
  return TransparencyMethod::Additive;
}

/// Tinted glass: always alpha blend (color accuracy matters).
inline TransparencyMethod tintedGlassMethod(GraphicsTier) {
  return TransparencyMethod::AlphaBlend;
}

}  // namespace litholattice
