// texture_recipe.cpp: line-based recipe text <-> TextureRecipe.
//
// Format: one node per line, first token is the node name, the rest are
// key=value pairs (no spaces inside a value). '#' starts a comment line,
// blank lines are ignored. Unknown nodes / keys / malformed values are
// errors (reported with the 1-based line number), never silently dropped,
// so authored recipes cannot quietly change meaning.
//
// Node vocabulary (params in parentheses; defaults in texture_recipe.h):
//   noise    (kind=value|gradient, scale, octaves, lacunarity, gain, seed)
//   voronoi  (scale, mode=f1|edge, seed)
//   warp     (strength, scale, octaves, lacunarity, gain, seed)
//   ridged   ()
//   billow   ()
//   curve    (contrast, gamma, posterize, invert, clamp_min, clamp_max)
//            -- "remap" is accepted as an alias and serialises as "curve".
//   blend    (mode=multiply|add|min|max|screen, mix)
//   grid     (cells, width, sharpness)
//   constant (value)
//   palette  (stops=pos:#rrggbb[,pos:#rrggbbaa...])
//   glow     (threshold, intensity, color=#rrggbb[aa])
//   dither   (amount)
#include "litholattice/texture/texture_recipe.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <utility>

namespace litholattice {
namespace {

bool fail(std::string* err, int line, const std::string& what) {
  if (err) *err = "line " + std::to_string(line) + ": " + what;
  return false;
}

std::string trim(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

std::vector<std::string> splitWs(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    size_t j = i;
    while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j;
  }
  return out;
}

std::vector<std::string> splitChar(const std::string& s, char c) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == c) {
      out.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  return out;
}

bool parseFloat(std::string_view s, float* out) {
  if (s.empty()) return false;
  const std::string str(s);
  char* end = nullptr;
  const float v = std::strtof(str.c_str(), &end);
  if (end != str.c_str() + str.size() || !std::isfinite(v)) return false;
  *out = v;
  return true;
}

bool parseInt(std::string_view s, int* out) {
  if (s.empty()) return false;
  const std::string str(s);
  char* end = nullptr;
  const long v = std::strtol(str.c_str(), &end, 10);
  if (end != str.c_str() + str.size() || v < -1000000 || v > 1000000) return false;
  *out = static_cast<int>(v);
  return true;
}

bool parseBool(std::string_view s, bool* out) {
  if (s == "1" || s == "true") { *out = true; return true; }
  if (s == "0" || s == "false") { *out = false; return true; }
  return false;
}

int hexDigit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "#rrggbb" or "#rrggbbaa" (the leading '#' is optional on input).
bool parseColor(std::string_view s, TextureColor* out) {
  if (!s.empty() && s[0] == '#') s.remove_prefix(1);
  if (s.size() != 6 && s.size() != 8) return false;
  int v[8];
  for (size_t i = 0; i < s.size(); ++i) {
    v[i] = hexDigit(s[i]);
    if (v[i] < 0) return false;
  }
  auto byte = [&](int i) { return static_cast<uint8_t>(v[2 * i] * 16 + v[2 * i + 1]); };
  out->r = byte(0);
  out->g = byte(1);
  out->b = byte(2);
  out->a = s.size() == 8 ? byte(3) : uint8_t{255};
  return true;
}

std::string colorToText(const TextureColor& c) {
  char buf[16];
  if (c.a == 255)
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x", c.r, c.g, c.b);
  else
    std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", c.r, c.g, c.b, c.a);
  return buf;
}

// %.9g round-trips any float exactly (FLT_DECIMAL_DIG), keeping text short
// for the common authored values ("2", "0.5", "1.25").
std::string floatToText(float v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(v));
  return buf;
}

// Key/value view over one line's tokens[1..]; rejects duplicates so a typo'd
// repeated key cannot silently win.
struct Params {
  std::vector<std::pair<std::string, std::string>> kv;
  const std::string* get(const std::string& key) const {
    for (const auto& p : kv)
      if (p.first == key) return &p.second;
    return nullptr;
  }
  bool hasUnknown(std::initializer_list<const char*> known, std::string* bad) const {
    for (const auto& p : kv) {
      bool ok = false;
      for (const char* k : known) ok = ok || p.first == k;
      if (!ok) { *bad = p.first; return true; }
    }
    return false;
  }
};

