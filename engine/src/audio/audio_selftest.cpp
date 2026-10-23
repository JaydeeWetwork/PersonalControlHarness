// Offline synth checks: render samples into buffers and measure them. Never opens a device.
#include "litholattice/audio/audio_selftest.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>
#include "litholattice/audio/audio_engine.h"
#include "litholattice/audio/voice_allocator.h"
#include "litholattice/audio/spatial_audio.h"
#include "litholattice/audio/occlusion_thread.h"
#include "litholattice/audio/voxel_raycast.h"
#include <algorithm>
#include <functional>
#include <random>
#include <set>
#include <tuple>
#include <unordered_set>
#include "litholattice/audio/notes.h"
#include "litholattice/core/spsc_queue.h"
#include "litholattice/audio/simd_mix.h"
#include "litholattice/audio/effects.h"
#include "litholattice/audio/degradation.h"
#include "litholattice/audio/occlusion_cadence.h"
#include "litholattice/audio/streaming.h"
#include "litholattice/audio/codecs.h"
#include <cstdint>
#include <chrono>
#include <cstring>
#include <mutex>
#include <span>

namespace litholattice {
namespace {

constexpr float kRate = 48000.f;

std::vector<float> renderVoice(Voice& v, size_t samples) {
  std::vector<float> out(samples);
  for (float& s : out) s = v.render();
  return out;
}

/// Frequency from rising zero crossings (DC removed first, so pulses of any duty work).
double measureFrequency(const std::vector<float>& x) {
  double mean = 0;
  for (float s : x) mean += s;
  mean /= double(x.size());
  int crossings = 0;
  for (size_t i = 1; i < x.size(); ++i) crossings += (x[i - 1] - mean < 0) && (x[i] - mean >= 0);
  return crossings / (double(x.size()) / kRate);
}

double peak(const std::vector<float>& x, size_t from, size_t to) {
  double p = 0;
  for (size_t i = from; i < to && i < x.size(); ++i) p = std::max(p, double(std::abs(x[i])));
  return p;
}

bool near(double a, double b, double tol) { return std::abs(a - b) <= tol; }

}  // namespace

int runAudioSelfTest() {
  int fails = 0;
  auto check = [&](bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++fails;
  };
  std::printf("Audio self-test (offline, no device)\n");

  check(near(noteFrequency(69), 440.0, 1e-9) && near(noteFrequency(36), 65.41, 0.01) && noteName(60) == "C4" &&
            noteName(99) == "D#7" && noteName(-1) == "-",
        "notes: A4 = 440 Hz, C2 65.4 Hz, names C4 / D#7, '-' for none");

  const Adsr flat{0.f, 0.f, 1.f, 0.f};
  struct Case { Waveform w; Duty d; double hz; const char* name; };
  for (const Case& c : {Case{Waveform::Pulse, Duty::Half, 440.0, "pulse 50% at A4 measures 440 Hz"},
                        Case{Waveform::Triangle, Duty::Half, noteFrequency(48), "triangle at C3 measures 130.8 Hz"},
                        Case{Waveform::Saw, Duty::Half, 1000.0, "saw at 1000 Hz"},
                        Case{Waveform::Wave, Duty::Half, 220.0, "4-bit wave table at 220 Hz"}}) {
    Voice v(kRate);
    v.noteOn({c.hz, c.w, c.d, flat, 1.f});
    check(near(measureFrequency(renderVoice(v, size_t(kRate))), c.hz, c.hz * 0.01), c.name);
  }
  {
    bool ok = true;
    for (Duty d : {Duty::Eighth, Duty::Quarter, Duty::Half, Duty::ThreeQuarters}) {
      Voice v(kRate);
      v.noteOn({500.0, Waveform::Pulse, d, flat, 1.f});
      const auto x = renderVoice(v, 48000);
      double high = 0;
      for (float s : x) high += s > 0;
      ok = ok && near(high / double(x.size()), dutyFraction(d), 0.01);
    }
    check(ok, "pulse duty 12.5 / 25 / 50 / 75 % (fraction of high samples)");
  }
  {
    Voice v(kRate);
    const Adsr adsr{0.01f, 0.04f, 0.5f, 0.05f};
    v.noteOn({200.0, Waveform::Saw, Duty::Half, adsr, 1.f});
    std::vector<float> env;
    for (int i = 0; i < 4800; ++i) { v.render(); env.push_back(v.envelope().level()); }  // 100 ms gate
    v.noteOff();
    for (int i = 0; i < 4800; ++i) { v.render(); env.push_back(v.envelope().level()); }
    const bool attack = near(env[239], 0.5, 0.02) && near(env[479], 1.0, 0.01);  // halfway / top at 10 ms
    const bool decay = near(env[2399], 0.5, 0.01) && near(env[4799], 0.5, 0.001);  // sustain from 50 ms
    const bool release = near(env[4800 + 1199], 0.25, 0.02) && env[4800 + 2500] == 0.f && !v.active();
    check(attack && decay && release, "ADSR: 10 ms attack, 40 ms decay to 0.5, 50 ms release to silence");
  }
  {
    Lfsr l;
    int ones = 0, period = 0;
    std::vector<int> first(64);
    for (int i = 0; i < 64; ++i) first[i] = l.step();
    for (int i = 64; i < 70000; ++i) ones += l.step();
    Lfsr s;
    s.setShortMode(true);
    std::vector<int> seq(600);  // check window 100..300 + period < 200
    for (int& b : seq) b = s.step();
    for (int p = 1; p < 200 && !period; ++p) {
      bool rep = true;
      for (int i = 100; i < 300 && rep; ++i) rep = seq[i] == seq[i + p];
      if (rep) period = p;
    }
    check(ones > 30000 && ones < 40000 && period == 127, "LFSR noise: 15-bit is balanced, 7-bit short mode repeats every 127");
  }
  {
    Synth sid(SynthPreset::sid3(), kRate), dmg(SynthPreset::dmg(), kRate);
    dmg.noteOn(2, {220.0, Waveform::Pulse});
    dmg.noteOn(3, {220.0, Waveform::Saw});
    sid.noteOn(1, {220.0, Waveform::Noise});
    check(sid.voiceCount() == 3 && dmg.voiceCount() == 4 && dmg.voice(2).waveform() == Waveform::Wave &&
              dmg.voice(3).waveform() == Waveform::Noise && sid.voice(1).waveform() == Waveform::Noise,
          "presets: SID3 = 3 full voices; DMG = 2 pulse + wave + noise (unsupported waveforms coerced)");
  }
  {
    SpscQueue<int, 64> q;
    constexpr int kN = 200000;
    std::thread producer([&] { for (int i = 0; i < kN;) if (q.tryPush(i)) ++i; });
    bool ordered = true;
    for (int expect = 0; expect < kN;)
      if (const auto v = q.tryPop()) { ordered = ordered && *v == expect; ++expect; }
    producer.join();
    check(ordered && !q.tryPop(), "SPSC command queue: 200k items across threads, in order, no locks");
  }
  {
    AudioEngine eng(kRate);
    NoteSpec tone{noteFrequency(69), Waveform::Pulse, Duty::Quarter, {0.005f, 0.05f, 0.6f, 0.05f}, 0.5f};
    eng.post(AudioCommand::noteOn(Bus::Music, 0, tone));
    std::vector<float> st(48000 * 2);
    eng.render(st.data(), 48000, 2);
    std::vector<float> left(48000);
    for (size_t i = 0; i < left.size(); ++i) left[i] = st[i * 2];
    const bool pitch = near(measureFrequency(left), 440.0, 4.4) && st[1000] == st[1001];
    eng.post(AudioCommand::noteOff(Bus::Music, 0));
    eng.render(st.data(), 9600, 2);
    std::vector<float> tail(st.begin() + 2 * 4800, st.begin() + 2 * 9600);
    check(pitch && peak(tail, 0, tail.size()) == 0.0, "engine: queued A4 note plays 440 Hz in stereo; note-off fades to silence");
  }
  {
    AudioEngine eng(kRate);
    eng.setEnabled(false);
    check(!eng.post(AudioCommand::noteOff(Bus::Effects, 0)), "muted engine drops commands");
  }
  {
    VoiceAllocator va(3);
    const size_t a = va.acquire(10), b = va.acquire(11), c = va.acquire(12);
    const size_t d = va.acquire(13);  // steals the oldest (10)
    check(a != b && b != c && d == a && !va.release(10) && va.release(13) == a && va.acquire(11) == b,
          "voice allocator: stable per source, steals the oldest when full");
  }
  {
    // Spatial: attenuation, panning, occlusion, echo delay -- the Daemon's ears.
    SpatialAudio sa;
    auto open = [](float, float, float, float, float, float) { return false; };
    auto wall = [](float, float, float, float, float, float) { return true; };
    Listener l{0, 0, 0, 0, 1, 64};  // origin, facing +Z, hears 64 blocks
    SoundEvent at0{0, 0, 0, 1, 1.0f, 1.0f, 0, 7};
    SoundEvent at16{16, 0, 0, 1, 1.0f, 1.0f, 0, 7};
    SoundEvent far{100, 0, 0, 1, 1.0f, 1.0f, 0, 7};
    check(near(sa.perceive(at0, l, open).volume, 1.0, 1e-5) &&
          near(sa.perceive(at16, l, open).volume, 0.5, 1e-5) &&
          sa.perceive(far, l, open).volume == 0.0f &&
          near(sa.perceive(at16, l, wall).volume, 0.25, 1e-5),
          "spatial: 1/(1+d/16) falloff, walls halve volume, out-of-range silent");
    SoundEvent right{10, 0, 0, 1, 1.0f, 1.0f, 0, 7};
    SoundEvent left{-10, 0, 0, 1, 1.0f, 1.0f, 0, 7};
    SoundEvent ahead{0, 0, 10, 1, 1.0f, 1.0f, 0, 7};
    check(near(sa.perceive(right, l, open).pan, 1.0, 1e-5) &&
          near(sa.perceive(left, l, open).pan, -1.0, 1e-5) &&
          near(sa.perceive(ahead, l, open).pan, 0.0, 1e-5),
          "spatial: stereo pan hard-right / hard-left / center");
    SoundEvent d34{34, 0, 0, 1, 1.0f, 1.0f, 0, 7};
    check(near(sa.perceive(d34, l, open).delay, 100.0, 0.5),
          "spatial: echo delay 100 ms at 34 blocks (340 m/s)");
  }
  {
    // AI hearing: the Daemon queries what it can hear, loudest first.
    SpatialAudio sa;
    auto open = [](float, float, float, float, float, float) { return false; };
    Listener mob{0, 0, 0, 0, 1, 64};
    sa.emit({10, 0, 0, 1, 1.0f, 1.0f, 0, 1});  // loud footstep nearby
    sa.emit({20, 0, 0, 1, 0.5f, 1.0f, 1, 2});  // quieter, farther
    sa.emit({200, 0, 0, 1, 1.0f, 1.0f, 2, 3}); // out of earshot
    auto heard = sa.audibleSounds(mob, open);
    check(heard.size() == 2 && heard[0].sourceId == 1 && heard[1].sourceId == 2,
          "spatial: audibleSounds filters by range, loudest first");
    sa.update(600, 512);  // every event older than 512 ticks
    check(sa.audibleSounds(mob, open).empty(), "spatial: update() ages sounds out");
  }
  {
    // H-AUD-5: audibleSounds sorts by PERCEIVED volume: a near quiet event
    // (0.5 at 2 blocks -> ~0.44 perceived) outranks a far loud one
    // (1.0 at 48 blocks -> 0.25 perceived), the reverse of raw-volume order.
    SpatialAudio sa;
    auto open = [](float, float, float, float, float, float) { return false; };
    Listener mob{0, 0, 0, 0, 1, 64};
    sa.emit({48, 0, 0, 1, 1.0f, 1.0f, 0, 10});  // loud but far
    sa.emit({2, 0, 0, 1, 0.5f, 1.0f, 0, 11});   // quiet but near
    auto heard = sa.audibleSounds(mob, open);
    check(heard.size() == 2 && heard[0].sourceId == 11 && heard[1].sourceId == 10,
          "spatial: audibleSounds sorts by perceived volume (near-quiet beats far-loud)");
    // H-AUD-6: an event stamped with a future tick (tick counter reset on
    // world reload) must NOT be aged out by unsigned wraparound.
    SpatialAudio sa2;
    sa2.emit({10, 0, 0, 1, 1.0f, 1.0f, 100, 12});
    sa2.update(50, 512);  // current tick is BEFORE the event's stamp
    check(sa2.audibleSounds(mob, open).size() == 1,
          "spatial: update() keeps future-stamped events (no tick underflow)");
    sa2.update(700, 512);  // now genuinely 600 ticks old
    check(sa2.audibleSounds(mob, open).empty(),
          "spatial: update() still ages out genuinely old events");
  }
  {
    SpatialAudio sa;
    auto air = [](int, int, int) { return false; };
    auto rock = [](int, int, int) { return true; };
    check(near(sa.estimateReverb(0, 0, 0, air), 0.0, 1e-6) &&
          near(sa.estimateReverb(0, 0, 0, rock), 1.0, 1e-6),
          "spatial: reverb 0.0 in open air, 1.0 fully enclosed");
  }
  {
    // Flood-fill occlusion: bake once per tick, O(1) queries per voice
    // (research section 2.1, UnityVoxelAudio). Known layout: a full wall
    // plane at x == 8 spanning the baked box. Flood visibility and the
    // straight-ray DDA provably coincide here:
    //  - every x > 8 cell is unreachable by flood (no air path through the
    //    wall) and every straight ray from the listener crosses the wall;
    //  - every x < 8 air cell is reached by flood and its ray stays in air.
    // (Solid wall cells read occluded from the field; the DDA stops at the
    // target voxel without testing it, so the reference counts a solid
    // target as blocked too.)
    constexpr int kLX = 2, kLY = 2, kLZ = 2, kR = 12, kWallX = 8;
    auto wallSolid = [](int x, int, int) { return x == kWallX; };
    OcclusionField field;
    field.bake(kLX, kLY, kLZ, wallSolid, kR);
    bool ok = true;
    std::function<bool(int, int, int)> solidFn = wallSolid;
    for (int z = kLZ - kR; z <= kLZ + kR && ok; ++z)
      for (int y = kLY - kR; y <= kLY + kR && ok; ++y)
        for (int x = kLX - kR; x <= kLX + kR && ok; ++x) {
          const bool targetSolid = wallSolid(x, y, z);
          // A source co-located with the listener is never occluded (the
          // DDA steps before testing, so it wanders on sub-voxel rays —
          // an artifact of the raycaster, not of the visibility model).
          const bool sameVoxel = (x == kLX && y == kLY && z == kLZ);
          // Listener/targets sit off voxel boundaries so no DDA ray runs
          // exactly along a voxel edge (keeps tie-breaking unambiguous).
          const bool rayBlocked = !sameVoxel &&
              (targetSolid || voxelRaycastBlocked(2.25f, 2.75f, 2.125f,
                                                  float(x) + 0.5f, float(y) + 0.5f,
                                                  float(z) + 0.5f, solidFn));
          if (field.occluded(float(x) + 0.5f, float(y) + 0.5f, float(z) + 0.5f) != rayBlocked)
            ok = false;
        }
    // Enclosure: one wall plane in a 25^3 box -> 625 / 15625.
    ok = ok && near(field.enclosure(), 1.0 / double(2 * kR + 1), 1e-6);
    ok = ok && field.distance(2.5f, 2.5f, 2.5f) == 0;     // listener cell
    ok = ok && field.distance(10.5f, 2.5f, 2.5f) == -1;   // behind the wall
    check(ok, "occlusion field: baked flood matches per-pair DDA raycast on a full-wall layout");
  }
  {
    // Determinism: identical layout -> bit-identical baked bytes, even when
    // the solid set is built in a different insertion order (the
    // unordered_set below iterates in a scrambled order on purpose).
    struct TupleHash {
      size_t operator()(const std::tuple<int, int, int>& t) const noexcept {
        size_t h = std::hash<int>{}(std::get<0>(t));
        h ^= std::hash<int>{}(std::get<1>(t)) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(std::get<2>(t)) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
      }
    };
    std::vector<std::tuple<int, int, int>> cells;
    for (int z = -6; z <= 6; ++z)
      for (int y = -6; y <= 6; ++y)
        for (int x = -6; x <= 6; ++x)
          if ((x * 7 + y * 13 + z * 29) % 17 < 4 && !(x == 0 && y == 0 && z == 0))
            cells.emplace_back(x, y, z);
    const std::set<std::tuple<int, int, int>> ordered(cells.begin(), cells.end());
    std::mt19937 rng(0xF00D);
    std::shuffle(cells.begin(), cells.end(), rng);
    const std::unordered_set<std::tuple<int, int, int>, TupleHash> scrambled(cells.begin(), cells.end());
    auto solidA = [&](int x, int y, int z) { return ordered.count({x, y, z}) != 0; };
    auto solidB = [&](int x, int y, int z) { return scrambled.count({x, y, z}) != 0; };
    OcclusionField fa, fb;
    fa.bake(0, 0, 0, solidA, 10);
    fb.bake(0, 0, 0, solidB, 10);
    const bool bytesEqual =
        fa.visBytes().size() == fb.visBytes().size() &&
        fa.distBytes().size() == fb.distBytes().size() &&
        std::equal(fa.visBytes().begin(), fa.visBytes().end(), fb.visBytes().begin()) &&
        std::equal(fa.distBytes().begin(), fa.distBytes().end(), fb.distBytes().begin());
    check(bytesEqual && fa.checksum() == fb.checksum() &&
              fa.bakedFor(0, 0, 0) && !fa.bakedFor(1, 0, 0),
          "occlusion field: deterministic — same layout, scrambled build order -> identical bytes");
  }
  {
    // Bounded memory: radius clamps to kMaxRadius; out-of-field reads occluded.
    OcclusionField f;
    auto air = [](int, int, int) { return false; };
    f.bake(0, 0, 0, air, 1000);
    bool ok = f.radius() == OcclusionField::kMaxRadius;
    ok = ok && !f.occluded(0.5f, 0.5f, 0.5f);
    ok = ok && !f.occluded(32.5f, 0.5f, 0.5f);   // edge voxel 32: inside
    ok = ok && f.occluded(33.5f, 0.5f, 0.5f);    // voxel 33: outside -> occluded
    ok = ok && f.occluded(-33.5f, 0.5f, 0.5f);
    ok = ok && near(f.enclosure(), 0.0, 1e-9);
    check(ok, "occlusion field: radius clamps to 32, out-of-field reads occluded");
    OcclusionField g;
    g.bake(0, 0, 0, air, 0);
    check(g.radius() == 1 && !g.occluded(0.5f, 0.5f, 0.5f) &&
              !g.occluded(1.5f, 0.5f, 0.5f) && g.occluded(2.5f, 0.5f, 0.5f) &&
              !g.bakedFor(5, 5, 5),
          "occlusion field: degenerate radius clamps to 1");
  }
  {
    // Amortized == direct on random layouts. Bake once, then perceive each
    // event through the field; compare against the existing per-pair API fed
    // a per-pair flood (same visibility model, evaluated per pair — the
    // direct path). The reference uses DFS in reverse neighbor order to the
    // field's BFS on purpose: reachability is order-independent, so exact
    // agreement proves the field captures visibility, not traversal order.
    // Bit-identical volumes prove the bake amortizes cost without changing
    // answers.
    constexpr int kR = 16;
    std::mt19937 rng(0x0CC10510);
    std::set<std::tuple<int, int, int>> solids;
    for (int z = -kR; z <= kR; ++z)
      for (int y = -kR; y <= kR; ++y)
        for (int x = -kR; x <= kR; ++x)
          if (!(x == 0 && y == 0 && z == 0) && rng() % 100 < 18)
            solids.emplace(x, y, z);
    auto isSolid = [&](int x, int y, int z) { return solids.count({x, y, z}) != 0; };
    OcclusionField field;
    field.bake(0, 0, 0, isSolid, kR);
    auto perPairFlood = [&](float, float, float, float x1, float y1, float z1) {
      const int tx = int(std::floor(x1)), ty = int(std::floor(y1)), tz = int(std::floor(z1));
      if (tx < -kR || tx > kR || ty < -kR || ty > kR || tz < -kR || tz > kR) return true;
      if (isSolid(tx, ty, tz)) return true;
      const int S = 2 * kR + 1;
      std::vector<char> seen(size_t(S) * size_t(S) * size_t(S), 0);
      auto idx = [&](int x, int y, int z) {
        return (size_t(z + kR) * size_t(S) + size_t(y + kR)) * size_t(S) + size_t(x + kR);
      };
      struct C { int x, y, z; };
      std::vector<C> stack{{0, 0, 0}};
      seen[idx(0, 0, 0)] = 1;
      static constexpr int kNb[6][3] = {{0,0,-1},{0,0,1},{0,-1,0},{0,1,0},{-1,0,0},{1,0,0}};
      while (!stack.empty()) {
        const C c = stack.back();
        stack.pop_back();
        if (c.x == tx && c.y == ty && c.z == tz) return false;  // reached: audible
        for (const auto& nb : kNb) {
          const int nx = c.x + nb[0], ny = c.y + nb[1], nz = c.z + nb[2];
          if (nx < -kR || nx > kR || ny < -kR || ny > kR || nz < -kR || nz > kR) continue;
          if (isSolid(nx, ny, nz)) continue;
          const size_t ni = idx(nx, ny, nz);
          if (seen[ni]) continue;
          seen[ni] = 1;
          stack.push_back({nx, ny, nz});
        }
      }
      return true;  // unreachable: occluded
    };
    SpatialAudio sa;
    const Listener mob{0.25f, 0.5f, 0.75f, 0.0f, 1.0f, 64.0f};
    bool ok = true;
    for (int i = 0; i < 300 && ok; ++i) {
      SoundEvent e;
      e.x = float(int(rng() % (2 * kR + 1)) - kR) + 0.5f;
      e.y = float(int(rng() % (2 * kR + 1)) - kR) + 0.5f;
      e.z = float(int(rng() % (2 * kR + 1)) - kR) + 0.5f;
      e.type = 1;
      e.volume = 0.1f + 0.9f * float(rng() % 1000) / 1000.0f;
      e.tick = 0;
      e.sourceId = uint32_t(1000 + i);
      sa.emit(e);
      const PerceivedSound direct = sa.perceive(e, mob, perPairFlood);
      const PerceivedSound baked = sa.perceive(e, mob, field);
      const PerceivedSound viaQuery = sa.perceive(e, mob, field.query());
      ok = direct.volume == baked.volume && direct.pan == baked.pan &&
           direct.delay == baked.delay && baked.volume == viaQuery.volume;
    }
    const auto heardDirect = sa.audibleSounds(mob, perPairFlood);
    const auto heardBaked = sa.audibleSounds(mob, field);
    const auto heardQuery = sa.audibleSounds(mob, field.query());
    ok = ok && heardDirect.size() == heardBaked.size() &&
         heardBaked.size() == heardQuery.size();
    for (size_t i = 0; i < heardDirect.size() && ok; ++i)
      ok = heardDirect[i].sourceId == heardBaked[i].sourceId &&
           heardBaked[i].sourceId == heardQuery[i].sourceId;
    check(ok, "occlusion field: amortized path agrees bit-exactly with the direct per-pair path on random layouts");
  }
  // ---- Phase A: audio-path optimizations ----
  {
    // (1) Voice virtualization: inaudible voices keep their timeline (envelope,
    // phase, noise register, sweep, hold countdown) but skip mixing entirely;
    // un-virtualizing resumes bit-identically.
    const Adsr vadsr{0.01f, 0.05f, 0.8f, 0.2f};
    auto makeSynth = [&] {
      Synth s(SynthPreset::sid3(), kRate);
      s.noteOn(0, {220.0, Waveform::Saw, Duty::Half, vadsr, 0.7f, 3.0, 0.5f});  // sweep + hold
      s.noteOn(1, {440.0, Waveform::Pulse, Duty::Quarter, vadsr, 0.4f});
      s.noteOn(2, {110.0, Waveform::Noise, Duty::Half, vadsr, 0.5f, 0.f, 0.f, true});  // LFSR state
      return s;
    };
    auto renderSeg = [&](Synth& s, size_t n) {
      std::vector<float> b(n, 0.f);
      s.mixInto(b);
      return b;
    };
    auto same = [&](const std::vector<float>& a, const std::vector<float>& b) {
      return a.size() == b.size() &&
             std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
    };
    Synth ref = makeSynth();
    const auto rA = renderSeg(ref, 1000);
    renderSeg(ref, 2000);
    const auto rC = renderSeg(ref, 1000);
    Synth virt = makeSynth();
    const auto vA = renderSeg(virt, 1000);
    virt.setVirtualized(0, true);
    virt.setVirtualized(1, true);
    virt.setVirtualized(2, true);
    const bool threeVirtual = virt.virtualizedCount() == 3;
    const auto vB = renderSeg(virt, 2000);  // timelines advance, nothing mixed
    virt.setVirtualized(0, false);
    virt.setVirtualized(1, false);
    virt.setVirtualized(2, false);
    const auto vC = renderSeg(virt, 1000);
    const bool virtSilent =
        std::all_of(vB.begin(), vB.end(), [](float s) { return s == 0.f; });
    // A fresh noteOn starts audible again (clears virtualization).
    virt.setVirtualized(0, true);
    virt.noteOn(0, {220.0, Waveform::Saw, Duty::Half, vadsr, 0.7f});
    const bool retriggerAudible = !virt.virtualized(0) && virt.virtualizedCount() == 0;
    check(threeVirtual && same(rA, vA) && same(rC, vC) && virtSilent && retriggerAudible,
          "virtualization: silent advance, bit-identical resume (sweep/hold/noise), retrigger un-virtualizes");
  }
  {
    // (1) Virtualization happens BEFORE stealing: the oldest inaudible voice
    // goes first, so a new source never kills an audible note while a
    // virtualized voice sits idle.
    VoiceAllocator va(3);
    const size_t a = va.acquire(10), b = va.acquire(11), c = va.acquire(12);
    (void)c;
    va.setVirtualized(b, true);  // middle voice is inaudible (not the oldest)
    const size_t d = va.acquire(13);
    const bool stoleVirtual = (d == b) && !va.virtualized(d);  // flag cleared on acquire
    const size_t e = va.acquire(14);  // nothing virtualized now: steals the oldest (a)
    check(stoleVirtual && e == a && !va.release(11) && va.release(14) == a,
          "allocator: steals oldest virtualized voice before the oldest audible one");
  }
  {
    // (2) SIMD batch mixing is bit-identical to scalar (memcmp, incl. odd sizes
    // and unaligned tails), on SSE2 / NEON / scalar fallback.
    std::mt19937 rng(0x51D);
    bool ok = true;
    for (size_t n : {1u, 2u, 3u, 4u, 5u, 7u, 8u, 16u, 100u, 511u, 512u, 1023u, 4096u}) {
      std::vector<float> dst1(n), dst2(n), src(n);
      for (size_t i = 0; i < n; ++i) {
        const float u = float(rng()) / float(rng.max());
        dst1[i] = dst2[i] = (u - 0.5f) * 8.f;
        src[i] = (float(rng()) / float(rng.max()) - 0.5f) * 2.f;
      }
      simd::addInto(dst1.data(), src.data(), n);
      for (size_t i = 0; i < n; ++i) dst2[i] += src[i];
      ok = ok && std::memcmp(dst1.data(), dst2.data(), n * sizeof(float)) == 0;
      for (int ch : {1, 2, 3}) {
        std::vector<float> o1(n * size_t(ch)), o2(n * size_t(ch));
        simd::applyGainClampDuplicate(o1.data(), src.data(), n, 0.6f, ch);
        for (size_t i = 0; i < n; ++i) {
          const float s = src[i] * 0.6f;
          const float cl = s < -1.f ? -1.f : (s > 1.f ? 1.f : s);
          for (int k = 0; k < ch; ++k) o2[i * size_t(ch) + size_t(k)] = cl;
        }
        ok = ok && std::memcmp(o1.data(), o2.data(), o1.size() * sizeof(float)) == 0;
      }
    }
    check(ok, "SIMD: addInto + gain/clamp/duplicate bit-identical to scalar");
  }
  {
    // (2) Synth::mixInto (SIMD batch) == per-sample scalar reference, bit-exact.
    const Adsr flatAdsr{0.f, 0.f, 1.f, 0.f};
    Synth s(SynthPreset::sid3(), kRate);
    s.noteOn(0, {220.0, Waveform::Saw, Duty::Half, flatAdsr, 0.5f});
    s.noteOn(2, {110.0, Waveform::Pulse, Duty::Eighth, flatAdsr, 0.3f});
    std::vector<float> batched(997, 0.f);
    s.mixInto(batched);
    Synth r(SynthPreset::sid3(), kRate);
    r.noteOn(0, {220.0, Waveform::Saw, Duty::Half, flatAdsr, 0.5f});
    r.noteOn(2, {110.0, Waveform::Pulse, Duty::Eighth, flatAdsr, 0.3f});
    std::vector<float> ref(997, 0.f);
    for (size_t i = 0; i < ref.size(); ++i)
      ref[i] = r.voice(0).render() + r.voice(2).render();
    check(std::memcmp(batched.data(), ref.data(), ref.size() * sizeof(float)) == 0,
          "synth mix: SIMD batch accumulation bit-identical to the scalar per-sample loop");
  }
  {
    // (3) Occlusion bake cadence: re-bake every N ticks, when the listener
    // moves more than X blocks, or on chunk change. Default = chunk change.
    auto air = [](int, int, int) { return false; };
    OcclusionBakeScheduler sched;
    check(sched.cadence().bakeEveryTicks == 0 && sched.cadence().moveThreshold == 0.f &&
              sched.cadence().bakeOnChunkChange && sched.cadence().radius == 16,
          "occlusion cadence: default = bake on chunk change, no tick cadence, radius 16");
    bool ok = sched.maybeBake(0, 0.5f, 0.5f, 0.5f, air);   // first call always bakes
    ok = ok && !sched.maybeBake(1, 0.5f, 0.5f, 0.5f, air);  // same chunk: no re-bake
    ok = ok && !sched.maybeBake(2, 5.5f, 0.5f, 0.5f, air);  // moved, same chunk: no re-bake
    ok = ok && sched.maybeBake(3, 16.5f, 0.5f, 0.5f, air);  // crossed into chunk 1: bakes
    ok = ok && !sched.maybeBake(4, 17.5f, 0.5f, 0.5f, air);  // still chunk 1: no re-bake
    ok = ok && sched.maybeBake(5, -0.5f, 0.5f, 0.5f, air);  // chunk -1 (floor): bakes
    ok = ok && sched.bakes() == 3 && !sched.field().occluded(-0.5f, 0.5f, 0.5f);
    check(ok, "occlusion cadence: default bakes only on chunk change");
    // High-end "ultra" option: per-tick bakes via bakeEveryTicks = 1.
    OcclusionCadence ultra;
    ultra.bakeEveryTicks = 1;
    ultra.bakeOnChunkChange = false;
    OcclusionBakeScheduler su(ultra);
    ok = su.maybeBake(0, 0.5f, 0.5f, 0.5f, air);      // first call always bakes
    ok = ok && su.maybeBake(1, 0.5f, 0.5f, 0.5f, air);  // tick cadence: bakes
    ok = ok && !su.maybeBake(1, 0.5f, 0.5f, 0.5f, air); // same tick twice: no re-bake
    ok = ok && su.bakes() == 2;
    check(ok, "occlusion cadence: ultra option bakes every tick");
    OcclusionCadence tc;
    tc.bakeEveryTicks = 4;
    tc.bakeOnChunkChange = false;
    tc.radius = 8;
    OcclusionBakeScheduler s2(tc);
    ok = true;
    for (uint64_t t = 0; t < 8; ++t)
      ok = ok && (s2.maybeBake(t, 0.5f, 0.5f, 0.5f, air) == (t == 0 || t == 4));
    ok = ok && s2.bakes() == 2 && s2.field().radius() == 8;
    check(ok, "occlusion cadence: every-N-ticks, knobs exposed");
    OcclusionCadence mc;
    mc.bakeEveryTicks = 1000;
    mc.moveThreshold = 2.0f;
    mc.bakeOnChunkChange = false;
    OcclusionBakeScheduler s3(mc);
    ok = s3.maybeBake(0, 0.5f, 0.5f, 0.5f, air);          // first bake
    ok = ok && !s3.maybeBake(1, 1.0f, 0.5f, 0.5f, air);   // moved 0.5 < 2: no bake
    ok = ok && s3.maybeBake(2, 5.5f, 0.5f, 0.5f, air);    // moved 5 > 2: early bake
    ok = ok && s3.bakes() == 2 && s3.field().bakedFor(5, 0, 0) &&
         !s3.field().occluded(5.5f, 0.5f, 0.5f);
    check(ok, "occlusion cadence: move-beyond-threshold triggers an early re-bake");
  }
  {
    // (4) Effect bypass policy: virtualized voices always skip the chain;
    // voices beyond kEffectBypassDistance (48 blocks, reverb tail below the
    // masking floor - see effects.h) skip it too.
    check(kEffectBypassDistance == 48.0f,
          "effect bypass: threshold is the named constant kEffectBypassDistance (48 blocks)");
    check(effectBypassed(true, 0.f) && effectBypassed(true, 1000.f) &&
              effectBypassed(false, 48.01f) && !effectBypassed(false, 48.0f) &&
              !effectBypassed(false, 0.f),
          "effect bypass: virtualized always bypassed; distant (> 48 blocks) bypassed; near kept");
  }
  {
    // (4) Engine wiring: bypassed (the default) == dry, zero frames filtered;
    // enabled -> a real reverb tail appears after the dry note dies.
    const Adsr perc{0.005f, 0.05f, 0.0f, 0.05f};  // self-terminating: decay to 0 -> Idle
    const NoteSpec hit{220.0, Waveform::Saw, Duty::Half, perc, 0.8f};
    auto renderOne = [&](bool bypass) {
      AudioEngine eng(kRate);
      eng.setEffectBypassed(bypass);
      eng.post(AudioCommand::noteOn(Bus::Effects, 3, hit));  // DMG noise voice
      std::vector<float> st(size_t(kRate) * 2, 0.f);        // 1 s stereo
      eng.render(st.data(), size_t(kRate), 2);
      return std::make_pair(std::move(st), eng.effectFramesProcessed());
    };
    const auto dry = renderOne(true);
    const auto wet = renderOne(false);
    // The percussive note is long dead in the second half-second.
    const double dryTail = peak(dry.first, size_t(kRate), dry.first.size());
    const double wetTail = peak(wet.first, size_t(kRate), wet.first.size());
    check(dry.second == 0 && wet.second == size_t(kRate) && dryTail == 0.0 && wetTail > 1e-4,
          "effect bypass: bypassed == dry with zero frames filtered; enabled -> reverb tail");
  }
  {
    // (5) Async streaming: decode runs on the worker thread, never on the
    // audio thread; chunks cross via the double-buffered HandoffQueue.
    constexpr size_t kTotal = 48000;  // 1 s of stereo
    std::mutex m;
    std::vector<std::thread::id> decodeThreads;
    size_t produced = 0;
    AudioStreamer streamer(4096, 8);
    streamer.play([&](float* out, size_t maxFrames) {
      {
        std::lock_guard<std::mutex> l(m);
        decodeThreads.push_back(std::this_thread::get_id());
      }
      size_t n = 0;
      for (; n < maxFrames && produced + n < kTotal; ++n) {
        const double s =
            std::sin(2.0 * 3.141592653589793 * 440.0 * double(produced + n) / double(kRate));
        out[n * 2] = float(s);
        out[n * 2 + 1] = float(s);
      }
      produced += n;
      return n;
    });
    std::vector<float> left;
    left.reserve(kTotal);
    size_t validTotal = 0;
    // Bound the loop by wall time, not by a fixed number of render calls: on
    // a loaded machine (ctest running in parallel) the worker can fall behind,
    // and an underrun render returns 0 frames, so a call count alone could run
    // out before the stream ends. Back off briefly on an underrun.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!streamer.finished() && std::chrono::steady_clock::now() < deadline) {
      std::vector<float> chunk(8192 * 2, 0.f);
      const size_t valid = streamer.render(chunk.data(), 8192);  // the "audio thread"
      if (valid == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      validTotal += valid;
      for (size_t f = 0; f < valid && left.size() < kTotal; ++f) left.push_back(chunk[f * 2]);
    }
    streamer.stop();  // join the worker before reading its id
    std::vector<std::thread::id> ids;
    {
      std::lock_guard<std::mutex> l(m);
      ids = decodeThreads;
    }
    const bool allOnWorker =
        !ids.empty() && std::all_of(ids.begin(), ids.end(), [&](const std::thread::id& id) {
          return id == streamer.workerId();
        });
    const bool neverOnAudioThread = std::none_of(ids.begin(), ids.end(),
                                                 [&](const std::thread::id& id) {
                                                   return id == std::this_thread::get_id();
                                                 });
    check(allOnWorker && neverOnAudioThread && validTotal == kTotal && streamer.finished() &&
              left.size() == kTotal && near(measureFrequency(left), 440.0, 4.4),
          "streaming: worker-thread decode, double-buffered handoff, 440 Hz intact, clean EOS");
  }
  {
    // (6) LoadShedder: hysteresis, escalation, recovery, configurability.
    using ns = std::chrono::nanoseconds;
    const ns budget(10'000'000);  // 10 ms callback budget
    LoadShedder shed;
    check(shed.level() == 0, "shedder: starts at level 0 (current behavior)");
    for (int i = 0; i < 7; ++i) shed.update(ns(8'000'000), budget);  // 0.8 load x7
    const bool noEarly = shed.level() == 0;
    shed.update(ns(8'000'000), budget);  // 8th consecutive over-threshold
    const bool l1 = shed.level() == 1;
    for (int i = 0; i < 8; ++i) shed.update(ns(10'000'000), budget);  // 1.0 load
    const bool l3 = shed.level() == 3;
    for (int i = 0; i < 63; ++i) shed.update(ns(1'000'000), budget);  // calm x63
    const bool noEarlyRecover = shed.level() == 3;
    shed.update(ns(1'000'000), budget);  // 64th calm callback
    const bool recovers = shed.level() == 2;
    check(noEarly && l1 && l3 && noEarlyRecover && recovers,
          "shedder: 0->1->3 after 8 over-budget callbacks, one level per 64 calm");
    DegradationPolicy custom;
    custom.warnLoad = 0.5f;
    custom.escalateAfter = 1;
    LoadShedder shed2(custom);
    shed2.update(ns(6'000'000), budget);
    check(shed2.level() == 1 && shed2.policy().warnLoad == 0.5f, "shedder: thresholds configurable");
  }
  {
    // (6) Engine integration: the shed level drives real actions; with no
    // pressure the level stays 0 and behavior is unchanged.
    using ns = std::chrono::nanoseconds;
    const ns budget(10'000'000);
    AudioEngine eng(kRate);
    std::vector<float> st(2048 * 2, 0.f);
    eng.render(st.data(), 2048, 2);
    check(eng.shedLevel() == 0 && eng.lastAudioLoad() < 0.7,
          "degradation: no pressure -> level 0, behavior unchanged");
    for (int i = 0; i < 8; ++i) eng.updateShed(ns(8'000'000), budget);  // 0.8 load -> level 1
    check(eng.shedLevel() == 1, "engine: shed level follows measured load");
    const Adsr tail{0.005f, 5.0f, 0.9f, 0.1f};
    eng.post(AudioCommand::noteOn(Bus::Effects, 0,
                                  {220.0, Waveform::Pulse, Duty::Half, tail, 0.1f}));
    std::vector<float> blk(512 * 2, 0.f);
    eng.render(blk.data(), 512, 2);  // attack/decay: must never be auto-virtualized
    const bool attackKept = !eng.effects().virtualized(0);
    eng.post(AudioCommand::noteOff(Bus::Effects, 0));
    for (int i = 0; i < 10; ++i) eng.render(blk.data(), 512, 2);  // release tail decays
    const bool tailShed = eng.effects().virtualized(0);
    check(attackKept && tailShed,
          "engine: level 1 virtualizes quiet release tails, never fresh attacks");
    for (int i = 0; i < 8; ++i) eng.updateShed(ns(9'500'000), budget);  // 0.95 load -> level 2
    eng.render(blk.data(), 512, 2);
    const size_t vc = eng.effects().voiceCount();  // DMG = 4
    bool capped = eng.shedLevel() == 2;
    for (size_t i = vc / 2; i < vc; ++i) capped = capped && eng.effects().virtualized(i);
    check(capped, "engine: level 2 caps the voice count (upper half virtualized)");
  }
  {
    // (6) Level 3: half-rate render keeps the timeline (and pitch) exact -
    // aliasing instead of a dropout.
    Synth s(SynthPreset::sid3(), kRate);
    s.setRateDivider(2);
    const Adsr flatAdsr{0.f, 0.f, 1.f, 0.f};
    s.noteOn(0, {440.0, Waveform::Pulse, Duty::Half, flatAdsr, 0.5f});
    std::vector<float> x(48000, 0.f);
    for (size_t off = 0; off < x.size();) {
      const size_t n = std::min(size_t(512), x.size() - off);
      s.mixInto({x.data() + off, n});
      off += n;
    }
    check(near(measureFrequency(x), 440.0, 4.4) && peak(x, 1000, 47000) > 0.4,
          "degradation: half-rate render preserves pitch/timeline (aliasing, not dropout)");
  }
  {
    // (7) Codec stubs: sniffing, WAV decode, FLAC/Opus/Vorbis header parsing.
    auto put16le = [](std::vector<uint8_t>& v, uint16_t x) {
      v.push_back(static_cast<uint8_t>(x & 0xFF));
      v.push_back(static_cast<uint8_t>(x >> 8));
    };
    auto put16be = [](std::vector<uint8_t>& v, uint16_t x) {
      v.push_back(static_cast<uint8_t>(x >> 8));
      v.push_back(static_cast<uint8_t>(x & 0xFF));
    };
    auto put32le = [](std::vector<uint8_t>& v, uint32_t x) {
      v.push_back(static_cast<uint8_t>(x & 0xFF));
      v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
      v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
      v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
    };
    auto put32be = [](std::vector<uint8_t>& v, uint32_t x) {
      v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
      v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
      v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
      v.push_back(static_cast<uint8_t>(x & 0xFF));
    };
    auto tag = [](std::vector<uint8_t>& v, const char* t) {
      for (int i = 0; t[i]; ++i) v.push_back(static_cast<uint8_t>(t[i]));
    };
    auto makeWav = [&](uint16_t fmtTag, uint16_t ch, uint32_t rate, uint16_t bits,
                       const std::vector<uint8_t>& samples) {
      std::vector<uint8_t> v;
      tag(v, "RIFF");
      put32le(v, 36 + static_cast<uint32_t>(samples.size()));
      tag(v, "WAVE");
      tag(v, "fmt ");
      put32le(v, 16);
      put16le(v, fmtTag);
      put16le(v, ch);
      put32le(v, rate);
      put32le(v, rate * ch * bits / 8);
      put16le(v, static_cast<uint16_t>(ch * bits / 8));
      put16le(v, bits);
      tag(v, "data");
      put32le(v, static_cast<uint32_t>(samples.size()));
      v.insert(v.end(), samples.begin(), samples.end());
      return v;
    };
    auto makeOggPage = [&](const std::vector<uint8_t>& packet) {
      std::vector<uint8_t> v;
      tag(v, "OggS");
      v.push_back(0);  // version
      v.push_back(2);  // BOS flag
      for (int i = 0; i < 8; ++i) v.push_back(0);  // granule position
      put32le(v, 0x12345678);                      // serial
      put32le(v, 0);                               // sequence
      put32le(v, 0);                               // checksum (stub skips verify)
      v.push_back(1);                              // one segment
      v.push_back(static_cast<uint8_t>(packet.size()));
      v.insert(v.end(), packet.begin(), packet.end());
      return v;
    };
    std::string err;

    // --- sniffing ---
    std::vector<uint8_t> wavProbe = makeWav(1, 1, 8000, 8, {128});
    std::vector<uint8_t> flacProbe = {'f', 'L', 'a', 'C'};
    std::vector<uint8_t> opusHead;
    tag(opusHead, "OpusHead");
    std::vector<uint8_t> opusProbe = makeOggPage(opusHead);
    std::vector<uint8_t> vorbisIdent = {0x01};
    tag(vorbisIdent, "vorbis");
    std::vector<uint8_t> vorbisProbe = makeOggPage(vorbisIdent);
    std::vector<uint8_t> junk = {'N', 'O', 'T', 'A', 'U', 'D', 'I', 'O'};
    check(sniffAudioCodec(wavProbe.data(), wavProbe.size()) == AudioCodec::kWav &&
              sniffAudioCodec(flacProbe.data(), flacProbe.size()) == AudioCodec::kFlac &&
              sniffAudioCodec(opusProbe.data(), opusProbe.size()) == AudioCodec::kOpus &&
              sniffAudioCodec(vorbisProbe.data(), vorbisProbe.size()) == AudioCodec::kVorbis &&
              sniffAudioCodec(junk.data(), junk.size()) == AudioCodec::kUnknown &&
              sniffAudioCodec(nullptr, 0) == AudioCodec::kUnknown,
          "codecs: sniff wav/flac/opus/vorbis/unknown");

    // --- WAV 16-bit stereo decode ---
    {
      // frame0: L=0, R=32767; frame1: L=-32768, R=-32767
      std::vector<uint8_t> s = {0x00, 0x00, 0xFF, 0x7F, 0x00, 0x80, 0x01, 0x80};
      std::vector<uint8_t> wav = makeWav(1, 2, 44100, 16, s);
      auto dec = makeAudioDecoder(wav.data(), wav.size(), err);
      bool ok = dec && dec->codec() == AudioCodec::kWav && dec->implemented();
      AudioFormatInfo f = dec ? dec->format() : AudioFormatInfo{};
      ok = ok && f.valid && f.sampleRate == 44100 && f.channels == 2 &&
           f.bitsPerSample == 16 && f.totalFrames == 2;
      float out[4] = {9.f, 9.f, 9.f, 9.f};
      const size_t got = dec ? dec->decode(out, 2, err) : 0;
      ok = ok && got == 2 && near(out[0], 0.0, 1e-6) &&
           near(out[1], 32767.0 / 32768.0, 1e-6) && near(out[2], -1.0, 1e-6) &&
           near(out[3], -32767.0 / 32768.0, 1e-6);
      const size_t eos = dec ? dec->decode(out, 2, err) : 99;
      ok = ok && eos == 0;  // end of stream
      check(ok, "codecs: wav 16-bit stereo decodes exactly, EOS at end");
    }
    // --- WAV 8-bit mono: unsigned conversion + mono->stereo duplication ---
    {
      std::vector<uint8_t> wav = makeWav(1, 1, 22050, 8, {0, 128, 255});
      auto dec = makeAudioDecoder(wav.data(), wav.size(), err);
      float out[6] = {9.f, 9.f, 9.f, 9.f, 9.f, 9.f};
      const size_t got = dec ? dec->decode(out, 3, err) : 0;
      bool ok = got == 3 && near(out[0], -1.0, 1e-6) && near(out[1], -1.0, 1e-6) &&
                near(out[2], 0.0, 1e-6) && near(out[3], 0.0, 1e-6) &&
                near(out[4], 127.0 / 128.0, 1e-6) && near(out[5], 127.0 / 128.0, 1e-6);
      check(ok, "codecs: wav 8-bit mono converts unsigned and duplicates to stereo");
    }
    // --- WAV float32 stereo ---
    {
      float src[2] = {0.5f, -0.25f};
      std::vector<uint8_t> s(8);
      std::memcpy(s.data(), src, 8);
      std::vector<uint8_t> wav = makeWav(3, 2, 48000, 32, s);
      auto dec = makeAudioDecoder(wav.data(), wav.size(), err);
      float out[2] = {9.f, 9.f};
      const size_t got = dec ? dec->decode(out, 1, err) : 0;
      check(got == 1 && near(out[0], 0.5, 1e-6) && near(out[1], -0.25, 1e-6),
            "codecs: wav float32 stereo passes through exactly");
    }
    // --- WAV error paths ---
    {
      std::vector<uint8_t> wav = makeWav(1, 2, 44100, 16, {0, 0, 0, 0});
      std::vector<uint8_t> cut(wav.begin(), wav.begin() + 20);  // truncated header
      auto d1 = makeAudioDecoder(cut.data(), cut.size(), err);
      bool ok = !d1 && !err.empty();
      std::vector<uint8_t> alaw = makeWav(6, 1, 8000, 8, {128});  // A-law: unsupported
      auto d2 = makeAudioDecoder(alaw.data(), alaw.size(), err);
      ok = ok && !d2 && !err.empty();
      std::vector<uint8_t> many = makeWav(1, 6, 48000, 16, std::vector<uint8_t>(24, 0));
      auto d3 = makeAudioDecoder(many.data(), many.size(), err);  // 6ch: rejected
      ok = ok && !d3 && !err.empty();
      check(ok, "codecs: wav truncated / unsupported tag / >stereo fail cleanly");
    }
    // --- FLAC: STREAMINFO parses, frames stubbed ---
    {
      std::vector<uint8_t> flac;
      tag(flac, "fLaC");
      flac.push_back(0x80);  // last block, type 0 = STREAMINFO
      flac.push_back(0); flac.push_back(0); flac.push_back(34);
      put16be(flac, 4096);  // min block size
      put16be(flac, 4096);  // max block size
      flac.push_back(0); flac.push_back(0); flac.push_back(0);  // min frame size
      flac.push_back(0); flac.push_back(0); flac.push_back(0);  // max frame size
      const uint32_t rate = 44100, total = 1000;
      flac.push_back(static_cast<uint8_t>(rate >> 12));
      flac.push_back(static_cast<uint8_t>((rate >> 4) & 0xFF));
      flac.push_back(static_cast<uint8_t>(((rate & 15) << 4) | ((2 - 1) << 1) | ((16 - 1) >> 4)));
      flac.push_back(static_cast<uint8_t>((((16 - 1) & 15) << 4) | ((uint64_t(total) >> 32) & 15)));  // top 4 of the 36-bit sample count
      put32be(flac, total);
      flac.insert(flac.end(), 16, 0);  // MD5 (stub does not verify)
      auto dec = makeAudioDecoder(flac.data(), flac.size(), err);
      bool ok = dec && dec->codec() == AudioCodec::kFlac && !dec->implemented();
      AudioFormatInfo f = dec ? dec->format() : AudioFormatInfo{};
      ok = ok && f.valid && f.sampleRate == 44100 && f.channels == 2 &&
           f.bitsPerSample == 16 && f.totalFrames == 1000;
      float out[2];
      const size_t got = dec ? dec->decode(out, 1, err) : 99;
      ok = ok && got == 0 && !err.empty();  // stubbed: clean not-implemented error
      check(ok, "codecs: flac STREAMINFO parses (44.1k/stereo/16-bit/1000f), frames stubbed");
    }
    // --- Opus: OpusHead parses, packets stubbed ---
    {
      std::vector<uint8_t> head;
      tag(head, "OpusHead");
      head.push_back(1);    // version
      head.push_back(2);    // channels
      put16le(head, 312);   // pre-skip
      put32le(head, 48000);  // input sample rate
      put16le(head, 0);     // output gain
      head.push_back(0);    // channel mapping family
      std::vector<uint8_t> ogg = makeOggPage(head);
      auto dec = makeAudioDecoder(ogg.data(), ogg.size(), err);
      bool ok = dec && dec->codec() == AudioCodec::kOpus && !dec->implemented();
      AudioFormatInfo f = dec ? dec->format() : AudioFormatInfo{};
      ok = ok && f.valid && f.sampleRate == 48000 && f.channels == 2;
      float out[2];
      const size_t got = dec ? dec->decode(out, 1, err) : 99;
      ok = ok && got == 0 && !err.empty();
      check(ok, "codecs: opus OpusHead parses (48k/stereo), packets stubbed");
    }
    // --- Vorbis: identification header parses, packets stubbed ---
    {
      std::vector<uint8_t> ident = {0x01};
      tag(ident, "vorbis");
      put32le(ident, 0);       // version
      ident.push_back(2);      // channels
      put32le(ident, 44100);   // sample rate
      put32le(ident, 0);       // bitrate maximum
      put32le(ident, 128000);  // bitrate nominal
      put32le(ident, 0);       // bitrate minimum
      std::vector<uint8_t> ogg = makeOggPage(ident);
      auto dec = makeAudioDecoder(ogg.data(), ogg.size(), err);
      bool ok = dec && dec->codec() == AudioCodec::kVorbis && !dec->implemented();
      AudioFormatInfo f = dec ? dec->format() : AudioFormatInfo{};
      ok = ok && f.valid && f.sampleRate == 44100 && f.channels == 2;
      float out[2];
      const size_t got = dec ? dec->decode(out, 1, err) : 99;
      ok = ok && got == 0 && !err.empty();
      check(ok, "codecs: vorbis identification parses (44.1k/stereo), packets stubbed");
    }
    // --- factory rejects unknown data ---
    {
      auto dec = makeAudioDecoder(junk.data(), junk.size(), err);
      check(!dec && !err.empty(), "codecs: factory rejects unknown data with an error");
    }
  }

  // --- agent-audio tap (MCP audio.capture_chunk) ---
  {
    AudioEngine eng(48000.f);
    float out[4096];
    check(!eng.tapEnabled(), "audio: tap starts disarmed");
    check(eng.drainTap(out, 4096) == 0, "audio: drainTap on a fresh engine yields nothing");
    eng.setTapEnabled(true);
    check(eng.tapEnabled(), "audio: tap arms");
    float mix[512];
    eng.render(mix, 512, 1);  // silence, but the tap still records frames
    size_t n = eng.drainTap(out, 4096);
    bool silent = n == 512;
    for (size_t i = 0; i < n; ++i) silent = silent && out[i] == 0.f;
    check(silent, "audio: silent mix taps as 512 zero frames");
    // A real note taps as nonzero samples.
    NoteSpec tone{noteFrequency(69), Waveform::Pulse, Duty::Quarter, {0.005f, 0.05f, 0.6f, 0.05f}, 0.5f};
    eng.post(AudioCommand::noteOn(Bus::Music, 0, tone));
    eng.render(mix, 512, 1);
    n = eng.drainTap(out, 4096);
    float peak = 0.f;
    for (size_t i = 0; i < n; ++i) peak = std::max(peak, std::fabs(out[i]));
    check(n == 512 && peak > 0.001f, "audio: tap captures the playing note (nonzero)");
    eng.post(AudioCommand::noteOff(Bus::Music, 0));
    // Degradation ladder intact: force full shed (level 3); the tap keeps working.
    for (int i = 0; i < 10; ++i)  // hysteresis: escalateAfter = 8 consecutive
      eng.updateShed(std::chrono::nanoseconds(1000000000), std::chrono::nanoseconds(1));
    check(eng.shedLevel() == 3, "audio: forced shed reaches level 3");
    eng.render(mix, 512, 1);
    n = eng.drainTap(out, 4096);
    check(n == 512, "audio: tap drains under full shed (ladder intact)");
    // Disarm: nothing recorded afterwards.
    eng.setTapEnabled(false);
    eng.render(mix, 512, 1);
    check(eng.drainTap(out, 4096) == 0, "audio: disarmed tap records nothing");
    // Overflow drops the oldest instead of growing: 2 s capacity = 96000 frames.
    eng.setTapEnabled(true);
    for (int i = 0; i < 200; ++i) eng.render(mix, 512, 1);  // 102400 frames > capacity
    n = eng.drainTap(out, 4096);
    check(n == 4096, "audio: tap ring overflows by dropping oldest (bounded)");
  }

  // OcclusionThread: dedicated 32/16/8 Hz occlusion sim, clean-room.
  {
    OcclusionThread oc;
    // start(Inline) must not spawn a thread or poison later start() calls.
    oc.setOcclusionTest([](float, float, float, float, float, float) { return false; });
    oc.start(OcclusionRate::Inline);
    check(oc.rate() == OcclusionRate::Inline, "occlusion: start(Inline) sets rate without spawning");
    oc.start(OcclusionRate::Full);  // must actually start now (regression: running_ stuck true)
    check(oc.rate() == OcclusionRate::Full, "occlusion: start(Full) after Inline really starts");
    size_t q = oc.addQuery(0, 0, 0, 10, 0, 0);
    // Gain starts at 1.0 (clear) before the thread's first tick.
    check(oc.gain(q) == 1.0f, "occlusion: fresh query gain is 1.0 (clear)");
    // Let the thread tick at least once (32 Hz => ~31 ms); never blocks the test.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    // Test occlusion fn returns false (clear) => gain stays 1.0 after smoothing.
    check(oc.gain(q) > 0.99f, "occlusion: clear path keeps gain at 1.0");
    oc.removeQuery(q);
    oc.stop();
    // Out-of-range gain reads are safe.
    check(oc.gain(9999) == 1.0f, "occlusion: out-of-range gain reads 1.0 (safe)");
  }
  {
    // H-AUD-1: gain() reads the lock-free published snapshot — queries added
    // (structural appends) are visible to gain() with no thread running.
    OcclusionThread oc;
    const size_t q0 = oc.addQuery(0, 0, 0, 10, 0, 0);
    const size_t q1 = oc.addQuery(0, 0, 0, 20, 0, 0);
    check(oc.gain(q0) == 1.0f && oc.gain(q1) == 1.0f && oc.gain(9999) == 1.0f,
          "occlusion: gain() sees appended queries via the published snapshot");
    // H-AUD-3: setInline(false) leaves inline mode and restarts at Full
    // (the old false branch was a no-op; recovery needed an explicit start()).
    oc.setOcclusionTest([](float, float, float, float, float, float) { return false; });
    oc.setInline(true);
    const bool wasInline = oc.rate() == OcclusionRate::Inline;
    oc.setInline(false);
    check(wasInline && oc.rate() == OcclusionRate::Full,
          "occlusion: setInline(false) exits inline mode and restarts at Full");
    oc.stop();
  }
  {
    // voxelRaycastBlocked: same-voxel rays are never blocked (no wall can
    // sit between two points in one voxel). Regression: the DDA used to
    // step out and test neighbors on sub-voxel rays.
    auto solid = [](int, int, int) { return true; };  // everything solid
    check(!voxelRaycastBlocked(2.2f, 2.7f, 2.1f, 2.8f, 2.3f, 2.9f, solid),
          "raycast: same-voxel ray is clear even when all neighbors are solid");
    check(voxelRaycastBlocked(2.5f, 2.5f, 2.5f, 5.5f, 2.5f, 2.5f, solid),
          "raycast: cross-voxel ray through solid is blocked");
    auto air = [](int, int, int) { return false; };
    check(!voxelRaycastBlocked(0.5f, 0.5f, 0.5f, 60.5f, 0.5f, 0.5f, air),
          "raycast: long ray through air is clear");
    check(voxelRaycastBlocked(0.5f, 0.5f, 0.5f, 100.5f, 0.5f, 0.5f, air),
          "raycast: beyond 64-block hearing range reads blocked");
  }

  std::printf(fails ? "AUDIO SELFTEST FAILED (%d)\n" : "AUDIO SELFTEST OK\n", fails);
  return fails;
}

}  // namespace litholattice
