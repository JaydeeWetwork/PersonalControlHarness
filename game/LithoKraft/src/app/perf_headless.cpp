// --perf-headless [json]: windowless measurements. (1) Worldgen and mesh throughput at
// 1 / 2 / 4 / all threads. (2) Save and load: ms, bytes on disk and the autosave hitch (the
// main-thread encode while the sim is paused) for 16 / 32 / 64-chunk worlds. (3) Stress circuits
// (clock grid, wire meshes, repeater chains) on the real SimThread at 32..512 TPS: tick ms
// avg / p99, components, remeshes per second.
#include "litholattice/ai/ai_world.h"
#include "perf_headless.h"
#include "litholattice/render/texture_array.h"
#include "lithokraft/game/bench_stream.h"
#include <algorithm>
#include "litholattice/sim/random_ticks.h"
#include <cmath>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>
#include "lithokraft/content/appearance.h"
#include "lithokraft/content/blocks.h"
#include "lithokraft/content/circuit_scene.h"
#include "lithokraft/content/signal_world.h"
#include "lithokraft/game/dimensions.h"
#include "lithokraft/game/world_save.h"
#include "litholattice/core/process_memory.h"
#include "litholattice/core/threading.h"
#include "litholattice/core/time.h"
#include "litholattice/render/block_materials.h"
#include "litholattice/render/mesher.h"
#include "litholattice/sim/sim_thread.h"
#include "litholattice/world/mesh_input.h"