bool getFloat(const Params& p, const char* key, float* dst, std::string* err, int line) {
  const std::string* v = p.get(key);
  if (!v) return true;
  if (!parseFloat(*v, dst)) return fail(err, line, "bad number for '" + std::string(key) + "': " + *v);
  return true;
}
bool getInt(const Params& p, const char* key, int* dst, std::string* err, int line) {
  const std::string* v = p.get(key);
  if (!v) return true;
  if (!parseInt(*v, dst)) return fail(err, line, "bad integer for '" + std::string(key) + "': " + *v);
  return true;
}
bool getBool(const Params& p, const char* key, bool* dst, std::string* err, int line) {
  const std::string* v = p.get(key);
  if (!v) return true;
  if (!parseBool(*v, dst)) return fail(err, line, "bad boolean for '" + std::string(key) + "': " + *v);
  return true;
}

bool parseLine(const std::string& line, int lineNo, TextureRecipe* out, std::string* err) {
  const std::vector<std::string> tok = splitWs(line);
  if (tok.empty()) return true;
  const std::string& name = tok[0];
  Params p;
  for (size_t i = 1; i < tok.size(); ++i) {
    const size_t eq = tok[i].find('=');
    if (eq == std::string::npos) return fail(err, lineNo, "expected key=value, got '" + tok[i] + "'");
    const std::string key = tok[i].substr(0, eq);
    if (p.get(key)) return fail(err, lineNo, "duplicate key '" + key + "'");
    p.kv.emplace_back(key, tok[i].substr(eq + 1));
  }
  std::string bad;
  TextureNode n;
  auto unknown = [&](std::initializer_list<const char*> known) {
    return p.hasUnknown(known, &bad) ? fail(err, lineNo, "unknown key '" + bad + "' for node '" + name + "'") : true;
  };
  auto genParams = [&]() {
    return getFloat(p, "scale", &n.scale, err, lineNo) && getInt(p, "octaves", &n.octaves, err, lineNo) &&
           getFloat(p, "lacunarity", &n.lacunarity, err, lineNo) && getFloat(p, "gain", &n.gain, err, lineNo) &&
           getInt(p, "seed", &n.seedOffset, err, lineNo);
  };

  if (name == "noise") {
    n.type = TextureNodeType::Noise;
    if (!unknown({"kind", "scale", "octaves", "lacunarity", "gain", "seed"})) return false;
    if (const std::string* k = p.get("kind")) {
      if (*k == "value") n.noiseKind = NoiseKind::Value;
      else if (*k == "gradient") n.noiseKind = NoiseKind::Gradient;
      else return fail(err, lineNo, "noise kind must be value|gradient, got '" + *k + "'");
    }
    if (!genParams()) return false;
  } else if (name == "voronoi") {
    n.type = TextureNodeType::Voronoi;
    if (!unknown({"scale", "mode", "seed"})) return false;
    if (const std::string* m = p.get("mode")) {
      if (*m == "f1") n.voronoiMode = VoronoiMode::F1;
      else if (*m == "edge") n.voronoiMode = VoronoiMode::Edge;
      else return fail(err, lineNo, "voronoi mode must be f1|edge, got '" + *m + "'");
    }
    if (!getFloat(p, "scale", &n.scale, err, lineNo) || !getInt(p, "seed", &n.seedOffset, err, lineNo)) return false;
  } else if (name == "warp") {
    n.type = TextureNodeType::Warp;
    if (!unknown({"strength", "scale", "octaves", "lacunarity", "gain", "seed"})) return false;
    if (!getFloat(p, "strength", &n.strength, err, lineNo) || !genParams()) return false;
  } else if (name == "ridged") {
    n.type = TextureNodeType::Ridged;
    if (!unknown({})) return false;
  } else if (name == "billow") {
    n.type = TextureNodeType::Billow;
    if (!unknown({})) return false;
  } else if (name == "curve" || name == "remap") {
    n.type = TextureNodeType::Curve;
    if (!unknown({"contrast", "gamma", "posterize", "invert", "clamp_min", "clamp_max"})) return false;
    if (!getFloat(p, "contrast", &n.contrast, err, lineNo) || !getFloat(p, "gamma", &n.gamma, err, lineNo) ||
        !getInt(p, "posterize", &n.posterize, err, lineNo) || !getBool(p, "invert", &n.invert, err, lineNo) ||
        !getFloat(p, "clamp_min", &n.clampMin, err, lineNo) || !getFloat(p, "clamp_max", &n.clampMax, err, lineNo))
      return false;
  } else if (name == "blend") {
    n.type = TextureNodeType::Blend;
    if (!unknown({"mode", "mix"})) return false;
    if (const std::string* m = p.get("mode")) {
      if (*m == "multiply") n.blendMode = BlendMode::Multiply;
      else if (*m == "add") n.blendMode = BlendMode::Add;
      else if (*m == "min") n.blendMode = BlendMode::Min;
      else if (*m == "max") n.blendMode = BlendMode::Max;
      else if (*m == "screen") n.blendMode = BlendMode::Screen;
      else return fail(err, lineNo, "blend mode must be multiply|add|min|max|screen, got '" + *m + "'");
    }
    if (!getFloat(p, "mix", &n.mix, err, lineNo)) return false;
  } else if (name == "grid") {
    n.type = TextureNodeType::Grid;
    if (!unknown({"cells", "width", "sharpness"})) return false;
    if (!getInt(p, "cells", &n.cells, err, lineNo) || !getFloat(p, "width", &n.lineWidth, err, lineNo) ||
        !getFloat(p, "sharpness", &n.sharpness, err, lineNo))
      return false;
  } else if (name == "constant") {
    n.type = TextureNodeType::Constant;
    if (!unknown({"value"})) return false;
    if (!getFloat(p, "value", &n.value, err, lineNo)) return false;
  } else if (name == "palette") {
    n.type = TextureNodeType::Palette;
    if (!unknown({"stops"})) return false;
    const std::string* s = p.get("stops");
    if (!s) return fail(err, lineNo, "palette needs stops=pos:#color[,pos:#color...]");
    for (const std::string& part : splitChar(*s, ',')) {
      const size_t colon = part.find(':');
      if (colon == std::string::npos) return fail(err, lineNo, "bad palette stop '" + part + "' (want pos:#color)");
      GradientStop stop;
      if (!parseFloat(part.substr(0, colon), &stop.pos)) return fail(err, lineNo, "bad palette stop position in '" + part + "'");
      if (!parseColor(part.substr(colon + 1), &stop.color)) return fail(err, lineNo, "bad palette stop color in '" + part + "'");
      n.stops.push_back(stop);
    }
    if (n.stops.empty()) return fail(err, lineNo, "palette needs at least one stop");
  } else if (name == "glow") {
    n.type = TextureNodeType::Glow;
    if (!unknown({"threshold", "intensity", "color"})) return false;
    if (!getFloat(p, "threshold", &n.threshold, err, lineNo) || !getFloat(p, "intensity", &n.intensity, err, lineNo)) return false;
    if (const std::string* c = p.get("color")) {
      if (!parseColor(*c, &n.glowColor)) return fail(err, lineNo, "bad glow color '" + *c + "'");
    }
  } else if (name == "dither") {
    n.type = TextureNodeType::Dither;
    if (!unknown({"amount"})) return false;
    if (!getFloat(p, "amount", &n.amount, err, lineNo)) return false;
  } else {
    return fail(err, lineNo, "unknown node '" + name + "'");
  }

  // Range validation: keep authored values inside the domain the generator
  // is specified for, so generation never has to guess.
  if ((n.type == TextureNodeType::Noise || n.type == TextureNodeType::Voronoi || n.type == TextureNodeType::Warp) &&
      !(n.scale > 0.0f && n.scale <= 1024.0f))
    return fail(err, lineNo, "scale must be in (0, 1024]");
  if (n.octaves < 1 || n.octaves > 16) return fail(err, lineNo, "octaves must be in [1, 16]");
  if (n.posterize < 0 || n.posterize > 256) return fail(err, lineNo, "posterize must be in [0, 256]");
  if (n.cells < 1 || n.cells > 4096) return fail(err, lineNo, "grid cells must be in [1, 4096]");
  for (const GradientStop& s : n.stops)
    if (!(s.pos >= 0.0f && s.pos <= 1.0f)) return fail(err, lineNo, "palette stop positions must be in [0, 1]");
  // Numeric bands for the previously unvalidated floats (P3-3, Appendix E
  // M2): out-of-band values drove the generator into float overflow ->
  // llround/lround UB (lacunarity compounding over octaves), pow into
  // inf/NaN, and warp offsets out of int range. Bands are generous around
  // every authored/default value; they reject only what generation cannot
  // evaluate safely. parseFloat already rejects non-finite input.
  if (n.type == TextureNodeType::Noise || n.type == TextureNodeType::Warp) {
    if (!(n.lacunarity >= 1.0f && n.lacunarity <= 4.0f))
      return fail(err, lineNo, "lacunarity must be in [1, 4]");
    if (!(n.gain >= 0.0f && n.gain <= 1.0f)) return fail(err, lineNo, "gain must be in [0, 1]");
  }
  if (n.type == TextureNodeType::Warp && !(n.strength >= 0.0f && n.strength <= 8.0f))
    return fail(err, lineNo, "warp strength must be in [0, 8]");
  if (n.type == TextureNodeType::Curve) {
    if (!(n.gamma >= 0.01f && n.gamma <= 10.0f)) return fail(err, lineNo, "gamma must be in [0.01, 10]");
    if (!(n.contrast >= -16.0f && n.contrast <= 16.0f))
      return fail(err, lineNo, "contrast must be in [-16, 16]");
    if (!(n.clampMin >= -4.0f && n.clampMin <= 4.0f) || !(n.clampMax >= -4.0f && n.clampMax <= 4.0f))
      return fail(err, lineNo, "clamp_min/clamp_max must be in [-4, 4]");
  }
  if (n.type == TextureNodeType::Blend && !(n.mix >= 0.0f && n.mix <= 1.0f))
    return fail(err, lineNo, "blend mix must be in [0, 1]");
  if (n.type == TextureNodeType::Grid && !(n.sharpness >= 0.0f && n.sharpness <= 64.0f))
    return fail(err, lineNo, "grid sharpness must be in [0, 64]");
  if (n.type == TextureNodeType::Grid && !(n.lineWidth >= 0.0f && n.lineWidth <= 1.0f))
    return fail(err, lineNo, "grid width must be in [0, 1]");
  if (n.type == TextureNodeType::Constant && !(n.value >= -4.0f && n.value <= 4.0f))
    return fail(err, lineNo, "constant value must be in [-4, 4]");
  if (n.type == TextureNodeType::Glow) {
    if (!(n.threshold >= -1.0f && n.threshold <= 2.0f))
      return fail(err, lineNo, "glow threshold must be in [-1, 2]");
    if (!(n.intensity >= 0.0f && n.intensity <= 64.0f))
      return fail(err, lineNo, "glow intensity must be in [0, 64]");
  }
  if (n.type == TextureNodeType::Dither && !(n.amount >= 0.0f && n.amount <= 255.0f))
    return fail(err, lineNo, "dither amount must be in [0, 255]");
  if (n.type == TextureNodeType::Palette && n.stops.size() > kMaxPaletteStops)
    return fail(err, lineNo, "palette has too many stops (max " + std::to_string(kMaxPaletteStops) + ")");
  out->nodes.push_back(std::move(n));
  return true;
}

}  // namespace

