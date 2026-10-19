#include "litholattice/procgen/modifiers.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "litholattice/procgen/seed.h"

namespace litholattice::procgen {
namespace {

class Fractal final : public INoise {
 public:
  Fractal(std::shared_ptr<const INoise> source, const FractalSettings& s, uint64_t seed)
      : source_(std::move(source)), s_(s) {
    s_.octaves = std::clamp(s_.octaves, 1, 16);
    Rng rng(seed);
    float amp = 1.f;
    for (int o = 0; o < s_.octaves; ++o) {
      shift_.push_back({rng.range(-1000.f, 1000.f), rng.range(-1000.f, 1000.f), rng.range(-1000.f, 1000.f),
                        rng.range(-1000.f, 1000.f)});
      norm_ += amp;
      amp *= s_.gain;
    }
  }

  float sample2(float x, float y) const override {
    return combine([&](int o, float f) { return source_->sample2(x * f + shift_[o][0], y * f + shift_[o][1]); });
  }
  float sample3(float x, float y, float z) const override {
    return combine([&](int o, float f) {
      return source_->sample3(x * f + shift_[o][0], y * f + shift_[o][1], z * f + shift_[o][2]);
    });
  }
  float sample4(float x, float y, float z, float w) const override {
    return combine([&](int o, float f) {
      return source_->sample4(x * f + shift_[o][0], y * f + shift_[o][1], z * f + shift_[o][2], w * f + shift_[o][3]);
    });
  }
  NoiseRange range() const override {
    return s_.type == FractalType::Ridged || s_.type == FractalType::Turbulence ||
                   s_.type == FractalType::PingPong
               ? NoiseRange{0.f, 1.f}
               : NoiseRange{};
  }

 private:
  template <typename Octave>
  float combine(Octave&& octave) const {
    float sum = 0, amp = 1, freq = 1;
    for (int o = 0; o < s_.octaves; ++o) {
      const float n = octave(o, freq);
      switch (s_.type) {
        case FractalType::FBm: sum += n * amp; break;
        case FractalType::Billow: sum += (2.f * std::abs(n) - 1.f) * amp; break;
        case FractalType::Turbulence: sum += std::abs(n) * amp; break;
        case FractalType::Ridged: { const float r = 1.f - std::abs(n); sum += r * r * amp; break; }
        case FractalType::PingPong: {
          // Triangular ping-pong fold, folded twice across [-1, 1] (see modifiers.h).
          const float v = (n + 1.f) * 1.5f;
          const float m = v - 2.f * std::floor(v * 0.5f);  // v mod 2, in [0, 2)
          sum += (1.f - std::abs(1.f - m)) * amp;
          break;
        }
      }
      amp *= s_.gain;
      freq *= s_.lacunarity;
    }
    return sum / norm_;
  }

  std::shared_ptr<const INoise> source_;
  FractalSettings s_;
  std::vector<std::array<float, 4>> shift_;
  float norm_ = 0;
};

class DomainWarp final : public INoise {
 public:
  DomainWarp(std::shared_ptr<const INoise> source, std::shared_ptr<const INoise> warp, float amplitude, float frequency)
      : source_(std::move(source)), warp_(std::move(warp)), amp_(amplitude), freq_(frequency) {}