namespace lithokraft {
namespace {

/// fopen for writing without MSVC's C4996 (fopen_s there).
inline std::FILE* openForWrite(const char* path) {
#if defined(_MSC_VER)
  std::FILE* f = nullptr;
  return fopen_s(&f, path, "w") == 0 ? f : nullptr;
#else
  return std::fopen(path, "w");
#endif
}

double pct(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
}
double avg(const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return v.empty() ? 0 : s / double(v.size()); }

/// Times every tick() of the wrapped simulation (SimThread calls it on the sim thread only).
class TimedSimulation final : public IWorldSimulation {
 public:
  explicit TimedSimulation(std::unique_ptr<IWorldSimulation> s) : s_(std::move(s)) {}
  void scan(const ChunkMap& w) override { s_->scan(w); }
  bool canPlace(const ChunkMap& w, int x, int y, int z, BlockId b) const override { return s_->canPlace(w, x, y, z, b); }
  bool stillSupported(const ChunkMap& w, int x, int y, int z) const override { return s_->stillSupported(w, x, y, z); }
  void onBlockChanged(ChunkMap& w, int x, int y, int z, BlockId b) override { s_->onBlockChanged(w, x, y, z, b); }
  bool use(ChunkMap& w, int x, int y, int z, const DirtyFn& d) override { return s_->use(w, x, y, z, d); }
  void command(ChunkMap& w, const SimCommand& c, const DirtyFn& d) override { s_->command(w, c, d); }
  void tick(ChunkMap& w, const TickArea& a, const DirtyFn& d) override {
    const auto t0 = Clock::now();
    s_->tick(w, a, d);
    const double ms = msBetween(t0, Clock::now());
    std::lock_guard l(m_); ticks_.push_back(ms);
  }
  size_t activeCount() const override { return s_->activeCount(); }
  void takeEvents(std::vector<SimEvent>& out) override { s_->takeEvents(out); }
  void registerHandshake(litholattice::TimingHandshakeAuthority& a) override { s_->registerHandshake(a); }
  SignalSim* signalSim() {  // phase-split timings for the sweep (hs-design §7)
    auto* sw = dynamic_cast<SignalWorldSimulation*>(s_.get());
    return sw ? &sw->signal() : nullptr;
  }
  std::vector<double> take() { std::lock_guard l(m_); return std::move(ticks_); }
 private:
  std::unique_ptr<IWorldSimulation> s_;
  std::mutex m_;
  std::vector<double> ticks_;
};

/// Holds meshes like a renderer would (bytes by key), so the stream soak sees what a real
/// backend would keep alive.
class NullRenderer final : public IRenderer {
 public:
  struct NullTextures final : ITextureArray {
    bool upload(const TextureRegistry&) override { return true; }
    int layerCount() const override { return 0; }
    int tileSize() const override { return 0; }
  };
  const char* name() const override { return "null"; }
  std::string deviceInfo() const override { return "null"; }
  ITextureArray& textureArray() override { return tex_; }
  void setMaterials(const BlockMaterials&) override {}
  int loadShaders(const ShaderLibrary&) override { return 0; }
  void uploadEntityMesh(const std::vector<PackedVertex>&) override {}
  void uploadChunkMesh(const ChunkMesh& m) override {
    const int64_t k = packPos(m.cx, m.sy, m.cz);
    if (m.verts.empty()) meshes_.erase(k); else meshes_[k] = m.verts.size() * sizeof(PackedVertex);
  }
  void clearChunkMeshes() override { meshes_.clear(); }
  void renderFrame(const FrameParams&) override {}
  RenderStats stats() const override { return {uint32_t(meshes_.size()), 0}; }
  size_t bytes() const { size_t b = 0; for (const auto& [k, v] : meshes_) b += v; return b; }
 private:
  NullTextures tex_;
  std::unordered_map<int64_t, size_t> meshes_;
};

}  // namespace

int runPerfHeadless(const std::string& jsonPath, const BenchSceneConfig& bench) {
  std::string json = "{\n";
  DimensionRegistry reg = createDimensions();
  Dimension* wafer = reg.find("wafer");
  if (!wafer || !wafer->generator) { std::printf("perf: no wafer generator\n"); return 1; }
  const unsigned all = std::max(1u, std::thread::hardware_concurrency());

  // (1) Thread scaling
  std::printf("== worldgen / mesh throughput by thread count (%u hardware threads)\n", all);
  json += "  \"thread_scaling\": [";
  std::vector<unsigned> counts{1, 2, 4};
  if (all > 4) counts.push_back(all);
  const BlockMaterials mats(blockRegistry().size());
  for (size_t ci = 0; ci < counts.size(); ++ci) {
    const unsigned n = counts[ci];
    JobPool pool(n);
    constexpr int kSide = 16;  // 256 columns
    HandoffQueue<std::unique_ptr<ChunkColumn>> out;
    auto t0 = Clock::now();
    {
      JobBatch b(pool);
      for (int z = 0; z < kSide; ++z)
        for (int x = 0; x < kSide; ++x) b.submit([&, x, z] { out.push(wafer->generator->generate(x + 1000, z)); });
    }
    const double genS = secondsBetween(t0, Clock::now());
    std::vector<std::unique_ptr<ChunkColumn>> cols;
    out.drainInto(cols);
    ChunkMap map;
    for (auto& c : cols) if (c) map.insert(std::move(c));
    std::vector<std::shared_ptr<MeshInput>> ins;
    for (const auto& [k, col] : map.columns())
      for (int sy = 0; sy < kSubChunks; ++sy)
        if (col->subs[size_t(sy)]) ins.push_back(snapshotForMeshing(map, blockRegistry(), col->cx, sy, col->cz, 0, 0, wafer->belowWorldFill));
    std::atomic<size_t> verts{0};
    t0 = Clock::now();
    {
      JobBatch b(pool);
      for (auto& in : ins) b.submit([&, in] { verts += meshSubChunk(*in, MeshContext{blockRegistry(), mats, &blockAppearance()}).verts.size(); });
    }
    const double meshS = secondsBetween(t0, Clock::now());
    std::printf("  threads=%-3u gen %.0f columns/s (%.2f ms/column avg)   mesh %.0f sub-chunks/s\n", n, 256 / genS, genS * 1000.0 * n / 256,
                double(ins.size()) / meshS);
    char buf[200];
    std::snprintf(buf, sizeof buf, "%s\n    {\"threads\": %u, \"gen_columns_per_s\": %.1f, \"mesh_subchunks_per_s\": %.1f}", ci ? "," : "", n, 256 / genS,
                  double(ins.size()) / meshS);
    json += buf;
  }
  json += "\n  ],\n";

  // (2) Save / load
  std::printf("== save / load (wafer, all threads)\n");
  json += "  \"save_load\": [";
  JobPool pool(all);
  const int sizes[] = {16, 32, 64};
  for (size_t si = 0; si < std::size(sizes); ++si) {
    const int size = sizes[si];
    wafer->load(pool, size);
    SignalSim sig;
    sig.scan(wafer->chunks);
    const auto dir = std::filesystem::temp_directory_path() / ("lithokraft_perf_save_" + std::to_string(size));
    std::filesystem::remove_all(dir);
    double hitchMs = 0, saveMs = 0, loadMs = 0, dirtyHitchMs = 0;
    uint64_t bytes = 0;
    {
      WorldSave save(dir);
      auto t0 = Clock::now();
      save.saveDimension("wafer", wafer->chunks, sig, false);  // worst case: every column changed (pause = copy all)
      hitchMs = msBetween(t0, Clock::now());
      save.flush();
      saveMs = msBetween(t0, Clock::now());
      // Typical autosave: the columns the sim remeshed since the last save (here 2% of them).
      std::unordered_set<uint64_t> dirty;
      size_t i = 0;
      for (const auto& [k, col] : wafer->chunks.columns()) if (i++ % 50 == 0) dirty.insert(k);
      for (int rep = 0; rep < 3; ++rep) {  // best of 3: the first pause after a big free also pays page faults
        t0 = Clock::now();
        save.saveDimension("wafer", wafer->chunks, sig, false, &dirty);
        const double ms = msBetween(t0, Clock::now());
        dirtyHitchMs = rep ? std::min(dirtyHitchMs, ms) : ms;
        save.flush();
      }
    }
    for (const auto& e : std::filesystem::recursive_directory_iterator(dir)) if (e.is_regular_file()) bytes += e.file_size();
    ChunkMap fresh;
    for (const auto& [k, col] : wafer->chunks.columns()) { auto c = std::make_unique<ChunkColumn>(); c->cx = col->cx; c->cz = col->cz; fresh.insert(std::move(c)); }
    {
      WorldSave save(dir);
      std::vector<uint8_t> blob;
      const auto t0 = Clock::now();
      save.loadDimension("wafer", fresh, blob);
      loadMs = msBetween(t0, Clock::now());
    }
    const size_t colsN = wafer->chunks.columns().size();
    std::printf("  %3dx%-3d chunks (%5zu columns): autosave pause %.2f ms (2%% changed) / %.1f ms (all changed), full save %.1f ms, load %.1f ms, "
                "%.2f MB on disk (%.0f B/column)\n", size, size, colsN, dirtyHitchMs, hitchMs, saveMs, loadMs, double(bytes) / 1048576.0,
                double(bytes) / double(colsN));
    char buf[300];
    std::snprintf(buf, sizeof buf, "%s\n    {\"chunks\": %d, \"columns\": %zu, \"autosave_pause_ms\": %.3f, \"pause_all_changed_ms\": %.2f, \"save_ms\": %.2f, \"load_ms\": %.2f, \"bytes\": %llu}",
                  si ? "," : "", size, colsN, dirtyHitchMs, hitchMs, saveMs, loadMs, (unsigned long long)bytes);
    json += buf;
    std::filesystem::remove_all(dir);
  }
  json += "\n  ],\n";

  // (2a) Random ticks: 8 picks per section per world tick around the player, every block
  // opted in (worst case: each pick reaches the game callback), radius 8 and 12 chunks.
  std::printf("== ai (wafer 32x32 terrain, wandering walkers, 8 path searches per AI tick)\n");
  json += "  \"ai\": [";
  {
    namespace ai = litholattice::ai;
    DimensionRegistry areg = createDimensions();
    Dimension* ad = areg.find("wafer");
    ad->load(pool, 32);
    ai::WalkCache nav(ad->chunks, blockRegistry(), {});
    auto walk = ai::makeWalk();
    int ai_i = 0;
    for (int cap : {64, 128}) {
      ai::AiConfig cfg; cfg.mobCap = cap; cfg.pathsPerTick = 8; cfg.seed = 7;
      ai::AiWorld aw(nav, cfg, &pool);
      for (int i = 0; i < cap; ++i) {
        const int x = 16 * 16 + (i % 16) * 4, z = 16 * 16 + (i / 16) * 4;
        int y = kWorldHeight - 2;
        while (y > 1 && !blockRegistry().has(ad->chunks.get(x, y - 1, z), kSolid)) --y;
        std::vector<ai::UtilityAction> acts = {{"wander", {[](const ai::AiContext&) { return 1.f; }}, 1.f, [](ai::AiContext& c) {
          if (!c.mob.goal) c.mob.goal = ai::Cell{c.mob.pos.x + int(c.rng.next() % 41) - 20, c.mob.pos.y, c.mob.pos.z + int(c.rng.next() % 41) - 20};
        }}};
        aw.spawn({x, y, z}, walk.get(), ai::btUtility(std::move(acts)));
      }
      aw.setObservers({{16 * 16 + 32, 0, 16 * 16 + 32}});
      for (int t = 0; t < 20; ++t) aw.tick();  // warm the walk cache
      MsSamples ms;
      for (int t = 0; t < 200; ++t) { aw.tick(); ms.add(aw.stats().tickMs); }
      std::printf("  %3d mobs: %.3f ms/AI tick avg, %.3f ms p99 (%.2f%% of a 10 Hz tick), %zu cached columns, %zu B cached nav\n", cap, MsSamples::avg(ms.run),
                  MsSamples::pct(ms.run, 0.99), MsSamples::avg(ms.run) / 100.0 * 100.0, nav.cachedColumns(), nav.cachedBytes());
      char buf[200];
      std::snprintf(buf, sizeof buf, "%s\n    {\"mobs\": %d, \"ms_avg\": %.4f, \"ms_p99\": %.4f}", ai_i++ ? "," : "", cap, MsSamples::avg(ms.run),
                    MsSamples::pct(ms.run, 0.99));
      json += buf;
    }
    ad->unload();
  }
  json += "\n  ],\n";

  std::printf("== random ticks (wafer 32x32, every block opted in, 8 per section)\n");
  json += "  \"random_ticks\": [";
  {
    DimensionRegistry rreg = createDimensions();
    Dimension* rt = rreg.find("wafer");
    rt->load(pool, 32);
    const std::vector<uint8_t> everyBlock(blockRegistry().size(), 1);
    uint64_t sink = 0;
    int ri = 0;
    for (int radius : {8, 12}) {
      constexpr int kTicks = 200;
      uint64_t sampled = 0;
      MsSamples ms;
      for (int t = 0; t < kTicks; ++t) {
        const auto t0 = Clock::now();
        sampled += runRandomTicks(rt->chunks, blockRegistry(), everyBlock, 16, 16, radius, uint64_t(t), kDefaultRandomTicksPerSection,
                                  [&](int x, int y, int z, BlockId b, uint32_t r) { sink += uint64_t(x ^ y ^ z ^ b) + r; });
        ms.add(msBetween(t0, Clock::now()));
      }
      std::printf("  radius %2d: %.0f picks/tick, %.3f ms/tick avg, %.3f ms p99 (%.2f%% of a 32 TPS tick)\n", radius, double(sampled) / kTicks,
                  MsSamples::avg(ms.run), MsSamples::pct(ms.run, 0.99), MsSamples::avg(ms.run) / 31.25 * 100.0);
      char buf[200];
      std::snprintf(buf, sizeof buf, "%s\n    {\"radius\": %d, \"picks_per_tick\": %.0f, \"ms_avg\": %.4f, \"ms_p99\": %.4f}", ri++ ? "," : "", radius,
                    double(sampled) / kTicks, MsSamples::avg(ms.run), MsSamples::pct(ms.run, 0.99));
      json += buf;
    }
    if (sink == 42) std::printf(" ");
    rt->unload();
  }
  json += "\n  ],\n";

  // (2b) Stream soak: the --bench-path stream flight (32 blocks/s, radius 8) for 10 simulated
  // minutes at 60 updates/s, with a renderer stand-in that keeps meshes like a backend. Shows
  // whether streaming state plateaus (caches) or grows without bound (leak).
  std::printf("== stream soak (10 simulated minutes, 60 updates/s, null renderer)\n");
  json += "  \"stream_soak\": [";
  {
    NullRenderer nr;
    const auto matsPtr = std::make_shared<const BlockMaterials>(blockRegistry().size());
    BenchStreamer bs(nr, pool, wafer->generator, 16, wafer->belowWorldFill, 8);
    const uint64_t ram0 = processWorkingSetBytes();
    for (int minute = 1; minute <= 10; ++minute) {
      for (int step = 0; step < 3600; ++step) {
        const float t = float((minute - 1) * 3600 + step) / 60.f;
        bs.update(16 * 16 - 32 + 32.f * std::fmod(t, 60.f), 128.f, matsPtr);  // like soak: each minute restarts the flight
        if ((step & 15) == 0) pool.waitIdle();  // keep the generation in step with the simulated clock
      }
      bs.takeSecond();
      const uint64_t ram = processWorkingSetBytes();
      std::printf("  minute %2d: ram %+.1f MB, loaded columns %zu, requested %zu, meshed columns %zu, renderer meshes %u (%.1f MB)\n", minute,
                  (double(ram) - double(ram0)) / 1048576.0, bs.loadedCount(), bs.requestedCount(), bs.meshedColumnCount(), nr.stats().meshes,
                  double(nr.bytes()) / 1048576.0);
      char buf[220];
      std::snprintf(buf, sizeof buf, "%s\n    {\"minute\": %d, \"ram_delta_mb\": %.1f, \"loaded\": %zu, \"requested\": %zu, \"renderer_meshes\": %u}",
                    minute > 1 ? "," : "", minute, (double(ram) - double(ram0)) / 1048576.0, bs.loadedCount(), bs.requestedCount(), nr.stats().meshes);
      json += buf;
    }
  }
  json += "\n  ],\n";

  // (3) Stress circuits on the real SimThread (or the requested --bench-scene)
  const bool useBenchScene = bench.scene != "default" && bench.scene != "circuits";
  std::printf("== stress %s, 3 s per rate\n", useBenchScene ? bench.scene.c_str() : "circuits (clock grid + wire meshes + repeater chains)");
  json += "  \"circuits\": [";
  wafer->load(pool, 8);
  const int y = wafer->chunks.surfaceY(8, 8) + 1;
  int placed = 0;
  if (bench.scene == "display") {
    placed = buildDisplayScene(wafer->chunks, 4, y, 4, bench.size);
  } else if (bench.scene == "framebuffer") {
    buildFramebufferScene(wafer->chunks, 4, y, 4, bench.size);
    placed = bench.size * bench.size;
  } else if (bench.scene == "redirector") {
    placed = buildRedirectorScene(wafer->chunks, 4, y, 4, bench.size);
  } else if (bench.scene == "wall-pixel") {
    buildWallPixelScene(wafer->chunks, 4, y, 4, bench.size, bench.rows);
    placed = bench.size * bench.rows;
  } else if (bench.scene == "pattern-wall") {
    buildPatternWall(wafer->chunks, 4, y, 4, bench.size, bench.pattern);
    placed = bench.size * bench.size;
  } else if (bench.scene == "sawtooth") {
    buildSawtoothWall(wafer->chunks, 4, y, 4, bench.size, bench.rows, bench.pattern);
    placed = bench.size * bench.rows;
  } else if (bench.scene == "wire-torture") {
    buildWireTorture(wafer->chunks, 4, y, 4);
    placed = 24 * 48;
  } else {
    placed = buildCircuitScene(wafer->chunks, 4, y, 4, 120);
  }
  const int rates[] = {32, 64, 128, 256, 512};
  for (size_t ri = 0; ri < std::size(rates); ++ri) {
    std::atomic<int> remesh{0};
    auto timed = std::make_unique<TimedSimulation>(std::make_unique<SignalWorldSimulation>());
    TimedSimulation* t = timed.get();
    SimConfig sc;
    sc.tickRate = rates[ri]; sc.radiusChunks = 8; sc.belowWorldFill = wafer->belowWorldFill;
    sc.timingHandshake = bench.timingHandshake; sc.handshakePhasesPerCycle = bench.handshakePhases;
    SimThread sim(wafer->chunks, blockRegistry(), std::move(timed), std::move(sc), [&](std::shared_ptr<MeshInput>) { ++remesh; });
    sim.setPlayerChunk(4, 4);
    sim.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    t->take(); remesh = 0;
    std::this_thread::sleep_for(std::chrono::seconds(3));
    const std::vector<double> ticks = t->take();
    const int rm = remesh.load();
    const SimStats st = sim.stats();
    // Handshake-only numbers (hs-design §7): overruns, mean barrier wait, phase split.
    uint64_t hsOverruns = 0, hsOverrunNs = 0, hsBarrierNs = 0, hsPhases = 0;
    double phComps = 0, phMark = 0, phWires = 0, phLamps = 0;
    if (bench.timingHandshake && sim.handshakeAuthority()) {
      // A-M6: this reader is the MAIN thread while the sim still runs —
      // it must take a snapshot, never the authority's live stats() struct.
      const litholattice::HsStats hs = sim.handshakeAuthority()->statsSnapshot();
      hsOverruns = hs.overrunPhases; hsOverrunNs = hs.overrunNsTotal;
      hsBarrierNs = hs.barrierWaitNsTotal; hsPhases = hs.phases;
    }
    if (SignalSim* sig = t->signalSim()) {
      const SignalSim::PhaseMs pm = sig->takePhaseMs();
      phComps = pm.comps; phMark = pm.mark; phWires = pm.wires; phLamps = pm.lamps;
    }
    sim.stop();
    std::printf("  %3d TPS: %4zu ticks/3s (%.0f achieved), tick avg %.3f ms p99 %.3f ms, components %u, remesh %.0f/s, budget %.1f%%%s\n", rates[ri],
                ticks.size(), double(ticks.size()) / 3.0, avg(ticks), pct(ticks, 0.99), st.active, rm / 3.0, avg(ticks) * rates[ri] / 10.0,
                bench.timingHandshake ? " [handshake]" : " [legacy]");
    if (bench.timingHandshake)
      std::printf("    handshake: overrun phases %llu, overrun ns %llu, mean barrier wait %.0f ns/phase, phase ms comps %.2f mark %.2f wires %.2f lamps %.2f\n",
                  (unsigned long long)hsOverruns, (unsigned long long)hsOverrunNs,
                  hsPhases ? double(hsBarrierNs) / double(hsPhases) : 0.0, phComps, phMark, phWires, phLamps);
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s\n    {\"tps\": %d, \"achieved_tps\": %.1f, \"tick_ms_avg\": %.4f, \"tick_ms_p99\": %.4f, \"components\": %u, \"remesh_per_s\": %.1f, \"mode\": \"%s\", \"phases_per_cycle\": %d, \"hs_overrun_phases\": %llu, \"hs_overrun_ns\": %llu, \"hs_barrier_wait_ns_mean\": %.1f, \"phase_ms\": {\"comps\": %.3f, \"mark\": %.3f, \"wires\": %.3f, \"lamps\": %.3f}}",
                  ri ? "," : "", rates[ri], double(ticks.size()) / 3.0, avg(ticks), pct(ticks, 0.99), st.active, rm / 3.0,
                  bench.timingHandshake ? "handshake" : "legacy", bench.handshakePhases,
                  (unsigned long long)hsOverruns, (unsigned long long)hsOverrunNs,
                  hsPhases ? double(hsBarrierNs) / double(hsPhases) : 0.0, phComps, phMark, phWires, phLamps);
    json += buf;
  }
  char tail[160];
  std::snprintf(tail, sizeof tail, "\n  ],\n  \"circuit_blocks\": %d,\n  \"ram_mb\": %.1f\n}\n", placed, double(processWorkingSetBytes()) / 1048576.0);
  json += tail;
  std::printf("circuit scene: %d blocks; process RAM %.1f MB\n", placed, double(processWorkingSetBytes()) / 1048576.0);
  if (!jsonPath.empty())
    if (std::FILE* f = openForWrite(jsonPath.c_str())) { std::fputs(json.c_str(), f); std::fclose(f); }
  return 0;
}

}  // namespace lithokraft
