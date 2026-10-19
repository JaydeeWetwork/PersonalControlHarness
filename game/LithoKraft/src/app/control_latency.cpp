// Input-to-application latency harness for the control path (JD's
// control-serialization design, 2026-10-13): how long does a control edit
// take from the producer's submit call to its application on the sim
// thread? Command edits carry a sequence index in SimCommand::a; the
// fixture simulation records the application timestamp in command().
// Two patterns: paced (one submit every ~2 ms) and burst (64 submits at
// once). Measured at 512 TPS, so one tick = 1.953125 ms.
#include "control_latency.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "litholattice/sim/sim_thread.h"
#include "litholattice/world/chunk_map.h"
#include "lithokraft/content/blocks.h"

namespace lithokraft {
namespace {

using Clock = std::chrono::steady_clock;

struct LatencySim final : litholattice::IWorldSimulation {
  std::vector<int64_t>* applyNs = nullptr;  // indexed by SimCommand::a
  std::atomic<int>* applied = nullptr;
  void scan(const litholattice::ChunkMap&) override {}
  void onBlockChanged(litholattice::ChunkMap&, int, int, int, litholattice::BlockId) override {}
  void tick(litholattice::ChunkMap&, const litholattice::TickArea&, const DirtyFn&) override {}
  void command(litholattice::ChunkMap&, const litholattice::SimCommand& c, const DirtyFn&) override {
    const int64_t t = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    if (c.a >= 0 && size_t(c.a) < applyNs->size()) (*applyNs)[size_t(c.a)] = t;
    applied->fetch_add(1, std::memory_order_acq_rel);
  }
};

int64_t nowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

double pct(std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  const size_t i = std::min(sorted.size() - 1, size_t(p * double(sorted.size())));
  return sorted[i];
}

void report(const char* name, std::vector<double> ms, double tickMs) {
  std::sort(ms.begin(), ms.end());
  std::printf("control-latency %s: n=%zu min=%.3f p50=%.3f p95=%.3f p99=%.3f max=%.3f ms | p50=%.2f p99=%.2f max=%.2f ticks\n",
              name, ms.size(), ms.empty() ? 0.0 : ms.front(), pct(ms, 0.50), pct(ms, 0.95), pct(ms, 0.99),
              ms.empty() ? 0.0 : ms.back(), pct(ms, 0.50) / tickMs, pct(ms, 0.99) / tickMs,
              (ms.empty() ? 0.0 : ms.back()) / tickMs);
}

}  // namespace

int runControlLatency() {
  using namespace litholattice;
  ChunkMap w;
  w.setRegistry(&blockRegistry());
  for (int cx = -1; cx <= 1; ++cx)
    for (int cz = -1; cz <= 1; ++cz) {
      auto c = std::make_unique<ChunkColumn>();
      c->cx = cx; c->cz = cz;
      w.insert(std::move(c));
    }

  constexpr int kPaced = 300;
  constexpr int kBurst = 64;
  std::vector<int64_t> applyNs(size_t(kPaced + kBurst), 0);
  std::atomic<int> applied{0};
  auto sim = std::make_unique<LatencySim>();
  sim->applyNs = &applyNs;
  sim->applied = &applied;

  SimConfig cfg;
  cfg.tickRate = 512;
  cfg.radiusChunks = 1;
  cfg.randomTicksPerSection = 0;
  SimThread st(w, blockRegistry(), std::move(sim), std::move(cfg), [](std::shared_ptr<MeshInput>) {});
  st.setPlayerChunk(0, 0);
  st.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let the loop reach steady pace

  const double tickMs = 1000.0 / 512.0;
  std::vector<int64_t> submitNs(size_t(kPaced + kBurst), 0);

  // ---- paced submits: one every ~2 ms (just over one tick) ----
  for (int i = 0; i < kPaced; ++i) {
    EditCmd e{EditCmd::Kind::Command};
    e.command = SimCommand{/*code*/ 1, 0, 0, 0, /*a*/ i, 0};
    submitNs[size_t(i)] = nowNs();
    st.edit(e);
    std::this_thread::sleep_for(std::chrono::microseconds(2000));
  }
  // ---- burst submits: 64 at once ----
  const auto burstStart = Clock::now();
  for (int j = 0; j < kBurst; ++j) {
    const int i = kPaced + j;
    EditCmd e{EditCmd::Kind::Command};
    e.command = SimCommand{1, 0, 0, 0, i, 0};
    submitNs[size_t(i)] = nowNs();
    st.edit(e);
  }
  (void)burstStart;

  const auto deadline = Clock::now() + std::chrono::seconds(15);
  while (applied.load(std::memory_order_acquire) < kPaced + kBurst && Clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  const int got = applied.load();
  st.stop();
  std::printf("control-latency: applied %d / %d command edits\n", got, kPaced + kBurst);
  if (got < kPaced + kBurst) {
    std::printf("CONTROL LATENCY: FAILED (lost edits)\n");
    return 1;
  }
  std::vector<double> pacedMs, burstMs;
  for (int i = 0; i < kPaced; ++i)
    pacedMs.push_back(double(applyNs[size_t(i)] - submitNs[size_t(i)]) / 1e6);
  for (int j = 0; j < kBurst; ++j) {
    const int i = kPaced + j;
    burstMs.push_back(double(applyNs[size_t(i)] - submitNs[size_t(i)]) / 1e6);
  }
  report("paced", pacedMs, tickMs);
  report("burst", burstMs, tickMs);
  std::printf("CONTROL LATENCY: OK\n");
  return 0;
}

}  // namespace lithokraft
