// Chunk shading shared by the OpenGL (chunk.frag) and Vulkan (vk/chunk.frag) programs.
// The including file declares the inputs (vWorld, vNormal, vAO, vUV, vLayer, vVal, vMode, vRot),
// the samplers uTex / uPal, the frame and material values (uCam, uSky, uFogEnd, uTime, uTile,
// uWirePal, uRing, uLampPal, uLampMask, uLampRim, uRepMask, uMemMask, uAnim*, MAX_ANIMS) and the
// output `frag`. Base pack file; a pack may override it (OpenGL only: Vulkan SPIR-V is built in).
//
// MODE NUMBERS - mirror litholattice::ShadeMode in renderer.h EXACTLY. Do not hardcode
// bare numbers below; use these constants. (Fix 2026-10-11: vMode==12 was the 7-segment
// branch while kShadeReadout=15, so the catchor readout never rendered.)
const int MODE_EMISSIVE = 1, MODE_CUTOUT = 2, MODE_WIRE0 = 3, MODE_WIRE1 = 4;
const int MODE_PIPS = 8, MODE_CELLS = 9, MODE_ANIMATED = 10, MODE_GLASS = 11, MODE_READOUT = 15;
const int MODE_GROUND_PULSE = 16;  // mirror kShadeGroundPulse (renderer.h)
// ---- Wafer ground pulse (JD-locked 2026-10-11; biome blending 2026-10-11) ----
// The wafer's heartbeat: time-driven emissive add for kShadeGroundPulse quads.
// Pure function of (time, chunk, blended params): per-chunk phase from a hash of
// the chunk coords (cheap, no per-block timers); the waveform params are the
// chunk column's weight-blended biome params, baked at mesh time into the
// ChunkMesh and pushed per draw call (see render/ground_pulse.h). Dead-Zone-heavy
// blends carry ~zero amplitude, so the pulse fades out smoothly approaching Dead
// Zones instead of cutting off. CPU mirror: groundPulseEmissive(t, cx, cz, params)
// in render/ground_pulse.h (same waveform, same tunings - the tunings live ONLY
// in the C++ table; the shader evaluates whatever the mesh carries).
#ifndef LITHO_PULSE_PUSH
uniform vec4 uPulseP0;  // this chunk mesh: (freqHz, sharpness, amplitude, -)
uniform vec4 uPulseP1;  // this chunk mesh: (tint.rgb, -)
uniform float uGroundPulseTime;  // sim-time seconds (NOT wall clock: pausing freezes it)
uniform float uGroundPulseOn;    // render.ground_pulse cvar, 1 = on (live toggle, no remesh)
#endif
// Vulkan: uPulseP0/uPulseP1 come from the push-constant block and
// uGroundPulseTime/uGroundPulseOn from the Frame block; vk/chunk.frag defines
// LITHO_PULSE_PUSH and declares them, so the block above is skipped there.
vec3 groundPulseColor(float t, vec3 wpos) {
  if (uPulseP0.z <= 0.0) return vec3(0.0);  // unpowered blend: skip the math
  vec2 cc = floor(wpos.xz / 16.0);
  float ph = fract(sin(dot(cc, vec2(127.1, 311.7))) * 43758.5453123);
  float w = 0.5 + 0.5 * sin(6.28318530718 * (uPulseP0.x * t + ph));
  w = pow(w, uPulseP0.y);
  return uPulseP1.rgb * (uPulseP0.z * w);
}
// ---- Wafer ground traces (JD 2026-10-11; landed end-to-end 2026-10-13, P1-RENDER) ----
// GLSL mirror of groundTraceEmissive() in render/ground_traces.h: Manhattan
// trace lines with energy packets travelling along them, per-chunk blended
// params pushed per draw call like the pulse. Same shapes and tunings as the
// CPU mirror; only the per-line hash differs (classic sin-hash here, integer
// hash on CPU - cross-implementation line equality is not required, see the
// header). Composes additively with the pulse on kShadeGroundPulse quads.
#ifndef LITHO_TRACE_PUSH
uniform vec4 uTraceP0;  // this chunk mesh: (density, speed, amplitude, dirMix)
uniform vec4 uTraceP1;  // this chunk mesh: (tint.rgb, -)
uniform float uGroundTraceTime;  // sim-time seconds (same clock as the pulse)
uniform float uGroundTraceOn;    // render.ground_traces cvar, 1 = on (live toggle)
#endif
// Vulkan: uTraceP0/uTraceP1 come from the push-constant block and
// uGroundTraceTime/uGroundTraceOn from the Frame block; vk/chunk.frag defines
// LITHO_TRACE_PUSH and declares them, so the block above is skipped there.
float groundTraceHash(float line, float seed) {
  return fract(sin(line * 374.761393 + seed * 668.265263) * 43758.5453123);
}
vec3 groundTraceColor(float t, vec3 wpos) {
  if (uTraceP0.z <= 0.0 || uTraceP0.x <= 0.0) return vec3(0.0);  // unpowered blend
  float density = uTraceP0.x, speed = uTraceP0.y, amp = uTraceP0.z, dirMix = uTraceP0.w;
  float pk = density * 0.25;  // packets per block along a line
  float hMix = 1.0 - dirMix, vMix = dirMix;
  // H-set: lines run along X at z = n / density.
  float zn = wpos.z * density;
  float lz = floor(zn);
  float fz = zn - lz;
  float h1 = groundTraceHash(lz, 1.0);
  float lineH = 0.35 + 0.65 * h1;
  float dl = min(fz, 1.0 - fz);
  float wH = 1.0 - smoothstep(0.02, 0.08, dl);
  float sgnH = h1 < 0.5 ? 1.0 : -1.0;
  float puF = fract(sgnH * wpos.x * pk - t * speed + h1);
  float packH = smoothstep(0.0, 0.05, puF) * (1.0 - smoothstep(0.05, 0.28, puF));
  // V-set: lines run along Z at x = n / density.
  float xn = wpos.x * density;
  float lx = floor(xn);
  float fx = xn - lx;
  float h2 = groundTraceHash(lx, 2.0);
  float lineV = 0.35 + 0.65 * h2;
  float dv = min(fx, 1.0 - fx);
  float wV = 1.0 - smoothstep(0.02, 0.08, dv);
  float sgnV = h2 < 0.5 ? 1.0 : -1.0;
  float pvF = fract(sgnV * wpos.z * pk - t * speed + h2);
  float packV = smoothstep(0.0, 0.05, pvF) * (1.0 - smoothstep(0.05, 0.28, pvF));
  float lineGlow = hMix * wH * lineH + vMix * wV * lineV;
  float packGlow = hMix * wH * lineH * packH + vMix * wV * lineV * packV;
  float a = amp * (0.30 * lineGlow + 0.70 * packGlow);
  return uTraceP1.rgb * a;
}
const vec3 RING_BACK = vec3(22.0, 25.0, 34.0) / 255.0, RING_UNLIT = vec3(44.0, 50.0, 66.0) / 255.0;
const vec3 CORE[2] = vec3[2](vec3(1.0, 0.878, 0.722), vec3(0.867, 0.969, 1.0));
const vec3 RIM = vec3(185.0, 168.0, 240.0) / 255.0;
const vec3 PIP_LIT = vec3(255.0, 211.0, 107.0) / 255.0, CELL_LIT = vec3(143.0, 214.0, 232.0) / 255.0;
const vec3 CELL_OFF = vec3(30.0, 34.0, 48.0) / 255.0;
vec2 rotUV(vec2 uv, int r) {   // same quarter turns as the mesher
  for (int i = 0; i < r; ++i) uv = vec2(uv.y, 1.0 - uv.x);
  return uv;
}
vec4 mask(int layer, vec2 uv) {  // nearest fetch (masks store indices in R)
  ivec2 p = clamp(ivec2(uv * float(uTile)), ivec2(0), ivec2(uTile - 1));
  return texelFetch(uTex, ivec3(p, layer), 0);
}
void main() {
#ifdef LITHO_MODEL_DITHER
  // Near-camera model dither fade (JD 2026-10-13): deterministic
  // interleaved-gradient-noise screen-door discard from the fragment's
  // view depth - the identical formula to the CPU path
  // (render/model_dither.h), so backends agree on the discard decision.
  // Only the GL includer defines LITHO_MODEL_DITHER + the uniforms; the
  // Vulkan includer does not (no band plumbing there yet).
  if (uModelDither > 0.5) {
    float ditherDist = length(uCam - vWorld);
    float ditherFade = clamp((ditherDist - uModelDitherBand.x) /
                                 max(uModelDitherBand.y - uModelDitherBand.x, 0.0001),
                             0.0, 1.0);
    if (ditherFade < 1.0) {
      float ign = fract(52.9829189 * fract(0.06711056 * gl_FragCoord.x +
                                           0.00583715 * gl_FragCoord.y));
      if (ign >= ditherFade) discard;
    }
  }
#endif
  vec3 n = vNormal;
  vec4 t = texture(uTex, vec3(vUV, float(vLayer)));
  vec3 base = t.rgb; float a = t.a; float emissive = 0.0;
  if (vMode == MODE_EMISSIVE) {
    emissive = 0.85;
  } else if (vMode == MODE_CUTOUT) {
    if (a < 0.5) discard;
  } else if (vMode == MODE_WIRE0 || vMode == MODE_WIRE1) {                       // wire
    int ch = vMode - MODE_WIRE0, s = clamp(vVal, 0, 64);
    float fs = float(s);
    float lum = max(t.r, max(t.g, t.b));
    vec3 ramp = texelFetch(uPal, ivec2(s, uWirePal[ch]), 0).rgb;
    vec3 col = mix(t.rgb, clamp(ramp * (lum / 0.85), 0.0, 1.0), smoothstep(0.45, 0.80, lum));
    col = mix(col, CORE[ch], 0.9 * smoothstep(0.90, 0.99, lum) * smoothstep(40.0, 64.0, fs));
    vec4 m = mask(uRing[ch], rotUV(vUV, (4 - vRot) & 3));   // ring mask uses the unrotated tile UV
    float idx = floor(m.r * 255.0 / 16.0 + 0.5);
    float level = clamp(fs - 4.0 * idx, 0.0, 4.0);
    float litA = level > 0.0 ? 0.25 + 0.75 * level / 4.0 : 0.0;
    vec3 rc = mix(mix(RING_BACK, RING_UNLIT, m.g), CORE[ch], m.g * litA);
    float ra = max(m.g, m.b);
    float A = ra + a * (1.0 - ra);
    if (A < 0.5) discard;
    base = (rc * ra + col * a * (1.0 - ra)) / max(A, 1e-4);
    a = 1.0;
    vec2 tuv = rotUV(vUV, (4 - vRot) & 3);                    // unrotated tile UV (as the ring)
    // Quarter-mark dots (CloudAtlas dot mask: R = index x 64, G = coverage): dot k (0 N, 1 E,
    // 2 S, 3 W) lit when s >= 16 (k + 1), in the ring's lit colour.
    if (uDot[ch] > 0) {
      vec4 dm = mask(uDot[ch], tuv);
      float didx = floor(dm.r * 255.0 / 64.0 + 0.5);
      float dA = dm.g * step(16.0 * (didx + 1.0), fs);
      base = mix(base, CORE[ch], dA);
    }
    // DEBUG overlay (F3 only): the 0-64 number from the debug digit tile (5 x 2 glyphs of 12 x 18
    // at x = 12 col, y = 32 row, in 64ths), centred on the node with the halo.
    if (uDebug != 0 && uDigits > 0) {
      vec2 p = (tuv - 0.5) * 64.0;
      int tens = s / 10, ones = s - tens * 10;
      bool two = s >= 10;
      float totalW = two ? 25.0 : 12.0;
      vec2 lp = p - vec2(-totalW * 0.5, -9.0);
      if (lp.x >= 0.0 && lp.y >= 0.0 && lp.x < totalW && lp.y < 18.0) {
        int glyph = ones; float lx = lp.x;
        if (two) { if (lx < 12.0) glyph = tens; else if (lx < 13.0) glyph = -1; else lx -= 13.0; }
        if (glyph >= 0) {
          vec4 dg = mask(uDigits, vec2((float(glyph % 5) * 12.0 + lx) / 64.0, (float(glyph / 5) * 32.0 + lp.y) / 64.0));
          base = mix(base, mix(vec3(0.08, 0.09, 0.12), vec3(1.0), dg.r), dg.a);
        }
      }
    }
    emissive = s > 0 ? 0.55 : 0.0;  } else if (vMode >= 5 && vMode <= 7) {                       // color lamp 16 / 32 / 64
    int tier = vMode - 5, n = 16 << tier, s = clamp(vVal, 1, 64);
    vec3 m = texture(uTex, vec3(vUV, float(uLampMask[tier]))).rgb;
    vec3 c = texelFetch(uPal, ivec2((s - 1) % n, uLampPal[tier]), 0).rgb;   // WRAP: (s-1) mod N
    float lum = max(t.r, max(t.g, t.b));
    vec3 col = mix(c, vec3(1.0), 0.30 * clamp((lum - 0.78) / 0.22, 0.0, 1.0)) * min(1.0, lum / 0.784);
    col = mix(col, vec3(1.0), 0.55 * m.g);
    vec3 o = mix(t.rgb, col, m.r);
    o = mix(o, c, 0.35 * m.b);
    float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
    o = mix(o, RIM, 0.60 * pow(1.0 - luma, 10.0) * texture(uTex, vec3(vUV, float(uLampRim[tier]))).r);
    base = o; emissive = 0.8 * m.r + 0.1;
  } else if (vMode == MODE_PIPS) {                                     // repeater delay pips
    vec4 m = mask(uRepMask, rotUV(vUV, (4 - vRot) & 3));
    int idx = int(floor(m.r * 255.0 / 28.0 + 0.5));
    if (idx >= 1 && idx <= vVal) base = mix(base, PIP_LIT, m.g);
  } else if (vMode == MODE_CELLS) {                                     // memory cell value
    vec4 m = mask(uMemMask, rotUV(vUV, (4 - vRot) & 3));
    int idx = int(floor((m.r * 255.0 + 1.0) / 4.0 + 0.5));
    bool lit = idx >= 1 && idx <= vVal;
    float lum = max(t.r, max(t.g, t.b));
    base = mix(base, lit ? CELL_LIT * lum : CELL_OFF, m.g);
  } else if (vMode == MODE_ANIMATED) {                                    // animated face (frame strip)
    int i = clamp(vVal, 0, MAX_ANIMS - 1), len = max(uAnimLen[i], 1);
    int f = uAnimSeq[clamp(uAnimStart[i] + int(uTime * 1000.0 / max(uAnimMs[i], 1.0)) % len, 0, 63)];
    base = texture(uTex, vec3(vUV, float(vLayer + f))).rgb;
    emissive = 0.4;
  }
  else if (vMode == MODE_GLASS) {                                    // glass: screen-door translucency
    const float BAYER[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    ivec2 q = ivec2(gl_FragCoord.xy) & 3;
    // Screen-door rule matches the PBR path and the CPU screenDoorTest
    // (fix 2026-10-11): discard iff floor(alpha*255) <= BAYER*17.
    if (floor(a * 255.0) <= BAYER[q.y * 4 + q.x] * 17.0) discard;
  } else if (vMode == MODE_READOUT) {                                    // 7-segment readout (catchor match value)
    const int SEG[10] = int[10](0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F);
    vec4 m = mask(vLayer, rotUV(vUV, (4 - vRot) & 3));
    int seg = int(floor(m.r * 255.0 / 16.0 + 0.5));
    int v = vVal & 127;
    bool ones = seg >= 8;
    int digit = ones ? v % 10 : (v / 10) % 10, bit = ones ? seg - 8 : seg - 1;
    bool on = seg >= 1 && seg <= 14 && (ones || v >= 10) && ((SEG[digit] >> bit) & 1) != 0;
    if (!on || m.g < 0.5) discard;
    base = (vVal & 128) != 0 ? vec3(1.0, 0.827, 0.420) : vec3(0.561, 0.839, 0.910);
    emissive = 0.9;
  }
  vec3 L = normalize(vec3(0.4, 0.85, 0.3));
  float diff = max(dot(n, L), 0.0);
  float fill = max(dot(n, normalize(vec3(-0.5, 0.3, -0.6))), 0.0);
  float ao = 0.45 + 0.55 * vAO;
  vec3 col = base * (0.55 + 0.50 * diff + 0.20 * fill) * ao;
  col = mix(col, base, emissive);
  if (vMode == MODE_GROUND_PULSE && uGroundPulseOn > 0.5) col += groundPulseColor(uGroundPulseTime, vWorld);
  if (vMode == MODE_GROUND_PULSE && uGroundTraceOn > 0.5) col += groundTraceColor(uGroundTraceTime, vWorld);
  float fog = smoothstep(uFogEnd * 0.55, uFogEnd, length(uCam - vWorld));
  frag = vec4(mix(col, uSky, fog), 1.0);
}
