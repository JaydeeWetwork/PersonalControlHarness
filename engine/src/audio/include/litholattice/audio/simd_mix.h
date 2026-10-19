// SIMD batch mixing for the audio hot path.
//
// WHAT: element-wise add of a rendered voice buffer into the mix, and the
// master-gain / clamp / channel-duplicate store at the end of the render.
// WHY: the per-sample voice render is stateful and cannot be vectorized, but
// the accumulation of each voice's block and the final output store are pure
// data-parallel loops - the hottest remaining per-sample work.
//
// CORRECTNESS: every operation is per-element and applied in the same order
// as the scalar loop, so the vectorized result is BIT-IDENTICAL to scalar
// (proven by the self-test, which memcmps SIMD vs scalar output). Dispatch is
// compile-time:
//   x86_64            -> SSE2 (baseline on x86_64, no extra flags needed)
//   AArch64 / ARMv7+  -> NEON
//   anything else     -> scalar fallback
#pragma once
#include <cmath>
#include <cstddef>

#if defined(__x86_64__) || defined(_M_X64)
#define LITHOLATTICE_SIMD_SSE2 1
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__aarch64__) || defined(_M_ARM64)
#define LITHOLATTICE_SIMD_NEON 1
#include <arm_neon.h>
#endif

namespace litholattice {
namespace simd {

/// dst[i] += src[i] for i in [0, n). Bit-identical to the scalar loop.
inline void addInto(float* dst, const float* src, size_t n) {
#if defined(LITHOLATTICE_SIMD_SSE2)
  size_t i = 0;
  for (; i + 4 <= n; i += 4)
    _mm_storeu_ps(dst + i, _mm_add_ps(_mm_loadu_ps(dst + i), _mm_loadu_ps(src + i)));
  for (; i < n; ++i) dst[i] += src[i];
#elif defined(LITHOLATTICE_SIMD_NEON)
  size_t i = 0;
  for (; i + 4 <= n; i += 4)
    vst1q_f32(dst + i, vaddq_f32(vld1q_f32(dst + i), vld1q_f32(src + i)));
  for (; i < n; ++i) dst[i] += src[i];
#else
  for (size_t i = 0; i < n; ++i) dst[i] += src[i];
#endif
}

/// out[(i*channels)+c] = clamp(in[i] * gain, -1, 1) for i in [0, n), all c.
/// Fast paths for mono and stereo; other channel counts use scalar.
/// Bit-identical to the scalar loop (same per-element op order; the SSE
/// min/max collapse to the same value as the ternary for every input,
// including NaN, which propagates unchanged through both).
inline void applyGainClampDuplicate(float* out, const float* in, size_t n, float gain,
                                   int channels) {
#if defined(LITHOLATTICE_SIMD_SSE2)
  if (channels == 1) {
    const __m128 g = _mm_set1_ps(gain);
    const __m128 lo = _mm_set1_ps(-1.f), hi = _mm_set1_ps(1.f);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
      const __m128 x = _mm_mul_ps(_mm_loadu_ps(in + i), g);
      _mm_storeu_ps(out + i, _mm_max_ps(lo, _mm_min_ps(hi, x)));
    }
    for (; i < n; ++i) {
      const float s = in[i] * gain;
      out[i] = s < -1.f ? -1.f : (s > 1.f ? 1.f : s);
    }
    return;
  }
  if (channels == 2) {
    const __m128 g = _mm_set1_ps(gain);
    const __m128 lo = _mm_set1_ps(-1.f), hi = _mm_set1_ps(1.f);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
      __m128 x = _mm_mul_ps(_mm_loadu_ps(in + i), g);
      x = _mm_max_ps(lo, _mm_min_ps(hi, x));  // x = (x0..x3), clamped
      _mm_storeu_ps(out + 2 * i, _mm_unpacklo_ps(x, x));      // x0,x0,x1,x1
      _mm_storeu_ps(out + 2 * i + 4, _mm_unpackhi_ps(x, x));  // x2,x2,x3,x3
    }
    for (; i < n; ++i) {
      const float s = in[i] * gain;
      const float c = s < -1.f ? -1.f : (s > 1.f ? 1.f : s);
      out[2 * i] = c;
      out[2 * i + 1] = c;
    }
    return;
  }
#endif
#if defined(LITHOLATTICE_SIMD_NEON)
  if (channels == 1) {
    const float32x4_t g = vdupq_n_f32(gain);
    const float32x4_t lo = vdupq_n_f32(-1.f), hi = vdupq_n_f32(1.f);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
      const float32x4_t x = vmulq_f32(vld1q_f32(in + i), g);
      vst1q_f32(out + i, vmaxq_f32(lo, vminq_f32(hi, x)));
    }
    for (; i < n; ++i) {
      const float s = in[i] * gain;
      out[i] = s < -1.f ? -1.f : (s > 1.f ? 1.f : s);
    }
    return;
  }
  if (channels == 2) {
    const float32x4_t g = vdupq_n_f32(gain);
    const float32x4_t lo = vdupq_n_f32(-1.f), hi = vdupq_n_f32(1.f);
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
      float32x4_t x = vmulq_f32(vld1q_f32(in + i), g);
      x = vmaxq_f32(lo, vminq_f32(hi, x));
      const float32x2x2_t z = vzip_f32(vget_low_f32(x), vget_high_f32(x));
      vst1q_f32(out + 2 * i, vcombine_f32(z.val[0], z.val[1]));  // x0,x0,x1,x1,...
    }
    for (; i < n; ++i) {
      const float s = in[i] * gain;
      const float c = s < -1.f ? -1.f : (s > 1.f ? 1.f : s);
      out[2 * i] = c;
      out[2 * i + 1] = c;
    }
    return;
  }