  float sample2(float x, float y) const override {
    const float fx = x * freq_, fy = y * freq_;
    return source_->sample2(x + amp_ * warp_->sample2(fx, fy), y + amp_ * warp_->sample2(fx + kShift[0], fy + kShift[1]));
  }
  float sample3(float x, float y, float z) const override {
    const float fx = x * freq_, fy = y * freq_, fz = z * freq_;
    return source_->sample3(x + amp_ * warp_->sample3(fx, fy, fz),
                            y + amp_ * warp_->sample3(fx + kShift[0], fy + kShift[1], fz + kShift[2]),
                            z + amp_ * warp_->sample3(fx + kShift[3], fy + kShift[4], fz + kShift[5]));
  }
  float sample4(float x, float y, float z, float w) const override {
    const float fx = x * freq_, fy = y * freq_, fz = z * freq_, fw = w * freq_;
    return source_->sample4(x + amp_ * warp_->sample4(fx, fy, fz, fw),
                            y + amp_ * warp_->sample4(fx + kShift[0], fy + kShift[1], fz + kShift[2], fw),
                            z + amp_ * warp_->sample4(fx + kShift[3], fy + kShift[4], fz + kShift[5], fw),
                            w + amp_ * warp_->sample4(fx + kShift[6], fy + kShift[7], fz, fw + kShift[0]));
  }
  NoiseRange range() const override { return source_->range(); }

 private:
  static constexpr float kShift[8] = {131.7f, -71.3f, 53.9f, 217.1f, -151.3f, 89.7f, -199.1f, 37.3f};
  std::shared_ptr<const INoise> source_, warp_;
  float amp_, freq_;
};

constexpr std::array<std::pair<FractalType, std::string_view>, 6> kFractalNames{
    {{FractalType::FBm, "fbm"}, {FractalType::Ridged, "ridged"}, {FractalType::Billow, "billow"},
     {FractalType::Turbulence, "turbulence"}, {FractalType::PingPong, "pingpong"},
     {FractalType::PingPong, "ping_pong"}}};

}  // namespace

std::unique_ptr<INoise> createFractal(std::shared_ptr<const INoise> source, const FractalSettings& settings,
                                      uint64_t seed) {
  return std::make_unique<Fractal>(std::move(source), settings, seed);
}

std::unique_ptr<INoise> createDomainWarp(std::shared_ptr<const INoise> source, std::shared_ptr<const INoise> warp,
                                         float amplitude, float frequency) {
  return std::make_unique<DomainWarp>(std::move(source), std::move(warp), amplitude, frequency);
}

CurlVec2 curlNoise2(const INoise& potential, float x, float y, float eps) {
  const float e = eps > 0.f ? eps : 0.01f;
  const float dpsiDy = (potential.sample2(x, y + e) - potential.sample2(x, y - e)) / (2.f * e);
  const float dpsiDx = (potential.sample2(x + e, y) - potential.sample2(x - e, y)) / (2.f * e);
  return {dpsiDy, -dpsiDx};
}

CurlVec3 curlNoise3(const INoise& potX, const INoise& potY, const INoise& potZ, float x, float y, float z,
                    float eps) {
  const float e = eps > 0.f ? eps : 0.01f;
  const float inv = 1.f / (2.f * e);
  // curl = (dPz/dy - dPy/dz, dPx/dz - dPz/dx, dPy/dx - dPx/dy)
  const float dPzDy = (potZ.sample3(x, y + e, z) - potZ.sample3(x, y - e, z)) * inv;
  const float dPyDz = (potY.sample3(x, y, z + e) - potY.sample3(x, y, z - e)) * inv;
  const float dPxDz = (potX.sample3(x, y, z + e) - potX.sample3(x, y, z - e)) * inv;
  const float dPzDx = (potZ.sample3(x + e, y, z) - potZ.sample3(x - e, y, z)) * inv;
  const float dPyDx = (potY.sample3(x + e, y, z) - potY.sample3(x - e, y, z)) * inv;
  const float dPxDy = (potX.sample3(x, y + e, z) - potX.sample3(x, y - e, z)) * inv;
  return {dPzDy - dPyDz, dPxDz - dPzDx, dPyDx - dPxDy};
}

std::string_view toString(FractalType t) {
  for (const auto& [v, n] : kFractalNames)
    if (v == t) return n;
  return "?";
}

std::optional<FractalType> parseFractalType(std::string_view s) {
  for (const auto& [v, n] : kFractalNames)
    if (n == s) return v;
  return std::nullopt;
}

}  // namespace litholattice::procgen