bool parseRecipe(std::string_view text, TextureRecipe* out, std::string* err) {
  if (!out) {
    if (err) *err = "null output";
    return false;
  }
  if (text.size() > kMaxRecipeTextBytes) return fail(err, 0, "recipe text too large");
  TextureRecipe recipe;
  int lineNo = 0;
  size_t start = 0;
  while (start <= text.size()) {
    const size_t nl = text.find('\n', start);
    const std::string_view raw = text.substr(start, nl == std::string_view::npos ? text.size() - start : nl - start);
    ++lineNo;
    const std::string line = trim(raw);
    if (!line.empty() && line[0] != '#') {
      if (recipe.nodes.size() >= kMaxRecipeNodes)
        return fail(err, lineNo, "too many nodes (max " + std::to_string(kMaxRecipeNodes) + ")");
      if (!parseLine(line, lineNo, &recipe, err)) return false;
    }
    if (nl == std::string_view::npos) break;
    start = nl + 1;
  }
  if (recipe.nodes.empty()) return fail(err, 0, "recipe has no nodes");
  *out = std::move(recipe);
  if (err) err->clear();
  return true;
}

std::string recipeToText(const TextureRecipe& recipe) {
  std::string out;
  auto line = [&](const std::string& s) { out += s; out += '\n'; };
  for (const TextureNode& n : recipe.nodes) {
    std::string s;
    auto kv = [&](const char* key, const std::string& v) { s += ' '; s += key; s += '='; s += v; };
    auto kf = [&](const char* key, float v) { kv(key, floatToText(v)); };
    auto ki = [&](const char* key, int v) { kv(key, std::to_string(v)); };
    switch (n.type) {
      case TextureNodeType::Noise:
        s = "noise";
        kv("kind", n.noiseKind == NoiseKind::Value ? "value" : "gradient");
        kf("scale", n.scale); ki("octaves", n.octaves); kf("lacunarity", n.lacunarity); kf("gain", n.gain); ki("seed", n.seedOffset);
        break;
      case TextureNodeType::Voronoi:
        s = "voronoi";
        kf("scale", n.scale);
        kv("mode", n.voronoiMode == VoronoiMode::F1 ? "f1" : "edge");
        ki("seed", n.seedOffset);
        break;
      case TextureNodeType::Warp:
        s = "warp";
        kf("strength", n.strength); kf("scale", n.scale); ki("octaves", n.octaves); kf("lacunarity", n.lacunarity); kf("gain", n.gain); ki("seed", n.seedOffset);
        break;
      case TextureNodeType::Ridged: s = "ridged"; break;
      case TextureNodeType::Billow: s = "billow"; break;
      case TextureNodeType::Curve:
        s = "curve";
        kf("contrast", n.contrast); kf("gamma", n.gamma); ki("posterize", n.posterize);
        kv("invert", n.invert ? "1" : "0");
        kf("clamp_min", n.clampMin); kf("clamp_max", n.clampMax);
        break;
      case TextureNodeType::Blend:
        s = "blend";
        kv("mode", n.blendMode == BlendMode::Multiply ? "multiply" : n.blendMode == BlendMode::Add ? "add"
                   : n.blendMode == BlendMode::Min ? "min" : n.blendMode == BlendMode::Max ? "max" : "screen");
        kf("mix", n.mix);
        break;
      case TextureNodeType::Grid:
        s = "grid";
        ki("cells", n.cells); kf("width", n.lineWidth); kf("sharpness", n.sharpness);
        break;
      case TextureNodeType::Constant:
        s = "constant";
        kf("value", n.value);
        break;
      case TextureNodeType::Palette: {
        s = "palette";
        std::string stops;
        for (size_t i = 0; i < n.stops.size(); ++i) {
          if (i) stops += ',';
          stops += floatToText(n.stops[i].pos);
          stops += ':';
          stops += colorToText(n.stops[i].color);
        }
        kv("stops", stops);
        break;
      }
      case TextureNodeType::Glow:
        s = "glow";
        kf("threshold", n.threshold); kf("intensity", n.intensity); kv("color", colorToText(n.glowColor));
        break;
      case TextureNodeType::Dither:
        s = "dither";
        kf("amount", n.amount);
        break;
    }
    line(s);
  }
  return out;
}

}  // namespace litholattice