#endif
  for (size_t i = 0; i < n; ++i) {
    const float s = in[i] * gain;
    const float c = s < -1.f ? -1.f : (s > 1.f ? 1.f : s);
    for (int ch = 0; ch < channels; ++ch) out[i * size_t(channels) + size_t(ch)] = c;
  }
}

/// out[i] += in[i/2] for i in [0, n): zero-order-hold duplication for the
/// half-rate degradation path (Synth::mixInto at shed level 3).
/// Bit-identical to the scalar loop (same per-element op order).
inline void addIntoDuplicated(float* out, const float* in, size_t n) {
#if defined(LITHOLATTICE_SIMD_SSE2)
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const __m128 x = _mm_loadu_ps(in + i / 2);
    const __m128 dup = _mm_unpacklo_ps(x, x);  // x0,x0,x1,x1
    _mm_storeu_ps(out + i, _mm_add_ps(_mm_loadu_ps(out + i), dup));
  }
  for (; i < n; ++i) out[i] += in[i / 2];
#elif defined(LITHOLATTICE_SIMD_NEON)
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const float32x4_t x = vld1q_f32(in + i / 2);
    const float32x2x2_t z = vzip_f32(vget_low_f32(x), vget_low_f32(x));  // x0,x0,x1,x1
    const float32x4_t dup = vcombine_f32(z.val[0], z.val[1]);
    vst1q_f32(out + i, vaddq_f32(vld1q_f32(out + i), dup));
  }
  for (; i < n; ++i) out[i] += in[i / 2];
#else
  for (size_t i = 0; i < n; ++i) out[i] += in[i / 2];
#endif
}

/// Replace non-finite samples (NaN/Inf) with 0.0f, in place. Runs as one pass
/// over the final mix before the gain/clamp store: a NaN reaching the device
/// (or the MCP tap) is silence at best. Scalar loop — compilers auto-vectorize
/// it; clarity beats hand-rolled bit tricks for a scrub.
inline void scrubNonFinite(float* data, size_t n) {
  for (size_t i = 0; i < n; ++i)
    if (!std::isfinite(data[i])) data[i] = 0.f;
}

}  // namespace simd
}  // namespace litholattice
