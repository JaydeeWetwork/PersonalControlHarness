// litholattice_tests: engine-only tests (no game code is linked). Every module's checks can run
// on their own: `litholattice_tests <module>` (ctest runs one test per module), or all of them
// with no argument. Fixture blocks and simulations stand in for a game.
#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include "litholattice/assets/assets_selftest.h"
#include "litholattice/audio/audio_selftest.h"
#include "litholattice/core/threading.h"
#include "litholattice/core/job_system.h"
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#include "litholattice/core/tick_arena.h"
#include "litholattice/core/fail_fast.h"
#if !defined(_WIN32)
#include <cstdlib>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <csignal>
#endif
#include "litholattice/input/input_selftest.h"
#include "litholattice/mods/mods_selftest.h"
#include "litholattice/crypto/crypto_selftest.h"
#include "litholattice/subdiv/subdiv_selftest.h"
#include "litholattice/texture/texture_selftest.h"
#include "litholattice/procgen/procgen_selftest.h"
#include "litholattice/anim/anim_selftest.h"
#include "litholattice/procgen/trees.h"
#include "litholattice/procgen/shrubs.h"
#include "litholattice/procgen/water.h"
#include "litholattice/procgen/litho_formula.h"
#include "litholattice/procgen/litho_formula_2d.h"
#include "litholattice/render/block_materials.h"
#include "litholattice/render/mesher.h"
#include "litholattice/render/binary_greedy_mesher.h"
#include "litholattice/render/micro_mesher.h"
#include "litholattice/subdiv/octree.h"
#include "litholattice/subdiv/subdiv_store.h"
#include "litholattice/render/pbr_materials.h"
#include "litholattice/render/material_payload.h"
#include "litholattice/render/pbr_brdf.h"
#include "litholattice/render/pbr_lights.h"
#include "litholattice/render/pbr_transparency_tiers.h"
#include "litholattice/render/pbr_software_shaders.h"
#include "litholattice/render/pbr_emissive_provider.h"
#include "litholattice/render/ground_pulse.h"
#include "litholattice/render/ground_traces.h"
#include "litholattice/render/pbr_gl_upload.h"
#include "litholattice/render/pbr_quad_mesh.h"
#include "litholattice/core/math.h"
#include "litholattice/render/cull_prototypes.h"
#include "litholattice/render/software_renderer.h"
#include "litholattice/render/frame_pacer.h"
#include "litholattice/render/texture_array.h"
#include "litholattice/sim/det_sched.h"
#include "litholattice/sim/random_ticks.h"
#include "litholattice/sim/trace_jit.h"
#include "litholattice/sim/trace_jit_compose.h"
#include "litholattice/sim/signal_propagate.h"
#include "litholattice/assets/box_model.h"
#include "litholattice/ai/ai_world.h"
#include "litholattice/assets/mod_manifest.h"
#include "litholattice/storage/block_names.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include "litholattice/sim/sim_thread.h"
#include "litholattice/storage/storage.h"
#include "litholattice/storage/anvil_region.h"
#include "litholattice/storage/file_contract.h"
#include "litholattice/storage/async_io.h"
#include "litholattice/storage/mod_sandbox.h"
#include "litholattice/storage/sandboxed_io.h"
#if defined(__linux__)
#include <sys/stat.h>  // mkfifo (sandboxed-open FIFO refusal test)
#endif
#include <filesystem>
#include <fstream>
#include <random>
#include "litholattice/ui/ui_selftest.h"
#include "litholattice/world/chunk_map.h"
#include "litholattice/world/mesh_input.h"
#include "litholattice/world/raycast.h"
#include "litholattice/net/priority.h"
#include "litholattice/net/protocol.h"
#include "litholattice/net/quantize.h"
#include "litholattice/net/snapshot.h"
#include "litholattice/net/channels.h"
#include "litholattice/net/confinement.h"
#include "litholattice/net/confinement_selftest.h"
#include "litholattice/net/handshake.h"
#include "litholattice/net/encryption.h"
#include "litholattice/script/script_engine.h"
#include "litholattice/script/suspension_policy.h"
#include "litholattice/core/log.h"
#include "litholattice/core/test_env.h"
#include "litholattice/script/script_binding_allowlist.h"
#include "litholattice/console/console_selftest.h"
#include "litholattice/index/index_selftest.h"
#include "litholattice/mcp/mcp_selftest.h"
#include "litholattice/defrag/defrag_selftest.h"
#include "litholattice/mods/mod_interface.h"
#include "litholattice/mods/mod_loader.h"
#include "litholattice/render/software_test.h"
#include "litholattice/render_vk/sprite_vertices.h"  // B2: Vulkan sprite conversion contract

using namespace litholattice;

namespace {

int g_fails = 0;
bool g_throwCopyArmed = false;  // arms the throwing-copy fixture in testJobSystem (A-M4)
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fails;
}

// ---- fixture "game": three blocks
constexpr BlockId kRock = 1, kHalf = 2, kPlant = 3;
const BlockRegistry& fixtureBlocks() {
  static const BlockRegistry reg = [] {
    BlockRegistry r;
    r.reserve(4);
    r.add({"Rock", kSolid | kOpaque, Shape::Cube, "rock"});
    r.add({"Half", kSolid, Shape::Slab, "half"});
    r.add({"Plant", kTransparent, Shape::Cross, "plant"});
    return r;
  }();
  return reg;
}

// Identity name remap for a registry: save-time BlockId i -> its StableId.
// Mirrors what readStableRemap() produces when the save's name table matches the
// live registry exactly (used for round-trip tests that skip the LLBN file).
storage::StableRemap identityStableRemap(const BlockRegistry& reg) {
  storage::StableRemap r;
  r.map.reserve(reg.size());
  for (size_t i = 0; i < reg.size(); ++i) r.map.push_back(reg.stableIdOf(BlockId(i)));
  return r;
}

// Polls `pred` until it holds or 5 s pass (a deadline, not a fixed sleep).
template <class F> bool waitUntil(F pred) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!pred()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::yield();
  }
  return true;
}

ChunkMap worldWithColumns(int n, const BlockRegistry* reg = nullptr) {
  ChunkMap w;
  w.setRegistry(reg);  // Infinite Registry: get()/set() translate BlockId <-> StableId
  for (int cz = 0; cz < n; ++cz)
    for (int cx = 0; cx < n; ++cx) {
      auto c = std::make_unique<ChunkColumn>();
      c->cx = cx; c->cz = cz;
      w.insert(std::move(c));
    }
  return w;
}

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ULL) {
  for (size_t i = 0; i < n; ++i) { h ^= static_cast<const unsigned char*>(p)[i]; h *= 1099511628211ULL; }
  return h;
}

void testTickArena();  // defined below testCore
void testDetSched();   // defined below testRandomTicks
void testTraceJitCompose();  // defined below testDetSched
void testSignalPropagate();  // defined below testTraceJitCompose
void testJobSystem();  // defined below testCore
void testFileContract();  // defined below testAnvilRegion
void testAsyncIo();       // defined below testFileContract
void testModSandbox();    // defined below testAsyncIo
void testSandboxedOpen();  // defined below testModSandbox
void testBackupDepth();    // defined below testFileContract

void testCore() {
  std::printf("core: threading primitives\n");
  {
    TripleBuffer<int> tb;
    tb.writeSlot() = 1; tb.publish();
    tb.writeSlot() = 2; tb.publish();
    const int a = tb.read(), b = tb.read();
    check(a == 2 && b == 2, "triple buffer: reader sees the latest published value, stays on it");
  }
  {
    TripleBuffer<uint64_t> tb;
    std::atomic<bool> stop{false};
    std::thread writer([&] { for (uint64_t i = 1; !stop; ++i) { tb.writeSlot() = i; tb.publish(); } });
    uint64_t last = 0; bool monotonic = true;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);  // the writer may start late
    while (last < 100000 && std::chrono::steady_clock::now() < until) {
      const uint64_t v = tb.read(); monotonic = monotonic && v >= last; last = v;
    }
    stop = true; writer.join();
    check(monotonic && last > 0, "triple buffer across threads: values never go backwards");
  }
  {
    HandoffQueue<int> q;
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t) producers.emplace_back([&q, t] { for (int i = 0; i < 10000; ++i) q.push(t * 10000 + i); });
    std::vector<int> got;
    for (auto& p : producers) p.join();
    q.drainInto(got);
    long long sum = 0; for (int v : got) sum += v;
    check(got.size() == 40000 && sum == 40000LL * 39999 / 2, "hand-off queue: 4 producers, nothing lost or duplicated");
  }
  {
    JobPool pool(3);
    std::atomic<int> n{0};
    for (int i = 0; i < 1000; ++i) pool.submit([&n] { ++n; });
    pool.waitIdle();
    const bool all = n == 1000;
    std::printf("  (two job errors are expected next)\n"); std::fflush(stdout);
    JobBatch batch(pool);
    for (int i = 0; i < 10; ++i) batch.submit([i] { if (i % 5 == 0) throw std::runtime_error("fixture"); });
    const int failed = batch.wait();
    check(all && failed == 2 && pool.failedJobs() == 0, "job pool runs every job; a batch counts its throwing jobs, the pool survives");
  }
  {
    // Review A-L3: parse success must mean valid JSON — no trailing input
    // after the top value, and no strtod-only number spellings.
    Json j;
    bool ok = true;
    ok = ok && Json::parse("{\"a\":1}", j) && j.getNum("a") == 1;
    ok = ok && Json::parse("  [1, -2.5e2, true, null]  ", j);
    ok = ok && !Json::parse("{\"a\":1} garbage", j);
    ok = ok && !Json::parse("{\"a\":1} {\"b\":2}", j);
    ok = ok && !Json::parse("NaN", j);
    ok = ok && !Json::parse("Infinity", j);
    ok = ok && !Json::parse("-Infinity", j);
    ok = ok && !Json::parse("0x1p4", j);
    ok = ok && !Json::parse("[1, 0x10]", j);
    ok = ok && !Json::parse("01", j);        // leading zero is not JSON
    ok = ok && !Json::parse("1.", j);        // fraction needs a digit
    ok = ok && !Json::parse("[truex]", j);   // literal must end the token
    ok = ok && Json::parse("-0.5e+10", j) && j.num == -0.5e10;
    check(ok, "core: Json::parse rejects trailing input, NaN/Infinity/hex-float spellings, and malformed numbers");
  }
  testJobSystem();
  testTickArena();
}

void testFailFast() {
  std::printf("core: fail-fast corruption policy\n");
  {
    litholattice::SimCorruptionReport r;
    r.what = "hash-mismatch";
    r.subsystem = "det_sched";
    r.file = "det_sched.cpp";
    r.line = 110;
    r.tick = 1234;
    r.expectedHash = 0x0123456789abcdefULL;
    r.actualHash = 0xfedcba9876543210ULL;
    r.detail = "chunk (3,-5) root hash changed after tick";
    const std::string text = litholattice::formatSimCorruptionReport(r);
    const bool ok = text.find("hash-mismatch") != std::string::npos &&
                    text.find("det_sched") != std::string::npos &&
                    text.find("det_sched.cpp") != std::string::npos &&
                    text.find("110") != std::string::npos && text.find("1234") != std::string::npos &&
                    text.find("0123456789abcdef") != std::string::npos &&
                    text.find("fedcba9876543210") != std::string::npos &&
                    text.find("chunk (3,-5)") != std::string::npos;
    check(ok, "fail-fast: formatted report contains what, subsystem, file, line, tick, both hashes, detail");
  }
  {
    litholattice::SimCorruptionReport r;  // all optional fields left empty
    r.what = "exception-in-tick";
    r.subsystem = "sim_thread";
    const std::string text = litholattice::formatSimCorruptionReport(r);
    check(text.find("<unknown>") != std::string::npos && text.find("detail: -") != std::string::npos,
          "fail-fast: unknown tick/file render as <unknown>, empty detail as -");
  }
  check(!litholattice::simStateCorrupted(0xdeadbeefULL, 0xdeadbeefULL),
        "fail-fast: matching hashes => no corruption (stays silent)");
  check(litholattice::simStateCorrupted(0xdeadbeefULL, 0xdeadbeefULL ^ 1ULL),
        "fail-fast: mismatched hashes => corruption flagged");
  {
    const std::runtime_error e(
        "det-sched: a system job threw; tick aborted (world may be half-updated)");
    const litholattice::SimCorruptionReport r =
        litholattice::simCorruptionFromException(e, "det_sched", 777);
    check(r.what == "exception-in-tick" && r.subsystem == "det_sched" && r.tick == 777 &&
              r.detail == e.what(),
          "fail-fast: scheduler throw converts to a report carrying subsystem, tick, and message");
  }
#if defined(_WIN32)
  std::printf("  (skipped: POSIX-only subprocess death test)\n");
#else
  // Death path: calling simFailFast in-process would abort the test binary, so the
  // child is forked; it must die on SIGABRT (no return, no unwind, no continue).
  {
    char errPath[] = "/tmp/litholattice_failfast_stderr_XXXXXX";
    const int fd = ::mkstemp(errPath);
    const bool made = fd >= 0;
    check(made, "fail-fast: death-test stderr capture file created");
    bool died = false, reported = false;
    if (made) {
      const pid_t pid = ::fork();
      check(pid >= 0, "fail-fast: death-test child forked");
      if (pid == 0) {
        ::dup2(fd, STDERR_FILENO);
        ::close(fd);
        litholattice::SimCorruptionReport r;
        r.what = "death-test-probe";
        r.subsystem = "test";
        r.tick = 42;
        litholattice::simFailFast(r);  // must not return
        _exit(99);                     // unreachable; guards against a returning abort
      }
      ::close(fd);
      int status = 0;
      ::waitpid(pid, &status, 0);
      died = WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
      std::FILE* f = std::fopen(errPath, "r");
      if (f) {
        char line[1024];
        while (std::fgets(line, sizeof line, f))
          if (std::strstr(line, "FATAL sim corruption") && std::strstr(line, "death-test-probe"))
            reported = true;
        std::fclose(f);
      }
      ::unlink(errPath);
    }
    check(died, "fail-fast: simFailFast aborts the process (SIGABRT), never returns");
    check(reported, "fail-fast: the report reached stderr before the abort");
  }
#endif
}

void testJobSystem() {
  std::printf("core: work-stealing job system (Chase-Lev deques, priorities, fences, io, affinity)\n");
  {
    ChaseLevDeque<int, 16> dq;
    for (int i = 0; i < 14; ++i) { int v = i; dq.pushBottom(std::move(v)); }
    bool ok = true; int expect = 13, v = -1;
    while (dq.popBottom(v)) ok = ok && v == expect--;
    check(ok && expect == -1, "chase-lev: owner push/pop is LIFO");
  }
  {
    ChaseLevDeque<int, 16> dq;
    bool ok = true;
    for (int i = 0; i < 15; ++i) { int v = i; ok = ok && dq.pushBottom(std::move(v)); }
    int v = 99;
    ok = ok && !dq.pushBottom(std::move(v)) && v == 99;  // full: the item is untouched
    check(ok, "chase-lev: a full deque refuses the push without touching the item");
  }
  {
    // Review A-M4: popBottom copies BEFORE committing bottom_, so a throwing
    // copy leaves the job queued (the old order lost it and leaked the
    // caller's in-flight count, hanging waitIdle).
    struct ThrowCopy {
      int v = -1;
      ThrowCopy() = default;
      explicit ThrowCopy(int x) : v(x) {}
      ThrowCopy(const ThrowCopy& o) : v(o.v) { if (g_throwCopyArmed) throw std::runtime_error("copy boom"); }
      ThrowCopy(ThrowCopy&& o) noexcept : v(o.v) {}
      ThrowCopy& operator=(const ThrowCopy& o) { if (g_throwCopyArmed) throw std::runtime_error("copy boom"); v = o.v; return *this; }
      ThrowCopy& operator=(ThrowCopy&& o) noexcept { v = o.v; return *this; }
    };
    ChaseLevDeque<ThrowCopy, 16> dq;
    for (int i = 0; i < 3; ++i) { ThrowCopy t(i); dq.pushBottom(std::move(t)); }
    ThrowCopy out;
    g_throwCopyArmed = true;
    bool threw = false;
    try { dq.popBottom(out); } catch (const std::exception&) { threw = true; }
    g_throwCopyArmed = false;
    bool ok = threw && dq.popBottom(out) && out.v == 2 && dq.popBottom(out) && out.v == 1 &&
              dq.popBottom(out) && out.v == 0 && !dq.popBottom(out);
    check(ok, "chase-lev: a throwing copy in popBottom loses nothing — the job stays queued and pops later");
  }
  {
    ChaseLevDeque<int, 64> dq;
    for (int i = 0; i < 63; ++i) { int v = i; dq.pushBottom(std::move(v)); }
    std::atomic<int> stolen{0}, stolenSum{0};
    std::vector<std::thread> thieves;
    for (int t = 0; t < 3; ++t)
      thieves.emplace_back([&] { int v; while (dq.steal(v)) { ++stolen; stolenSum += v; } });
    int v = 0, popped = 0, poppedSum = 0;
    while (popped + stolen.load(std::memory_order_relaxed) < 63)
      if (dq.popBottom(v)) { ++popped; poppedSum += v; }
    for (auto& th : thieves) th.join();
    check(stolen + popped == 63 && stolenSum + poppedSum == 63 * 62 / 2,
          "chase-lev: owner + 3 thieves consume every item exactly once");
  }
  {
    JobSystem::Config cfg; cfg.workerThreads = 4;
    JobSystem js(cfg);
    std::atomic<int> n{0};
    for (int i = 0; i < 20000; ++i) js.submit([&n] { ++n; });
    js.waitIdle();
    check(n == 20000 && js.failedJobs() == 0, "jobsystem: 20000 jobs all run on 4 workers; waitIdle returns");
  }
  {
    JobSystem::Config cfg; cfg.workerThreads = 2;
    JobSystem js(cfg);
    std::atomic<int> n{0};
    for (int i = 0; i < 100; ++i)
      js.submit([&, i] { ++n; if (i % 10 == 0) throw std::runtime_error("fixture"); });
    js.waitIdle();
    check(n == 100 && js.failedJobs() == 10, "jobsystem: throwing jobs are counted, the system survives");
  }
  {
    // Priority order, deterministic: one worker held behind a gate while 20 low +
    // 20 high jobs queue; after the gate opens every high must finish before any low.
    JobSystem::Config cfg; cfg.workerThreads = 1;
    JobSystem js(cfg);
    std::atomic<bool> gate{false}, entered{false};
    js.submit([&] { entered.store(true, std::memory_order_release);
                    while (!gate.load(std::memory_order_acquire)) std::this_thread::yield(); },
              JobPriority::High);
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    std::mutex m; std::vector<int> order; order.reserve(40);
    for (int i = 0; i < 20; ++i) {
      js.submit([&] { std::lock_guard<std::mutex> l(m); order.push_back(0); }, JobPriority::Low);
      js.submit([&] { std::lock_guard<std::mutex> l(m); order.push_back(1); }, JobPriority::High);
    }
    gate.store(true, std::memory_order_release);
    js.waitIdle();
    bool ok = order.size() == 40;
    for (size_t i = 0; ok && i < 20; ++i) ok = order[i] == 1;
    for (size_t i = 20; ok && i < 40; ++i) ok = order[i] == 0;
    check(ok, "jobsystem: high-priority jobs drain before low-priority jobs (1 worker, gated)");
  }
  {
    // Steal storm: nested submits from worker threads across 4 workers.
    JobSystem::Config cfg; cfg.workerThreads = 4;
    JobSystem js(cfg);
    std::atomic<int> n{0};
    for (int i = 0; i < 64; ++i) js.submit([&] {
      ++n;
      for (int k = 0; k < 8; ++k) js.submit([&] { ++n; });
    });
    js.waitIdle();
    check(n == 64 + 64 * 8, "jobsystem: nested-submit steal storm (64 parents x 8 children, 4 workers)");
  }
  {
    JobSystem::Config cfg; cfg.workerThreads = 4;
    JobSystem js(cfg);
    std::atomic<int> n{0};
    JobFence f1(js), f2(js);
    for (int i = 0; i < 50; ++i) { f1.submit([&] { ++n; }); f2.submit([&] { ++n; }); }
    const int w1 = f1.wait(), w2 = f2.wait();
    check(w1 == 0 && w2 == 0 && n == 100,
          "jobsystem: two interleaved fences each wait only for their own jobs");
  }
  {
    JobSystem js;  // default config
    JobFence f(js);
    for (int i = 0; i < 10; ++i) f.submit([i] { if (i % 3 == 0) throw std::runtime_error("x"); });
    check(f.wait() == 4, "jobsystem: a fence counts its throwing jobs (4 of 10)");
  }
  {
    // Blocking I/O jobs must not occupy workers: 8 sleeping io jobs + 200 quick
    // worker jobs; everything completes.
    JobSystem::Config cfg; cfg.workerThreads = 2; cfg.ioThreads = 2;
    JobSystem js(cfg);
    std::atomic<int> ioDone{0}, workDone{0};
    for (int i = 0; i < 8; ++i)
      js.submitIo([&] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); ++ioDone; });
    for (int i = 0; i < 200; ++i) js.submit([&] { ++workDone; });
    js.waitIdle();
    check(ioDone == 8 && workDone == 200 && js.ioThreadCount() == 2,
          "jobsystem: blocking io jobs run on io threads; workers never stall");
  }
  {
    // Overflow path: one gated worker, 5000 queued jobs (deque holds 1024).
    JobSystem::Config cfg; cfg.workerThreads = 1;
    JobSystem js(cfg);
    std::atomic<bool> gate{false}, entered{false};
    js.submit([&] { entered.store(true, std::memory_order_release);
                    while (!gate.load(std::memory_order_acquire)) std::this_thread::yield(); });
    while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
    std::atomic<int> n{0};
    for (int i = 0; i < 5000; ++i) js.submit([&] { ++n; });
    gate.store(true, std::memory_order_release);
    js.waitIdle();
    check(n == 5000, "jobsystem: 5000 queued on one worker spill through overflow, none lost");
  }
  {
    // Shutdown race: destroy with in-flight work, 20 times. The destructor must
    // drain and join cleanly — no hang, no crash, every job ran.
    bool ok = true;
    for (int iter = 0; iter < 20; ++iter) {
      auto js = std::make_unique<JobSystem>(JobSystem::Config{});
      std::atomic<int> n{0};
      for (int i = 0; i < 2000; ++i) js->submit([&] { ++n; std::this_thread::yield(); });
      js.reset();
      ok = ok && n == 2000;
    }
    check(ok, "jobsystem: 20x shutdown with in-flight work drains and joins cleanly");
  }
#if defined(__linux__)
  {
    JobSystem::Config cfg; cfg.workerThreads = 2; cfg.workerAffinityMask = 0x1;  // core 0 only
    JobSystem js(cfg);
    std::atomic<int> ran{0}, pinned{0};
    for (int i = 0; i < 8; ++i) js.submit([&] {
      ++ran;
      cpu_set_t s; CPU_ZERO(&s);
      if (pthread_getaffinity_np(pthread_self(), sizeof(s), &s) == 0 && CPU_ISSET(0, &s)) ++pinned;
    });
    js.waitIdle();
    check(ran == 8 && pinned == 8, "jobsystem: Linux affinity mask applied to worker threads");
  }
  {
    JobSystem js;  // default policy: no pinning on this 2-core VM, must run clean
    std::atomic<int> n{0};
    for (int i = 0; i < 100; ++i) js.submit([&] { ++n; });
    js.waitIdle();
    check(n == 100, "jobsystem: default affinity policy runs clean (no pinning on small machines)");
  }
#endif
}

void testTickArena() {
  std::printf("core: tick arena (per-tick bump allocator)\n");
  {
    TickArena a(4096);
    struct Payload { int x; double y; char tag[7]; };
    Payload* p = a.alloc<Payload>();
    p->x = 42; p->y = 2.5; std::memcpy(p->tag, "signal", 7);
    check(p->x == 42 && p->y == 2.5 && std::memcmp(p->tag, "signal", 7) == 0 && a.used() >= sizeof(Payload),
          "tick arena: basic alloc returns usable, writable memory");
  }
  {
    TickArena a(4096);
    bool aligned = true;
    for (size_t al : {size_t{1}, size_t{2}, size_t{4}, size_t{8}, size_t{16}, alignof(std::max_align_t)}) {
      for (int i = 0; i < 32; ++i) {  // odd sizes on purpose: padding must not break alignment
        void* p = a.allocate(3 + (i % 17), al);
        aligned = aligned && (reinterpret_cast<uintptr_t>(p) % al == 0);
      }
    }
    struct alignas(16) Simdish { uint64_t v[2]; };
    static_assert(alignof(Simdish) == 16);
    Simdish* s = a.alloc<Simdish>();
    aligned = aligned && (reinterpret_cast<uintptr_t>(s) % 16 == 0);
    check(aligned, "tick arena: 1/2/4/8/16/max_align_t requests all come back aligned");
  }
  {
    TickArena a(1024);
    void* first = a.allocate(100, 8);
    const size_t usedBefore = a.used();
    a.reset();
    const bool reclaimed = a.used() == 0;
    void* second = a.allocate(100, 8);
    // Determinism: the same call sequence after reset reproduces the identical layout.
    check(reclaimed && second == first && a.used() == usedBefore,
          "tick arena: reset() reclaims everything; the same sequence reproduces the same layout");
  }
  {
    TickArena a(256);  // overflow policy: grow via chained blocks, retained across reset()
    void* p1 = a.allocate(200, 8);
    const size_t reservedBefore = a.totalReserved();
    void* p2 = a.allocate(512, 8);  // does not fit in the 56 bytes left: must grow, not fail
    const bool grew = a.blockCount() == 2 && a.totalReserved() > reservedBefore;
    const size_t reservedAfter = a.totalReserved();
    std::memset(p1, 0xAB, 200); std::memset(p2, 0xCD, 512);
    bool intact = true;
    for (int i = 0; i < 200; ++i) intact = intact && static_cast<unsigned char*>(p1)[i] == 0xAB;
    for (int i = 0; i < 512; ++i) intact = intact && static_cast<unsigned char*>(p2)[i] == 0xCD;
    a.reset();
    void* q1 = a.allocate(200, 8);
    void* q2 = a.allocate(512, 8);  // same pattern: reuses the overflow block, no new block
    check(grew && intact && q1 == p1 && q2 == p2 && a.blockCount() == 2 &&
              a.totalReserved() == reservedAfter,
          "tick arena: overflow grows a chained block; retained blocks make warm ticks malloc-free");
  }
  {
    TickArena a(64);  // tiny primary: forces overflow on the first realistic pass
    TickArena::Marker m = a.mark();
    int* v = a.alloc<int>(16);
    for (int i = 0; i < 16; ++i) v[i] = i;
    a.rewind(m);
    int* w = a.alloc<int>(16);  // must land exactly where v did
    bool same = (w == v);
    for (int i = 0; i < 16; ++i) w[i] = 100 + i;
    check(same && a.used() == m.used + 16 * sizeof(int) && w[15] == 115,
          "tick arena: mark()/rewind() gives cheap nested checkpoints inside a tick");
  }
  {
    // Review A-L4: a moved-from arena is an inert shell (blockCount 0);
    // the moved-to arena allocates normally. (Allocating FROM the shell
    // fails fast by design — a death path, not exercised in-process.)
    TickArena a(4096);
    TickArena b(std::move(a));
    int* p = b.alloc<int>(4);
    p[0] = 7; p[3] = 9;
    check(a.blockCount() == 0 && a.capacity() == 0 && b.blockCount() == 1 && p[0] == 7 && p[3] == 9,
          "tick arena: move transfers the blocks; the moved-from shell is inert");
  }
  {
    TickArena a(4096);  // small primary: the warmup pass must overflow into chained blocks
    for (int i = 0; i < 200; ++i) a.allocate(8 + (i * 13) % 120, 8);
    const size_t reservedWarm = a.totalReserved();
    const size_t blocksWarm = a.blockCount();
    a.reset();
    const bool warmedIntoOverflow = blocksWarm > 1;
    bool ok = warmedIntoOverflow;
    for (int tick = 0; tick < 1000 && ok; ++tick) {
      TickArena::Handle handles[200];
      for (int i = 0; i < 200; ++i) {
        uint32_t* slot = a.alloc<uint32_t>();
        *slot = uint32_t(tick * 200 + i);
        handles[i] = a.toHandle(slot);  // handles, not pointers, across the tick's work
      }
      for (int i = 0; i < 200; ++i)
        ok = ok && *static_cast<uint32_t*>(a.fromHandle(handles[i])) == uint32_t(tick * 200 + i);
      ok = ok && a.totalReserved() == reservedWarm && a.blockCount() == blocksWarm;  // steady: no growth
      a.reset();
      ok = ok && a.used() == 0;
    }
    check(ok, "tick arena: 1000 ticks x 200 allocs + reset: handles resolve, steady-state never grows");
  }
}

void testInfiniteRegistryWorld();  // defined below testWorld()
void testInfiniteRegistryPhase2();  // defined below testInfiniteRegistryWorld()
void testInfiniteRegistryNet();     // defined below testNet()
void testWorld() {
  std::printf("world: chunk map, raycast, mesh snapshots\n");
  ChunkMap w = worldWithColumns(2, &fixtureBlocks());
  const bool outside = !w.set(40, 10, 5, kRock) && w.get(40, 10, 5) == kAir;  // no column there
  w.set(3, 20, 4, kRock); w.setState(3, 20, 4, makeState(5, 2));
  w.set(17, 0, 1, kRock);
  check(outside && w.get(3, 20, 4) == kRock && stValue(w.getState(3, 20, 4)) == 5 && stFacing(w.getState(3, 20, 4)) == 2 &&
            w.surfaceY(3, 4) == 20 && w.sub(0, 1, 0) != nullptr && w.sub(0, 2, 0) == nullptr,
        "chunk map: set / get / state across sub-chunks, missing columns refuse edits, empty sub-chunks stay null");
  w.set(3, 20, 4, kAir);
  check(w.get(3, 20, 4) == kAir && w.sub(0, 1, 0)->nonAir == 0, "chunk map: clearing the last block leaves an empty sub-chunk");
  int hit[3], prev[3];
  w.set(5, 30, 5, kRock);
  const bool down = raycast(w, 5.5f, 40.5f, 5.5f, 0, -1, 0, 20, hit, prev);
  const bool miss = !raycast(w, 5.5f, 40.5f, 5.5f, 0, 1, 0, 20, hit, prev);
  raycast(w, 5.5f, 40.5f, 5.5f, 0, -1, 0, 20, hit, prev);
  check(down && miss && hit[1] == 30 && prev[1] == 31 && hit[0] == 5 && hit[2] == 5, "raycast: first solid cell and the cell in front of it");
  const auto snap = snapshotForMeshing(w, fixtureBlocks(), 1, 0, 0, 7, 9, kRock);
  const bool border = snap->id[meshPadIndex(2, 1, 2)] == kRock;   // (17, 0, 1) local (1, 0, 1)
  const bool below = snap->id[meshPadIndex(5, 0, 5)] == kRock;    // y = -1 reads as the fill block
  const auto snapAir = snapshotForMeshing(w, fixtureBlocks(), 1, 0, 0, 7, 9);
  check(border && below && snapAir->id[meshPadIndex(5, 0, 5)] == kAir && snap->epoch == 7 && snap->seq == 9,
        "mesh snapshot: padded copy, below-world fill block, epoch / seq stamped");
  testInfiniteRegistryWorld();
  testInfiniteRegistryPhase2();
}

// ---- Infinite Registry (phase 1): StableIds, per-entry CRC32, VoxelPalette ----

void testInfiniteRegistryWorld() {
  std::printf("world: infinite registry (stable ids, crc32, voxel palette)\n");

  // FNV-1a-64 against the standard test vectors.
  check(fnv1a64("") == 0xcbf29ce484222325ULL && fnv1a64("a") == 0xaf63dc4c8601ec8cULL,
        "registry: FNV-1a-64 matches the standard test vectors");

  // kAirStableId is the kind-tagged hash of the air string id (phase 2: the kind
  // prefix changed every stable id; piecewise feeding == hashing the join).
  check(kAirStableId == fnv1a64("block:litholattice:air") &&
            kAirStableId == stableIdFor("block", "litholattice:air") &&
            isAirStableId(kAirStableId) && !isAirStableId(kAirStableId + 1),
        "registry: kAirStableId = FNV-1a-64(\"block:litholattice:air\")");

  // Stable IDs: deterministic across sessions, found by id, first-wins on dup.
  BlockRegistry r1, r2;
  const BlockId rock1 = r1.add({"Rock", kSolid, Shape::Cube, "rock", {}, 0, 0, 0, 0.8f, false, 0, "test:rock"});
  const BlockId rock2 = r2.add({"Rock", kSolid, Shape::Cube, "rock", {}, 0, 0, 0, 0.8f, false, 0, "test:rock"});
  const StableId rockSid = stableIdFor("block", "test:rock");
  check(r1.stableIdOf(rock1) == rockSid && r2.stableIdOf(rock2) == rockSid,
        "registry: default stable ID is the FNV-1a-64 of the string id, identical across sessions");
  check(r1.findByStableId(rockSid) && *r1.findByStableId(rockSid) == rock1 &&
            !r1.findByStableId(rockSid + 1),
        "registry: findByStableId round-trips; unknown id -> nullopt");
  check(r1.defByStableId(rockSid).tex == "rock" && r1.defByStableId(0).tex.empty(),
        "registry: defByStableId reaches the def (unknown -> air def)");
  const BlockId dup = r1.add({"Rock2", kSolid, Shape::Cube, "rock", {}, 0, 0, 0, 0.8f, false, 0, "test:rock"});
  check(dup == rock1 && r1.size() == 2,  // air + rock: duplicate string id is first-wins
        "registry: duplicate string id returns the existing id, never overwrites");

  // addWithStableId: mod-supplied ids; duplicates rejected on either key.
  auto ok = r1.addWithStableId({"ModBlock", kSolid, Shape::Cube, "mod"}, 0x123456789ABCDEF0ULL);
  check(ok && *ok == 2 && r1.stableIdOf(*ok) == 0x123456789ABCDEF0ULL &&
            r1.findByStableId(0x123456789ABCDEF0ULL) == ok,
        "registry: addWithStableId stores the caller-supplied id");
  check(!r1.addWithStableId({"Other", kSolid, Shape::Cube, "other"}, 0x123456789ABCDEF0ULL) &&
            !r1.addWithStableId({"Rock2b", kSolid, Shape::Cube, "rock2b"}, rockSid) &&
            !r1.addWithStableId({"Rock3", kSolid, Shape::Cube, "rock", {}, 0, 0, 0, 0.8f, false, 0, "test:rock"}, 0x9999ULL) &&
            r1.size() == 3 && r1.stableIdOf(rock1) == rockSid,
        "registry: duplicate stable id OR duplicate string id -> nullopt, originals untouched");

  // Per-entry CRC32 (WS7): standard vector, deterministic, content-sensitive, id-blind.
  {
    const uint8_t nine[] = {'1','2','3','4','5','6','7','8','9'};
    check(crc32(nine, 9) == 0xCBF43926u, "registry: CRC32 matches the standard check value");
  }
  const uint32_t crcA = r1.crcOf(rock1);
  const uint32_t crcB = r2.crcOf(rock2);
  BlockRegistry r3;
  const BlockId rock3 = r3.add({"Rock", kSolid, Shape::Cube, "rock", {}, 0, 0, 0, 0.8f, false, 0, "test:rock"});
  const BlockId hardRock = r3.add({"HardRock", kSolid, Shape::Cube, "rock", {}, 0, 0, 0, 0.2f, false, 0, "test:hardrock"});
  check(crcA == crcB && crcA == r3.crcOf(rock3) && crcA != 0,
        "registry: identical defs hash identically across sessions (crcOf)");
  check(r3.crcOf(hardRock) != r3.crcOf(rock3),
        "registry: different content (friction) -> different CRC");
  {
    // Same content, different string id: the CRC must not move (id is identity, not content).
    BlockRegistry r4;
    const BlockId same1 = r4.add({"Rock", kSolid, Shape::Cube, "rock", {}, 0, 0, 0, 0.8f, false, 0, "test:rock"});
    const BlockId same2 = r4.add({"Rock", kSolid, Shape::Cube, "rock", {}, 0, 0, 0, 0.8f, false, 0, "test:rock-alias"});
    check(r4.crcOf(same1) == r4.crcOf(same2) && r4.stableIdOf(same1) != r4.stableIdOf(same2),
          "registry: CRC covers content only (string id excluded); stable IDs still differ");
  }
  // Logged form: decimal + hex both render.
  {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%u (0x%08X)", crcA, crcA);
    check(std::strlen(buf) > 4, "registry: crc32 logs as decimal + hex");
  }

  // VoxelPalette: fresh, round-trip, width steps 8 -> 16, fill/clear/equality.
  {
    VoxelPalette p(kSubChunkVolume);
    check(p.width() == VoxelPalette::Width::W8 && p.paletteSize() == 1 && p.at(0) == kAirStableId &&
              p.rawIndex(0) == 0,
          "palette: fresh palette is all air, 8-bit, one entry");
    const StableId s1 = 111, s2 = 222;
    p.set(7, s1);
    p.set(4095, s2);
    check(p.at(7) == s1 && p.at(4095) == s2 && p.at(0) == kAirStableId && p.paletteSize() == 3,
          "palette: set/at round-trip, untouched voxels stay air");
    p.set(7, kAirStableId);
    check(p.at(7) == kAirStableId, "palette: setting air clears the voxel");
    // Push past 255 distinct non-air entries: the width must step to 16-bit.
    VoxelPalette q(kSubChunkVolume);
    for (int i = 0; i < 300; ++i) q.set(size_t(i), StableId(1000 + i));
    check(q.width() == VoxelPalette::Width::W16 && q.paletteSize() == 301,
          "palette: 300 distinct entries step the width 8 -> 16");
    bool kept = true;
    for (int i = 0; i < 300; ++i) kept = kept && q.at(size_t(i)) == StableId(1000 + i);
    check(kept && q.at(300) == kAirStableId, "palette: all 300 values survive the width step");
    // fill / clear / equality.
    VoxelPalette f(kSubChunkVolume);
    f.fill(s1);
    check(f.at(0) == s1 && f.at(4095) == s1 && f.paletteSize() == 2, "palette: fill sets every voxel");
    VoxelPalette f2(kSubChunkVolume);
    f2.fill(s1);
    check(f == f2, "palette: equality holds for identical fills");
    f.clear();
    check(f.at(0) == kAirStableId && f.paletteSize() == 1 && f.width() == VoxelPalette::Width::W8,
          "palette: clear resets to fresh");
    // resolveBlockIds: palette -> BlockId LUT through a registry.
    BlockRegistry lr;
    const BlockId a = lr.add({"A", kSolid, Shape::Cube, "a", {}, 0, 0, 0, 0.8f, false, 0, "t:a"});
    const BlockId b = lr.add({"B", kSolid, Shape::Cube, "b", {}, 0, 0, 0, 0.8f, false, 0, "t:b"});
    VoxelPalette v(kSubChunkVolume);
    v.set(0, lr.stableIdOf(a));
    v.set(1, lr.stableIdOf(b));
    std::vector<BlockId> lut;
    v.resolveBlockIds(lr, lut);
    check(lut.size() == v.paletteSize() && lut[v.rawIndex(0)] == a && lut[v.rawIndex(1)] == b &&
              lut[0] == kAir,
          "palette: resolveBlockIds builds a correct BlockId LUT (air stays kAir)");
  }

  // SubChunk over the palette: StableId set/at, nonAir accounting.
  {
    BlockRegistry lr;
    const BlockId a = lr.add({"A", kSolid, Shape::Cube, "a", {}, 0, 0, 0, 0.8f, false, 0, "t:a"});
    SubChunk s;
    s.set(3, 5, 7, lr.stableIdOf(a));
    check(s.at(3, 5, 7) == lr.stableIdOf(a) && s.nonAir == 1 && s.at(0, 0, 0) == kAirStableId,
          "subchunk: set/at with StableIds, nonAir counts non-air voxels");
    s.set(3, 5, 7, kAirStableId);
    check(s.at(3, 5, 7) == kAirStableId && s.nonAir == 0, "subchunk: clearing restores air");
  }

  // ChunkMap translates through its registry (BlockId currency preserved for game logic).
  {
    BlockRegistry lr;
    const BlockId a = lr.add({"A", kSolid, Shape::Cube, "a", {}, 0, 0, 0, 0.8f, false, 0, "t:a"});
    ChunkMap w = worldWithColumns(1, &lr);
    w.set(8, 65, 8, a);
    check(w.get(8, 65, 8) == a && w.sub(0, 4, 0)->at(8, 1, 8) == lr.stableIdOf(a) &&
              w.surfaceY(8, 8) == 65,
          "chunkmap: set/get keep the BlockId currency; voxels underneath hold StableIds");
    ChunkMap bare = worldWithColumns(1);  // no registry: surfaceY still works (air compare)
    check(bare.surfaceY(0, 0) == -1, "chunkmap: surfaceY needs no registry");
  }
}

// ---- Infinite Registry (phase 2): unified kind-tagged ContentRegistry ----

void testInfiniteRegistryPhase2() {
  std::printf("world: infinite registry phase 2 (unified content registry)\\n");

  // Kind-tagged derivation: same name + different kinds -> different stable ids;
  // same (kind, name) -> identical ids across sessions.
  {
    const StableId b = stableIdFor("block", "t:thing");
    const StableId i = stableIdFor("item", "t:thing");
    check(b != i, "registry: kind prefix separates block/item stable ids");
    check(stableIdFor("block", "t:thing") == b && stableIdFor("item", "t:thing") == i,
          "registry: kind-tagged derivation is deterministic");
    // Piecewise feeding is byte-identical to hashing the joined literal.
    check(stableIdFor("block", "litholattice:air") == fnv1a64("block:litholattice:air"),
          "registry: stableIdFor(kind, id) == fnv1a64(\"kind:id\")");
  }

  ContentRegistry cr;
  // Typed views share the registry underneath.
  BlockRegistry blocks(cr);
  ItemRegistry items(cr);
  check(&blocks.content() == &cr && &items.content() == &cr,
        "registry: typed views share one ContentRegistry");

  // Duplicate rejection is per (kind, id): the same name lives as both kinds.
  const BlockId thingB = blocks.add({"Thing", kSolid, Shape::Cube, "thing", {}, 0, 0, 0, 0.8f, false, 0, "m:thing"});
  const ItemId thingI = items.add({"Thing", 64, 0, {}, "m:thing"});
  check(thingB == 1 && thingI == 1,
        "registry: same name registers as both block and item (per-kind namespaces)");
  check(blocks.findById("m:thing") && *blocks.findById("m:thing") == thingB &&
            items.findById("m:thing") && *items.findById("m:thing") == thingI,
        "registry: (kind, id) lookups stay separate");
  const BlockId thingB2 = blocks.add({"Thing2", kSolid, Shape::Cube, "thing", {}, 0, 0, 0, 0.8f, false, 0, "m:thing"});
  check(thingB2 == thingB && blocks.size() == 2,
        "registry: duplicate block string id is first-wins (air + thing)");
  const ItemId thingI2 = items.add({"Thing2", 16, 0, {}, "m:thing"});
  check(thingI2 == thingI && items.size() == 1,
        "registry: duplicate item string id is first-wins");

  // Supplied stable ids are rejected GLOBALLY (a stable id names one entry).
  {
    auto okB = blocks.addWithStableId({"Sup", kSolid, Shape::Cube, "sup", {}, 0, 0, 0, 0.8f, false, 0, "m:sup"},
                                      0xABCD1234ULL);
    check(okB.has_value(), "registry: block with supplied stable id registers");
    auto dupI = items.addWithStableId({"SupItem", 64, 0, {}, "m:supitem"}, 0xABCD1234ULL);
    check(!dupI.has_value() && items.size() == 1,
          "registry: supplied stable id colliding across kinds -> nullopt");
    auto dupB = blocks.addWithStableId({"Sup2", kSolid, Shape::Cube, "sup2", {}, 0, 0, 0, 0.8f, false, 0, "m:sup2"},
                                       0xABCD1234ULL);
    check(!dupB.has_value(), "registry: supplied stable id colliding within kind -> nullopt");
  }

  // kindOf: the unified identity hook.
  {
    const StableId bs = blocks.stableIdOf(thingB);
    const StableId is = items.stableIdOf(thingI);
    check(cr.kindOf(bs) && *cr.kindOf(bs) == EntryKind::Block &&
              cr.kindOf(is) && *cr.kindOf(is) == EntryKind::Item &&
              !cr.kindOf(0xDEADBEEFDEADBEEFULL),
          "registry: kindOf resolves block/item/unknown stable ids");
    // Kind-filtered lookups: an item's stable id never resolves as a block and vice versa.
    check(!blocks.findByStableId(is) && !items.findByStableId(bs) &&
              blocks.findByStableId(bs) == thingB && items.findByStableId(is) == thingI,
          "registry: cross-kind findByStableId is kind-checked");
    check(blocks.defByStableId(is).tex.empty(),
          "registry: defByStableId on an item stable id -> air def (safe)");
  }

  // placesBlock round-trip: BlockItems are a field, not a mapping table.
  {
    const StableId lampBlock = blocks.stableIdOf(thingB);
    const ItemId lampItem = items.add({"Thing Item", 64, lampBlock, {}, "m:thing_item"});
    const auto found = items.findById("m:thing_item");
    check(found && *found == lampItem && items.def(lampItem).placesBlock == lampBlock,
          "registry: placesBlock round-trips through the item def");
    const auto backToBlock = blocks.findByStableId(items.def(lampItem).placesBlock);
    check(backToBlock && *backToBlock == thingB,
          "registry: placesBlock stable id resolves back to the block");
    // Not a block-item: placesBlock == 0.
    const ItemId rock = items.add({"Rock", 64, 0, {}, "m:rock"});
    check(items.def(rock).placesBlock == 0, "registry: plain items have placesBlock == 0");
  }

  // Item CRC32 (WS7): deterministic, content-sensitive, id-blind.
  {
    ItemRegistry r1, r2;
    const ItemId a1 = r1.add({"Copper Ingot", 64, 0, {}, "m:copper_ingot"});
    const ItemId a2 = r2.add({"Copper Ingot", 64, 0, {}, "m:copper_ingot"});
    check(r1.crcOf(a1) == r2.crcOf(a2) && r1.crcOf(a1) != 0,
          "registry: item CRC deterministic across sessions");
    const ItemId b1 = r1.add({"Copper Nugget", 64, 0, {}, "m:copper_nugget"});
    check(r1.crcOf(b1) != r1.crcOf(a1), "registry: different item content -> different CRC");
    ItemRegistry r3;
    const ItemId c1 = r3.add({"Copper Ingot", 64, 0, {}, "m:copper_ingot"});
    const ItemId c2 = r3.add({"Copper Ingot", 64, 0, {}, "m:copper_renamed"});
    check(r3.crcOf(c1) == r3.crcOf(c2) && r3.stableIdOf(c1) != r3.stableIdOf(c2),
          "registry: item CRC covers content only (string id excluded)");
    // placesBlock participates in the CRC.
    ItemRegistry r4;
    const StableId fakeBlock = 0x1111ULL;
    const ItemId d1 = r4.add({"Placer", 64, fakeBlock, {}, "m:placer"});
    const ItemId d2 = r4.add({"Placer", 64, 0, {}, "m:placer2"});
    check(r4.crcOf(d1) != r4.crcOf(d2), "registry: placesBlock feeds the item CRC");
  }

  // itemDef by value (review F-M3): the returned def is a snapshot copy —
  // later registrations (which may reallocate the def vector) cannot tear
  // or invalidate what the caller already took.
  {
    ItemRegistry r;
    const ItemId a = r.add({"Copy", 64, 0, {}, "m:copy"});
    const ItemDef snap = r.def(a);
    for (int i = 0; i < 64; ++i)
      r.add({"Filler", 64, 0, {}, "m:filler" + std::to_string(i)});
    check(snap.name == "Copy" && r.def(a).name == "Copy" && r.size() == 65,
          "registry: itemDef returns a by-value snapshot that survives later registrations");
  }

  // BlockId cap (review F-M5): registration fails closed at 65536 blocks
  // instead of truncating the 65537th BlockId onto air.
  {
    ContentRegistry big;
    BlockDef proto{"Cap", kSolid, Shape::Cube, "cap", {}, 0, 0, 0, 0.8f, false, 0, ""};
    bool filled = true;
    for (uint32_t i = 1; i <= 0xFFFFu; ++i) {  // air already holds index 0
      const std::string sid = "cap:b" + std::to_string(i);  // alive for the call; the registry owns its copy
      proto.id = sid;
      if (big.addBlock(proto) != BlockId(i)) { filled = false; break; }
    }
    proto.id = "cap:overflow";
    const BlockId refused = big.addBlock(proto);
    check(filled && big.blockCount() == 65536 && refused == kAir && !big.findBlock("cap:overflow"),
          "registry: the 65537th block is refused (logged, kAir) — BlockId never truncates onto air");
  }

  // Item id 0 is the empty sentinel.
  {
    ItemRegistry r;
    check(r.def(0).name.empty() && r.stableIdOf(0) == 0 && r.crcOf(0) == 0 &&
              r.idOf(0).empty() && !r.findById("nope"),
          "registry: item id 0 is the empty sentinel");
    const ItemId first = r.add({"First", 64, 0, {}, "m:first"});
    check(first == 1 && r.size() == 1, "registry: real items start at id 1");
  }

  // The extension door: registerKind.
  {
    ContentRegistry er;
    const EntryKind fluid = er.registerKind("fluid");
    check(fluid != EntryKind::Block && fluid != EntryKind::Item,
          "registry: registerKind mints a new kind");
    check(er.registerKind("fluid") == fluid, "registry: registerKind dedups on name");
    check(er.kindName(fluid) == "fluid" && er.kindName(EntryKind::Block) == "block" &&
              er.kindName(EntryKind::Item) == "item",
          "registry: kindName round-trips");
    const StableId f = stableIdFor(er.kindName(fluid), "m:water");
    check(f != stableIdFor("block", "m:water") && f != stableIdFor("item", "m:water"),
          "registry: extension kinds join the kind-tagged id space");
    check(er.stableIdOf(fluid, "m:water") == std::nullopt,
          "registry: unregistered (kind, id) -> nullopt");
  }

  // Seal & hot-swap (debug prototyping).
  {
    ContentRegistry sr;
    const BlockId b = sr.addBlock({"Hot", kSolid, Shape::Cube, "h", {}, 0, 0, 0, 0.8f, false, 0, "t:hot"});
    check(!sr.isSealed(), "registry: starts unsealed");
    // Stage an update: live def unchanged until refresh.
    BlockDef upd = sr.blockDef(b);
    upd.name = "Hotter";
    check(sr.stageBlockUpdate(b, upd), "registry: stageBlockUpdate accepts when unsealed");
    check(sr.blockDef(b).name == "Hot", "registry: live def keeps old version while staged");
    check(sr.stagedUpdateCount() == 1, "registry: staged count tracks");
    const uint64_t e0 = sr.definitionEpoch();
    check(sr.refreshStaged() == 1, "registry: refreshStaged applies one update");
    check(sr.blockDef(b).name == "Hotter", "registry: live def updated after refresh");
    check(sr.definitionEpoch() == e0 + 1, "registry: epoch bumps on refresh");
    check(sr.stagedUpdateCount() == 0, "registry: staged cleared after refresh");
    // Stable ID unchanged by the update (voxels need no remap).
    check(sr.blockStableId(b) == stableIdFor("block", "t:hot"),
          "registry: stable id survives a hot-swap update");
    // Wrong string id rejected (rename is not an update).
    BlockDef bad = sr.blockDef(b);
    bad.id = "t:other";
    check(!sr.stageBlockUpdate(b, bad), "registry: stage rejects string-id change");
    // Out of range rejected.
    check(!sr.stageBlockUpdate(9999, upd), "registry: stage rejects bad BlockId");
    // Seal: everything refuses.
    sr.seal();
    check(sr.isSealed(), "registry: seal sticks");
    bool threw = false;
    try { sr.addBlock({"Nope", kSolid, Shape::Cube, "n", {}, 0, 0, 0, 0.8f, false, 0, "t:nope"}); }
    catch (const std::runtime_error&) { threw = true; }
    check(threw, "registry: addBlock throws when sealed");
    check(!sr.addBlockWithStableId({"Nope", kSolid, Shape::Cube, "n", {}, 0, 0, 0, 0.8f, false, 0, "t:nope2"}, 12345),
          "registry: addBlockWithStableId returns nullopt when sealed");
    check(!sr.stageBlockUpdate(b, upd), "registry: stageBlockUpdate refuses when sealed");
    check(sr.refreshStaged() == 0, "registry: refreshStaged no-ops when sealed with empty stage");
  }

  // BlockRegistry facade owning mode (phase-1 construction style still works).
  {
    BlockRegistry ro;
    const BlockId a = ro.add({"A", kSolid, Shape::Cube, "a", {}, 0, 0, 0, 0.8f, false, 0, "t:a"});
    check(a == 1 && ro.size() == 2 && ro.kindOf(ro.stableIdOf(a)) == EntryKind::Block,
          "registry: owning BlockRegistry facade behaves as before");
    // The unified stableIdOf hook.
    check(ro.content().stableIdOf(EntryKind::Block, "t:a") == ro.stableIdOf(a) &&
              !ro.content().stableIdOf(EntryKind::Item, "t:a"),
          "registry: content().stableIdOf(kind, id) resolves per kind");
    // Aliases survived the move to the kind-keyed map.
    ro.addAlias("t:old_a", "t:a");
    check(ro.findById("t:old_a") && *ro.findById("t:old_a") == a &&
              ro.content().stableIdOf(EntryKind::Block, "t:old_a") == ro.stableIdOf(a),
          "registry: block aliases resolve through findById and stableIdOf");
  }

  // Kind-mismatch detection for the voxel LUT path (fail-fast predicate).
  // resolveBlockIds() asserts on exactly this predicate in debug builds; in
  // release the kind-filtered findByStableId maps the offender to kAir instead.
  {
    ContentRegistry kr;
    BlockRegistry kb(kr);
    ItemRegistry ki(kr);
    const BlockId b = kb.add({"B", kSolid, Shape::Cube, "b", {}, 0, 0, 0, 0.8f, false, 0, "t:b"});
    const ItemId it = ki.add({"I", 64, 0, {}, "t:i"});
    const StableId itemSid = ki.stableIdOf(it);
    (void)b;
    check(kr.kindOf(itemSid) == EntryKind::Item,
          "registry: kindOf flags an item stable id (LUT assert predicate)");
    check(!kb.findByStableId(itemSid),
          "registry: block-view findByStableId rejects item stable ids (release fallback -> air)");
    // And the honest path still resolves.
    VoxelPalette v(kSubChunkVolume);
    v.set(0, kb.stableIdOf(b));
    std::vector<BlockId> lut;
    v.resolveBlockIds(kb, lut);
    check(lut.size() == v.paletteSize() && lut[v.rawIndex(0)] == b,
          "registry: LUT build still resolves block palettes");
  }
}



// ---- binary greedy mesher: agreement with the classic mesher ----

uint64_t testSplitmix64(uint64_t& s) {
  uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

using FaceKey = std::array<int, 4>;  // emitting voxel (x, y, z) + facing 0..5

// One entry per unit face the classic mesher emitted (cubes emit one quad per face).
std::map<FaceKey, int> classicFaces(const ChunkMesh& mesh) {
  static const int kNrm[6][3] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
  std::map<FaceKey, int> faces;
  for (size_t q = 0; q + 4 <= mesh.verts.size(); q += 4) {
    const int f = mesh.verts[q].faceAo & 7;
    int mn[3] = {mesh.verts[q].x, mesh.verts[q].y, mesh.verts[q].z};
    for (size_t c = 1; c < 4; ++c) {
      if (int(mesh.verts[q + c].x) < mn[0]) mn[0] = mesh.verts[q + c].x;
      if (int(mesh.verts[q + c].y) < mn[1]) mn[1] = mesh.verts[q + c].y;
      if (int(mesh.verts[q + c].z) < mn[2]) mn[2] = mesh.verts[q + c].z;
    }
    FaceKey k;
    for (int a = 0; a < 3; ++a) k[a] = mn[a] / 16 - (kNrm[f][a] > 0 ? 1 : 0);
    k[3] = f;
    ++faces[k];
  }
  return faces;
}

std::map<FaceKey, int> binaryFaces(const std::vector<BinaryQuad>& quads) {
  std::map<FaceKey, int> faces;
  for (const BinaryQuad& q : quads)
    expandBinaryQuad(q, [&](int x, int y, int z, int d) { ++faces[{x, y, z, d}]; });
  return faces;
}

void testBinaryMesher() {
  std::printf("render: binary greedy mesher (agreement with the classic mesher)\n");
  const BlockRegistry& reg = fixtureBlocks();
  const BlockMaterials mats(reg.size());
  const MeshContext ctx{reg, mats};

  using Pat = std::function<bool(int, int, int)>;
  const Pat randomFill = [](int x, int y, int z) {
    uint64_t s = uint64_t(x) | (uint64_t(y) << 16) | (uint64_t(z) << 32) | 0x12345678ULL;
    return testSplitmix64(s) % 100 < 45;
  };

  // Same visible surface as the classic mesher: expand the merged quads back to unit
  // faces and require exact multiset equality (no missing faces, none doubled).
  auto checkAgreement = [&](const char* name, Pat pat) {
    ChunkMap w = worldWithColumns(1, &fixtureBlocks());
    for (int y = 0; y < 16; ++y)
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x)
          if (pat(x, y, z)) w.set(x, y, z, kRock);
    const auto snap = snapshotForMeshing(w, fixtureBlocks(), 0, 0, 0, 1, 1, kAir);
    const ChunkMesh ref = meshSubChunk(*snap, ctx);
    std::vector<BinaryQuad> quads;
    std::array<uint8_t, 4096> mat{};
    binaryMeshSubChunk(*snap, ctx, mat.data(), quads);
    const bool same = classicFaces(ref) == binaryFaces(quads);
    std::printf("  (binary: %zu quads vs classic %zu quads)\n", quads.size(), ref.verts.size() / 4);
    check(same, name);
  };

  checkAgreement("binary mesher: single cube covers exactly the classic mesher's 6 faces",
                 [](int x, int y, int z) { return x == 8 && y == 8 && z == 8; });
  checkAgreement("binary mesher: two adjacent cubes agree (shared face culled in both)",
                 [](int x, int y, int z) { return y == 8 && z == 8 && (x == 8 || x == 9); });
  checkAgreement("binary mesher: flat plane agrees",
                 [](int, int y, int) { return y == 0; });
  checkAgreement("binary mesher: checkerboard agrees",
                 [](int x, int y, int z) { return ((x + y + z) & 1) == 0; });
  checkAgreement("binary mesher: random fill agrees (hidden-face culling matches)", randomFill);
  checkAgreement("binary mesher: staircase + overhang agrees", [](int x, int y, int z) {
    return y * 2 <= x || (x >= 4 && x < 8 && z >= 4 && z < 8 && y >= 10 && y < 12);
  });

  // Degenerate inputs.
  {
    std::array<uint8_t, 4096> empty{};
    std::vector<BinaryQuad> q;
    check(binaryMeshChunk16(empty.data(), nullptr, q) == 0 && q.empty(),
          "binary mesher: empty chunk -> zero quads");
  }
  {
    std::array<uint8_t, 4096> full;
    full.fill(1);
    std::vector<BinaryQuad> q;
    binaryMeshChunk16(full.data(), nullptr, q);
    bool dirs[6] = {};
    bool ok = q.size() == 6;
    for (const BinaryQuad& bq : q) {
      ok = ok && bq.w == 16 && bq.h == 16 && bq.material == 0 && bq.dir < 6;
      dirs[bq.dir] = true;
    }
    for (bool d : dirs) ok = ok && d;
    check(ok, "binary mesher: full solid cube -> exactly 6 quads, one 16x16 per facing");
  }

  // Determinism: same bytes in, bit-identical quad stream out.
  {
    std::array<uint8_t, 4096> solid{};
    for (int y = 0; y < 16; ++y)
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) solid[(y * 16 + z) * 16 + x] = randomFill(x, y, z) ? 1 : 0;
    std::vector<BinaryQuad> q1, q2;
    binaryMeshChunk16(solid.data(), nullptr, q1);
    binaryMeshChunk16(solid.data(), nullptr, q2);
    const bool same = q1.size() == q2.size() &&
                      (q1.empty() || std::memcmp(q1.data(), q2.data(), q1.size() * sizeof(BinaryQuad)) == 0);
    check(same, "binary mesher: double run on random fill is bit-identical");
  }

  // Material passthrough: merging stops at material boundaries; quads carry the index.
  {
    std::array<uint8_t, 4096> solid;
    solid.fill(1);
    std::array<uint8_t, 4096> mat{};
    for (int y = 0; y < 16; ++y)
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) mat[(y * 16 + z) * 16 + x] = y < 8 ? 1 : 2;
    std::vector<BinaryQuad> q;
    binaryMeshChunk16(solid.data(), mat.data(), q);
    bool matsOk = true;
    for (const BinaryQuad& bq : q)
      expandBinaryQuad(bq, [&](int x, int y, int z, int) {
        matsOk = matsOk && mat[(y * 16 + z) * 16 + x] == bq.material;
      });
    // 4 sides split into 2 quads each at the y=8 boundary + top + bottom = 10.
    check(q.size() == 10 && matsOk,
          "binary mesher: material boundary blocks merging (10 quads), each quad carries its voxels' material");
  }

  // The MeshInput wrapper derives the same solidity the raw API takes.
  {
    ChunkMap w = worldWithColumns(1, &fixtureBlocks());
    w.set(8, 8, 8, kRock);
    const auto snap = snapshotForMeshing(w, fixtureBlocks(), 0, 0, 0, 1, 1, kAir);
    std::vector<BinaryQuad> qw, qr;
    binaryMeshSubChunk(*snap, ctx, nullptr, qw);
    std::array<uint8_t, 4096> solid{};
    solid[(8 * 16 + 8) * 16 + 8] = 1;
    binaryMeshChunk16(solid.data(), nullptr, qr);
    const bool same = qw.size() == qr.size() &&
                      (qw.empty() || std::memcmp(qw.data(), qr.data(), qw.size() * sizeof(BinaryQuad)) == 0);
    check(same, "binary mesher: MeshInput wrapper matches the raw 16^3 API on the same voxels");
  }
}

// Transparent binary-mesher pass (fix 2026-10-11): non-opaque cubes must emit quads
// on the BinaryQuad/PBR path; faces between two voxels of the same transparent-flagged
// id are culled (glass-against-glass, mirroring the classic mesher).
void testBinaryMesherTransparent() {
  std::printf("render: binary mesher transparent pass\n");
  BlockRegistry reg;
  reg.reserve(4);
  const BlockId rock = reg.add({"Rock", kSolid | kOpaque, Shape::Cube, "rock"});
  const BlockId glass = reg.add({"Glass", kSolid | kTransparent, Shape::Cube, "glass"});
  const BlockId leaves = reg.add({"Leaves", kSolid, Shape::Cube, "leaves"});  // non-opaque, not flagged
  BlockMaterials mats(reg.size());
  const MeshContext ctx{reg, mats};
  std::array<uint8_t, 4096> mat{};

  auto meshGlass = [&](ChunkMap& w) {
    const auto snap = snapshotForMeshing(w, reg, 0, 0, 0, 1, 1, kAir);
    std::vector<BinaryQuad> opaque, transp;
    binaryMeshSubChunk(*snap, ctx, mat.data(), opaque);
    binaryMeshSubChunkTransparent(*snap, ctx, mat.data(), transp);
    return std::make_pair(opaque.size(), transp.size());
  };

  // Single glass cube: opaque pass emits nothing, transparent pass emits 6 quads.
  {
    ChunkMap w = worldWithColumns(1, &reg);
    w.set(8, 8, 8, glass);
    auto [no, nt] = meshGlass(w);
    check(no == 0 && nt == 6,
          "binary mesher transparent: single glass cube -> 0 opaque + 6 transparent quads");
  }
  // Two adjacent glass cubes: shared faces culled in the transparent pass (10 quads).
  {
    ChunkMap w = worldWithColumns(1, &reg);
    w.set(8, 8, 8, glass);
    w.set(9, 8, 8, glass);
    auto [no, nt] = meshGlass(w);
    // 12 faces - 2 shared (culled) = 10 faces, greedily merged to 6 quads
    // (top/bottom/front/back pairs merge 2x1; the two outer sides stay 1x1).
    // Without the glass-against-glass cull this would be 8 (cf. the leaves case).
    check(no == 0 && nt == 6,
          "binary mesher transparent: adjacent glass pair culls shared faces (6 merged quads)");
  }
  // Glass next to rock: rock's face toward glass stays (opaque pass); glass face
  // toward rock is culled (transparent pass) -> 6 + 5 = 11 total quads.
  {
    ChunkMap w = worldWithColumns(1, &reg);
    w.set(8, 8, 8, glass);
    w.set(9, 8, 8, rock);
    auto [no, nt] = meshGlass(w);
    check(no == 6 && nt == 5,
          "binary mesher transparent: glass/rock adjacency -> 6 opaque + 5 transparent quads");
  }
  // Non-flagged translucent (leaves): emits, but internal faces are NOT culled
  // (mirrors the classic mesher, which only culls kTransparent-flagged pairs).
  {
    ChunkMap w = worldWithColumns(1, &reg);
    w.set(8, 8, 8, leaves);
    w.set(9, 8, 8, leaves);
    auto [no, nt] = meshGlass(w);
    // 12 faces, none culled (not kTransparent-flagged), greedily merged to 8 quads
    // (4 coplanar pairs merge 2x1; the 2 shared internal faces stay unmerged).
    check(no == 0 && nt == 8,
          "binary mesher transparent: non-flagged pair keeps internal faces (8 merged quads)");
  }
  // Determinism: same bytes in, bit-identical quad stream out.
  {
    ChunkMap w = worldWithColumns(1, &reg);
    w.set(8, 8, 8, glass);
    w.set(9, 8, 8, glass);
    w.set(8, 9, 8, rock);
    const auto snap = snapshotForMeshing(w, reg, 0, 0, 0, 1, 1, kAir);
    std::vector<BinaryQuad> q1, q2;
    binaryMeshSubChunkTransparent(*snap, ctx, mat.data(), q1);
    binaryMeshSubChunkTransparent(*snap, ctx, mat.data(), q2);
    const bool same = q1.size() == q2.size() &&
                      (q1.empty() || std::memcmp(q1.data(), q2.data(), q1.size() * sizeof(BinaryQuad)) == 0);
    check(same, "binary mesher transparent: double run is bit-identical");
  }
}

// Micro-face mesher for subdivided blocks (workstream 2, 2026-10-11): LOD policy,
// greedy micro-face merging, determinism, and the chunk-build integration point.
// The SparseOctree side is workstream 1's contract; these tests run against the
// stopgap octree.cpp until their implementation lands (same public API).
namespace {
// All four corners of a MicroQuad must lie inside [o, o+1]^3 (block AABB) for
// the FOV-culling composition argument: chunk-level frustum tests cover micro
// geometry unchanged because subdivision never widens chunk bounds.
bool microQuadsInsideBlock(const std::vector<micro::MicroQuad>& q, float ox, float oy, float oz) {
  constexpr float eps = 1e-5f;
  for (const micro::MicroQuad& mq : q) {
    const int a = mq.dir / 2;
    const int t0 = (a == 0) ? 1 : 0;
    const int t1 = (a == 2) ? 1 : 2;
    for (int cv = 0; cv < 2; ++cv)
      for (int cu = 0; cu < 2; ++cu) {
        float c[3] = {mq.x, mq.y, mq.z};
        c[t0] += cu ? mq.w : 0.0f;
        c[t1] += cv ? mq.h : 0.0f;
        if (c[0] < ox - eps || c[0] > ox + 1.0f + eps || c[1] < oy - eps || c[1] > oy + 1.0f + eps ||
            c[2] < oz - eps || c[2] > oz + 1.0f + eps)
          return false;
      }
  }
  return true;
}
}  // namespace

void testMicroMesher() {
  std::printf("render: micro-face mesher (subdivided blocks, LOD)\n");
  using micro::MicroMeshParams;
  using micro::MicroQuad;
  using subdiv::SparseOctree;

  check(sizeof(MicroQuad) == 24 && sizeof(BinaryQuad) == 8,
        "micro mesher: MicroQuad is 24 bytes; BinaryQuad layout untouched at 8 bytes");

  // Octree stopgap contract sanity (workstream 1's own tests supersede these).
  {
    SparseOctree t;
    check(t.empty(), "micro mesher: fresh tree is empty");
    t.set(6, 0, 0, 0, 1);
    t.set(6, 1, 0, 0, 2);
    check(!t.empty() && t.get(6, 0, 0, 0) == 1 && t.get(6, 1, 0, 0) == 2,
          "micro mesher: octree set/get round-trip at level 6");
    t.set(6, 0, 0, 0, 0);
    check(t.get(6, 0, 0, 0) == 0 && t.leafCount() == 1,
          "micro mesher: clearing a cell collapses back to one leaf");
    t.set(6, 1, 0, 0, 0);
    check(t.empty() && t.leafCount() == 0, "micro mesher: clearing all cells empties the tree");
  }

  // LOD policy: powers-of-two distance thresholds.
  {
    bool ok = micro::selectLOD(0.0f) == 6 && micro::selectLOD(3.99f) == 6 &&
              micro::selectLOD(4.0f) == 5 && micro::selectLOD(7.99f) == 5 &&
              micro::selectLOD(8.0f) == 4 && micro::selectLOD(16.0f) == 3 &&
              micro::selectLOD(32.0f) == 2 && micro::selectLOD(64.0f) == 1 &&
              micro::selectLOD(128.0f) == 0 && micro::selectLOD(1.0e6f) == 0 &&
              micro::selectLOD(-5.0f) == 6;
    check(ok, "micro mesher: selectLOD follows the 4/8/16/32/64/128m policy");
  }

  // LOD0 of a subdivided block == plain cube mesh: 6 unit quads at the origin.
  {
    SparseOctree t;
    for (int z = 0; z < 2; ++z)
      for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) t.set(1, x, y, z, 3);
    MicroMeshParams p;
    p.originX = 10.0f;
    p.originY = 20.0f;
    p.originZ = 30.0f;
    std::vector<MicroQuad> q;
    const size_t n = micro::meshOctreeLOD(t, 0, p, q);
    // Expected min corners per dir: +X(11,20,30) -X(10,20,30) +Y(10,21,30)
    // -Y(10,20,30) +Z(10,20,31) -Z(10,20,30).
    const float ex[6] = {11, 10, 10, 10, 10, 10};
    const float ey[6] = {20, 20, 21, 20, 20, 20};
    const float ez[6] = {30, 30, 30, 30, 31, 30};
    bool seen[6] = {};
    bool ok = n == 6 && q.size() == 6;
    for (const MicroQuad& mq : q) {
      if (mq.dir > 5 || seen[mq.dir]) {
        ok = false;
        continue;
      }
      seen[mq.dir] = true;
      ok = ok && mq.w == 1.0f && mq.h == 1.0f && mq.material == 3 && mq.lod == 0 &&
           mq.x == ex[mq.dir] && mq.y == ey[mq.dir] && mq.z == ez[mq.dir];
    }
    for (bool s : seen) ok = ok && s;
    check(ok, "micro mesher: LOD0 of a subdivided block is exactly the plain 6-quad cube");
  }

  // LOD6 of a known tree: one level-6 cell -> 6 quads of 1/64 extent.
  {
    SparseOctree t;
    t.set(6, 0, 0, 0, 1);
    MicroMeshParams p;
    std::vector<MicroQuad> q;
    const size_t n = micro::meshOctreeLOD(t, 6, p, q);
    const float e = 1.0f / 64.0f;
    bool ok = n == 6 && q.size() == 6;
    bool plusX = false;
    for (const MicroQuad& mq : q) {
      ok = ok && mq.w == e && mq.h == e && mq.material == 1 && mq.lod == 6;
      if (mq.dir == 0) plusX = plusX || (mq.x == e && mq.y == 0.0f && mq.z == 0.0f);
    }
    check(ok && plusX, "micro mesher: LOD6 single micro-cell emits 6 quads of 1/64 extent");
  }

  // Greedy merging: two adjacent micro-cells -> one 2x1x1 box (shared face culled).
  {
    SparseOctree t;
    t.set(6, 0, 0, 0, 1);
    t.set(6, 1, 0, 0, 1);
    MicroMeshParams p;
    std::vector<MicroQuad> q;
    const size_t n = micro::meshOctreeLOD(t, 6, p, q);
    const float e = 1.0f / 64.0f;
    bool ok = n == 6 && q.size() == 6;
    bool topMerged = false;
    for (const MicroQuad& mq : q) {
      if (mq.dir == 2) topMerged = topMerged || (mq.w == 2.0f * e && mq.h == e && mq.y == e);
    }
    check(ok && topMerged, "micro mesher: adjacent micro-cells greedy-merge (shared face culled)");
  }

  // Determinism: same tree twice -> bit-identical quad stream.
  {
    SparseOctree t;
    for (int i = 0; i < 200; ++i) {
      uint64_t seed = uint64_t(i) * 0x9E3779B97F4A7C15ULL + 0x12345;
      const uint64_t s = testSplitmix64(seed);
      t.set(6, int(s % 64), int((s >> 8) % 64), int((s >> 16) % 64), uint8_t(1 + s % 3));
    }
    MicroMeshParams p;
    std::vector<MicroQuad> a, b;
    micro::meshOctreeLOD(t, 6, p, a);
    micro::meshOctreeLOD(t, 6, p, b);
    const bool same = a.size() == b.size() && !a.empty() &&
                      std::memcmp(a.data(), b.data(), a.size() * sizeof(MicroQuad)) == 0;
    std::printf("  (determinism tree: %zu quads)\\n", a.size());
    check(same, "micro mesher: LOD6 double run on a detailed tree is bit-identical");
  }

  // Empty tree -> no output, `out` untouched.
  {
    SparseOctree t;
    MicroMeshParams p;
    std::vector<MicroQuad> q(1);  // sentinel proves the vector is not modified
    check(micro::meshOctreeLOD(t, 6, p, q) == 0 && q.size() == 1,
          "micro mesher: empty tree emits nothing");
  }

  // LOD monotonicity: a detailed tree never loses quads going to finer LODs.
  {
    SparseOctree t;
    for (int z = 0; z < 2; ++z)
      for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) t.set(1, x, y, z, 2);  // solid block
    for (int x = 0; x < 64; x += 2)
      for (int z = 0; z < 64; z += 2) t.set(6, x, 63, z, 0);  // checkerboard pits on top
    MicroMeshParams p;
    size_t counts[7];
    for (int lod = 0; lod <= 6; ++lod) {
      std::vector<MicroQuad> q;
      counts[lod] = micro::meshOctreeLOD(t, lod, p, q);
    }
    std::printf("  (monotonicity quad counts LOD0..6: %zu %zu %zu %zu %zu %zu %zu)\\n", counts[0],
                counts[1], counts[2], counts[3], counts[4], counts[5], counts[6]);
    bool ok = counts[0] == 6 && counts[6] > counts[0];
    for (int lod = 1; lod <= 6; ++lod) ok = ok && counts[lod] >= counts[lod - 1];
    check(ok, "micro mesher: LOD quad counts are monotonic non-decreasing for a detailed tree");
  }

  // Boundary culling via neighborSolid: a micro-cell on the +X block face loses
  // its +X quad when the neighbour is opaque.
  {
    SparseOctree t;
    t.set(6, 63, 0, 0, 1);
    MicroMeshParams p;
    std::vector<MicroQuad> open, shut;
    micro::meshOctreeLOD(t, 6, p, open);
    p.neighborSolid[0] = 1;  // +X neighbour is solid
    micro::meshOctreeLOD(t, 6, p, shut);
    bool noPlusX = true;
    for (const MicroQuad& mq : shut) noPlusX = noPlusX && mq.dir != 0;
    check(open.size() == 6 && shut.size() == 5 && noPlusX,
          "micro mesher: neighborSolid culls the hidden boundary face");
  }

  // FOV-culling composition: micro-geometry stays inside the block AABB, so the
  // existing chunk-level frustum test (render.frustum_margin_deg) covers it and
  // subdivision never widens chunk bounds.
  {
    SparseOctree t;
    for (int i = 0; i < 120; ++i) {
      uint64_t seed = uint64_t(i) * 0xD1B54A32D192ED03ULL + 0x777;
      const uint64_t s = testSplitmix64(seed);
      t.set(6, int(s % 64), int((s >> 8) % 64), int((s >> 16) % 64), uint8_t(1 + s % 5));
    }
    MicroMeshParams p;
    p.originX = -5.5f;
    p.originY = 100.25f;
    p.originZ = 3.0f;
    std::vector<MicroQuad> q;
    micro::meshOctreeLOD(t, 6, p, q);
    check(!q.empty() && microQuadsInsideBlock(q, p.originX, p.originY, p.originZ),
          "micro mesher: all micro-quads lie inside the block AABB (chunk frustum culling covers them)");
  }

  // Chunk-build integration (real SubdivStore, workstream 1): empty store ->
  // zero behavior change; subdivided blocks get micro-geometry at the
  // camera-selected LOD; other blocks untouched.
  {
    subdiv::SubdivStore store;
    std::vector<MicroQuad> q;
    check(micro::appendChunkMicroGeometry(store, 0, 0, 0, 0.0f, 0.0f, 0.0f, q) == 0 && q.empty(),
          "micro mesher: empty SubdivStore -> no output (plain chunks untouched)");

    SparseOctree solid;
    for (int z = 0; z < 2; ++z)
      for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x) solid.set(1, x, y, z, 4);
    store.setBlockOctree(3, 5, 7, solid);

    // Camera at the block centre -> LOD 6; solid 64^3 block -> 6 merged quads.
    std::vector<MicroQuad> near;
    const size_t nn =
        micro::appendChunkMicroGeometry(store, 16, 0, -32, 19.5f, 5.5f, -24.5f, near);
    bool baked = nn == 6 && near.size() == 6;
    for (const MicroQuad& mq : near) {
      baked = baked && mq.lod == 6 && mq.material == 4 && mq.x >= 19.0f && mq.x <= 20.0f &&
              mq.y >= 5.0f && mq.y <= 6.0f && mq.z >= -25.0f && mq.z <= -24.0f;
    }
    check(baked, "micro mesher: near camera -> LOD6 micro-geometry with the block origin baked in");

    // Camera far away -> LOD 0 single cube.
    std::vector<MicroQuad> far;
    const size_t nf = micro::appendChunkMicroGeometry(store, 16, 0, -32, 1.0e6f, 0.0f, 0.0f, far);
    bool coarse = nf == 6 && far.size() == 6;
    for (const MicroQuad& mq : far) coarse = coarse && mq.lod == 0;
    check(coarse, "micro mesher: far camera -> LOD0 single cube");

    // Neighbour-opacity callback threads through to boundary culling.
    SparseOctree edge;
    edge.set(6, 63, 0, 0, 1);
    store.setBlockOctree(0, 0, 0, edge);
    std::vector<MicroQuad> culled;
    auto solidPX = [](int bx, int by, int bz, uint8_t solid[6]) {
      if (bx == 0 && by == 0 && bz == 0) solid[0] = 1;  // only the edge block's +X neighbour
    };
    micro::appendChunkMicroGeometry(store, 0, 0, 0, 0.5f, 0.5f, 0.5f, culled, solidPX);
    // Block (3,5,7) is ~9.1m away -> LOD 5 -> 6 quads; block (0,0,0) at LOD 6
    // loses its +X face -> 5 quads.
    bool seen5 = false;
    size_t total = culled.size();
    for (const MicroQuad& mq : culled) seen5 = seen5 || (mq.lod == 6 && mq.dir != 0);
    check(total == 11 && seen5,
          "micro mesher: integration honours the neighbour-solid callback per block");
  }
}

// PBR material registry tests live inside testRender() rather than as a separate ctest
// module: the registry is part of the render library, and keeping one render entry keeps the
// module list ("core world render sim ai storage procgen ui assets audio input net") stable.
void testPbrMaterials() {
  std::printf("render: PBR material registry + chunk palette + defaults data file\n");
  MaterialRegistry reg;
  MaterialDef iron;
  iron.metalness = 1.0f;
  iron.roughness = 0.35f;
  iron.albedoTexture = "iron_block";
  const uint16_t ironId = reg.registerMaterial("test:iron", iron);
  check(ironId == 0, "pbr: first registration gets compact ID 0");
  check(reg.idFor("test:iron") == 0 && reg.idFor("test:missing") == MaterialRegistry::kInvalidMaterialId,
        "pbr: string ID lookup hits and misses");
  const MaterialDef* ironDef = reg.defFor(0);
  check(ironDef && ironDef->metalness == 1.0f && ironDef->opaque(),
        "pbr: compact ID lookup returns the registered def");
  check(reg.defFor(7) == nullptr, "pbr: compact ID lookup of an unregistered ID is null");
  const std::string* ironName = reg.stringIdFor(0);
  check(ironName && *ironName == "test:iron", "pbr: compact ID -> string ID for saves");

  // Duplicate handling: identical params are idempotent, conflicting params are an error.
  const uint16_t ironAgain = reg.registerMaterial("test:iron", iron);
  check(ironAgain == 0 && reg.size() == 1, "pbr: identical re-registration is idempotent");
  MaterialDef conflict = iron;
  conflict.metalness = 0.0f;
  const uint16_t conflictId = reg.registerMaterial("test:iron", conflict);
  check(conflictId == MaterialRegistry::kInvalidMaterialId && reg.size() == 1 &&
        reg.defFor(0)->metalness == 1.0f,
        "pbr: conflicting re-registration errors and keeps the original");

  // Palette round-trip: string IDs -> voxel indices -> global IDs -> string IDs.
  MaterialDef stone;
  stone.albedoTexture = "stone";
  reg.registerMaterial("test:stone", stone);
  ChunkPalette pal;
  std::string perr;
  const std::vector<std::string> ids{"test:stone", "test:iron", "test:stone"};
  check(pal.build(reg, ids, &perr), "pbr: palette builds from the chunk's string IDs");
  check(pal.size() == 3 && pal.globalId(0) == reg.idFor("test:stone") &&
        pal.globalId(1) == reg.idFor("test:iron") && pal.globalId(2) == reg.idFor("test:stone"),
        "pbr: voxel index -> 16-bit global ID decode matches");
  const std::vector<std::string> back = pal.stringIds(reg);
  check(back == ids, "pbr: palette round-trips string IDs in voxel-index order");
  ChunkPalette tooBig;
  check(!tooBig.build(reg, std::vector<std::string>(300, "test:stone"), &perr),
        "pbr: palette rejects more than 256 materials");
  ChunkPalette unknown;
  check(!unknown.build(reg, {"test:nope"}, &perr), "pbr: palette rejects unknown string IDs");

  // Defaults data file: the engine ships LithoKraft materials as data, not C++ literals.
  MaterialRegistry defaults;
  std::string derr;
  check(defaults.loadFile(LITHOKRAFT_MATERIALS_PATH, &derr), "pbr: defaults data file loads");
  check(defaults.size() == 16, "pbr: defaults file registers 16 materials");
  const MaterialDef* dIron = defaults.defFor(defaults.idFor("lithokraft:iron"));
  const MaterialDef* dTorch = defaults.defFor(defaults.idFor("lithokraft:torch"));
  const MaterialDef* dWater = defaults.defFor(defaults.idFor("lithokraft:water"));
  const MaterialDef* dGlass = defaults.defFor(defaults.idFor("lithokraft:glass"));
  const MaterialDef* dLava = defaults.defFor(defaults.idFor("lithokraft:lava"));
  check(dIron && dIron->metalness == 1.0f && dIron->roughness == 0.35f,
        "pbr: lithokraft:iron is metallic (spot check)");
  check(dTorch && dTorch->emissiveIntensity == 2.0f && dTorch->emissive[0] == 1.0f,
        "pbr: lithokraft:torch is emissive (spot check)");
  check(dWater && !dWater->opaque() && dWater->transparency == TransparencyMethod::AlphaBlend,
        "pbr: lithokraft:water is translucent via alpha blend (spot check)");
  check(dGlass && dGlass->transparency == TransparencyMethod::ScreenDoor && dGlass->ior == 1.52f,
        "pbr: lithokraft:glass is screen-door with glass IOR (spot check)");
  check(dLava && dLava->emissiveIntensity == 3.0f, "pbr: lithokraft:lava is emissive (spot check)");
  // File-level error paths.
  MaterialRegistry bad;
  check(!bad.loadText("{not json", &derr), "pbr: malformed file text fails to load");
  MaterialRegistry dup;
  check(!dup.loadText("{\"materials\": [{\"id\": \"x:a\", \"roughness\": 0.5}, {\"id\": \"x:a\", \"roughness\": 0.6}]}", &derr),
        "pbr: conflicting duplicate IDs in a data file fail the load");
  MaterialRegistry idem;
  check(idem.loadText("{\"materials\": [{\"id\": \"x:a\", \"roughness\": 0.5}, {\"id\": \"x:a\", \"roughness\": 0.5}]}", &derr) &&
        idem.size() == 1,
        "pbr: identical duplicate IDs in a data file load idempotently");
}

// PBR BRDF core tests: known answers, energy conservation, mirror/monotonicity.
void testPbrBrdf() {
  std::printf("render: PBR Cook-Torrance BRDF core\n");
  constexpr float kPi = 3.14159265358979323846f;
  auto near = [](float a, float b, float eps) { return std::fabs(a - b) <= eps; };
  const std::array<float, 3> n{0.0f, 1.0f, 0.0f};
  const std::array<float, 3> up{0.0f, 1.0f, 0.0f};
  const std::array<float, 3> white{1.0f, 1.0f, 1.0f};

  // Known answers of the sub-terms.
  {
    const auto f0 = CookTorrance::fresnelSchlick({0.04f, 0.04f, 0.04f}, 1.0f);
    check(near(f0[0], 0.04f, 1e-6f) && near(f0[1], 0.04f, 1e-6f) && near(f0[2], 0.04f, 1e-6f),
          "pbr-brdf: dielectric at normal incidence reflects F0 = 0.04");
    const auto fg = CookTorrance::fresnelSchlick({0.04f, 0.04f, 0.04f}, 0.0f);
    check(near(fg[0], 1.0f, 1e-6f) && near(fg[1], 1.0f, 1e-6f) && near(fg[2], 1.0f, 1e-6f),
          "pbr-brdf: grazing incidence reflects white");
    check(near(CookTorrance::distributionGgx(1.0f, 1.0f), 1.0f / kPi, 1e-6f),
          "pbr-brdf: GGX D(NdotH = 1, roughness = 1) = 1/pi");
    check(near(CookTorrance::geometrySmith(1.0f, 1.0f, 0.3f), 1.0f, 1e-6f) &&
          near(CookTorrance::geometrySmith(1.0f, 1.0f, 0.9f), 1.0f, 1e-6f),
          "pbr-brdf: Smith geometry is exactly 1 at normal incidence, any roughness");
  }

  // Known answer of the full shade: rough (r = 1) dielectric at V = L = N.
  //   specular = F0 * D * G / (4 NdotV NdotL) = 0.04 / (4 pi); diffuse = 0.96 * albedo / pi.
  {
    const auto lo = CookTorrance::shade({0.8f, 0.8f, 0.8f}, 0.0f, 1.0f, n, up, up, white);
    const float expected = 0.96f * 0.8f / kPi + 0.04f / (4.0f * kPi);
    check(near(lo[0], expected, 2e-4f) && near(lo[1], expected, 2e-4f) && near(lo[2], expected, 2e-4f),
          "pbr-brdf: rough dielectric matches the hand-computed combined value");
  }

  // Perfect-mirror-ish check: at roughness ~0 the specular lobe is tight around the
  // reflection direction. Light at 30 deg off the normal; reflection dir = (-0.5, 0.866, 0).
  {
    const std::array<float, 3> light{0.5f, 0.8660254f, 0.0f};
    const std::array<float, 3> refl{-0.5f, 0.8660254f, 0.0f};
    const auto peak = CookTorrance::shade({0.8f, 0.8f, 0.8f}, 0.0f, 0.02f, n, refl, light, white);
    const auto off = CookTorrance::shade({0.8f, 0.8f, 0.8f}, 0.0f, 0.02f, n, up, light, white);
    check(peak[0] > off[0] * 100.0f,
          "pbr-brdf: near-mirror specular lobe is tight around the reflection direction");
  }

  // Monotonicity: higher roughness never increases peak specular (V = L = N), dielectric or metal.
  {
    const float roughs[] = {0.05f, 0.2f, 0.5f, 1.0f};
    bool ok = true;
    for (int m = 0; m < 2; ++m) {
      const std::array<float, 3> albedo = m == 0 ? std::array<float, 3>{0.8f, 0.8f, 0.8f}
                                                : std::array<float, 3>{1.0f, 0.7f, 0.5f};
      float prev = std::numeric_limits<float>::infinity();
      for (float r : roughs) {
        const auto lo = CookTorrance::shade(albedo, (float)m, r, n, up, up, white);
        ok = ok && lo[0] <= prev + 1e-6f;
        prev = lo[0];
      }
    }
    check(ok, "pbr-brdf: peak specular never increases with roughness (dielectric and metal)");
  }

  // Zero cases: light or view behind the surface contributes nothing.
  {
    const auto lightBehind = CookTorrance::shade({0.8f, 0.8f, 0.8f}, 0.0f, 0.5f, n, up,
                                                 {0.0f, -1.0f, 0.0f}, white);
    const auto viewBehind = CookTorrance::shade({0.8f, 0.8f, 0.8f}, 0.0f, 0.5f, n,
                                                {0.0f, -1.0f, 0.0f}, up, white);
    check(lightBehind[0] == 0.0f && lightBehind[1] == 0.0f && lightBehind[2] == 0.0f &&
          viewBehind[0] == 0.0f && viewBehind[1] == 0.0f && viewBehind[2] == 0.0f,
          "pbr-brdf: light/view behind the surface returns black");
  }

  // Energy conservation: total outgoing power must not exceed the incoming power
  // (lightColor = 1 => incoming = NdotL), across a sweep of incident angles x roughness
  // values x metalness 0/1.
  //
  // The integral is split by term because the specular lobe gets arbitrarily narrow at
  // low roughness: a plain outgoing-direction grid cannot resolve it (a sample landing
  // on the lobe gets over-weighted by its cell and the test fails on quadrature error,
  // not on the BRDF). Instead the specular part is integrated in half-vector space:
  //   P_spec = light * int_H D(H) * F * G * (V.H) dOmegaH,  V = 2(L.H)H - L
  // (change of variables from the V integral; dOmegaV = 4(V.H) dOmegaH). The lobe peak
  // is always at H = N, so thetaH uses a geometrically spaced grid dense at the pole;
  // the half-vector sliver with L.H <= 0 maps to no physical V and is excluded. The
  // diffuse part is smooth in V, so a coarse outgoing grid suffices there.
  {
    const float thetas[] = {15.0f, 35.0f, 55.0f, 75.0f};
    const float roughs[] = {0.05f, 0.15f, 0.3f, 0.6f, 1.0f};
    constexpr int kTH = 96, kPH = 24;  // half-vector grid (theta dense at the pole)
    float thG[kTH], thW[kTH];
    for (int k = 0; k < kTH; ++k) {
      const float t = (float)k / (kTH - 1);
      thG[k] = (kPi * 0.5f) * t * t * t;
    }
    for (int k = 0; k < kTH; ++k) {
      const float a = k > 0 ? thG[k - 1] : thG[k], b = k < kTH - 1 ? thG[k + 1] : thG[k];
      thW[k] = (b - a) * 0.5f;
    }
    const float dPhiH = 2.0f * kPi / kPH;
    constexpr int kTV = 8, kPV = 16;  // coarse outgoing grid for the smooth diffuse term
    const float dThetaV = (kPi * 0.5f) / kTV, dPhiV = 2.0f * kPi / kPV;
    auto dot3 = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
      return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    bool ok = true;
    for (float ti : thetas) {
      const float rad = ti * kPi / 180.0f;
      const std::array<float, 3> light{std::sin(rad), std::cos(rad), 0.0f};
      const float nDotL = std::cos(rad);  // incoming power with unit light color
      for (float r : roughs) {
        for (int m = 0; m < 2; ++m) {
          const float metalness = (float)m;
          const std::array<float, 3> albedo = m == 0 ? std::array<float, 3>{0.8f, 0.8f, 0.8f}
                                                    : std::array<float, 3>{1.0f, 0.7f, 0.5f};
          const std::array<float, 3> f0{0.04f * (1.0f - metalness) + albedo[0] * metalness,
                                       0.04f * (1.0f - metalness) + albedo[1] * metalness,
                                       0.04f * (1.0f - metalness) + albedo[2] * metalness};
          std::array<float, 3> pSpec{0.0f, 0.0f, 0.0f}, pDiff{0.0f, 0.0f, 0.0f};
          for (int i = 0; i < kTH; ++i) {
            const float cosH = std::cos(thG[i]), sinH = std::sin(thG[i]);
            for (int j = 0; j < kPH; ++j) {
              const float ph = (float)j * dPhiH;
              const std::array<float, 3> h{sinH * std::cos(ph), cosH, sinH * std::sin(ph)};
              const float lDotH = dot3(light, h);
              if (lDotH <= 0.0f) continue;  // == V.H; sliver maps to no physical V
              const std::array<float, 3> v{2.0f * lDotH * h[0] - light[0],
                                           2.0f * lDotH * h[1] - light[1],
                                           2.0f * lDotH * h[2] - light[2]};
              const float nDotV = dot3(n, v);
              if (nDotV <= 0.0f) continue;
              const float d = CookTorrance::distributionGgx(cosH, r);
              const float g = CookTorrance::geometrySmith(nDotV, nDotL, r);
              const auto f = CookTorrance::fresnelSchlick(f0, nDotV);
              const float w = d * g * lDotH * sinH * thW[i] * dPhiH * nDotL;
              pSpec[0] += f[0] * w;
              pSpec[1] += f[1] * w;
              pSpec[2] += f[2] * w;
            }
          }
          for (int i = 0; i < kTV; ++i) {
            const float tv = (i + 0.5f) * dThetaV;
            const float cosT = std::cos(tv), sinT = std::sin(tv);
            const float dOmega = sinT * dThetaV * dPhiV;
            for (int j = 0; j < kPV; ++j) {
              const float ph = (float)j * dPhiV;
              const std::array<float, 3> v{sinT * std::cos(ph), cosT, sinT * std::sin(ph)};
              const auto f = CookTorrance::fresnelSchlick(f0, cosT);
              const float w = nDotL * cosT * dOmega / kPi * (1.0f - metalness);
              pDiff[0] += (1.0f - f[0]) * albedo[0] * w;
              pDiff[1] += (1.0f - f[1]) * albedo[1] * w;
              pDiff[2] += (1.0f - f[2]) * albedo[2] * w;
            }
          }
          // 5% headroom covers the Schlick-Fresnel approximation (within ~2% below 75 deg)
          // plus quadrature error; a real energy-creation bug (missing 4 or pi) blows the
          // ratio to 3-12x, so the bound stays discriminating.
          ok = ok && pSpec[0] + pDiff[0] <= nDotL * 1.05f && pSpec[1] + pDiff[1] <= nDotL * 1.05f &&
               pSpec[2] + pDiff[2] <= nDotL * 1.05f;
        }
      }
    }
    check(ok, "pbr-brdf: outgoing power <= incoming across angles x roughness x metalness");
  }
}

// Fixture ILighting: the fill reports light at exactly the listed blocks.
struct FixtureLighting final : ILighting {
  std::map<std::array<int, 3>, uint8_t> level;
  uint8_t lightAt(int x, int y, int z, uint8_t) const override {
    auto it = level.find({x, y, z});
    return it == level.end() ? 0 : it->second;
  }
  uint8_t skyLight(uint8_t dim) const override { return skyLightForDimension(dim); }
};

void testPbrLights() {
  std::printf("render: PBR many-lights hybrid (K-nearest analytic lights)\n");
  auto near = [](float a, float b, float eps) { return std::fabs(a - b) <= eps; };

  // Synthetic emissive layout: red at d=2, green at d=5, blue at d=10 from origin.
  auto layout = [](auto&& sink) {
    sink(2.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f);
    sink(5.f, 0.f, 0.f, 0.f, 1.f, 0.f, 1.f);
    sink(10.f, 0.f, 0.f, 0.f, 0.f, 1.f, 1.f);
  };

  check(LightCollector().maxLights() == 8, "pbr-lights: default K is 8");

  {
    LightCollector c(2);
    const auto lights = c.collect(0.f, 0.f, 0.f, layout);
    check(lights.size() == 2, "pbr-lights: K=2 returns exactly 2 of 3 sources");
    check(lights.size() == 2 && near(lights[0].distance, 2.f, 1e-5f) &&
              near(lights[1].distance, 5.f, 1e-5f) && lights[0].distance < lights[1].distance,
          "pbr-lights: nearest-first order with Euclidean distances 2, 5");
    check(lights.size() == 2 && near(lights[0].r, 1.f, 1e-6f) && near(lights[1].g, 1.f, 1e-6f),
          "pbr-lights: colours ride along with their positions (red near, green far)");
  }

  {  // K cap: 10 sources along +x at d=1..10, K=3 keeps the 3 nearest.
    LightCollector c(3);
    const auto lights = c.collect(0.f, 0.f, 0.f, [](auto&& sink) {
      for (int i = 1; i <= 10; ++i)
        sink((float)i, 0.f, 0.f, 1.f, 1.f, 1.f, 1.f);
    });
    check(lights.size() == 3, "pbr-lights: K=3 caps 10 sources at 3 lights");
    check(lights.size() == 3 && near(lights[0].distance, 1.f, 1e-5f) &&
              near(lights[1].distance, 2.f, 1e-5f) && near(lights[2].distance, 3.f, 1e-5f),
          "pbr-lights: capped set is the 3 nearest (d = 1, 2, 3)");
  }

  {  // Empty scene and K=0 yield no lights; zero-intensity sources are ignored.
    LightCollector c(4);
    const auto empty = c.collect(0.f, 0.f, 0.f, [](auto&&) {});
    check(empty.empty(), "pbr-lights: empty scene -> no lights");
    LightCollector zero(0);
    const auto none = zero.collect(0.f, 0.f, 0.f, layout);
    check(none.empty(), "pbr-lights: K=0 -> no lights");
    const auto dim = c.collect(0.f, 0.f, 0.f, [](auto&& sink) {
      sink(1.f, 0.f, 0.f, 1.f, 1.f, 1.f, 0.f);  // intensity 0: not a source
    });
    check(dim.empty(), "pbr-lights: zero-intensity candidate is ignored");
  }

  {  // Ties resolve in enumeration order (first-enumerated wins).
    LightCollector c(1);
    const auto lights = c.collect(0.f, 0.f, 0.f, [](auto&& sink) {
      sink(2.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f);   // red enumerated first
      sink(-2.f, 0.f, 0.f, 0.f, 0.f, 1.f, 1.f);  // blue, same distance
    });
    check(lights.size() == 1 && near(lights[0].r, 1.f, 1e-6f),
          "pbr-lights: equal-distance tie keeps the first-enumerated source");
  }

  {  // Irradiance mapping: level/64, sky-only case, max-guards-partial-fill.
    const Irradiance dark = irradianceFromLightLevel(0, 0);
    check(near(dark.diffuse, 0.f, 1e-6f) && near(dark.ambient, 0.f, 1e-6f),
          "pbr-lights: irradiance(0, 0) = (0, 0)");
    const Irradiance full = irradianceFromLightLevel(64, 0);
    check(near(full.diffuse, 1.f, 1e-6f) && near(full.ambient, 0.f, 1e-6f),
          "pbr-lights: irradiance(64, 0) = (1, 0)");
    const Irradiance skyOnly = irradianceFromLightLevel(0, 64);
    check(near(skyOnly.diffuse, 1.f, 1e-6f) && near(skyOnly.ambient, 1.f, 1e-6f),
          "pbr-lights: sky-only irradiance(0, 64) = (1, 1)");
    const Irradiance half = irradianceFromLightLevel(32, 0);
    check(near(half.diffuse, 0.5f, 1e-6f) && near(half.ambient, 0.f, 1e-6f),
          "pbr-lights: irradiance(32, 0) = (0.5, 0)");
    const Irradiance stale = irradianceFromLightLevel(16, 64);
    check(near(stale.diffuse, 1.f, 1e-6f),
          "pbr-lights: diffuse takes max(block, sky) so daylight is 1 even on a stale fill");
  }

  {  // Attenuated radiance: rgb * intensity / (1 + d^2), feedable to shade().
    const AnalyticLight at{0.f, 0.f, 0.f, 1.f, 0.5f, 0.25f, 2.f, 0.f};
    const auto rad0 = attenuatedRadiance(at);
    check(near(rad0[0], 2.f, 1e-6f) && near(rad0[1], 1.f, 1e-6f) && near(rad0[2], 0.5f, 1e-6f),
          "pbr-lights: radiance at d=0 is rgb * intensity (no singularity)");
    AnalyticLight far = at;
    far.distance = 1.f;
    const auto rad1 = attenuatedRadiance(far);
    check(near(rad1[0], 1.f, 1e-6f) && near(rad1[1], 0.5f, 1e-6f) && near(rad1[2], 0.25f, 1e-6f),
          "pbr-lights: radiance at d=1 halves (inverse-square with +1 softener)");
  }

  {  // Adapter: candidates without flood-fill light are filtered out.
    FixtureLighting fx;
    fx.level[{5, 0, 0}] = 32;  // only the green source's block carries light
    LightCollector c(8);
    const auto lights = c.collect(0.f, 0.f, 0.f,
                                  filterCandidatesViaLighting(fx, 1, layout));
    check(lights.size() == 1 && near(lights[0].g, 1.f, 1e-6f),
          "pbr-lights: ILighting adapter drops candidates with no fill light");
  }
}

// PBR transparency tiers tests: ladder order, runtime selection, per-tier behavior,
// screen-door determinism, reflection/transmission energy split, refraction stub seam,
// tier x material-method interaction, opaque bypass.
void testPbrTiers() {
  std::printf("render: PBR transparency tiers (Simple -> ScreenDoor -> PbrReflections -> TrueRefraction stub)\n");
  auto near = [](float a, float b, float eps) { return std::fabs(a - b) <= eps; };

  // Enum order is JD's locked quality ladder.
  check(static_cast<int>(PbrTransparencyTier::Simple) < static_cast<int>(PbrTransparencyTier::ScreenDoor) &&
        static_cast<int>(PbrTransparencyTier::ScreenDoor) < static_cast<int>(PbrTransparencyTier::PbrReflections) &&
        static_cast<int>(PbrTransparencyTier::PbrReflections) < static_cast<int>(PbrTransparencyTier::TrueRefraction),
        "pbr-tiers: enum order is Simple < ScreenDoor < PbrReflections < TrueRefraction");

  // Config: default Simple ("just works"), settable at runtime.
  TransparencyTierConfig cfg;
  check(cfg.tier == PbrTransparencyTier::Simple, "pbr-tiers: default tier is Simple (\"just works\")");
  cfg.setTier(PbrTransparencyTier::ScreenDoor);
  check(cfg.tier == PbrTransparencyTier::ScreenDoor && cfg.getTier() == PbrTransparencyTier::ScreenDoor,
        "pbr-tiers: tier is settable at runtime");
  check(std::string(pbrTransparencyTierName(PbrTransparencyTier::PbrReflections)) == "PbrReflections" &&
        std::string(pbrTransparencyTierName(PbrTransparencyTier::Simple)) == "Simple",
        "pbr-tiers: tier names round-trip for UI/logging");

  // Fixtures: dielectric glass-like material + a head-on shading context.
  MaterialDef glass;
  glass.albedo = {0.9f, 0.95f, 1.0f};
  glass.roughness = 0.05f;
  glass.metalness = 0.0f;
  glass.ior = 1.52f;
  glass.alpha = 0.4f;
  glass.transparency = TransparencyMethod::ScreenDoor;
  TransparentShadingContext ctx;
  ctx.normal = {0.f, 1.f, 0.f};
  ctx.viewDir = {0.f, 1.f, 0.f};
  ctx.lightDir = {0.f, 1.f, 0.f};
  ctx.lightColor = {1.f, 1.f, 1.f};
  ctx.background = {0.2f, 0.2f, 0.2f};
  ctx.pixelX = 7;
  ctx.pixelY = 13;

  // Screen-door determinism: same pixel + same alpha -> same decision, via existing machinery.
  check(screenDoorTest(7, 13, alphaToByte(0.4f)) == screenDoorTest(7, 13, alphaToByte(0.4f)),
        "pbr-tiers: screen-door discard is deterministic for the same pixel");
  check(!screenDoorTest(3, 5, 0) && screenDoorTest(3, 5, 255),
        "pbr-tiers: screen-door alpha edges (0 discards, 255 keeps)");

  // Simple tier: classic alpha blend, no PBR.
  {
    TransparencyTierConfig c;
    c.setTier(PbrTransparencyTier::Simple);
    const TransparencyResult r = shadeTransparent(glass, c, ctx, {0.f, 0.f, 0.f});
    check(near(r.color[0], 0.9f * 0.4f + 0.2f * 0.6f, 1e-6f) &&
          near(r.color[1], 0.95f * 0.4f + 0.2f * 0.6f, 1e-6f) &&
          near(r.color[2], 1.0f * 0.4f + 0.2f * 0.6f, 1e-6f) && !r.discarded &&
          !r.refractionRequested,
          "pbr-tiers: Simple tier is the classic alpha blend, no PBR, no discard, no stub");
  }

  // ScreenDoor tier: full PBR shading + 4x4 Bayer discard, no sorting needed.
  {
    TransparencyTierConfig c;
    c.setTier(PbrTransparencyTier::ScreenDoor);
    const TransparencyResult r = shadeTransparent(glass, c, ctx, {0.f, 0.f, 0.f});
    const std::array<float, 3> ref = CookTorrance::shade(
        glass.albedo, glass.metalness, glass.roughness, ctx.normal, ctx.viewDir, ctx.lightDir,
        ctx.lightColor);
    check(near(r.color[0], ref[0], 1e-6f) && near(r.color[1], ref[1], 1e-6f) &&
          near(r.color[2], ref[2], 1e-6f),
          "pbr-tiers: ScreenDoor tier shades with the full PBR pipeline");
    check(r.discarded == !screenDoorTest(7, 13, alphaToByte(0.4f)),
          "pbr-tiers: ScreenDoor tier discards exactly per the 4x4 Bayer test");
    const TransparencyResult r2 = shadeTransparent(glass, c, ctx, {0.f, 0.f, 0.f});
    check(r.discarded == r2.discarded, "pbr-tiers: ScreenDoor tier is deterministic per pixel");
  }

  // PbrReflections tier: energy split sanity + composition without refraction.
  {
    // Dielectric glass (IOR 1.52) at normal incidence reflects ~= F0 = 0.04.
    const ReflectionTransmissionSplit split = splitReflectionTransmission(dielectricF0(1.52f), 1.0f, 0.4f);
    check(near(dielectricF0(1.52f), 0.04f, 0.01f) && near(split.reflected, 0.04f, 0.01f),
          "pbr-tiers: dielectric glass at normal incidence reflects ~= F0 = 0.04");
    check(near(split.reflected + split.transmitted, split.reflected + (1.0f - split.reflected) * 0.4f, 1e-6f) &&
          split.reflected + split.transmitted <= 1.0f,
          "pbr-tiers: reflection/transmission split conserves energy (nothing created)");
    TransparencyTierConfig c;
    c.setTier(PbrTransparencyTier::PbrReflections);
    const TransparencyResult r = shadeTransparent(glass, c, ctx, {0.f, 0.f, 0.f});
    const std::array<float, 3> ref = CookTorrance::shade(
        glass.albedo, glass.metalness, glass.roughness, ctx.normal, ctx.viewDir, ctx.lightDir,
        ctx.lightColor);
    check(near(r.color[0], ref[0] * 0.4f + 0.2f * 0.6f, 1e-5f) &&
          near(r.color[1], ref[1] * 0.4f + 0.2f * 0.6f, 1e-5f) &&
          near(r.color[2], ref[2] * 0.4f + 0.2f * 0.6f, 1e-5f) && !r.discarded &&
          !r.refractionRequested,
          "pbr-tiers: PbrReflections = PBR reflection + simple alpha transmission, no refraction");
  }

  // TrueRefraction tier: stub exposes the documented seam, does not claim to refract.
  {
    TransparencyTierConfig c;
    c.setTier(PbrTransparencyTier::TrueRefraction);
    const TransparencyResult r = shadeTransparent(glass, c, ctx, {1.f, 2.f, 3.f});
    check(r.refractionRequested, "pbr-tiers: TrueRefraction tier requests refraction (stub)");
    check(r.color[0] == 0.f && r.color[1] == 0.f && r.color[2] == 0.f && !r.discarded,
          "pbr-tiers: TrueRefraction stub leaves color black — it does not fake a refracted color");
    check(near(r.refraction.surfacePoint[0], 1.f, 1e-6f) &&
          near(r.refraction.surfacePoint[1], 2.f, 1e-6f) &&
          near(r.refraction.surfacePoint[2], 3.f, 1e-6f) && r.refraction.pixelX == 7 &&
          r.refraction.pixelY == 13 && near(r.refraction.ior, 1.52f, 1e-6f),
          "pbr-tiers: stub request carries surface point, pixel, and IOR for the Vulkan path");
    const float len = std::sqrt(r.refraction.refractDir[0] * r.refraction.refractDir[0] +
                                r.refraction.refractDir[1] * r.refraction.refractDir[1] +
                                r.refraction.refractDir[2] * r.refraction.refractDir[2]);
    check(near(len, 1.f, 1e-5f) && near(r.refraction.refractDir[1], -1.f, 1e-5f) &&
          !r.refraction.totalInternalReflection,
          "pbr-tiers: stub request carries a unit Snell refracted ray (straight through at normal incidence, no TIR on air->glass entry)");
  }

  // Tier x material-method interaction.
  check(tierTransparencyMethod(PbrTransparencyTier::Simple, TransparencyMethod::AlphaBlend) ==
            TransparencyMethod::AlphaBlend,
        "pbr-tiers: Simple tier honors the per-material method");
  check(tierTransparencyMethod(PbrTransparencyTier::ScreenDoor, TransparencyMethod::AlphaBlend) ==
            TransparencyMethod::ScreenDoor,
        "pbr-tiers: ScreenDoor tier funnels blend methods into the PBR + dither pipeline");
  check(tierTransparencyMethod(PbrTransparencyTier::ScreenDoor, TransparencyMethod::Additive) ==
            TransparencyMethod::Additive &&
        tierTransparencyMethod(PbrTransparencyTier::TrueRefraction, TransparencyMethod::Additive) ==
            TransparencyMethod::Additive,
        "pbr-tiers: additive glow stays additive at every tier");
  check(tierTransparencyMethod(PbrTransparencyTier::PbrReflections, TransparencyMethod::AlphaBlend) ==
            TransparencyMethod::AlphaBlend &&
        tierTransparencyMethod(PbrTransparencyTier::PbrReflections, TransparencyMethod::WeightedOIT) ==
            TransparencyMethod::WeightedOIT,
        "pbr-tiers: PbrReflections keeps each method's blend strategy");

  // Additive materials bypass the tier framework; opaque materials never enter it.
  {
    MaterialDef glow;
    glow.alpha = 0.5f;
    glow.transparency = TransparencyMethod::Additive;
    TransparencyTierConfig c;
    c.setTier(PbrTransparencyTier::TrueRefraction);
    const TransparencyResult g = shadeTransparent(glow, c, ctx, {0.f, 0.f, 0.f});
    check(!g.refractionRequested && !g.discarded,
          "pbr-tiers: additive glow never enters the refraction stub");
    MaterialDef rock;  // default alpha = 1.0 -> opaque
    const TransparencyResult r = shadeTransparent(rock, c, ctx, {0.f, 0.f, 0.f});
    check(!r.discarded && !r.refractionRequested,
          "pbr-tiers: opaque materials bypass the transparency framework at every tier");
  }
}

// Test emissive provider: one analytic light straight above the fragment.
// (Stands in for the world-side forEachEmissive seam documented in
// pbr_software_shaders.h.)
struct TestEmissiveProvider final : IEmissiveProvider {
  void forEachEmissive(
      const std::function<void(float, float, float, float, float, float, float)>& sink) const override {
    sink(0.f, 10.f, 0.f, 1.f, 0.9f, 0.8f, 2.0f);  // pos, linear rgb, intensity
  }
};

void testPbrSoftware() {
  std::printf("render: PBR software-renderer integration (registry -> BRDF -> lights -> pixel)\n");
  auto chanClose = [](uint32_t a, uint32_t b, int tol) {
    for (int s = 16; s >= 0; s -= 8) {
      const int da = int((a >> s) & 0xFF), db = int((b >> s) & 0xFF);
      if (std::abs(da - db) > tol) return false;
    }
    return true;
  };
  auto packLinear = [](const std::array<float, 3>& lo) {
    const uint8_t r = uint8_t(pbrLinearToSrgb(lo[0]) * 255.0f + 0.5f);
    const uint8_t g = uint8_t(pbrLinearToSrgb(lo[1]) * 255.0f + 0.5f);
    const uint8_t b = uint8_t(pbrLinearToSrgb(lo[2]) * 255.0f + 0.5f);
    return 0xFF000000u | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
  };

  // Registry with a metal (specular path) and an emissive dielectric (emissive path).
  MaterialRegistry preg;
  MaterialDef iron;
  iron.albedo = {0.56f, 0.57f, 0.58f};
  iron.roughness = 0.35f;
  iron.metalness = 1.0f;
  const uint16_t ironId = preg.registerMaterial("test:iron", iron);
  MaterialDef torch;
  torch.albedo = {0.9f, 0.85f, 0.8f};
  torch.roughness = 0.9f;
  torch.metalness = 0.0f;
  torch.emissive = {1.0f, 0.45f, 0.1f};
  torch.emissiveIntensity = 2.5f;
  const uint16_t torchId = preg.registerMaterial("test:torch", torch);
  check(ironId != MaterialRegistry::kInvalidMaterialId &&
        torchId != MaterialRegistry::kInvalidMaterialId && ironId != torchId,
        "pbr-software: fixture materials register with distinct compact IDs");

  // Context: zero flood-fill irradiance (pixel must equal the analytic term exactly),
  // camera straight above the fragment, one test light straight above it too.
  TestEmissiveProvider provider;
  PbrFragmentContext pctx;
  pctx.registry = &preg;
  pctx.emissive = &provider;
  pctx.blockLight = 0;
  pctx.skyLight = 0;
  pctx.camX = 0.f; pctx.camY = 10.f; pctx.camZ = 0.f;

  // Synthetic fragment: up normal at the origin, carrying the iron compact ID.
  FragmentInput fin{};  // value-init: unset map pointers are null, not indeterminate
  fin.varyings.nx = 0.f; fin.varyings.ny = 1.f; fin.varyings.nz = 0.f;
  fin.varyings.wx = 0.f; fin.varyings.wy = 0.f; fin.varyings.wz = 0.f;
  fin.varyings.materialId = ironId;
  fin.pbr = &pctx;

  // Parity: same inputs through CookTorrance::shade directly. The shader derives
  // n = v = l = (0,1,0) and light distance 10 from the context above.
  {
    const std::array<float, 3> n{0.f, 1.f, 0.f}, v{0.f, 1.f, 0.f}, l{0.f, 1.f, 0.f};
    const AnalyticLight al{0.f, 10.f, 0.f, 1.f, 0.9f, 0.8f, 2.0f, 10.f};
    const std::array<float, 3> lo =
        CookTorrance::shade(iron.albedo, iron.metalness, iron.roughness, n, v, l,
                            attenuatedRadiance(al));
    check(chanClose(pbrFragmentShader(fin), packLinear(lo), 2),
          "pbr-software: fragment == CookTorrance::shade on identical inputs (epsilon)");
  }

  // Chain: material-ID -> registry -> pixel. The torch fragment must match a direct
  // shade of the torch def plus its emissive term, and differ from the iron pixel.
  {
    fin.varyings.materialId = torchId;
    const uint32_t torchPx = pbrFragmentShader(fin);
    fin.varyings.materialId = ironId;
    const uint32_t ironPx = pbrFragmentShader(fin);
    const std::array<float, 3> n{0.f, 1.f, 0.f}, v{0.f, 1.f, 0.f}, l{0.f, 1.f, 0.f};
    const AnalyticLight al{0.f, 10.f, 0.f, 1.f, 0.9f, 0.8f, 2.0f, 10.f};
    std::array<float, 3> lo =
        CookTorrance::shade(torch.albedo, torch.metalness, torch.roughness, n, v, l,
                            attenuatedRadiance(al));
    for (int i = 0; i < 3; ++i) lo[i] += torch.emissive[i] * torch.emissiveIntensity;
    check(chanClose(torchPx, packLinear(lo), 2),
          "pbr-software: emissive material resolves through the registry (shade + emissive)");
    check(torchPx != ironPx,
          "pbr-software: different compact IDs shade to different pixels (registry chain)");
  }

  // Graceful degradation: unknown material / null context fall back to basic shading.
  {
    fin.varyings.materialId = 0xFFFF;
    fin.pbr = &pctx;
    check(pbrFragmentShader(fin) == basicFragmentShader(fin),
          "pbr-software: unregistered material ID falls back to basicFragmentShader");
    fin.varyings.materialId = ironId;
    fin.pbr = nullptr;
    check(pbrFragmentShader(fin) == basicFragmentShader(fin),
          "pbr-software: null PBR context falls back to basicFragmentShader");
    fin.pbr = &pctx;
  }

  // Tier selection: config default is Basic, settable; the selector maps tiers to the
  // exact shader functions; the renderer honors it.
  {
    SoftwareShadingConfig cfg;
    check(cfg.getTier() == SoftwareShadingTier::Basic,
          "pbr-software: default software tier is Basic (basic path intact)");
    cfg.setTier(SoftwareShadingTier::Pbr);
    check(cfg.getTier() == SoftwareShadingTier::Pbr,
          "pbr-software: software tier is settable at runtime");
    check(selectFragmentShader(SoftwareShadingTier::Basic) == &basicFragmentShader &&
          selectFragmentShader(SoftwareShadingTier::Pbr) == &pbrFragmentShader,
          "pbr-software: tier selects basicFragmentShader vs pbrFragmentShader");
    SoftwareRenderer sr;
    check(sr.shadingTier() == SoftwareShadingTier::Basic,
          "pbr-software: renderer default tier is Basic");
    sr.setShadingTier(SoftwareShadingTier::Pbr);
    sr.setPbrContext(&pctx);
    check(sr.shadingTier() == SoftwareShadingTier::Pbr,
          "pbr-software: renderer tier and PBR context are settable");
  }

  // Renderer smoke: per-pixel path draws through the tier-selected shader. A top-face
  // quad with identity-ish MVP; Basic and Pbr tiers must agree here because the mesh
  // carries no material ID (documented vertex-format seam -> graceful fallback).
  {
    SoftwareRenderer sr;
    sr.resize(800, 600);
    sr.setPerPixelShading(true);
    // MVP: block units [0,16] -> NDC [-1,1] on x/y, z passthrough, w = 1.
    float mvp[16] = {};
    mvp[0] = 0.125f; mvp[5] = 0.125f; mvp[10] = 1.f;
    mvp[12] = -1.f; mvp[13] = -1.f; mvp[15] = 1.f;
    // Quad (0,0)-(16,16) at z = 0.5 blocks (depth 0.5 < 1.0), top face (face 2),
    // wound in the mesher convention (CCW seen from the face normal, i.e. from +Z):
    // after the software renderer's y-down flip this lands CW on screen (cross < 0),
    // which isBackface keeps. (Fix 2026-10-11: the old `cross <= 0` cull was inverted
    // and this quad was wound backwards to match it.)
    PackedVertex q[4] = {};
    const uint16_t c[4][3] = {{0, 0, 8}, {256, 0, 8}, {256, 256, 8}, {0, 256, 8}};
    for (int i = 0; i < 4; ++i) {
      q[i].x = c[i][0]; q[i].y = c[i][1]; q[i].z = c[i][2];
      q[i].faceAo = 2;  // top
    }
    sr.setShadingTier(SoftwareShadingTier::Basic);
    sr.clear(0xFF000000);
    sr.renderMesh(q, 4, mvp);
    const uint32_t basicPx = sr.framebuffer().pixels[size_t(300) * 800 + 400];
    sr.setShadingTier(SoftwareShadingTier::Pbr);
    sr.setPbrContext(&pctx);
    sr.clear(0xFF000000);
    sr.renderMesh(q, 4, mvp);
    const uint32_t pbrPx = sr.framebuffer().pixels[size_t(300) * 800 + 400];
    check(basicPx != 0xFF000000,
          "pbr-software: per-pixel path rasterizes the quad (pixel differs from clear)");
    check(pbrPx == basicPx,
          "pbr-software: PBR tier without material IDs degrades to the basic pixel");
  }
}

// Backface-culling convention pin (fix 2026-10-11): the software renderer's screen
// is y-down (transformVertex flips Y), which is orientation-reversing. A quad wound
// in the mesher convention (CCW seen from the face normal, what glFrontFace(GL_CCW)
// keeps) must survive culling; the same quad wound backwards must be fully culled.
void testSoftwareBackfaceConvention() {
  std::printf("render: software backface-culling convention\n");
  float mvp[16] = {};
  mvp[0] = 0.125f; mvp[5] = 0.125f; mvp[10] = 1.f;
  mvp[12] = -1.f; mvp[13] = -1.f; mvp[15] = 1.f;
  auto renderQuad = [&](const uint16_t c[4][3]) {
    SoftwareRenderer sr;
    sr.resize(800, 600);
    PackedVertex q[4] = {};
    for (int i = 0; i < 4; ++i) {
      q[i].x = c[i][0]; q[i].y = c[i][1]; q[i].z = c[i][2];
      q[i].faceAo = 2; q[i].value = 200;  // bright: pixel must differ from clear
    }
    sr.clear(0xFF000000);
    sr.renderMesh(q, 4, mvp);
    return sr.framebuffer().pixels[size_t(300) * 800 + 400];
  };
  // Mesher convention for a +Z face: (0,0)->(16,0)->(16,16)->(0,16) in block units.
  const uint16_t front[4][3] = {{0, 0, 8}, {256, 0, 8}, {256, 256, 8}, {0, 256, 8}};
  // Reversed winding: must be culled.
  const uint16_t back[4][3] = {{0, 0, 8}, {0, 256, 8}, {256, 256, 8}, {256, 0, 8}};
  check(renderQuad(front) != 0xFF000000,
        "render: mesher-wound quad survives software backface culling");
  check(renderQuad(back) == 0xFF000000,
        "render: reverse-wound quad is fully culled by the software renderer");
}

// P1-RENDER honesty pack (2026-10-13): the software renderer's IRenderer
// surface is real — frustum culling in renderFrame (M1), the HUD/crosshair
// rasterizer (H3), the BinaryQuad PBR upload path with real material IDs
// (H2), and the ground-trace CPU-mirror consumer (H1).
void testSoftwareRenderHonesty() {
  std::printf("render: software renderer honesty (culling / HUD / PBR / traces)\n");
  // Shared fixture: the orthographic-ish MVP from the per-pixel test above
  // (block [0,16] -> NDC [-1,1] on x/y, z passthrough, w = 1).
  auto makeParams = [] {
    FrameParams p;
    p.width = 64; p.height = 64;
    p.viewProj = Mat4{};
    p.viewProj.m[0] = 0.125f; p.viewProj.m[5] = 0.125f; p.viewProj.m[10] = 1.f;
    p.viewProj.m[12] = -1.f; p.viewProj.m[13] = -1.f; p.viewProj.m[15] = 1.f;
    p.camPos = {8.f, 8.f, 8.f};
    p.sky = {0.58f, 0.70f, 0.86f};
    p.crosshair = false;
    return p;
  };
  // One quad spanning blocks x/y 4..12 at local z = 0 (faces the camera).
  auto makeQuad = [](uint8_t mode) {
    ChunkMesh m;
    const uint16_t c[4][3] = {{64, 64, 0}, {192, 64, 0}, {192, 192, 0}, {64, 192, 0}};
    for (int i = 0; i < 4; ++i) {
      PackedVertex v{};
      v.x = c[i][0]; v.y = c[i][1]; v.z = c[i][2];
      v.faceAo = 2; v.mode = mode; v.value = 200;
      m.verts.push_back(v);
    }
    return m;
  };
  const uint32_t kSkyPx = 0xFF000000u | (148u << 16) | (179u << 8) | 219u;

  // M1: renderFrame frustum-culls with the margin; drawn counts submitted only.
  {
    SoftwareRenderer sr;
    sr.setPerPixelShading(false);
    ChunkMesh front = makeQuad(kShadeTex);
    front.cx = 0; front.sy = 0; front.cz = 0;
    sr.uploadChunkMesh(front);
    ChunkMesh behind = makeQuad(kShadeTex);
    behind.cx = 0; behind.sy = 0; behind.cz = -5;  // far outside the frustum
    sr.uploadChunkMesh(behind);
    sr.renderFrame(makeParams());
    check(sr.stats().meshes == 2, "software-honesty: stats count both stored meshes");
    check(sr.stats().drawn == 1,
          "software-honesty: frustum culling submits only the in-frustum mesh (M1)");
  }

  // H3: HUD rect rasterizes over the world; text glyphs land; crosshair plots.
  {
    SoftwareRenderer sr;
    FrameParams p = makeParams();
    p.hud.rects.push_back(HudRect{20.f, 20.f, 24.f, 24.f, 1.f, 0.f, 0.f, 1.f});
    p.hud.texts.push_back(HudText{0.f, 0.f, "HI", 1.f, 1.f, 1.f, 1.f});
    sr.renderFrame(p);
    const auto& px = sr.framebuffer().pixels;
    check(px[size_t(32) * 64 + 32] == 0xFFFF0000u,
          "software-honesty: HUD rect rasterizes (center pixel is the rect color)");
    bool glyph = false;
    for (int y = 0; y < 16 && !glyph; ++y)
      for (int x = 0; x < 24 && !glyph; ++x) {
        const uint32_t v = px[size_t(y) * 64 + size_t(x)];
        glyph = ((v >> 16) & 0xFF) > 200 && ((v >> 8) & 0xFF) > 200 && (v & 0xFF) > 200;
      }
    check(glyph, "software-honesty: HUD text glyph rasterizes (near-white pixel in the text cell)");
    FrameParams p2 = makeParams();
    p2.crosshair = true;
    sr.renderFrame(p2);
    check(sr.framebuffer().pixels[size_t(32) * 64 + 32] == 0xFFFFFFFFu,
          "software-honesty: crosshair plots at the frame center");
    check(sr.framebuffer().pixels[0] == kSkyPx,
          "software-honesty: crosshair frame corner stays sky (no stray overlay)");
  }

  // H2: supportsPbrPath tells the truth; BinaryQuad uploads shade by their
  // real per-vertex material ID (two materials -> two different pixels).
  {
    MaterialRegistry reg;
    MaterialDef stone;
    stone.albedo = {0.5f, 0.5f, 0.52f};
    stone.roughness = 0.95f;
    MaterialDef iron;
    iron.albedo = {0.55f, 0.58f, 0.62f};
    iron.roughness = 0.35f;
    iron.metalness = 1.0f;
    const uint16_t stoneId = reg.registerMaterial("test:stone", stone);
    const uint16_t ironId = reg.registerMaterial("test:iron", iron);
    SoftwareRenderer sr;
    check(sr.supportsPbrPath(), "software-honesty: supportsPbrPath() is true (PBR path landed)");
    sr.setPbrMaterials(reg);
    auto renderMat = [&](uint32_t material) {
      PbrChunkMesh pm;
      const uint16_t c[4][3] = {{64, 64, 0}, {192, 64, 0}, {192, 192, 0}, {64, 192, 0}};
      for (int i = 0; i < 4; ++i) {
        PbrQuadVertex v{};
        v.x = c[i][0]; v.y = c[i][1]; v.z = c[i][2];
        v.face = 4;  // +Z, facing the camera
        v.material = material;
        v.blockLight = 0; v.skyLight = 64;
        pm.verts.push_back(v);
      }
      sr.uploadPbrChunkMesh(pm);
      sr.renderFrame(makeParams());
      return sr.framebuffer().pixels[size_t(32) * 64 + 32];
    };
    const uint32_t stonePx = renderMat(stoneId);
    const uint32_t ironPx = renderMat(ironId);
    check(stonePx != kSkyPx && ironPx != kSkyPx,
          "software-honesty: PBR mesh rasterizes (center pixel is not sky)");
    check(stonePx != ironPx,
          "software-honesty: per-vertex material ID drives PBR shading (stone != iron)");
  }

  // H1: ground traces — the software backend consumes groundTraceEmissive.
  // Off must equal no-params (default frames unchanged); on must add light.
  {
    SoftwareRenderer sr;
    sr.setPerPixelShading(true);
    ChunkMesh m = makeQuad(kShadeGroundPulse);
    // Trace params: density .5, speed 1, amplitude .6, dirMix .5, white tint.
    m.traceP0[0] = 0.5f; m.traceP0[1] = 1.0f; m.traceP0[2] = 0.6f; m.traceP0[3] = 0.5f;
    m.traceP1[0] = 1.0f; m.traceP1[1] = 1.0f; m.traceP1[2] = 1.0f;
    sr.uploadChunkMesh(m);
    FrameParams off = makeParams();
    off.groundTraceOn = false;
    sr.renderFrame(off);
    const std::vector<uint32_t> offPx = sr.framebuffer().pixels;
    // Pick t so an H-set packet peaks at world x = 8 on the z = 0 line:
    // fract(sgn * 8 * pk - t * speed + h1) = 0.09 with pk = density * 0.25.
    const float h1 = groundTraceHash01(0, 1u);
    const float sgn = h1 < 0.5f ? 1.f : -1.f;
    FrameParams on = makeParams();
    on.groundTraceOn = true;
    on.groundTraceTimeSec = sgn * 8.f * 0.125f + h1 - 0.09f;
    sr.renderFrame(on);
    const auto& onPx = sr.framebuffer().pixels;
    int maxDiff = 0;
    for (size_t i = 0; i < onPx.size(); ++i) {
      const int d = int((onPx[i] >> 16) & 0xFF) - int((offPx[i] >> 16) & 0xFF);
      if (d > maxDiff) maxDiff = d;
    }
    check(maxDiff >= 10,
          "software-honesty: groundTraceOn adds trace emissive on the software backend (H1)");
    // Zero trace params (today's meshes everywhere else) -> no contribution
    // even with the toggle on: the default-frame gating pin.
    SoftwareRenderer sr2;
    sr2.setPerPixelShading(true);
    sr2.uploadChunkMesh(makeQuad(kShadeGroundPulse));
    FrameParams on2 = makeParams();
    on2.groundTraceOn = true;
    on2.groundTraceTimeSec = 3.25f;
    sr2.renderFrame(on2);
    check(sr2.framebuffer().pixels == offPx,
          "software-honesty: zero trace params render identically with traces on (default unchanged)");
  }
}

// P3-3 (Appendix D M2 + M3): the software sprite pass honours shade modes
// (AlphaBlend/Additive blend in producer order with depth TEST but no
// depth WRITE; Cutout stays the opaque luminance form), and FramePacer
// implements the render.fps_cap deadline (default off).
void testSoftwareSpritesAndPacer() {
  std::printf("render: software sprite shade modes (D-M2) + frame pacer (D-M3)\n");
  auto makeParams = [] {
    FrameParams p;
    p.width = 64; p.height = 64;
    p.viewProj = Mat4{};
    p.viewProj.m[0] = 0.125f; p.viewProj.m[5] = 0.125f; p.viewProj.m[10] = 1.f;
    p.viewProj.m[12] = -1.f; p.viewProj.m[13] = -1.f; p.viewProj.m[15] = 1.f;
    p.camPos = {8.f, 8.f, 8.f};
    p.sky = {0.58f, 0.70f, 0.86f};
    p.crosshair = false;
    return p;
  };
  auto solidPage = [](uint8_t r, uint8_t g, uint8_t b) {
    std::vector<uint8_t> px(4 * 4 * 4);
    for (int i = 0; i < 16; ++i) {
      px[size_t(i) * 4] = r; px[size_t(i) * 4 + 1] = g; px[size_t(i) * 4 + 2] = b; px[size_t(i) * 4 + 3] = 255;
    }
    return Image(4, 4, std::move(px));
  };
  auto makeSprite = [](float z, ShadeMode shade, float a) {
    SpriteDrawQuad q;
    q.corners[0] = {4.f, 4.f, z}; q.corners[1] = {12.f, 4.f, z};
    q.corners[2] = {12.f, 12.f, z}; q.corners[3] = {4.f, 12.f, z};
    q.uv[0][0] = 0; q.uv[0][1] = 0; q.uv[1][0] = 1; q.uv[1][1] = 0;
    q.uv[2][0] = 1; q.uv[2][1] = 1; q.uv[3][0] = 0; q.uv[3][1] = 1;
    q.shade = shade; q.a = a; q.page = 0;
    return q;
  };
  const size_t kCenter = size_t(32) * 64 + 32;
  {  // AlphaBlend: src-over blend of the true average color against sky.
    SoftwareRenderer sr;
    FrameParams p = makeParams();
    std::vector<Image> pages{solidPage(255, 0, 0)};
    p.spriteAtlasPages = pages;
    p.spriteAtlasVersion = 1;
    SpriteDrawQuad q = makeSprite(0.5f, kShadeAlphaBlend, 0.5f);
    p.spriteQuads = &q; p.spriteQuadCount = 1;
    sr.renderFrame(p);
    check(sr.framebuffer().pixels[kCenter] == 0xFFCA5A6Eu,
          "sprite-dm2: AlphaBlend sprite blends its average color over the sky (no opaque flattening)");
  }
  {  // Cutout: unchanged opaque luminance form.
    SoftwareRenderer sr;
    FrameParams p = makeParams();
    std::vector<Image> pages{solidPage(255, 0, 0)};
    p.spriteAtlasPages = pages;
    p.spriteAtlasVersion = 1;
    SpriteDrawQuad q = makeSprite(0.5f, kShadeCutout, 1.f);
    p.spriteQuads = &q; p.spriteQuadCount = 1;
    sr.renderFrame(p);
    check(sr.framebuffer().pixels[kCenter] == 0xFF363636u,
          "sprite-dm2: Cutout sprite keeps the flat luminance form (0.2126 * 255 = 54)");
  }
  {  // No depth write between blended sprites: a FARTHER blended quad
     // supplied after a nearer one must still blend (it would be
     // depth-rejected if the nearer quad had written depth).
    SoftwareRenderer sr;
    FrameParams p = makeParams();
    std::vector<Image> pages{solidPage(0, 0, 255), solidPage(255, 0, 0)};
    p.spriteAtlasPages = pages;
    p.spriteAtlasVersion = 1;
    SpriteDrawQuad qs[2] = {makeSprite(0.25f, kShadeAlphaBlend, 1.f),
                            makeSprite(0.5f, kShadeAlphaBlend, 1.f)};
    qs[0].page = 0;  // near, blue
    qs[1].page = 1;  // far, red — drawn second, must win (a = 1)
    p.spriteQuads = qs; p.spriteQuadCount = 2;
    sr.renderFrame(p);
    check(sr.framebuffer().pixels[kCenter] == 0xFFFF0000u,
          "sprite-dm2: blended sprites do not depth-write (farther blend still lands)");
  }
  {  // FramePacer: default off; cap clamps; deadline math; overrun free.
    FramePacer pacer;
    check(!pacer.enabled() && pacer.capFps() == 0, "pacer-dm3: frame cap defaults OFF");
    pacer.setCapFps(10000);
    check(pacer.capFps() == 1000, "pacer-dm3: cap clamps to 1000 fps");
    pacer.setCapFps(50);  // 20 ms period
    const auto t0 = FramePacer::Clock::now();
    pacer.beginFrame(t0);
    const auto slept = pacer.endFrameAndWait(t0);
    check(slept >= std::chrono::milliseconds(15) && slept <= std::chrono::milliseconds(25),
          "pacer-dm3: endFrameAndWait targets the 20 ms deadline at 50 fps");
    pacer.beginFrame(t0);
    check(pacer.endFrameAndWait(t0 + std::chrono::seconds(1)) == FramePacer::Clock::duration::zero(),
          "pacer-dm3: an overrun frame sleeps zero (no catch-up debt)");
    FramePacer off;
    off.beginFrame(t0);
    check(off.endFrameAndWait(t0) == FramePacer::Clock::duration::zero(),
          "pacer-dm3: uncapped pacer never sleeps");
  }
}

// PBR mesh chain (BinaryQuad -> registry, phase 6): BinaryQuad.material is an 8-bit
// voxel palette index; it resolves end-to-end through ChunkPalette (voxel index ->
// 16-bit global ID) and MaterialRegistry (global ID -> MaterialDef). This pins the whole
// passthrough: voxels in -> quads out -> defs, with material-aware merging intact.
void testPbrMeshChain() {
  std::printf("render: PBR mesh chain (BinaryQuad.material -> palette -> registry)\n");
  MaterialRegistry reg;
  MaterialDef stone;
  stone.albedo = {0.5f, 0.5f, 0.52f};
  stone.roughness = 0.95f;
  MaterialDef iron;
  iron.albedo = {0.55f, 0.58f, 0.62f};
  iron.roughness = 0.35f;
  iron.metalness = 1.0f;
  MaterialDef torch;
  torch.albedo = {0.6f, 0.45f, 0.25f};
  torch.roughness = 0.8f;
  torch.emissive = {1.0f, 0.62f, 0.25f};
  torch.emissiveIntensity = 2.0f;
  const uint16_t stoneId = reg.registerMaterial("test:stone", stone);
  const uint16_t ironId = reg.registerMaterial("test:iron", iron);
  const uint16_t torchId = reg.registerMaterial("test:torch", torch);
  check(stoneId == 0 && ironId == 1 && torchId == 2,
        "pbr-mesh-chain: fixture materials register at compact IDs 0, 1, 2");
  ChunkPalette pal;
  std::string perr;
  check(pal.build(reg, {"test:stone", "test:iron", "test:torch"}, &perr),
        "pbr-mesh-chain: chunk palette maps voxel indices 0, 1, 2 -> stone, iron, torch");

  const std::array<MaterialDef, 3> expect{stone, iron, torch};
  const std::array<uint16_t, 3> expectGid{stoneId, ironId, torchId};
  auto idx = [](int x, int y, int z) { return size_t((y * 16 + z) * 16 + x); };

  // Scene: a 4-voxel bar (x = 0..3 at y = 0, z = 0) with materials 0, 1, 1, 2.
  std::array<uint8_t, 4096> solid{}, mat{};
  for (int x = 0; x < 4; ++x) solid[idx(x, 0, 0)] = 1;
  mat[idx(0, 0, 0)] = 0;
  mat[idx(1, 0, 0)] = 1;
  mat[idx(2, 0, 0)] = 1;
  mat[idx(3, 0, 0)] = 2;
  std::vector<BinaryQuad> q;
  binaryMeshChunk16(solid.data(), mat.data(), q);

  // End-to-end: every emitted quad resolves q.material -> global ID -> def, and the
  // resolved def is exactly the material of the quad's own voxels.
  bool chainOk = !q.empty();
  for (const BinaryQuad& bq : q) {
    // Material-aware merging: every unit face inside one quad shares the quad's material.
    expandBinaryQuad(bq, [&](int x, int y, int z, int) {
      chainOk = chainOk && mat[idx(x, y, z)] == bq.material;
    });
    const bool inRange = bq.material < 3;
    const uint16_t gid = inRange ? pal.globalId(bq.material) : 0;
    const MaterialDef* d = inRange ? reg.defFor(gid) : nullptr;
    chainOk = chainOk && inRange && gid == expectGid[bq.material] && d != nullptr &&
              d->albedo == expect[bq.material].albedo &&
              d->roughness == expect[bq.material].roughness &&
              d->metalness == expect[bq.material].metalness;
  }
  check(chainOk,
        "pbr-mesh-chain: every quad resolves material index -> palette -> registry to the expected def");

  // Null material array: every quad carries material 0 and resolves to material 0's def.
  std::vector<BinaryQuad> q0;
  binaryMeshChunk16(solid.data(), nullptr, q0);
  ChunkPalette pal0;
  check(pal0.build(reg, {"test:stone"}, &perr),
        "pbr-mesh-chain: single-material chunk palette builds");
  bool nullOk = !q0.empty();
  for (const BinaryQuad& bq : q0) {
    const MaterialDef* d = reg.defFor(pal0.globalId(bq.material));
    nullOk = nullOk && bq.material == 0 && d != nullptr && *d == stone;
  }
  check(nullOk,
        "pbr-mesh-chain: null material array -> all quads carry 0 and resolve to the first def");

  // Material-aware merging: equal materials merge, different materials split.
  auto twoVoxels = [&](uint8_t m0, uint8_t m1) {
    std::array<uint8_t, 4096> s{}, m{};
    s[idx(0, 0, 0)] = 1;
    s[idx(1, 0, 0)] = 1;
    m[idx(0, 0, 0)] = m0;
    m[idx(1, 0, 0)] = m1;
    std::vector<BinaryQuad> qq;
    binaryMeshChunk16(s.data(), m.data(), qq);
    return qq;
  };
  const std::vector<BinaryQuad> split = twoVoxels(0, 1);
  bool splitOk = split.size() == 10;  // 5 exposed faces each; coplanar tops/bottoms stay split
  for (const BinaryQuad& bq : split) splitOk = splitOk && bq.w == 1 && bq.h == 1;
  check(splitOk, "pbr-mesh-chain: different materials block merging (2 voxels -> 10 unit quads)");
  const std::vector<BinaryQuad> merged = twoVoxels(0, 0);
  bool mergeOk = merged.size() == 6;
  int wide = 0;
  for (const BinaryQuad& bq : merged) {
    mergeOk = mergeOk && bq.material == 0;
    if (bq.w == 2 && bq.h == 1) ++wide;
  }
  check(mergeOk && wide == 4,
        "pbr-mesh-chain: equal materials merge across voxels (2 voxels -> 6 quads, 4 of them 2x1)");

  // Palette overflow decision (locked here): build() FAILS past 256 entries - an error,
  // never silent truncation. The 8-bit voxel index addresses exactly 0..255; there is no
  // 257th slot, so the caller must shrink the chunk's material set or fail the mesh job.
  // First-255-wins is rejected: it would silently re-material the overflow voxels.
  {
    ChunkPalette p256, p257;
    std::string e;
    const std::vector<std::string> exact256(256, "test:stone");
    const std::vector<std::string> over257(257, "test:stone");
    check(p256.build(reg, exact256, &e) && p256.size() == 256,
          "pbr-mesh-chain: exactly 256 palette entries is the valid maximum");
    check(!p257.build(reg, over257, &e),
          "pbr-mesh-chain: 257 palette entries fails the palette build (overflow = error)");
  }

  // Integration: real materials from the shipped data file through the mesh chain.
  MaterialRegistry dreg;
  std::string derr;
  check(dreg.loadFile(LITHOKRAFT_MATERIALS_PATH, &derr),
        "pbr-mesh-chain: defaults data file loads");
  ChunkPalette dpal;
  check(dpal.build(dreg, {"lithokraft:stone", "lithokraft:iron", "lithokraft:torch"}, &derr),
        "pbr-mesh-chain: palette builds from data-file string IDs");
  {
    std::array<uint8_t, 4096> s{}, m{};
    s[idx(0, 0, 0)] = 1;
    s[idx(1, 0, 0)] = 1;
    s[idx(2, 0, 0)] = 1;
    m[idx(0, 0, 0)] = 0;
    m[idx(1, 0, 0)] = 1;
    m[idx(2, 0, 0)] = 2;
    std::vector<BinaryQuad> qq;
    binaryMeshChunk16(s.data(), m.data(), qq);
    // Resolve one quad per material and spot-check the defs against the data file's values.
    const MaterialDef* seen[3] = {};
    for (const BinaryQuad& bq : qq) {
      if (bq.material < 3) seen[bq.material] = dreg.defFor(dpal.globalId(bq.material));
    }
    bool fileOk = seen[0] && seen[1] && seen[2];
    fileOk = fileOk && seen[0]->albedo[0] == 0.50f && seen[0]->roughness == 0.95f;
    fileOk = fileOk && seen[1]->metalness == 1.0f && seen[1]->roughness == 0.35f;
    fileOk = fileOk && seen[2]->emissiveIntensity == 2.0f && seen[2]->emissive[0] == 1.0f;
    check(fileOk,
          "pbr-mesh-chain: quads resolve to the data-file defs (stone/iron/torch spot checks)");
  }
}

// The vertex-format seam (worker 5's claim, pinned): PackedVertex is 16 bytes fully packed
// and carries no material slot, so classic-mesh fragments resolve to the 0xFFFF "none"
// marker. The marker is structurally unassignable - registerMaterial refuses once
// ids_.size() >= kMaxMaterials (0xFFFF), so 0xFFFE is the largest valid compact ID and
// defFor(0xFFFF) is always null - and the PBR tier degrades to basic shading per fragment.
void testPbrVertexSeam() {
  std::printf("render: PBR vertex-format seam (PackedVertex has no material slot)\n");
  check(MaterialRegistry::kInvalidMaterialId == 0xFFFF, "pbr-seam: the none-marker is 0xFFFF");
  MaterialRegistry reg;
  MaterialDef iron;
  iron.albedo = {0.55f, 0.58f, 0.62f};
  iron.roughness = 0.35f;
  iron.metalness = 1.0f;
  check(reg.registerMaterial("test:iron", iron) != MaterialRegistry::kInvalidMaterialId,
        "pbr-seam: fixture material registers");
  check(reg.defFor(MaterialRegistry::kInvalidMaterialId) == nullptr,
        "pbr-seam: defFor(0xFFFF) is null even on a populated registry");
  // Classic-mesh fragments start with the none-marker: ShaderVaryings defaults to 0xFFFF
  // (PackedVertex has no slot to stamp, so the software path keeps the default).
  ShaderVaryings v{};
  check(v.materialId == 0xFFFF, "pbr-seam: ShaderVaryings defaults materialId to 0xFFFF");
  // The fragment-level contract: 0xFFFF degrades to exactly the basic-shader pixel
  // (graceful degradation: no crash, no mis-shade).
  PbrFragmentContext ctx;
  ctx.registry = &reg;
  ctx.camX = 0.f;
  ctx.camY = 10.f;
  ctx.camZ = 0.f;
  FragmentInput fin{};  // value-init: varyings default-constructed -> materialId 0xFFFF
  fin.varyings.nx = 0.f;
  fin.varyings.ny = 1.f;
  fin.varyings.nz = 0.f;
  fin.varyings.wx = 0.f;
  fin.varyings.wy = 0.f;
  fin.varyings.wz = 0.f;
  fin.pbr = &ctx;
  check(fin.varyings.materialId == 0xFFFF && pbrFragmentShader(fin) == basicFragmentShader(fin),
        "pbr-seam: fragment with 0xFFFF degrades to exactly the basic-shader pixel");
}


// PBR phase 2, item A: startup wiring. The game resolves the data-file path and the engine
// loads it; a missing file is graceful (false + err, but the built-in default keeps the
// registry usable) — the engine's degrade-loudly init-error convention.
void testPbrStartup() {
  std::printf("render: PBR startup wiring (initMaterialRegistry)\n");
  MaterialRegistry reg;
  std::string err;
  check(initMaterialRegistry(reg, LITHOKRAFT_MATERIALS_PATH, &err), "pbr-startup: data file loads via the startup path");
  check(reg.size() == 17, "pbr-startup: 16 file materials plus the built-in default");
  check(reg.idFor(kDefaultMaterialStringId) == 0, "pbr-startup: built-in default is compact ID 0");
  check(reg.idFor("lithokraft:iron") != MaterialRegistry::kInvalidMaterialId,
        "pbr-startup: file materials present after startup init");
  const MaterialDef* d = reg.defFor(0);
  check(d && d->opaque() && d->metalness == 0.0f && d->roughness == 0.9f,
        "pbr-startup: built-in default is a sane matte dielectric");
  const std::string* dName = reg.stringIdFor(0);
  check(dName && *dName == kDefaultMaterialStringId, "pbr-startup: default string ID round-trips");

  MaterialRegistry missing;
  std::string merr;
  check(!initMaterialRegistry(missing, "/nonexistent-dir/materials.file", &merr) && !merr.empty(),
        "pbr-startup: missing file reports an error");
  check(missing.size() == 1 && missing.idFor(kDefaultMaterialStringId) == 0,
        "pbr-startup: missing file still leaves a usable registry with the default");
  MaterialRegistry noPath;
  check(!initMaterialRegistry(noPath, "", &merr) && noPath.size() == 1,
        "pbr-startup: empty path is graceful too");
}

// PBR phase 2, item B: per-voxel material bytes in chunk storage, MeshInput carriage, and the
// MATS payload — string-ID persistence with registry remap across sessions, unknown-string
// fallback, palette-overflow error, and old saves without the payload.
void testPbrChunkMaterials() {
  std::printf("render: per-voxel chunk material storage + MATS save/load round-trip\n");
  // Session A: the real startup path (default=0, stone=1, ..., iron=13).
  MaterialRegistry regA;
  std::string err;
  check(initMaterialRegistry(regA, LITHOKRAFT_MATERIALS_PATH, &err), "pbr-chunk: session A registry loads");
  // Session B: same strings, different registration order -> different compact IDs.
  MaterialRegistry regB;
  check(regB.registerMaterial(std::string(kDefaultMaterialStringId), defaultMaterialDef()) == 0,
        "pbr-chunk: session B default is compact ID 0");
  const uint16_t bIron = regB.registerMaterial("lithokraft:iron", *regA.defFor(regA.idFor("lithokraft:iron")));
  const uint16_t bStone = regB.registerMaterial("lithokraft:stone", *regA.defFor(regA.idFor("lithokraft:stone")));
  check(bIron == 1 && bStone == 2 && regA.idFor("lithokraft:iron") != bIron,
        "pbr-chunk: session B compact IDs differ from session A");

  // One column: palette [iron, stone], two sub-chunks with material bytes set per voxel.
  ChunkColumn col;
  col.cx = 1; col.cz = -2;
  ChunkPalette pal;
  check(pal.build(regA, {"lithokraft:iron", "lithokraft:stone"}, &err), "pbr-chunk: column palette builds");
  for (size_t i = 0; i < pal.size(); ++i) col.materialPalette[i] = pal.globalId(uint8_t(i));
  col.materialPaletteSize = pal.size();
  for (const int s : {0, 5}) {
    auto sub = std::make_unique<SubChunk>();
    sub->blocks.set(0, fixtureBlocks().stableIdOf(1));  // StableId currency (Infinite Registry)
    sub->nonAir = 1;
    sub->setMaterial(0, 0, 0, 1);     // voxel (0,0,0) -> palette index 1 (stone)
    sub->setMaterial(15, 15, 15, 0);  // voxel (15,15,15) -> palette index 0 (iron)
    check(sub->materialAt(0, 0, 0) == 1 && sub->materialAt(15, 15, 15) == 0 && sub->materialAt(1, 1, 1) == 0,
          "pbr-chunk: set/get material per voxel (unset reads 0)");
    col.subs[size_t(s)] = std::move(sub);
  }

  // Encode with session A's payload, decode with session B's: string IDs stable, compact remapped.
  storage::PayloadRegistry encA;
  encA.setRegistry(&fixtureBlocks());  // encode translates StableId -> save-time BlockId
  encA.add(makeMaterialPayload(regA));
  const storage::Bytes rec = encA.encode(col);
  storage::PayloadRegistry decB;
  decB.add(makeMaterialPayload(regB));
  std::string derr;
  auto col2 = decB.decode(rec, &derr);
  // Two-phase load (same contract as the game): install voxels from the parked scratch.
  const bool remapped = col2 && storage::remapColumn(*col2, identityStableRemap(fixtureBlocks()), &derr);
  check(remapped, "pbr-chunk: remapColumn installs decoded voxels");
  check(col2 != nullptr, "pbr-chunk: save->load round-trip decodes");
  if (col2) {
    check(col2->materialPaletteSize == 2, "pbr-chunk: palette size survives the round-trip");
    check(col2->materialPalette[0] == bIron && col2->materialPalette[1] == bStone,
          "pbr-chunk: string IDs stable across sessions, compact IDs remapped");
    check(col2->materialPalette[0] == regB.idFor("lithokraft:iron"),
          "pbr-chunk: palette resolves through the loading session's registry");
    bool bytesOk = true, blocksOk = true;
    for (const int s : {0, 5}) {
      const auto& sub = col2->subs[size_t(s)];
      bytesOk = bytesOk && sub && sub->materialAt(0, 0, 0) == 1 && sub->materialAt(15, 15, 15) == 0 &&
                sub->materialAt(8, 8, 8) == 0;
      blocksOk = blocksOk && sub && sub->blocks.at(0) == fixtureBlocks().stableIdOf(1);
    }
    check(bytesOk, "pbr-chunk: per-voxel material bytes survive the round-trip");
    check(blocksOk, "pbr-chunk: block data untouched by the material payload");
  }

  // MeshInput carries the bytes to the mesher.
  {
    auto mc = std::make_unique<ChunkColumn>();
    auto msub = std::make_unique<SubChunk>();
    msub->set(3, 5, 7, fixtureBlocks().stableIdOf(kRock));
    msub->setMaterial(3, 5, 7, 2);
    mc->subs[0] = std::move(msub);
    ChunkMap w;
    w.setRegistry(&fixtureBlocks());
    w.insert(std::move(mc));
    const auto snap = snapshotForMeshing(w, fixtureBlocks(), 0, 0, 0, 1, 1);
    check(snap->mat[meshPadIndex(4, 6, 8)] == 2, "pbr-chunk: mesh snapshot carries the material byte");
    check(snap->mat[meshPadIndex(5, 6, 8)] == 0, "pbr-chunk: mesh snapshot defaults unset voxels to 0");
  }

  const storage::PayloadType* pt = decB.find(kMaterialsTag);
  check(pt != nullptr, "pbr-chunk: MATS payload is registered");
  if (pt) {
    // Unknown string IDs on load -> the defined fallback, never garbage.
    storage::ByteWriter w;
    w.u16(1);
    const std::string gone = "test:gone";
    w.u16(uint16_t(gone.size()));
    for (const char ch : gone) w.u8(uint8_t(ch));
    w.u64(1);  // sub-chunk 0 present
    uint8_t zeros[kSubChunkVolume] = {};
    detail::encodeMatBytes(zeros, w);
    ChunkColumn c3;
    auto s3 = std::make_unique<SubChunk>();
    s3->blocks.set(0, fixtureBlocks().stableIdOf(1));
    s3->nonAir = 1;
    c3.subs[0] = std::move(s3);
    check(pt->load(w.out, c3), "pbr-chunk: unknown string ID loads (fallback)");
    check(c3.materialPaletteSize == 1 && c3.materialPalette[0] == regB.idFor(kDefaultMaterialStringId),
          "pbr-chunk: unknown string ID falls back to the built-in default");
    check(c3.subs[0]->materialAt(3, 4, 5) == 0, "pbr-chunk: fallback voxel byte is the fallback index");
    // Palette overflow on load: error, locked (never silent truncation).
    storage::ByteWriter wOver;
    wOver.u16(300);
    ChunkColumn cOver;
    check(!pt->load(wOver.out, cOver), "pbr-chunk: >256 palette entries fail the load");
  }

  // Old saves (no MATS payload at all): defaults, not garbage.
  {
    ChunkColumn oldCol;
    oldCol.cx = 3;
    auto oldSub = std::make_unique<SubChunk>();
    oldSub->blocks.set(7, fixtureBlocks().stableIdOf(2));
    oldSub->nonAir = 1;
    oldCol.subs[2] = std::move(oldSub);
    storage::PayloadRegistry oldReg;  // BLKS only
    oldReg.setRegistry(&fixtureBlocks());
    auto oldLoaded = decB.decode(oldReg.encode(oldCol), &derr);
    check(oldLoaded != nullptr, "pbr-chunk: save without MATS still loads");
    check(oldLoaded && oldLoaded->materialPaletteSize == 0,
          "pbr-chunk: save without MATS gets the default (empty) palette");
    check(oldLoaded && oldLoaded->subs[2]->materialAt(1, 2, 3) == 0,
          "pbr-chunk: save without MATS has zeroed material bytes");
  }
}

// PBR phase 2, item 3: world-side emissive enumeration (ChunkEmissiveProvider, the real
// IEmissiveProvider for the phase-1 seam). Cached scan + explicit refresh; the block-ID
// fallback keeps light-emitting blocks visible while no producer writes material bytes.
void testPbrEmissiveEnumeration() {
  std::printf("render: world-side emissive enumeration (ChunkEmissiveProvider)\n");
  MaterialRegistry reg;
  std::string err;
  check(initMaterialRegistry(reg, LITHOKRAFT_MATERIALS_PATH, &err), "pbr-emissive: registry loads");
  const uint16_t torchId = reg.idFor("lithokraft:torch");
  const uint16_t lavaId = reg.idFor("lithokraft:lava");
  const uint16_t stoneId = reg.idFor("lithokraft:stone");
  const MaterialDef* torchDef = reg.defFor(torchId);
  const MaterialDef* lavaDef = reg.defFor(lavaId);
  check(torchDef && torchDef->emissiveIntensity > 0.f && lavaDef && lavaDef->emissiveIntensity > 0.f,
        "pbr-emissive: torch and lava defs are emissive");

  // Block-ID fallback mapper: every kRock block reads as torch (the all-default case,
  // where no producer has written material bytes yet).
  auto rockIsTorch = [&](StableId sid) -> uint16_t {
    return sid == fixtureBlocks().stableIdOf(kRock) ? torchId : 0;
  };

  // --- Material-byte path: palette [default, torch, lava]; torches via explicit bytes.
  ChunkMap w = worldWithColumns(2, &fixtureBlocks());
  ChunkColumn* col00 = w.column(0, 0);
  ChunkPalette pal;
  check(pal.build(reg, {"lithokraft:torch", "lithokraft:lava"}, &err),
        "pbr-emissive: column palette builds");
  col00->materialPalette[0] = torchId;   // byte 0 -> torch
  col00->materialPalette[1] = lavaId;    // byte 1 -> lava
  col00->materialPaletteSize = 2;
  w.set(2, 64, 3, kRock);
  col00->subs[4]->setMaterial(2, 0, 3, 0);  // y=64 -> sub 4, local (2,0,3): torch
  w.set(5, 65, 6, kRock);
  col00->subs[4]->setMaterial(5, 1, 6, 1);  // local (5,1,6): lava
  w.set(9, 64, 9, kRock);                   // default byte 0 -> torch too (byte 0 IS torch here)
  ChunkEmissiveProvider prov(&reg);
  prov.refresh(w, 0, 0, 1);
  check(prov.candidateCount() == 3, "pbr-emissive: material-byte scan finds all 3 emissives");
  {
    const auto& cs = prov.candidates();
    // Scan order: sy ascending, voxel index ascending: (2,64,3) idx 50, (9,64,9) idx 153,
    // (5,65,6) idx 357.
    const bool torchOk = cs[0].x == 2 && cs[0].y == 64 && cs[0].z == 3 &&
                         cs[0].r == torchDef->emissive[0] && cs[0].intensity == torchDef->emissiveIntensity;
    const bool torch2Ok = cs[1].x == 9 && cs[1].y == 64 && cs[1].z == 9 &&
                          cs[1].intensity == torchDef->emissiveIntensity;
    const bool lavaOk = cs[2].x == 5 && cs[2].y == 65 && cs[2].z == 6 &&
                        cs[2].r == lavaDef->emissive[0] && cs[2].intensity == lavaDef->emissiveIntensity;
    check(torchOk && torch2Ok && lavaOk, "pbr-emissive: positions, colours and intensities match the defs");
    // IEmissiveProvider seam contract: voxel centers, world space, via the sink.
    bool seamOk = false;
    size_t n = 0;
    prov.forEachEmissive([&](float x, float y, float z, float r, float g, float b, float inten) {
      if (n == 0) seamOk = (x == 2.5f && y == 64.5f && z == 3.5f && r == torchDef->emissive[0] &&
                            g == torchDef->emissive[1] && b == torchDef->emissive[2] &&
                            inten == torchDef->emissiveIntensity);
      ++n;
    });
    check(seamOk && n == 3, "pbr-emissive: forEachEmissive yields voxel centers in scan order");
  }

  // --- All-default fallback: empty palette (old save), no bytes written, mapper finds torches.
  ChunkMap w2 = worldWithColumns(2, &fixtureBlocks());
  w2.set(4, 70, 4, kRock);   // column (0,0), sub 4
  w2.set(20, 70, 4, kRock);  // column (1,0), sub 4: across the chunk boundary
  ChunkEmissiveProvider prov2(&reg);
  prov2.refresh(w2, 0, 0, 1);
  check(prov2.candidateCount() == 0, "pbr-emissive: all-default world, no mapper -> no candidates");
  prov2.setBlockMaterial(rockIsTorch);
  check(prov2.needsRefresh(0, 0), "pbr-emissive: installing the mapper marks the cache dirty");
  prov2.refresh(w2, 0, 0, 1);
  check(prov2.candidateCount() == 2, "pbr-emissive: fallback finds torch blocks across the chunk boundary");
  {
    const auto& cs = prov2.candidates();
    check(cs[0].x == 4 && cs[1].x == 20, "pbr-emissive: fallback enumeration order is deterministic");
  }

  // --- Empty world: no columns with blocks -> no candidates, no crash.
  ChunkMap w3 = worldWithColumns(1, &fixtureBlocks());
  ChunkEmissiveProvider prov3(&reg);
  prov3.setBlockMaterial(rockIsTorch);
  prov3.refresh(w3, 0, 0, 1);
  check(prov3.candidateCount() == 0, "pbr-emissive: empty world -> no candidates");

  // --- Explicit bytes win over the fallback: byte says stone (not emissive), mapper says torch.
  ChunkMap w4 = worldWithColumns(1, &fixtureBlocks());
  ChunkColumn* c4 = w4.column(0, 0);
  c4->materialPalette[0] = stoneId;
  c4->materialPaletteSize = 1;
  w4.set(1, 64, 1, kRock);
  c4->subs[4]->setMaterial(1, 0, 1, 0);  // explicit stone: not emissive
  ChunkEmissiveProvider prov4(&reg);
  prov4.setBlockMaterial(rockIsTorch);
  prov4.refresh(w4, 0, 0, 0);
  check(prov4.candidateCount() == 0, "pbr-emissive: explicit non-emissive byte beats the fallback");

  // --- K-nearest through the real LightCollector: 10 torches, K=8, nearest-first.
  ChunkMap w5 = worldWithColumns(1, &fixtureBlocks());
  for (int x = 0; x < 10; ++x) w5.set(x, 64, 8, kRock);
  ChunkEmissiveProvider prov5(&reg);
  prov5.setBlockMaterial(rockIsTorch);
  prov5.refresh(w5, 0, 0, 0);
  check(prov5.candidateCount() == 10, "pbr-emissive: 10 torch voxels enumerated");
  LightCollector collector(8);
  const std::vector<AnalyticLight> lights =
      collector.collect(0.5f, 64.5f, 8.5f, [&](auto&& sink) { prov5.forEachEmissive(sink); });
  bool orderOk = lights.size() == 8;
  for (size_t i = 0; orderOk && i < lights.size(); ++i)
    orderOk = orderOk && lights[i].x == float(i) + 0.5f;  // x=0..7 nearest-first, 8/9 cut
  check(orderOk, "pbr-emissive: K=8 nearest-first ordering through LightCollector");

  // --- Place/break consistency: the game's edit funnel marks dirty, refresh re-scans.
  ChunkEmissiveProvider prov6(&reg);
  prov6.setBlockMaterial(rockIsTorch);
  ChunkMap w6 = worldWithColumns(1, &fixtureBlocks());
  prov6.refresh(w6, 0, 0, 0);
  check(!prov6.needsRefresh(0, 0), "pbr-emissive: fresh cache needs no refresh");
  w6.set(7, 64, 7, kRock);  // place a torch
  check(!prov6.needsRefresh(0, 0), "pbr-emissive: cache is stale-blind until the game marks dirty");
  prov6.markDirty();
  check(prov6.needsRefresh(0, 0), "pbr-emissive: markDirty() from the edit funnel invalidates");
  prov6.refresh(w6, 0, 0, 0);
  const bool placed = prov6.candidateCount() == 1 && prov6.candidates()[0].x == 7;
  w6.set(7, 64, 7, kAir);  // break it
  prov6.markDirty();
  prov6.refresh(w6, 0, 0, 0);
  check(placed && prov6.candidateCount() == 0, "pbr-emissive: place then break updates the enumeration");
  check(prov6.needsRefresh(1, 0), "pbr-emissive: moving the focus chunk needs a refresh");

  // --- Determinism: two refreshes (and a second provider) give identical sequences.
  ChunkEmissiveProvider prov7(&reg);
  prov7.setBlockMaterial(rockIsTorch);
  prov7.refresh(w5, 0, 0, 0);
  auto snap = prov7.candidates();
  prov7.refresh(w5, 0, 0, 0);
  bool same = prov7.candidates().size() == snap.size();
  for (size_t i = 0; same && i < snap.size(); ++i) {
    const auto& a = snap[i];
    const auto& b = prov7.candidates()[i];
    same = same && a.x == b.x && a.y == b.y && a.z == b.z && a.r == b.r && a.g == b.g &&
           a.b == b.b && a.intensity == b.intensity;
  }
  ChunkEmissiveProvider prov8(&reg);
  prov8.setBlockMaterial(rockIsTorch);
  prov8.refresh(w5, 0, 0, 0);
  same = same && prov8.candidates().size() == snap.size();
  for (size_t i = 0; same && i < snap.size(); ++i) same = same && prov8.candidates()[i].x == snap[i].x;
  check(same, "pbr-emissive: enumeration order identical across refreshes and providers");

  // --- Null registry degrades gracefully (no crash, no candidates).
  ChunkEmissiveProvider provNull(nullptr);
  provNull.refresh(w5, 0, 0, 0);
  check(provNull.candidateCount() == 0, "pbr-emissive: null registry -> no candidates, no crash");

  // --- Cost: refresh a denser 3x3 region, report the time (expected: tens of us).
  ChunkMap wCost = worldWithColumns(3, &fixtureBlocks());
  for (int i = 0; i < 200; ++i) wCost.set(i % 48, 64 + (i % 6) * 16, (i / 48) % 48, kRock);
  ChunkEmissiveProvider provCost(&reg);
  provCost.setBlockMaterial(rockIsTorch);
  const auto t0 = std::chrono::steady_clock::now();
  provCost.refresh(wCost, 1, 1, 1);
  const auto t1 = std::chrono::steady_clock::now();
  const long long us =
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  std::printf("  refresh(3x3 columns, %zu candidates) took %lld us\n", provCost.candidateCount(), us);
  check(provCost.candidateCount() == 200, "pbr-emissive: dense region finds every torch");
}

// PBR phase 2, Worker 3: GL backend wiring - material table packing (48-byte std430
// rows), the registry generation counter, BinaryQuad -> PbrQuadVertex expansion, the
// MeshInput::mat + palette end-to-end path (Worker 1's open gap), and headless
// shader-source validation of the attribute/uniform contract.
void testPbrGlWiring() {
  std::printf("render: PBR GL backend wiring (Worker 3)\n");

  // --- 1. Row packing matches the glslang-verified std430 layout (48 bytes/row).
  {
    MaterialRegistry reg;
    MaterialDef iron;
    iron.albedo = {0.8f, 0.81f, 0.82f};
    iron.roughness = 0.35f;
    iron.emissive = {1.0f, 0.5f, 0.25f};
    iron.emissiveIntensity = 2.0f;
    iron.metalness = 1.0f;
    iron.ior = 1.5f;
    iron.alpha = 0.9f;
    check(reg.registerMaterial("test:iron", iron) == 0, "pbr-gl: fixture material registers at ID 0");
    std::vector<PbrMaterialRow> rows;
    packPbrMaterialTable(reg, rows);
    check(rows.size() == 1 && sizeof(PbrMaterialRow) == 48,
          "pbr-gl: one 48-byte row per material (glslang-verified std430)");
    const PbrMaterialRow& r = rows[0];
    check(r.v[kPbrAlbedo] == 0.8f && r.v[kPbrAlbedo + 1] == 0.81f && r.v[kPbrAlbedo + 2] == 0.82f,
          "pbr-gl: albedo at floats 0..2 (byte 0)");
    check(r.v[kPbrRoughness] == 0.35f, "pbr-gl: roughness at float 3 (byte 12)");
    check(r.v[kPbrEmissive] == 1.0f && r.v[kPbrEmissive + 1] == 0.5f && r.v[kPbrEmissive + 2] == 0.25f,
          "pbr-gl: emissive at floats 4..6 (byte 16)");
    check(r.v[kPbrEmissiveIntensity] == 2.0f, "pbr-gl: emissiveIntensity at float 7 (byte 28)");
    check(r.v[kPbrMetalness] == 1.0f, "pbr-gl: metalness at float 8 (byte 32)");
    check(r.v[kPbrIor] == 1.5f, "pbr-gl: ior at float 9 (byte 36)");
    check(r.v[kPbrAlpha] == 0.9f, "pbr-gl: alpha at float 10 (byte 40)");
    check(r.v[11] == 0.f, "pbr-gl: pad float is zero");
    check(kPbrAlbedo * 4 == 0 && kPbrRoughness * 4 == 12 && kPbrEmissive * 4 == 16 &&
              kPbrEmissiveIntensity * 4 == 28 && kPbrMetalness * 4 == 32 &&
              kPbrIor * 4 == 36 && kPbrAlpha * 4 == 40,
          "pbr-gl: float indices match the documented std430 byte offsets");
    MaterialDef stone = defaultMaterialDef();
    stone.albedo = {0.1f, 0.2f, 0.3f};
    check(reg.registerMaterial("test:stone", stone) == 1, "pbr-gl: second material at ID 1");
    packPbrMaterialTable(reg, rows);
    check(rows.size() == 2 && rows[1].v[kPbrAlbedo] == 0.1f && rows[0].v[kPbrAlbedo] == 0.8f,
          "pbr-gl: rows are densely indexed by compact ID");
  }

  // --- 2. Generation counter: the backend's cheap dirty flag.
  {
    MaterialRegistry reg;
    check(reg.generation() == 0, "pbr-gl: fresh registry generation is 0");
    const MaterialDef d = defaultMaterialDef();
    reg.registerMaterial("test:a", d);
    check(reg.generation() == 1, "pbr-gl: generation bumps on new registration");
    reg.registerMaterial("test:a", d);  // idempotent: identical def
    check(reg.generation() == 1, "pbr-gl: idempotent re-registration does not bump");
    MaterialDef conflict = d;
    conflict.roughness = 0.1f;
    check(reg.registerMaterial("test:a", conflict) == MaterialRegistry::kInvalidMaterialId,
          "pbr-gl: conflicting re-registration rejected");
    check(reg.generation() == 1, "pbr-gl: rejected registration does not bump");
    reg.registerMaterial("test:b", d);
    check(reg.generation() == 2, "pbr-gl: generation bumps again on the next new ID");
  }

  // --- 3. Quad expansion: one hand-built quad per direction.
  {
    const int N[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const int T0[3] = {1, 0, 0};  // first tangent axis per face axis (X->Y, Y->X, Z->X)
    const int T1[3] = {2, 2, 1};  // second tangent axis (X->Z, Y->Z, Z->Y)
    const uint16_t palette[3] = {10, 20, 30};
    bool allOk = true;
    for (int dir = 0; dir < 6; ++dir) {
      BinaryQuad q{};
      q.x = 4; q.y = 5; q.z = 6; q.w = 2; q.h = 3;
      q.dir = uint8_t(dir);
      q.material = 1;
      const std::vector<BinaryQuad> quads{q};
      std::vector<PbrQuadVertex> verts;
      expandBinaryQuadsToPbr(quads, palette, 3, 7, 42, verts);
      bool ok = verts.size() == 4;
      const int a = dir / 2, t0 = T0[a], t1 = T1[a];
      const int base[3] = {4, 5, 6};
      int mn0 = 1 << 30, mx0 = -1, mn1 = 1 << 30, mx1 = -1;
      for (const PbrQuadVertex& v : verts) {
        const int c[3] = {v.x / 16, v.y / 16, v.z / 16};
        ok = ok && v.face == uint32_t(dir) && v.material == 20u;
        ok = ok && v.blockLight == 7 && v.skyLight == 42;
        ok = ok && c[a] == base[a];  // every corner lies on the face plane
        mn0 = std::min(mn0, c[t0]); mx0 = std::max(mx0, c[t0]);
        mn1 = std::min(mn1, c[t1]); mx1 = std::max(mx1, c[t1]);
      }
      ok = ok && mn0 == base[t0] && mx0 - mn0 == 2 && mn1 == base[t1] && mx1 - mn1 == 3;
      // Winding: ((v1-v0) x (v2-v0)) . faceNormal > 0 (CCW seen from outside).
      const float ax = float(int(verts[1].x) - int(verts[0].x));
      const float ay = float(int(verts[1].y) - int(verts[0].y));
      const float az = float(int(verts[1].z) - int(verts[0].z));
      const float bx = float(int(verts[2].x) - int(verts[0].x));
      const float by = float(int(verts[2].y) - int(verts[0].y));
      const float bz = float(int(verts[2].z) - int(verts[0].z));
      const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
      ok = ok && (nx * N[dir][0] + ny * N[dir][1] + nz * N[dir][2]) > 0;
      if (!ok) std::printf("  dir %d expansion mismatch\n", dir);
      allOk = allOk && ok;
    }
    check(allOk, "pbr-gl: expansion geometry, winding, material and light for all 6 dirs");
    // Out-of-range byte and null palette resolve to material 0 (built-in default).
    BinaryQuad q{};
    q.dir = 2;
    q.material = 9;
    std::vector<PbrQuadVertex> v1, v2;
    expandBinaryQuadsToPbr(std::vector<BinaryQuad>{q}, palette, 3, 0, 0, v1);
    expandBinaryQuadsToPbr(std::vector<BinaryQuad>{q}, nullptr, 0, 0, 0, v2);
    check(v1.size() == 4 && v1[0].material == 0 && v2.size() == 4 && v2[0].material == 0,
          "pbr-gl: out-of-range byte and null palette resolve to material 0");
    // Trim at kMaxQuadsPerMesh like the classic mesher.
    std::vector<BinaryQuad> many(size_t(kMaxQuadsPerMesh) + 5);
    for (auto& qq : many) { qq.dir = 2; qq.w = 1; qq.h = 1; }
    std::vector<PbrQuadVertex> trimmed;
    expandBinaryQuadsToPbr(many, palette, 3, 0, 0, trimmed);
    check(trimmed.size() == size_t(kMaxQuadsPerMesh) * 4,
          "pbr-gl: quad count trims at kMaxQuadsPerMesh");
    // Deterministic.
    const std::vector<BinaryQuad> quads{q};
    std::vector<PbrQuadVertex> d1, d2;
    expandBinaryQuadsToPbr(quads, palette, 3, 7, 42, d1);
    expandBinaryQuadsToPbr(quads, palette, 3, 7, 42, d2);
    check(d1.size() == d2.size() &&
              std::memcmp(d1.data(), d2.data(), d1.size() * sizeof(PbrQuadVertex)) == 0,
          "pbr-gl: expansion is deterministic");
  }

  // --- 4. End-to-end (Worker 1's gap): mat[] + palette ride the snapshot into
  // binaryMeshSubChunk, and the expansion resolves bytes to compact IDs.
  {
    MaterialRegistry reg;
    std::string err;
    check(initMaterialRegistry(reg, LITHOKRAFT_MATERIALS_PATH, &err), "pbr-gl: registry loads");
    const uint16_t torchId = reg.idFor("lithokraft:torch");
    const uint16_t stoneId = reg.idFor("lithokraft:stone");
    check(torchId != MaterialRegistry::kInvalidMaterialId, "pbr-gl: torch registered");
    const BlockRegistry& breg = fixtureBlocks();
    ChunkMap w = worldWithColumns(1, &breg);
    ChunkColumn* col = w.column(0, 0);
    col->materialPalette[0] = stoneId;
    col->materialPalette[1] = torchId;
    col->materialPaletteSize = 2;
    w.set(8, 8, 8, kRock);
    col->subs[0]->setMaterial(8, 8, 8, 1);  // torch byte on the rock voxel
    const auto snap = snapshotForMeshing(w, breg, 0, 0, 0, 1, 1, kAir);
    check(snap->matPaletteSize == 2 && snap->matPalette[1] == torchId,
          "pbr-gl: column palette rides the snapshot");
    const BlockMaterials mats(breg.size());
    std::vector<BinaryQuad> quads;
    uint8_t mat16[16 * 16 * 16];
    extractMaterial16(*snap, mat16);  // binaryMeshSubChunk wants the dense 16^3 interior
    binaryMeshSubChunk(*snap, MeshContext{breg, mats}, mat16, quads);
    check(!quads.empty(), "pbr-gl: binary mesher emits quads for the rock cube");
    bool allTorchByte = true;
    for (const BinaryQuad& bq : quads) allTorchByte = allTorchByte && bq.material == 1;
    check(allTorchByte, "pbr-gl: every quad carries the torch material byte");
    std::vector<PbrQuadVertex> verts;
    expandBinaryQuadsToPbr(quads, snap->matPalette, snap->matPaletteSize, 0, 64, verts);
    bool allTorchId = !verts.empty();
    for (const PbrQuadVertex& v : verts) allTorchId = allTorchId && v.material == torchId;
    check(allTorchId, "pbr-gl: material bytes resolve to the torch compact ID end-to-end");
  }

  // --- 5. Headless shader-source validation (no GL context): the attribute/uniform
  // contract the backend relies on must be present in the sources it embeds.
  {
    const std::string matsPath = LITHOKRAFT_MATERIALS_PATH;
    const std::string tail = "materials/lithokraft.materials";
    const std::string shaderDir = matsPath.substr(0, matsPath.size() - tail.size()) + "shaders/";
    auto read = [&](const std::string& name) {
      std::ifstream f(shaderDir + name, std::ios::binary);
      std::stringstream ss;
      ss << f.rdbuf();
      return ss.str();
    };
    const std::string vert = read("chunk_pbr.vert");
    const std::string frag = read("chunk_pbr.frag");
    const std::string lib = read("pbr_shading.glsl");
    check(!vert.empty() && !frag.empty() && !lib.empty(), "pbr-gl: shader sources readable");
    size_t inc = 0, pos = 0;
    while ((pos = frag.find("#include \"pbr_shading.glsl\"", pos)) != std::string::npos) { ++inc; pos += 10; }
    check(inc == 1, "pbr-gl: chunk_pbr.frag includes pbr_shading.glsl once");
    check(vert.find("layout(location = 0) in uvec3 aPos") != std::string::npos &&
              vert.find("layout(location = 1) in uint aFace") != std::string::npos &&
              vert.find("layout(location = 2) in uint aMaterial") != std::string::npos &&
              vert.find("layout(location = 3) in uvec2 aLight") != std::string::npos,
          "pbr-gl: vertex attributes match the PbrQuadVertex layout");
    check(frag.find("uniform samplerBuffer uPbrMaterials") != std::string::npos &&
              frag.find("uniform vec4 uPbrLightPos[8]") != std::string::npos &&
              frag.find("uniform vec3 uPbrLightColor[8]") != std::string::npos &&
              frag.find("uniform int uPbrLightCount") != std::string::npos &&
              frag.find("uniform int uPbrTier") != std::string::npos,
          "pbr-gl: fragment uniforms match the backend's uploads");
    check(frag.find("uniform sampler2D uSceneColor") != std::string::npos &&
              frag.find("uniform vec2 uSceneSize") != std::string::npos &&
              frag.find("uniform mat4 uSceneVP") != std::string::npos &&
              frag.find("uniform int uRefractionPass") != std::string::npos,
          "pbr-gl: TrueRefraction scene-color uniforms present (uSceneColor contract kept)");
    check(frag.find("vec3(0.0) * refr") == std::string::npos &&
              frag.find("pbrRefractSample(") != std::string::npos,
          "pbr-gl: tier-3 black stub replaced by the real refraction resolve");
    check(lib.find("pbrSnellRefract") != std::string::npos &&
              lib.find("pbrRefractionUvOffset") != std::string::npos &&
              lib.find("pbrRefractSample") != std::string::npos &&
              lib.find("pbrSrgbToLinear") != std::string::npos &&
              lib.find("pbrSpecularBrdf") != std::string::npos,
          "pbr-gl: pbr_shading.glsl carries the refraction helpers");
    check(frag.find("* 12") != std::string::npos && kPbrRowFloats == 12,
          "pbr-gl: TBO unpack stride matches packPbrMaterialRow");
    check(int(PbrTransparencyTier::Simple) == 0 && int(PbrTransparencyTier::ScreenDoor) == 1 &&
              int(PbrTransparencyTier::PbrReflections) == 2 &&
              int(PbrTransparencyTier::TrueRefraction) == 3,
          "pbr-gl: shader tier values match the PbrTransparencyTier order");
    check(lib.find("48 bytes") != std::string::npos,
          "pbr-gl: pbr_shading.glsl documents the corrected 48-byte row");
  }
}

void testPbrTrueRefraction() {
  std::printf("render: PBR TrueRefraction (Ultra) scene-color path (Worker 4)\n");
  auto near = [](float a, float b, float eps) { return std::fabs(a - b) <= eps; };
  auto near3 = [&](const std::array<float, 3>& a, const std::array<float, 3>& b, float eps) {
    return near(a[0], b[0], eps) && near(a[1], b[1], eps) && near(a[2], b[2], eps);
  };

  // --- 1. Snell known answer: air -> glass (n = 1.5) at 45 degrees incidence.
  {
    const float s = 0.70710678118f;  // sin/cos 45 deg
    bool tir = true;
    const std::array<float, 3> refr =
        snellRefract({s, s, 0.f}, {0.f, 1.f, 0.f}, 1.5f, &tir);
    // sinT = sin(45)/1.5 -> T = 28.1255 deg: (-sinT, -cosT, 0), unit length.
    check(!tir && near3(refr, {-0.47140452f, -0.88191710f, 0.f}, 1e-5f),
          "pbr-refraction: air->glass n=1.5 at 45 deg refracts to 28.13 deg");
    const float len = std::sqrt(refr[0] * refr[0] + refr[1] * refr[1] + refr[2] * refr[2]);
    check(near(len, 1.f, 1e-6f), "pbr-refraction: refracted ray is unit length");
  }

  // --- 2. Normal incidence goes straight through.
  {
    bool tir = true;
    const std::array<float, 3> refr = snellRefract({0.f, 1.f, 0.f}, {0.f, 1.f, 0.f}, 1.5f, &tir);
    check(!tir && near3(refr, {0.f, -1.f, 0.f}, 1e-6f),
          "pbr-refraction: normal incidence transmits straight through");
  }

  // --- 3. TIR past the critical angle (glass -> air exit case, eta = 1.5).
  {
    // Critical angle for 1.5 -> 1 is asin(1/1.5) = 41.81 deg; 60 deg is past it.
    const float s = 0.86602540f;  // sin 60 deg
    bool tir = false;
    const std::array<float, 3> dir =
        snellRefractEta({s, 0.5f, 0.f}, {0.f, 1.f, 0.f}, 1.5f, &tir);
    check(tir && near3(dir, {-s, 0.5f, 0.f}, 1e-5f),
          "pbr-refraction: TIR past the critical angle mirror-reflects the ray");
  }

  // --- 4. The air-entry model (eta < 1) can never TIR, even at grazing incidence.
  {
    bool tir = true;
    snellRefract({0.999999f, 0.001f, 0.f}, {0.f, 1.f, 0.f}, 1.5f, &tir);
    check(!tir, "pbr-refraction: air-entry (eta < 1) never hits TIR");
  }

  // --- 5. Screen-space UV offset: known answers under an identity view-projection.
  {
    const std::array<float, 16> identity{1.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f,
                                         0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
    // Ray along +x from the origin: surface uv (0.5, 0.5), probe (0.1,0,0) -> (0.55, 0.5).
    const std::array<float, 2> off =
        refractionScreenOffset({0.f, 0.f, 0.f}, {0.1f, 0.f, 0.f}, identity, 800.f, 600.f);
    check(near(off[0], 0.05f, 1e-6f) && near(off[1], 0.f, 1e-6f),
          "pbr-refraction: uv offset is the projected probe delta (0.05, 0)");
    const std::array<float, 2> offY =
        refractionScreenOffset({0.f, 0.f, 0.f}, {0.f, 0.1f, 0.f}, identity, 800.f, 600.f);
    check(near(offY[0], 0.f, 1e-6f) && near(offY[1], 0.05f, 1e-6f),
          "pbr-refraction: uv offset tracks the y axis too");
    // Long ray: raw 400 px offset clamps to the 96 px cap.
    const std::array<float, 2> clamped =
        refractionScreenOffset({0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, identity, 800.f, 600.f);
    check(near(clamped[0], 96.f / 800.f, 1e-6f) && near(clamped[1], 0.f, 1e-6f),
          "pbr-refraction: screen-space offset clamps at 96 px");
    // Degenerate projection (w <= 0): no offset invented.
    const std::array<float, 16> zero{0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f,
                                     0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f};
    const std::array<float, 2> degen =
        refractionScreenOffset({0.f, 0.f, 0.f}, {0.1f, 0.f, 0.f}, zero, 800.f, 600.f);
    check(degen[0] == 0.f && degen[1] == 0.f,
          "pbr-refraction: behind-camera projection yields zero offset");
  }

  // --- 6. sRGB -> linear transfer (inverse of pbrLinearToSrgb).
  {
    check(srgbToLinear(0.f) == 0.f && near(srgbToLinear(1.f), 1.f, 1e-6f),
          "pbr-refraction: srgbToLinear pins 0 and 1");
    check(near(srgbToLinear(0.5f), 0.21404114f, 1e-4f),
          "pbr-refraction: srgbToLinear(0.5) = 0.21404 (known answer)");
    check(near(srgbToLinear(pbrLinearToSrgb(0.37f)), 0.37f, 1e-5f),
          "pbr-refraction: srgbToLinear inverts pbrLinearToSrgb");
  }

  // --- 7. Fresnel-split resolve: known answer.
  {
    // F0 = 0.04, nDotV = 1 -> F = 0.04 exactly (Schlick at normal incidence).
    const std::array<float, 3> out = resolveTrueRefraction(
        {0.04f, 0.04f, 0.04f}, 1.0f, {0.02f, 0.03f, 0.04f}, {0.5f, 0.5f, 0.5f}, {0.01f, 0.f, 0.f});
    check(near3(out, {0.51f, 0.51f, 0.52f}, 1e-5f),
          "pbr-refraction: resolve = spec + (1-F)*transmitted + emissive");
  }

  // --- 8. Energy bound on the Fresnel split: weights F + (1 - F) = 1 by construction.
  {
    float worst = 0.f;
    for (float f0 : {0.04f, 0.5f})
      for (float nDotV : {0.2f, 0.6f, 1.0f})
        for (float t : {0.f, 0.5f, 1.f}) {
          // Worst case: unit-white mirror (specular == F exactly) over a unit sample.
          const std::array<float, 3> F = CookTorrance::fresnelSchlick({f0, f0, f0}, nDotV);
          const std::array<float, 3> out = resolveTrueRefraction(
              {f0, f0, f0}, nDotV, {F[0], F[1], F[2]}, {t, t, t}, {0.f, 0.f, 0.f});
          for (float c : out) worst = std::max(worst, c);
        }
    check(worst <= 1.f + 1e-5f,
          "pbr-refraction: fresnel split never exceeds 1 (no energy creation)");
  }

  // --- 9. Two-pass plumbing predicate (mirrors the uRefractionPass discard logic).
  {
    check(refractionPassKeepsFragment(1.0f, 0) && !refractionPassKeepsFragment(0.5f, 0),
          "pbr-refraction: pass 0 (scene build) keeps opaque fragments only");
    check(!refractionPassKeepsFragment(1.0f, 1) && refractionPassKeepsFragment(0.5f, 1),
          "pbr-refraction: pass 1 (resolve) keeps refractive fragments only");
    check(!refractionPassKeepsFragment(0.999f, 0) && refractionPassKeepsFragment(0.999f, 1),
          "pbr-refraction: alpha < 1 always refracts (no opaque leak into the copy)");
  }

  // --- 10. IRenderer::setPbrTier contract: default no-op is safe; an override
  // receives the value (this is the headless half of the UI -> settings -> renderer
  // chain; the GL33 backend stores it in pbrTier_).
  {
    struct TexArray final : ITextureArray {
      bool upload(const TextureRegistry&) override { return true; }
      int layerCount() const override { return 0; }
      int tileSize() const override { return 0; }
    };
    // Shared stub body: every IRenderer pure virtual except setPbrTier.
    struct TierRecorderBase : IRenderer {
      const char* name() const override { return "tier-recorder"; }
      std::string deviceInfo() const override { return "?"; }
      ITextureArray& textureArray() override { return tex; }
      void setMaterials(const BlockMaterials&) override {}
      int loadShaders(const ShaderLibrary&) override { return 0; }
      void uploadChunkMesh(const ChunkMesh&) override {}
      void uploadEntityMesh(const std::vector<PackedVertex>&) override {}
      void clearChunkMeshes() override {}
      void renderFrame(const FrameParams&) override {}
      RenderStats stats() const override { return RenderStats{}; }
      TexArray tex;
    };
    struct TierRecorder final : TierRecorderBase {
      PbrTransparencyTier seen = PbrTransparencyTier::Simple;
      bool called = false;
      void setPbrTier(PbrTransparencyTier t) override {
        seen = t;
        called = true;
      }
    };
    struct TierNoop final : TierRecorderBase {
      // Inherits the default setPbrTier no-op: a backend without a PBR path.
    };
    TierRecorder rec;
    rec.setPbrTier(PbrTransparencyTier::TrueRefraction);
    check(rec.called && rec.seen == PbrTransparencyTier::TrueRefraction,
          "pbr-refraction: setPbrTier reaches the renderer (Ultra tier recorded)");
    TierNoop noop;
    noop.setPbrTier(PbrTransparencyTier::TrueRefraction);  // default no-op: must not crash
    check(true, "pbr-refraction: default setPbrTier no-op is safe (no-PBR backend)");
  }
}

void testFrustumMargin() {
  std::printf("render: frustum margin culling (JD 2026-10-11)\n");
  // Camera at origin looking down -Z, 90-degree FOV, square aspect.
  // At depth |z| the frustum half-width is |z| (tan(45 deg) = 1).
  const float kHalfPi = 3.14159265358979323846f * 0.5f;
  const Mat4 vp = mul(perspective(kHalfPi, 1.f, 0.1f, 1000.f), lookDir({0, 0, 0}, {0, 0, -1}));
  const Vec3 cam{0, 0, 0};
  auto box = [](float cx, float cy, float cz) {
    return std::pair<Vec3, Vec3>{{cx - 8.f, cy - 8.f, cz - 8.f}, {cx + 8.f, cy + 8.f, cz + 8.f}};
  };
  {  // Dead ahead: inside under both.
    auto [mn, mx] = box(0, 0, -100);
    check(aabbInFrustum(vp, mn, mx) && aabbInFrustumMargin(vp, mn, mx, cam, 5.f),
          "frustum margin: centered chunk renders under exact and margin");
  }
  {  // x in [132,148] at z in [-108,-92]: every point has x > -z (max -z = 108
     // < min x = 132), so the exact test culls; 5 deg of margin reinstates it.
    auto [mn, mx] = box(140, 0, -100);
    check(!aabbInFrustum(vp, mn, mx) && aabbInFrustumMargin(vp, mn, mx, cam, 5.f),
          "frustum margin: chunk just outside exact frustum renders inside 5 deg margin");
  }
  {  // Far outside: culled under both.
    auto [mn, mx] = box(300, 0, -100);
    check(!aabbInFrustum(vp, mn, mx) && !aabbInFrustumMargin(vp, mn, mx, cam, 5.f),
          "frustum margin: chunk outside the margin still culled");
  }
  {  // Behind the camera: culled even at max margin (margin never resurrects it).
    auto [mn, mx] = box(0, 0, 50);
    check(!aabbInFrustum(vp, mn, mx) && !aabbInFrustumMargin(vp, mn, mx, cam, 15.f),
          "frustum margin: behind-camera chunk culled even at max margin");
  }
  {  // margin <= 0 degenerates to the exact test.
    auto [mn, mx] = box(140, 0, -100);
    check(aabbInFrustumMargin(vp, mn, mx, cam, 0.f) == aabbInFrustum(vp, mn, mx) &&
          aabbInFrustumMargin(vp, mn, mx, cam, -3.f) == aabbInFrustum(vp, mn, mx),
          "frustum margin: margin <= 0 is the exact test");
  }
  {  // Monotonicity: a wider margin never culls what a narrower margin kept.
    auto [mn, mx] = box(140, 0, -100);
    const bool m5 = aabbInFrustumMargin(vp, mn, mx, cam, 5.f);
    const bool m15 = aabbInFrustumMargin(vp, mn, mx, cam, 15.f);
    check(m5 && m15, "frustum margin: wider margin keeps what the narrower margin kept");
  }
}

void testCometTail() {
  std::printf("render: comet tail prototype (JD 2026-10-11)\n");
  const float kHalfPi = 3.14159265358979323846f * 0.5f;
  const Mat4 vpNegZ = mul(perspective(kHalfPi, 1.f, 0.1f, 1000.f), lookDir({0, 0, 0}, {0, 0, -1}));
  const Mat4 vpPosZ = mul(perspective(kHalfPi, 1.f, 0.1f, 1000.f), lookDir({0, 0, 0}, {0, 0, 1}));
  const Vec3 cam{0, 0, 0};
  auto box = [](float cx, float cy, float cz) {
    return std::pair<Vec3, Vec3>{{cx - 8.f, cy - 8.f, cz - 8.f}, {cx + 8.f, cy + 8.f, cz + 8.f}};
  };
  {
    // Union membership: chunk behind the CURRENT camera but visible 1 frame
    // ago stays submitted.
    CometTail ct;
    ct.configure(8);
    ct.beginFrame(vpNegZ);  // arms; history still empty
    check(ct.stored() == 0, "comet tail: first frame arms, history empty");
    ct.beginFrame(vpPosZ);  // history = {vpNegZ}
    check(ct.stored() == 1, "comet tail: second frame stores one frustum");
    auto [mn, mx] = box(0, 0, -100);  // behind current (+z) view, was visible
    check(ct.visible(mn, mx, cam, 5.f), "comet tail: chunk visible one frame ago stays submitted");
    auto [mn2, mx2] = box(300, 0, 100);  // never in any frustum
    check(!ct.visible(mn2, mx2, cam, 5.f), "comet tail: chunk never seen stays culled");
  }
  {
    // Expiry: entries older than N frames drop out.
    CometTail ct;
    ct.configure(2);
    ct.beginFrame(vpNegZ);
    ct.beginFrame(vpPosZ);  // history = {negZ}
    ct.beginFrame(vpPosZ);  // history = {negZ, posZ}
    check(ct.stored() == 2, "comet tail: history capped at N frames");
    auto [mn, mx] = box(0, 0, -100);
    check(ct.visible(mn, mx, cam, 5.f), "comet tail: chunk visible within window");
    ct.beginFrame(vpPosZ);  // history = {posZ, posZ}; negZ expired
    check(ct.stored() == 2 && !ct.visible(mn, mx, cam, 5.f),
          "comet tail: entries older than N frames expire");
  }
  {
    // Off (0 frames): nothing remembered.
    CometTail ct;
    ct.configure(0);
    ct.beginFrame(vpNegZ);
    ct.beginFrame(vpPosZ);
    auto [mn, mx] = box(0, 0, -100);
    check(!ct.visible(mn, mx, cam, 5.f), "comet tail: 0 frames disables the union");
  }
}

void testVelocityMargin() {
  std::printf("render: velocity-adaptive margin prototype (JD 2026-10-11)\n");
  {
    // Tracker: 1 rad/s yaw converges; still camera reads ~0.
    VelocityTracker vt;
    vt.update(0.f, 0.f, 0.0);  // arms
    for (int i = 1; i <= 10; ++i) vt.update(0.1f * float(i), 0.f, 0.1 * double(i));
    check(vt.yawVel() > 0.9f && vt.yawVel() < 1.1f && std::fabs(vt.pitchVel()) < 0.05f,
          "velocity tracker: 1 rad/s yaw converges, pitch stays ~0");
    VelocityTracker still;
    still.update(0.3f, -0.2f, 0.0);
    still.update(0.3f, -0.2f, 1.0);
    check(std::fabs(still.yawVel()) < 1e-4f && std::fabs(still.pitchVel()) < 1e-4f,
          "velocity tracker: still camera reads ~0 velocity");
  }
  {
    // Directional margin: turning right (yawVel > 0) widens the margin toward
    // camRight (+z at yaw 0) and narrows it behind. Still camera = base.
    const Vec3 right = cullCamRight(0.f, 0.f);
    const Vec3 up = cullCamUp(0.f, 0.f);
    check(std::fabs(right.z - 1.f) < 1e-5f && std::fabs(up.y - 1.f) < 1e-5f,
          "velocity margin: camera basis sane at yaw 0");
    const Vec3 toRight{0.f, 0.f, 1.f}, toLeft{0.f, 0.f, -1.f}, ahead{1.f, 0.f, 0.f};
    const float lead = directionalMarginDeg(5.f, 1.f, 0.f, 2.f, toRight, right, up);
    const float trail = directionalMarginDeg(5.f, 1.f, 0.f, 2.f, toLeft, right, up);
    const float side = directionalMarginDeg(5.f, 1.f, 0.f, 2.f, ahead, right, up);
    check(std::fabs(lead - 7.f) < 1e-4f && std::fabs(trail - 3.f) < 1e-4f &&
          std::fabs(side - 5.f) < 1e-4f,
          "velocity margin: leading side widened, trailing narrowed, perpendicular = base");
    const float calm = directionalMarginDeg(5.f, 0.f, 0.f, 2.f, toRight, right, up);
    check(std::fabs(calm - 5.f) < 1e-5f, "velocity margin: still camera degrades to base margin");
    const float huge = directionalMarginDeg(5.f, 100.f, 0.f, 2.f, toRight, right, up);
    const float neg = directionalMarginDeg(5.f, -100.f, 0.f, 2.f, toRight, right, up);
    check(huge == 30.f && neg == 0.f, "velocity margin: clamped to [0, 30]");
  }
}

void testTwoTier() {
  std::printf("render: two-tier margin prototype (JD 2026-10-11)\n");
  const float kHalfPi = 3.14159265358979323846f * 0.5f;
  const Mat4 vp = mul(perspective(kHalfPi, 1.f, 0.1f, 1000.f), lookDir({0, 0, 0}, {0, 0, -1}));
  const Vec3 cam{0, 0, 0};
  auto box = [](float cx, float cy, float cz) {
    return std::pair<Vec3, Vec3>{{cx - 8.f, cy - 8.f, cz - 8.f}, {cx + 8.f, cy + 8.f, cz + 8.f}};
  };
  {
    auto [mn, mx] = box(140, 0, -100);  // inside the 5 deg inner margin
    check(classifyTwoTier(vp, mn, mx, cam, 5.f, 15.f) == CullDecision::Full,
          "two-tier: chunk inside inner margin is Full");
  }
  {
    // (140,0,-100) fails the exact test but 10 deg reinstates it.
    auto [mn, mx] = box(140, 0, -100);
    check(classifyTwoTier(vp, mn, mx, cam, 0.f, 10.f) == CullDecision::MeshOnly,
          "two-tier: chunk between inner and outer is MeshOnly");
  }
  {
    // (300,0,-100) is outside 15 deg but inside 30 deg (hand-verified).
    auto [mn, mx] = box(300, 0, -100);
    check(classifyTwoTier(vp, mn, mx, cam, 5.f, 15.f) == CullDecision::Cull,
          "two-tier: chunk beyond outer margin is Cull");
    check(classifyTwoTier(vp, mn, mx, cam, 5.f, 30.f) == CullDecision::MeshOnly,
          "two-tier: wider outer tier reinstates it as MeshOnly");
  }
  {
    auto [mn, mx] = box(0, 0, 50);  // behind the camera
    check(classifyTwoTier(vp, mn, mx, cam, 5.f, 30.f) == CullDecision::Cull,
          "two-tier: behind-camera chunk never resurrected");
  }
  {
    // outer <= inner: MeshOnly is unreachable by construction.
    auto [mn, mx] = box(200, 0, -100);
    check(classifyTwoTier(vp, mn, mx, cam, 5.f, 5.f) == CullDecision::Cull,
          "two-tier: outer <= inner yields no MeshOnly");
  }
  {
    // Composed state: all prototypes on. Chunk behind current view but in
    // the comet history is kept (Full, cometKept = 1).
    CullProtoState st;
    CullProtoConfig cfg;
    cfg.cometTailFrames = 8;
    cfg.velocityMargin = true;
    cfg.velocityMarginGain = 2.f;
    cfg.twoTier = true;
    cfg.twoTierOuterDeg = 10.f;
    st.configure(cfg);
    const Mat4 vpPosZ = mul(perspective(kHalfPi, 1.f, 0.1f, 1000.f), lookDir({0, 0, 0}, {0, 0, 1}));
    st.beginFrame(vp, 0.f, 0.f, 0.0);
    st.beginFrame(vpPosZ, 0.f, 0.f, 0.016);
    auto [mn, mx] = box(0, 0, -100);
    const CullDecision d = st.decide(vpPosZ, mn, mx, cam, 5.f);
    const CullProtoStats s = st.stats();
    check(d == CullDecision::Full && s.cometKept == 1,
          "prototype state: history-visible chunk kept as Full via comet tail");
    auto [mn2, mx2] = box(0, 0, 100);  // dead ahead of current view
    check(st.decide(vpPosZ, mn2, mx2, cam, 5.f) == CullDecision::Full,
          "prototype state: current-view chunk is Full");
    auto [mn3, mx3] = box(400, 0, 100);  // far outside everything
    check(st.decide(vpPosZ, mn3, mx3, cam, 5.f) == CullDecision::Cull,
          "prototype state: far-outside chunk is Cull");
  }
  {
    // All prototypes OFF: decide() is exactly the base margin test.
    CullProtoState st;  // default config: everything off
    const Mat4 vpPosZ = mul(perspective(kHalfPi, 1.f, 0.1f, 1000.f), lookDir({0, 0, 0}, {0, 0, 1}));
    st.beginFrame(vpPosZ, 0.f, 0.f, 0.0);
    auto [mn, mx] = box(0, 0, 100);
    auto [mn2, mx2] = box(400, 0, 100);
    check(st.decide(vpPosZ, mn, mx, cam, 5.f) == CullDecision::Full &&
          st.decide(vpPosZ, mn2, mx2, cam, 5.f) == CullDecision::Cull,
          "prototype state: all off degrades exactly to base margin culling");
  }
}

void testGroundPulse() {
  std::printf("render: wafer ground pulse (time/chunk/biome pure function)\n");
  // Determinism: same chunk + time + params -> identical output, no state.
  const GroundPulseParams tf = groundPulseParams(GroundPulseBiome::TraceForest);
  const auto a = groundPulseEmissive(1.25f, 3, -7, tf);
  const auto b = groundPulseEmissive(1.25f, 3, -7, tf);
  check(a == b, "ground pulse: same chunk+time+params -> same intensity (deterministic)");
  // Per-chunk phase: neighbouring chunks (usually) disagree.
  int samePhase = 0;
  for (int i = 0; i < 16; ++i)
    if (groundPulsePhase01(i, 0) == groundPulsePhase01(0, 0)) ++samePhase;
  check(samePhase < 16, "ground pulse: chunk phase hash varies across chunks");
  check(groundPulsePhase01(5, 9) >= 0.f && groundPulsePhase01(5, 9) < 1.f, "ground pulse: phase in [0,1)");
  // Dead Zones: exactly zero, always (None params are the zero vector).
  const GroundPulseParams none = groundPulseParams(GroundPulseBiome::None);
  check(none.amplitude == 0.f && none.freqHz == 0.f && none.tintR == 0.f,
        "ground pulse: Dead Zones contribute zero params (unpowered)");
  const auto dead = groundPulseEmissive(123.456f, 3, -7, GroundPulseBiome::None);
  check(dead[0] == 0.f && dead[1] == 0.f && dead[2] == 0.f,
        "ground pulse: Dead Zones -> zero emissive at any time");
  // Blending: 60% Trace Forest + 40% Dicing Plains = the 60/40 param mix.
  float w[kGroundPulseBiomeCount]{};
  w[size_t(GroundPulseBiome::TraceForest)] = 0.6f;
  w[size_t(GroundPulseBiome::DicingPlains)] = 0.4f;
  const GroundPulseParams blend = blendPulseParams(w);
  const GroundPulseParams dp = groundPulseParams(GroundPulseBiome::DicingPlains);
  const bool mixOk = std::fabs(blend.freqHz - (0.6f * tf.freqHz + 0.4f * dp.freqHz)) < 1e-6f &&
                     std::fabs(blend.amplitude - (0.6f * tf.amplitude + 0.4f * dp.amplitude)) < 1e-6f &&
                     std::fabs(blend.tintB - (0.6f * tf.tintB + 0.4f * dp.tintB)) < 1e-6f &&
                     std::fabs(blend.sharpness - (0.6f * tf.sharpness + 0.4f * dp.sharpness)) < 1e-6f;
  check(mixOk, "ground pulse: 60/40 Trace Forest/Dicing Plains blends params by weight");
  // Renormalization (matches the procgen sibling's blendLerp): unnormalized
  // weights give the same relative blend.
  float wu[kGroundPulseBiomeCount]{};
  wu[size_t(GroundPulseBiome::TraceForest)] = 0.3f;
  wu[size_t(GroundPulseBiome::DicingPlains)] = 0.2f;
  const GroundPulseParams unnorm = blendPulseParams(wu);
  const bool renormOk = std::fabs(unnorm.freqHz - blend.freqHz) < 1e-6f &&
                        std::fabs(unnorm.amplitude - blend.amplitude) < 1e-6f;
  check(renormOk, "ground pulse: blend renormalizes by the weight sum");
  float wz[kGroundPulseBiomeCount]{};
  const GroundPulseParams zb = blendPulseParams(wz);
  const GroundPulseParams sf = groundPulseParams(GroundPulseBiome::SiliconFlats);
  check(zb.freqHz == sf.freqHz && zb.amplitude == sf.amplitude,
        "ground pulse: all-zero weights fall back to the Silicon Flats default");
  // Dead-Zone fade: amplitude scales with the live weight, no cliff.
  float wd[kGroundPulseBiomeCount]{};
  wd[size_t(GroundPulseBiome::TraceForest)] = 0.5f;
  wd[size_t(GroundPulseBiome::None)] = 0.5f;  // Dead Zones contribute zero
  const GroundPulseParams half = blendPulseParams(wd);
  check(std::fabs(half.amplitude - 0.5f * tf.amplitude) < 1e-6f,
        "ground pulse: 50% Dead Zone weight halves the amplitude (smooth fade-out)");
  float wall[kGroundPulseBiomeCount]{};
  wall[size_t(GroundPulseBiome::None)] = 1.f;
  const auto allDead = groundPulseEmissive(9.99f, -3, 11, blendPulseParams(wall));
  check(allDead[0] == 0.f && allDead[1] == 0.f && allDead[2] == 0.f,
        "ground pulse: fully dead blend -> zero emissive");
  // Pause -> frozen: the pulse clock is sim ticks, so equal ticks = equal pulse.
  const float t1 = groundPulseTimeSec(123456u, 512u);
  const float t2 = groundPulseTimeSec(123456u, 512u);
  check(t1 == t2 && groundPulseEmissive(t1, 3, -7, tf) == groundPulseEmissive(t2, 3, -7, tf),
        "ground pulse: same worldTick -> same time -> frozen pulse (pause-safe)");
  check(groundPulseTimeSec(0u, 0u) == 0.f, "ground pulse: zero tick rate -> zero time (no div-by-zero)");
  // Tuning sanity (JD-locked): spires brighter than plains, forest blue-dominant.
  const GroundPulseParams rs = groundPulseParams(GroundPulseBiome::ResonatorSpires);
  check(rs.amplitude > dp.amplitude && tf.tintB > tf.tintR && rs.sharpness > tf.sharpness,
        "ground pulse: spires bright/sharp, forest blue-dominant, plains subtle");
  // Pulse is bounded and non-negative (it is an additive glow).
  bool bounded = true;
  for (int i = 0; i < 64; ++i) {
    const auto e = groundPulseEmissive(float(i) * 0.37f, i - 32, 32 - i, blend);
    for (float c : e) bounded = bounded && c >= 0.f && c <= 1.f;
  }
  check(bounded, "ground pulse: emissive stays in [0,1] across time and chunks");
}

// B2 (JD wiring 2026-10-13): Vulkan sprite pass conversion contract.
// Unit-level only — no GPU, no loader: pins quad->vertex expansion order,
// input-order preservation (no re-sort of the producer's far-to-near
// blend order), run grouping by (shade, page), pipeline/alpha-test
// mapping, and skip-marking of non-sprite shades / out-of-range pages.
int testVkSpriteConversion() {
  using namespace vk_sprite;
  auto quad = [](ShadeMode shade, int32_t page, float x) {
    SpriteDrawQuad q;
    for (int i = 0; i < 4; ++i) {
      q.corners[i] = Vec3{x + float(i), float(i) * 2.f, 3.f};
      q.uv[i][0] = 0.1f * float(i);
      q.uv[i][1] = 0.2f * float(i);
    }
    q.r = 0.5f; q.g = 0.6f; q.b = 0.7f; q.a = 0.8f;
    q.shade = shade; q.page = page;
    return q;
  };
  // Deliberately NOT sorted by depth: order in = order out.
  const SpriteDrawQuad quads[5] = {quad(kShadeAlphaBlend, 0, 10.f), quad(kShadeAlphaBlend, 0, 20.f),
                                   quad(kShadeCutout, 0, 30.f), quad(kShadeAdditive, 1, 40.f),
                                   quad(kShadeTex, 0, 50.f)};
  std::vector<SpriteVertex> verts;
  buildSpriteVertices(quads, 5, verts);
  if (verts.size() != 30) { std::printf("FAIL: vk sprite verts %zu != 30\n", verts.size()); return 1; }
  // Quad 0 expansion: corners {0,1,2,0,2,3}, uv/tint/page verbatim.
  const float expectX[6] = {10, 11, 12, 10, 12, 13};
  for (int i = 0; i < 6; ++i) {
    const SpriteVertex& v = verts[size_t(i)];
    if (v.x != expectX[i] || v.a != 0.8f || v.page != 0.f || v.u != 0.1f * float(kSpriteCornerOrder[size_t(i)])) {
      std::printf("FAIL: vk sprite vertex %d expansion wrong (x=%f)\n", i, v.x);
      return 1;
    }
  }
  // Input order preserved (second quad starts at x=20, not re-sorted).
  if (verts[6].x != 20.f || verts[12].x != 30.f) {
    std::printf("FAIL: vk sprite order not preserved\n");
    return 1;
  }
  const auto runs = spriteRuns(quads, 5, 2);
  if (runs.size() != 4 || runs[0].vertexCount != 12 || runs[0].pipeline != 1 ||
      runs[1].pipeline != 0 || runs[2].pipeline != 2 || runs[2].page != 1 ||
      runs[3].pipeline != -1 || runs[1].firstVertex != 12) {
    std::printf("FAIL: vk sprite runs wrong (%zu runs)\n", runs.size());
    return 1;
  }
  // Page out of range -> skip-marked, still one run in order.
  const SpriteDrawQuad bad[1] = {quad(kShadeCutout, 7, 0.f)};
  const auto badRuns = spriteRuns(bad, 1, 2);
  if (badRuns.size() != 1 || badRuns[0].pipeline != -1) {
    std::printf("FAIL: vk sprite out-of-range page not skip-marked\n");
    return 1;
  }
  if (spriteAlphaTestForShade(kShadeCutout) != 0.5f ||
      spriteAlphaTestForShade(kShadeAlphaBlend) != 0.f ||
      spriteAlphaTestForShade(kShadeAdditive) != 0.f ||
      spritePipelineForShade(kShadeCutout) != 0 ||
      spritePipelineForShade(kShadeTex) != -1) {
    std::printf("FAIL: vk sprite shade mapping wrong\n");
    return 1;
  }
  std::printf("VK sprite conversion checks: expansion/order/runs/shade mapping OK\n");
  return 0;
}

void testRender() {
  std::printf("render: mesher (fixture blocks, no game appearance)\n");
  const BlockRegistry& reg = fixtureBlocks();
  const BlockMaterials mats(reg.size());
  auto mesh = [&](std::initializer_list<std::pair<std::array<int, 3>, BlockId>> cells, BlockId fill = kAir) {
    ChunkMap w = worldWithColumns(1, &reg);
    for (const auto& [p, b] : cells) w.set(p[0], p[1], p[2], b);
    return meshSubChunk(*snapshotForMeshing(w, reg, 0, 0, 0, 1, 1, fill), MeshContext{reg, mats});
  };
  const ChunkMesh one = mesh({{{8, 8, 8}, kRock}});
  const ChunkMesh two = mesh({{{8, 8, 8}, kRock}, {{9, 8, 8}, kRock}});
  const ChunkMesh floorAir = mesh({{{8, 0, 8}, kRock}});
  const ChunkMesh floorFill = mesh({{{8, 0, 8}, kRock}}, kRock);
  const ChunkMesh slab = mesh({{{8, 8, 8}, kHalf}});
  const ChunkMesh plant = mesh({{{8, 8, 8}, kPlant}});
  check(one.verts.size() == 24 && two.verts.size() == 40 && floorAir.verts.size() == 24 && floorFill.verts.size() == 20,
        "mesher: face culling between cubes and against the below-world fill block");
  bool slabHeight = true;
  for (const PackedVertex& v : slab.verts) slabHeight = slabHeight && v.y <= 8 * 16 + 8;
  check(slab.verts.size() == 24 && slabHeight && plant.verts.size() == 16 && plant.verts[0].mode == kShadeCutout,
        "mesher: slab is half height, cross sprite is 4 alpha-tested quads");
  // Golden: a small scene meshes to exactly the same vertices as when this test was written.
  // Re-recorded 2026-10-11 (Worker 5, PBR phase 2): the trace Step-1 work (2026-10-10 night)
  // deliberately changed the mesh format - PackedVertex gained a uint32_t `slot` field
  // (12 -> 16 bytes, see renderer.h) - without updating this value. Verified correct by
  // inspection before re-recording: stripping the 4-byte slot from every vertex reproduces
  // the previous golden 0x9a6c89e230a77b13 byte-for-byte (all other fields unchanged), and a
  // scratch harness confirmed the geometry (25 quads: shared rock face culled, slab half
  // height with bottom culled, plant cross sprite, corner rock 6 faces; CCW winding; AO sane;
  // deterministic across runs). Old value kept here for the record: 0x9a6c89e230a77b13.
  // Re-recorded 2026-10-11 (fix run): the audit's CRITICAL PackedVertex fix moved
  // `slot` from @6 to @12 (restoring the 12-byte GPU attribute prefix). Geometry is
  // identical (25 quads); only the vertex byte layout changed. Old value kept for
  // the record: 0xdcb1c88e3af94693 (and 0x9a6c89e230a77b13 before that).
  const ChunkMesh scene = mesh({{{1, 1, 1}, kRock}, {{2, 1, 1}, kRock}, {{1, 2, 1}, kHalf}, {{4, 1, 4}, kPlant}, {{15, 15, 15}, kRock}});
  const uint64_t h = fnv(scene.verts.data(), scene.verts.size() * sizeof(PackedVertex));
  std::printf("  scene hash %016llx, %zu quads\n", (unsigned long long)h, scene.verts.size() / 4);
  check(h == 0x77c08ffd8158c6d3ULL, "mesher golden: fixed scene hash (update deliberately if the mesh format changes)");
  testBinaryMesher();
  testBinaryMesherTransparent();
  testMicroMesher();
  testPbrMaterials();
  testPbrBrdf();
  testFrustumMargin();
  testCometTail();
  testVelocityMargin();
  testTwoTier();
  testPbrLights();
  testPbrTiers();
  testPbrSoftware();
  testSoftwareBackfaceConvention();
  testSoftwareRenderHonesty();
  testSoftwareSpritesAndPacer();
  testPbrMeshChain();
  testPbrVertexSeam();
  testPbrStartup();
  testPbrChunkMaterials();
  testPbrEmissiveEnumeration();
  testGroundPulse();
  testPbrGlWiring();
  testPbrTrueRefraction();
}

// ---- fixture simulation for SimThread
struct FixtureSim final : IWorldSimulation {
  std::atomic<int>* scans; std::atomic<int>* ticks; std::atomic<int>* changes; std::atomic<int>* commands;
  void scan(const ChunkMap&) override { ++*scans; }
  bool canPlace(const ChunkMap&, int, int, int, BlockId b) const override { return b != kHalf; }
  void onBlockChanged(ChunkMap&, int x, int y, int z, BlockId now) override { ++*changes; events.push_back({1, now, x, y, z}); }
  void command(ChunkMap&, const SimCommand& c, const DirtyFn&) override { if (c.code == 7) ++*commands; events.push_back({2, c.a, c.x, c.y, c.z}); }
  void tick(ChunkMap&, const TickArea&, const DirtyFn&) override { ++*ticks; }
  size_t activeCount() const override { return 42; }
  void takeEvents(std::vector<SimEvent>& out) override { out.insert(out.end(), events.begin(), events.end()); events.clear(); }
  std::vector<SimEvent> events;
};

// Changes cells from tick() (column 1,0), randomTick() (column 2,0) and command() (column 0,2).
struct ChangingSim final : IWorldSimulation {
  int ticks = 0, randoms = 0;
  void scan(const ChunkMap&) override {}
  void onBlockChanged(ChunkMap&, int, int, int, BlockId) override {}
  void command(ChunkMap& w, const SimCommand&, const DirtyFn& dirty) override { w.set(8, 10, 40, kRock); dirty(8, 10, 40); }
  void tick(ChunkMap& w, const TickArea&, const DirtyFn& dirty) override { if (++ticks == 5) { w.set(24, 10, 8, kRock); dirty(24, 10, 8); } }
  void randomTick(ChunkMap& w, const TickArea&, uint64_t, const DirtyFn& dirty) override {
    if (++randoms == 7) { w.set(40, 10, 8, kRock); dirty(40, 10, 8); }
  }
};

void testChangedColumns() {
  std::printf("sim: changed-column tracking (what a partial save writes)\n");
  ChunkMap w = worldWithColumns(3, &fixtureBlocks());
  w.set(8, 10, 8, kRock);
  SimConfig cfg;
  cfg.tickRate = 64; cfg.radiusChunks = 4;
  SimThread st(w, fixtureBlocks(), std::make_unique<ChangingSim>(), std::move(cfg), [](std::shared_ptr<MeshInput>) {});
  st.setPlayerChunk(0, 0);
  st.setRay({8.5f, 14.5f, 8.5f, 0, -1, 0});
  st.start();
  st.waitLoops(3);
  EditCmd place{EditCmd::Kind::Place}; place.block = kRock;  // player edit in column (0,0)
  st.edit(place);
  EditCmd cmd{EditCmd::Kind::Command}; cmd.command = SimCommand{7, 0, 0, 0, 0, 0};
  st.edit(cmd);
  st.waitLoops(6);
  st.stop();
  const auto changed = st.takeChangedColumns();
  const std::unordered_set<uint64_t> want{ChunkKey::make(0, 0), ChunkKey::make(1, 0), ChunkKey::make(2, 0), ChunkKey::make(0, 2)};
  check(changed == want, "SimThread: player edit, sim tick, random tick and command each mark their column changed (and only those)");
  check(st.takeChangedColumns().empty(), "SimThread: taking the changed columns (a save) clears them");
  st.start();
  st.waitLoops(4);
  st.stop();
  check(st.takeChangedColumns().empty(), "SimThread: ticks that change nothing mark nothing");
}

// Random ticks: a full section of an opted-in block; every pick reaches onRandomTick.
struct RandomSim final : IWorldSimulation {
  std::atomic<int>* worldTicks; std::atomic<int>* picks;
  void scan(const ChunkMap&) override {}
  void onBlockChanged(ChunkMap&, int, int, int, BlockId) override {}
  void tick(ChunkMap&, const TickArea&, const DirtyFn&) override { ++*worldTicks; }
  void onRandomTick(ChunkMap& w, int x, int y, int z, BlockId, uint32_t, const DirtyFn& dirty) override {
    ++*picks;
    w.setState(x, y, z, 1);
    dirty(x, y, z);
  }
};

void testRandomTicks() {
  std::printf("sim: random ticks (opt-in, per section, deterministic, dirty)\n");
  BlockRegistry reg;
  const BlockId grass = reg.add({"Grass", kSolid | kOpaque | kRandomTicks, Shape::Cube});
  const BlockId stone = reg.add({"Stone", kSolid | kOpaque, Shape::Cube});
  ChunkMap w = worldWithColumns(2, &reg);
  for (int z = 0; z < 16; ++z) for (int x = 0; x < 16; ++x) for (int y = 0; y < 16; ++y) {
    w.set(x, y, z, grass);        // column (0,0): one full opted-in section
    w.set(x + 16, y, z, stone);   // column (1,0): one full section, not opted in
  }
  const auto tickable = randomTickableBlocks(reg);
  std::vector<int> a, b;
  const auto collect = [](std::vector<int>& out) { return [&out](int x, int y, int z, BlockId, uint32_t) { out.push_back((y << 16) | (z << 8) | x); }; };
  const uint64_t sampled = runRandomTicks(w, reg, tickable, 0, 0, 2, 7, 8, collect(a));
  runRandomTicks(w, reg, tickable, 0, 0, 2, 7, 8, collect(b));
  bool inSection = true; for (int v : a) inSection = inSection && (v & 0xff) < 16;
  check(sampled == 16 && a.size() == 8 && a == b && inSection,
        "random ticks: 8 picks per non-empty section, only opted-in blocks reach the game, same tick -> same cells");
  std::vector<int> c;
  runRandomTicks(w, reg, tickable, 0, 0, 2, 8, 8, collect(c));
  check(c != a && runRandomTicks(w, reg, tickable, 0, 0, 2, 9, 0, collect(c)) == 0, "random ticks: differ per tick; 0 per section = off");
  std::atomic<int> worldTicks{0}, picks{0};
  auto sim = std::make_unique<RandomSim>();
  sim->worldTicks = &worldTicks; sim->picks = &picks;
  SimConfig cfg; cfg.tickRate = 64; cfg.radiusChunks = 2;
  cfg.domains.push_back({"fast", 512, true});  // a fast clock domain must not scale random ticks
  SimThread st(w, reg, std::move(sim), std::move(cfg), [](std::shared_ptr<MeshInput>) {});
  st.start();
  waitUntil([&] { return worldTicks > 8; });
  st.stop();
  const auto changed = st.takeChangedColumns();
  check(worldTicks > 5 && picks == 8 * worldTicks && changed.size() == 1 && changed.count(ChunkKey::make(0, 0)),
        "random ticks: 8 per section per WORLD tick on the sim thread (not per 512 Hz domain step); changes mark the column dirty");
}

// ---- tick stepper (CH-H5) + tick continuity (CH-H8) fixtures -----------------
struct StepRec {
  std::atomic<int> ticks{0}, randoms{0}, picks{0};
  std::mutex m;
  std::vector<uint64_t> randomIdx;  // tickIndex of every randomTick call, in order
};

struct StepSim final : IWorldSimulation {
  StepRec* rec;
  int pauseAtTick = -1;  // live arm: pause the sim from inside this tick (exact, not racy)
  std::function<void()> onPauseTick;
  void scan(const ChunkMap&) override {}
  void onBlockChanged(ChunkMap&, int, int, int, BlockId) override {}
  void tick(ChunkMap&, const TickArea&, const DirtyFn&) override {
    const int n = ++rec->ticks;
    if (n == pauseAtTick && onPauseTick) onPauseTick();
  }
  void randomTick(ChunkMap&, const TickArea&, uint64_t idx, const DirtyFn&) override {
    ++rec->randoms;
    std::lock_guard l(rec->m);
    rec->randomIdx.push_back(idx);
  }
  void onRandomTick(ChunkMap& w, int x, int y, int z, BlockId, uint32_t, const DirtyFn& dirty) override {
    ++rec->picks;
    w.setState(x, y, z, 1);
    dirty(x, y, z);
  }
};

void testStepTicks() {
  std::printf("sim: tick stepper exactness, stepped/live equivalence, stop/start continuity\n");
  const auto makeWorld = [](BlockRegistry& reg) {
    const BlockId grass = reg.add({"Grass", kSolid | kOpaque | kRandomTicks, Shape::Cube});
    const BlockId stone = reg.add({"Stone", kSolid | kOpaque, Shape::Cube});
    ChunkMap w = worldWithColumns(2, &reg);
    for (int z = 0; z < 16; ++z)
      for (int x = 0; x < 16; ++x)
        for (int y = 0; y < 16; ++y) {
          w.set(x, y, z, grass);        // column (0,0): one full opted-in section
          w.set(x + 16, y, z, stone);   // column (1,0): not opted in
        }
    return w;
  };
  const auto sectionStates = [](const ChunkMap& w) {
    std::vector<CellState> out;
    for (int y = 0; y < 16; ++y)
      for (int z = 0; z < 16; ++z)
        for (int x = 0; x < 16; ++x) out.push_back(w.getState(x, y, z));
    return out;
  };
  const auto makeCfg = [] {
    SimConfig cfg;
    cfg.tickRate = 32; cfg.radiusChunks = 2; cfg.randomTicksPerSection = 8;
    cfg.domains.push_back({"fast", 512, true});  // worldDiv = 512/32 = 16 master steps per world tick
    return cfg;
  };

  // ---- stepped arm: stepTicks BEFORE start, so the sim starts paused and
  // only stepped ticks ever run.
  BlockRegistry regA;
  ChunkMap wA = makeWorld(regA);
  StepRec recA;
  auto simA = std::make_unique<StepSim>(); simA->rec = &recA;
  SimThread stA(wA, regA, std::move(simA), makeCfg(), [](std::shared_ptr<MeshInput>) {});
  stA.setPlayerChunk(0, 0);
  stA.stepTicks(3);
  stA.start();
  // Wait on the PUBLISHED stat: ticksStepped increments mid-body, the
  // stats slot is published at body end — waiting on either alone races.
  waitUntil([&] { return stA.ticksStepped() == 3 && stA.stats().worldTick == 3; });
  check(stA.stats().worldTick == 3 && stA.ticksStepped() == 3,
        "stepTicks: stepTicks(3) at worldDiv 16 advances worldTick by exactly 3 (not by 3 master steps)");
  stA.stepTicks(1);
  waitUntil([&] { return stA.ticksStepped() == 4 && stA.stats().worldTick == 4; });
  check(stA.stats().worldTick == 4, "stepTicks: stepTicks(1) advances exactly 1 world tick (old code: usually 0)");
  stA.stepTicks(16);
  waitUntil([&] { return stA.ticksStepped() == 20 && stA.stats().worldTick == 20; });
  check(stA.stats().worldTick == 20 && recA.ticks == 20 && recA.randoms == 20,
        "stepTicks: 20 stepped world ticks, each running tick + randomTick");
  check(recA.picks == 8 * 20,
        "stepTicks: stepped path runs runRandomTicks (8 picks/section/tick; the old stepped branch omitted it)");

  // ---- live arm: same setup, 20 live ticks. The fixture pauses the sim
  // from inside tick 20 (at most one world tick runs per loop iteration at
  // worldDiv 16 with the 16-step catch-up cap), so the count is exact.
  BlockRegistry regB;
  ChunkMap wB = makeWorld(regB);
  StepRec recB;
  auto simB = std::make_unique<StepSim>(); simB->rec = &recB; simB->pauseAtTick = 20;
  SimThread* stBP = nullptr;
  simB->onPauseTick = [&] { if (stBP) stBP->setPaused(true); };
  SimThread stB(wB, regB, std::move(simB), makeCfg(), [](std::shared_ptr<MeshInput>) {});
  stBP = &stB;
  stB.setPlayerChunk(0, 0);
  stB.start();
  waitUntil([&] { return recB.randoms == 20; });
  stB.stop();
  check(recB.ticks == 20 && recB.randoms == 20 && recB.picks == recA.picks && recA.randomIdx == recB.randomIdx &&
            sectionStates(wA) == sectionStates(wB),
        "stepTicks: stepped-vs-live world state matches for the same 20 ticks (incl. random-tick picks + keyed indices)");

  // ---- resume-after-step, then continuity across a routine stop/start (H8)
  stA.setPaused(false);
  waitUntil([&] { return stA.stats().worldTick >= 23; });
  stA.stop();
  const uint64_t wBefore = stA.stats().worldTick;
  check(wBefore >= 23, "stepTicks: resume-after-step works (live ticks continue from the stepped count)");
  stA.start();
  waitUntil([&] { return stA.stats().worldTick > wBefore; });
  stA.stop();
  bool consecutive = recA.randomIdx.size() > 20 && !recA.randomIdx.empty() && recA.randomIdx.front() == 0;
  for (size_t i = 1; i < recA.randomIdx.size(); ++i)
    consecutive = consecutive && (recA.randomIdx[i] == recA.randomIdx[i - 1] + 1);
  check(consecutive,
        "continuity: world-tick indices run 0,1,2,... with NO reset across stop/start (keyed streams never replay)");
}

// ---- handshake group semantics (A-M1), poll (A-M3), stats snapshot (A-M6) ----
void testHandshakeGroups() {
  std::printf("sim: handshake groups / poll / stats snapshot\n");
  TimingHandshakeAuthority auth(TimingHandshakeAuthority::Config{});
  std::atomic<int> g0{0}, g1{0}, g2{0};
  HsComponentDesc d0; d0.name = "t.g0"; d0.group = 0;
  auth.registerComponent(d0, [&](uint64_t, uint64_t) { ++g0; });
  HsComponentDesc d1; d1.name = "t.g1"; d1.group = 1;
  auth.registerComponent(d1, [&](uint64_t, uint64_t) { ++g1; });
  HsComponentDesc d2; d2.name = "t.g2"; d2.group = 2;
  auth.registerComponent(d2, [&](uint64_t, uint64_t) { ++g2; });
  // No start(): runPhase executes shard work inline (deterministic fallback).
  check(auth.runPhase(0, 0, 0) && g0 == 1 && g1 == 0 && g2 == 0,
        "handshake groups: caller group 0 fires ONLY group-0 components (caller side is not a wildcard)");
  check(auth.runPhase(1, 0, 1) && g0 == 2 && g1 == 1 && g2 == 0,
        "handshake groups: caller group 1 fires group-1 + group-0 components only");
  check(auth.runPhase(2, 0, kAllGroups) && g0 == 3 && g1 == 2 && g2 == 1,
        "handshake groups: kAllGroups fires every group");
  const HsStats snap = auth.statsSnapshot();
  check(snap.phases == 3 && snap.perGroup[0].phases == 2 && snap.perGroup[1].phases == 1,
        "handshake stats: snapshot copies the live stats (per-group split intact, kAllGroups folds to slot 0)");
  TimingHandshakeAuthority fresh(TimingHandshakeAuthority::Config{});
  uint64_t ph = 0;
  TimingHandshakeAuthority::PublishedPhase pp;
  check(!fresh.canTick(0, ph) && !fresh.pollPhase(0, 0, pp),
        "handshake poll: with nothing published, canTick/pollPhase (the one shared poll) both say no");
  {
    // Review A-L9 dueFor semantics: HalfCycle at 4 phases/cycle fires on
    // phases 0 and 2 (divider = phasesPerCycle/2 = 2); an EXPLICIT
    // phaseDivider of 1 wins over the cadence and fires every phase (the
    // old "!= 1 means set" test could not express that).
    TimingHandshakeAuthority auth2(TimingHandshakeAuthority::Config{});
    std::atomic<int> half{0}, forced{0};
    HsComponentDesc dh; dh.name = "t.half"; dh.cadence = CadenceClass::HalfCycle;
    auth2.registerComponent(dh, [&](uint64_t, uint64_t) { ++half; });
    HsComponentDesc df; df.name = "t.forced1"; df.cadence = CadenceClass::HalfCycle; df.phaseDivider = 1;
    auth2.registerComponent(df, [&](uint64_t, uint64_t) { ++forced; });
    for (uint64_t p = 0; p < 4; ++p) auth2.runPhase(p, 0, 0);
    check(half == 2 && forced == 4,
          "handshake dueFor: HalfCycle divider is phasesPerCycle/2; explicit phaseDivider=1 overrides the cadence");
  }
}

// ---- deterministic scheduler fixtures ---------------------------------------
constexpr uint32_t kCellsLo = 100, kCellsHi = 101, kChecksum = 102, kLog = 200, kEnts = 201;

struct SchedWorld {
  std::vector<uint64_t> lo, hi;  // 128 cells each, separate resources
  uint64_t checksum = 0;
  std::vector<uint64_t> drainLog;  // (systemId << 32) | payload, in drain order
  std::vector<uint64_t> entities;
  uint64_t stage1Observed = 0;
  SchedWorld() : lo(128, 0), hi(128, 0) {}
};

uint64_t schedWorldHash(const SchedWorld& w) {
  uint64_t h = DeterministicScheduler::hashBytes(w.lo.data(), w.lo.size() * sizeof(uint64_t));
  h = DeterministicScheduler::hashBytes(w.hi.data(), w.hi.size() * sizeof(uint64_t), h);
  h = DeterministicScheduler::hashU64(w.checksum, h);
  return DeterministicScheduler::hashBytes(w.entities.data(), w.entities.size() * sizeof(uint64_t), h);
}

class NopSystem : public SimSystem {
 public:
  NopSystem(const char* name, std::vector<ResourceAccess> acc) : name_(name), acc_(std::move(acc)) {}
  const char* name() const override { return name_; }
  std::vector<ResourceAccess> access() const override { return acc_; }
  void update(SchedTickContext&) override {}

 private:
  const char* name_;
  std::vector<ResourceAccess> acc_;
};

/// Writes one 128-cell half with per-entity keyed RNG (no shared stream).
class AccSystem : public SimSystem {
 public:
  AccSystem(const char* name, uint32_t res, size_t entityBase)
      : name_(name), res_(res), entityBase_(entityBase) {}
  const char* name() const override { return name_; }
  std::vector<ResourceAccess> access() const override { return {writeRes(res_)}; }
  void update(SchedTickContext& ctx) override {
    auto* w = static_cast<SchedWorld*>(ctx.world);
    std::vector<uint64_t>& cells = (res_ == kCellsLo) ? w->lo : w->hi;
    for (size_t i = 0; i < cells.size(); ++i)
      cells[i] = detMix64(cells[i] + ctx.rng32(entityBase_ + i, 0xA11CE));
  }

 private:
  const char* name_;
  uint32_t res_;
  size_t entityBase_;
};

/// Reads both halves, writes a checksum: conflicts with both accumulators, so
/// it must land in a later stage.
class FoldSystem : public SimSystem {
 public:
  const char* name() const override { return "fold"; }
  std::vector<ResourceAccess> access() const override {
    return {readRes(kCellsLo), readRes(kCellsHi), writeRes(kChecksum)};
  }
  void update(SchedTickContext& ctx) override {
    auto* w = static_cast<SchedWorld*>(ctx.world);
    uint64_t h = DeterministicScheduler::hashBytes(w->lo.data(), w->lo.size() * sizeof(uint64_t));
    w->checksum = DeterministicScheduler::hashBytes(w->hi.data(), w->hi.size() * sizeof(uint64_t), h);
  }
};

/// Pushes Spawn commands with fixed payloads (drain-order test).
class PushSystem : public SimSystem {
 public:
  PushSystem(const char* name, uint32_t res, std::vector<uint64_t> payloads)
      : name_(name), res_(res), payloads_(std::move(payloads)) {}
  const char* name() const override { return name_; }
  std::vector<ResourceAccess> access() const override { return {writeRes(res_)}; }
  void update(SchedTickContext& ctx) override {
    for (uint64_t p : payloads_)
      ctx.commands.push(StructuralCommand{StructuralCommand::Kind::Spawn, 0, p, {}});
  }

 private:
  const char* name_;
  uint32_t res_;
  std::vector<uint64_t> payloads_;
};

/// Stage 0: enqueues a Custom spawn command. Never touches the world directly.
class SpawnOneSystem : public SimSystem {
 public:
  const char* name() const override { return "spawner"; }
  std::vector<ResourceAccess> access() const override { return {writeRes(kEnts)}; }
  void update(SchedTickContext& ctx) override {
    StructuralCommand c;
    c.kind = StructuralCommand::Kind::Custom;
    c.apply = [](void* wp) { static_cast<SchedWorld*>(wp)->entities.push_back(777); };
    ctx.commands.push(std::move(c));
  }
};

/// Reads kEnts, so it conflicts with the spawner and lands in a later stage:
/// it must observe the drained spawn.
class ObserveSystem : public SimSystem {
 public:
  const char* name() const override { return "observer"; }
  std::vector<ResourceAccess> access() const override { return {readRes(kEnts)}; }
  void update(SchedTickContext& ctx) override {
    static_cast<SchedWorld*>(ctx.world)->stage1Observed =
        static_cast<SchedWorld*>(ctx.world)->entities.size();
  }
};

void testDetSched() {
  std::printf("sim: deterministic scheduler (conflict-DAG stages, command buffers, keyed RNG)\n");

  // --- conflict-DAG -> greedy stages ---
  {
    DeterministicScheduler sched([](void*, size_t, const StructuralCommand&) {});
    sched.addSystem(std::make_unique<NopSystem>("A", std::vector<ResourceAccess>{writeRes(1)}));
    sched.addSystem(std::make_unique<NopSystem>("B", std::vector<ResourceAccess>{readRes(1)}));
    sched.addSystem(std::make_unique<NopSystem>("C", std::vector<ResourceAccess>{writeRes(2)}));
    sched.addSystem(std::make_unique<NopSystem>("D", std::vector<ResourceAccess>{readRes(2)}));
    sched.addSystem(std::make_unique<NopSystem>("E", std::vector<ResourceAccess>{writeRes(1)}));
    sched.addSystem(std::make_unique<NopSystem>("F", std::vector<ResourceAccess>{readRes(3)}));
    sched.addSystem(std::make_unique<NopSystem>("G", std::vector<ResourceAccess>{readRes(3)}));
    const auto& st = sched.stages();
    const bool ok = st.size() == 3 && st[0] == std::vector<size_t>{0, 2, 5, 6} &&
                    st[1] == std::vector<size_t>{1, 3} && st[2] == std::vector<size_t>{4};
    check(ok, "det-sched: stages: writers split stages, read-after-write orders, disjoint resources and "
              "read/read pairs run in parallel");
  }

  // --- command buffers drain in (system, seq) order ---
  {
    SchedWorld w;
    DeterministicScheduler sched([&](void* wp, size_t sysId, const StructuralCommand& c) {
      static_cast<SchedWorld*>(wp)->drainLog.push_back((sysId << 32) | c.payload);
    });
    sched.addSystem(std::make_unique<PushSystem>("P", kLog, std::vector<uint64_t>{11, 12}));
    sched.addSystem(std::make_unique<PushSystem>("Q", kLog + 1, std::vector<uint64_t>{21}));
    JobPool pool(2);  // P and Q push concurrently into their own private buffers
    sched.runTick(0, &w, pool);
    const bool ok = w.drainLog == std::vector<uint64_t>{11, 12, (1ULL << 32) | 21};
    check(ok, "det-sched: command buffers drain in (system, seq) order even when pushed from parallel systems");
  }

  // --- drained commands are visible to the next stage, never mid-stage ---
  {
    bool ok = true;
    for (unsigned threads : {1u, 4u}) {
      SchedWorld w;
      DeterministicScheduler sched([](void*, size_t, const StructuralCommand&) {});
      sched.addSystem(std::make_unique<SpawnOneSystem>());
      sched.addSystem(std::make_unique<ObserveSystem>());
      const auto& st = sched.stages();
      ok = ok && st.size() == 2 && st[0] == std::vector<size_t>{0} && st[1] == std::vector<size_t>{1};
      JobPool pool(threads);
      sched.runTick(5, &w, pool);
      ok = ok && w.entities == std::vector<uint64_t>{777} && w.stage1Observed == 1;
    }
    check(ok, "det-sched: structural command drained between stages; later stage observes it (1 and 4 threads)");
  }

  // --- per-entity keyed RNG: stable, distinct, thread-count independent ---
  {
    bool ok = detRand32(42, 7, 1) == detRand32(42, 7, 1);
    ok = ok && detRand32(42, 7, 1) != detRand32(43, 7, 1);
    ok = ok && detRand32(42, 7, 1) != detRand32(42, 8, 1);
    ok = ok && detRand32(42, 7, 1) != detRand32(42, 7, 2);
    std::vector<uint32_t> seq(256), par(256);
    for (size_t i = 0; i < seq.size(); ++i) seq[i] = detRand32(i, 99, 7);
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t)
      ts.emplace_back([&, t] {
        for (size_t i = size_t(t); i < par.size(); i += 4) par[i] = detRand32(i, 99, 7);
      });
    for (auto& th : ts) th.join();
    ok = ok && seq == par;
    const double d = detRand01(1, 2, 3);
    ok = ok && d >= 0.0 && d < 1.0 && d == detRand01(1, 2, 3);
    check(ok, "det-sched: keyed RNG stable per (entity, tick, salt), distinct across them, identical on 4 threads");
  }

  // --- threaded run hashes bit-identical to the single-threaded run ---
  {
    auto runWorld = [](unsigned threads, uint64_t ticks) {
      SchedWorld w;
      DeterministicScheduler sched([](void*, size_t, const StructuralCommand&) {});
      sched.addSystem(std::make_unique<AccSystem>("lo", kCellsLo, 0));
      sched.addSystem(std::make_unique<AccSystem>("hi", kCellsHi, 128));
      sched.addSystem(std::make_unique<FoldSystem>());
      const auto& st = sched.stages();  // lo+hi parallel, fold after
      const bool layout = st.size() == 2 && st[0].size() == 2 && st[1] == std::vector<size_t>{2};
      JobPool pool(threads);
      sched.runTicks(0, ticks, &w, pool);
      return std::make_pair(layout, schedWorldHash(w));
    };
    const uint64_t ticks = 64;
    const auto r1 = runWorld(1, ticks);
    const auto r2 = runWorld(2, ticks);
    const auto r4 = runWorld(4, ticks);
    const auto r4b = runWorld(4, ticks);
    const SchedWorld zero;
    const bool ok = r1.first && r1.second == r2.second && r2.second == r4.second && r4.second == r4b.second &&
                    r1.second != schedWorldHash(zero);
    check(ok, "det-sched: 64-tick run with keyed RNG + cross-stage checksum hashes bit-identical on 1, 2, 4 threads");
  }

  // --- JobSystem backend: threaded runs hash bit-identical to the JobPool path ---
  {
    auto runWorldJs = [](unsigned threads, uint64_t ticks) {
      SchedWorld w;
      DeterministicScheduler sched([](void*, size_t, const StructuralCommand&) {});
      sched.addSystem(std::make_unique<AccSystem>("lo", kCellsLo, 0));
      sched.addSystem(std::make_unique<AccSystem>("hi", kCellsHi, 128));
      sched.addSystem(std::make_unique<FoldSystem>());
      JobSystem::Config cfg; cfg.workerThreads = threads;
      JobSystem js(cfg);
      sched.runTicks(0, ticks, &w, js);
      return schedWorldHash(w);
    };
    auto runWorldPool1 = [] {
      SchedWorld w;
      DeterministicScheduler sched([](void*, size_t, const StructuralCommand&) {});
      sched.addSystem(std::make_unique<AccSystem>("lo", kCellsLo, 0));
      sched.addSystem(std::make_unique<AccSystem>("hi", kCellsHi, 128));
      sched.addSystem(std::make_unique<FoldSystem>());
      JobPool pool(1);
      sched.runTicks(0, 64, &w, pool);
      return schedWorldHash(w);
    };
    const uint64_t ticks = 64;
    const uint64_t ref = runWorldPool1();
    const uint64_t h1 = runWorldJs(1, ticks), h2 = runWorldJs(2, ticks);
    const uint64_t h4 = runWorldJs(4, ticks), h8 = runWorldJs(8, ticks);
    const bool okJs = ref == h1 && h1 == h2 && h2 == h4 && h4 == h8 && ref != 0;
    check(okJs, "det-sched: JobSystem backend hashes bit-identical to JobPool on 1, 2, 4, 8 workers");
  }
}

/// Recording ITraceJit: counts compile/execute/free and remembers which graph
/// ids were executed and freed. Nodes are never touched (nullptr, 0 count).
class FakeJit : public ITraceJit {
 public:
  int compiles = 0, executes = 0, frees = 0;
  uint64_t lastExecutedId = 0, lastFreedId = 0;

  CompiledGraph compile(ISignalNode** /*nodes*/, uint32_t count) override {
    ++compiles;
    CompiledGraph g;
    g.id = ++nextId_;
    g.nodeCount = count;
    g.tickCost = count;
    g.bytecode = {0xDE, 0xAD};
    return g;
  }
  void execute(const CompiledGraph& graph) override {
    ++executes;
    lastExecutedId = graph.id;
  }
  void free(CompiledGraph& graph) override {
    ++frees;
    lastFreedId = graph.id;
    graph.bytecode.clear();
  }

 private:
  uint64_t nextId_ = 0;
};

void testTraceJitCompose() {
  std::printf("sim: TraceJitSystem composition seam (JIT graph wrapped as a scheduler system)\n");

  // --- access() declares Write on the signal-network resource ---
  {
    FakeJit jit;
    TraceJitSystem sys(&jit, nullptr, 0);
    const auto acc = sys.access();
    const bool ok = acc.size() == 1 && acc[0].resource == kSignalNetworkRes && acc[0].write &&
                    std::strcmp(sys.name(), "trace_jit") == 0 && sys.signalResource() == kSignalNetworkRes;
    check(ok, "trace-jit-compose: default access() declares Write on kSignalNetworkRes");
  }
  {
    FakeJit jit;
    TraceJitSystem sys(&jit, nullptr, 0, /*signalResource=*/777, /*name=*/"chip0");
    const auto acc = sys.access();
    const bool ok = acc.size() == 1 && acc[0].resource == 777 && acc[0].write &&
                    std::strcmp(sys.name(), "chip0") == 0;
    check(ok, "trace-jit-compose: constructor resource id and name override the defaults");
  }

  // --- stage placement: conflicts with a same-resource writer, shares a stage with a disjoint reader ---
  {
    FakeJit jit;
    DeterministicScheduler sched([](void*, size_t, const StructuralCommand&) {});
    sched.addSystem(std::make_unique<TraceJitSystem>(&jit, nullptr, 0));  // id 0: Write(kSignalNetworkRes)
    sched.addSystem(std::make_unique<NopSystem>("w", std::vector<ResourceAccess>{writeRes(kSignalNetworkRes)}));
    sched.addSystem(std::make_unique<NopSystem>("r", std::vector<ResourceAccess>{readRes(900)}));
    const auto& st = sched.stages();
    const bool ok = st.size() == 2 && st[0] == std::vector<size_t>{0, 2} && st[1] == std::vector<size_t>{1};
    check(ok, "trace-jit-compose: conflicting writer on the signal resource lands in a later stage; "
              "disjoint reader shares the stage");
  }

  // --- execute is called exactly once per tick through the scheduler, on the owned graph ---
  {
    bool ok = true;
    for (unsigned threads : {1u, 4u}) {
      FakeJit jit;
      DeterministicScheduler sched([](void*, size_t, const StructuralCommand&) {});
      auto sys = std::make_unique<TraceJitSystem>(&jit, nullptr, 0);
      TraceJitSystem* raw = sys.get();
      const uint64_t graphId = raw->graph().id;
      sched.addSystem(std::move(sys));
      SchedWorld w;
      JobPool pool(threads);
      sched.runTicks(0, 8, &w, pool);
      ok = ok && jit.compiles == 1 && jit.executes == 8 && raw->ticksExecuted() == 8 &&
           jit.lastExecutedId == graphId && jit.frees == 0;
    }
    check(ok, "trace-jit-compose: execute runs once per tick through the scheduler (1 and 4 threads), "
              "on the owned graph, no frees mid-run");
  }

  // --- recompile between ticks swaps the graph and frees the old one ---
  {
    FakeJit jit;
    auto sys = std::make_unique<TraceJitSystem>(&jit, nullptr, 0);
    TraceJitSystem* raw = sys.get();
    const uint64_t id1 = raw->graph().id;
    DeterministicScheduler sched([](void*, size_t, const StructuralCommand&) {});
    sched.addSystem(std::move(sys));
    SchedWorld w;
    JobPool pool(1);
    sched.runTick(0, &w, pool);
    const bool sawFirst = jit.lastExecutedId == id1;
    raw->recompile(nullptr, 0);  // between ticks: legal
    const uint64_t id2 = raw->graph().id;
    sched.runTick(1, &w, pool);
    const bool ok = sawFirst && id2 != id1 && jit.lastExecutedId == id2 &&
                    jit.compiles == 2 && jit.frees == 1 && jit.lastFreedId == id1;
    check(ok, "trace-jit-compose: recompile between ticks swaps the graph (new id executed) and frees the old one");
  }

  // --- destruction frees the owned graph exactly once ---
  {
    FakeJit jit;
    {
      TraceJitSystem sys(&jit, nullptr, 0);
      const bool compiledOnce = jit.compiles == 1 && jit.frees == 0;
      if (!compiledOnce) check(false, "trace-jit-compose: constructor compiles exactly once");
    }
    check(jit.compiles == 1 && jit.frees == 1, "trace-jit-compose: destructor frees the owned graph exactly once");
  }
}

void testSignalPropagate() {
  std::printf("sim: signal propagation granularity policy (selectable PerChunk/PerNetwork/WholeWorld)\n");

  // --- default granularity ---
  {
    VectorPropagationPolicy pol;
    const bool ok = pol.granularity() == PropagationGranularity::PerChunk &&
                    kDefaultPropagationGranularity == PropagationGranularity::PerChunk;
    check(ok, "signal-propagate: default granularity is PerChunk");
  }

  // --- chunk key round-trip (negative coords survive the pack/unpack) ---
  {
    int cx = 0, cz = 0;
    unpackChunkPartitionId(chunkPartitionId(-37, 12345), cx, cz);
    check(cx == -37 && cz == 12345, "signal-propagate: chunkPartitionId/unpack round-trip (negative coords)");
  }

  // --- PerChunk: sorted chunk keys, deterministic across repeated calls and ticks ---
  {
    VectorPropagationPolicy pol;
    pol.setGranularity(PropagationGranularity::PerChunk);
    // Scrambled input (plus a duplicate key): canonical order must come out anyway.
    const std::vector<uint64_t> keys = {chunkPartitionId(3, -2), chunkPartitionId(-9, 7),
                                        chunkPartitionId(3, -5), chunkPartitionId(0, 0),
                                        chunkPartitionId(-9, 7)};
    pol.setChunks(keys);
    std::vector<SignalPartition> a, b, c;
    pol.partitionsForTick(12, a);
    pol.partitionsForTick(12, b);   // repeated call, same tick
    pol.partitionsForTick(999, c);  // different tick: partition shape is world-state-derived
    const auto sameIds = [](const std::vector<SignalPartition>& x,
                            const std::vector<SignalPartition>& y) {
      if (x.size() != y.size()) return false;
      for (size_t i = 0; i < x.size(); ++i)
        if (x[i].id != y[i].id || x[i].kind != y[i].kind) return false;
      return true;
    };
    std::vector<uint64_t> want = keys;
    std::sort(want.begin(), want.end());
    bool match = a.size() == want.size() && partitionsAreSorted(a);
    for (size_t i = 0; match && i < want.size(); ++i)
      match = a[i].kind == PropagationGranularity::PerChunk && a[i].id == want[i];
    check(match && sameIds(a, b) && sameIds(a, c),
          "signal-propagate: per-chunk yields sorted chunk keys, deterministic across repeated calls and ticks");
  }

  // --- PerChunk with an empty chunk set yields no partitions (valid: a quiet tick) ---
  {
    VectorPropagationPolicy pol;
    pol.setGranularity(PropagationGranularity::PerChunk);
    pol.setChunks({});
    std::vector<SignalPartition> a;
    pol.partitionsForTick(0, a);
    check(a.empty() && partitionsAreSorted(a),
          "signal-propagate: per-chunk with no ticking chunks yields an empty (valid) partition list");
  }

  // --- PerNetwork: canonical (sorted) network order, deterministic across calls ---
  {
    VectorPropagationPolicy pol;
    pol.setGranularity(PropagationGranularity::PerNetwork);
    pol.setNetworks({91, 5, 40, 7});
    std::vector<SignalPartition> a, b;
    pol.partitionsForTick(3, a);
    pol.partitionsForTick(3, b);
    const uint64_t want[] = {5, 7, 40, 91};
    bool ok = a.size() == 4 && partitionsAreSorted(a) && a.size() == b.size();
    for (size_t i = 0; ok && i < 4; ++i)
      ok = a[i].kind == PropagationGranularity::PerNetwork && a[i].id == want[i] && b[i].id == want[i];
    check(ok, "signal-propagate: per-network yields canonical (sorted) network order, deterministic across calls");
  }

  // --- WholeWorld: exactly one partition carrying the whole-world token ---
  {
    VectorPropagationPolicy pol;
    pol.setChunks({chunkPartitionId(1, 2)});  // irrelevant under WholeWorld
    pol.setNetworks({8, 9});
    pol.setGranularity(PropagationGranularity::WholeWorld);
    std::vector<SignalPartition> a;
    pol.partitionsForTick(0, a);
    const bool ok = a.size() == 1 && a[0].kind == PropagationGranularity::WholeWorld &&
                    a[0].id == kWholeWorldPartitionId;
    check(ok, "signal-propagate: whole-world yields exactly one partition carrying the whole-world token");
  }

  // --- runtime switching takes effect on the next call ---
  {
    VectorPropagationPolicy pol;
    pol.setChunks({chunkPartitionId(2, 0), chunkPartitionId(0, 1)});
    pol.setNetworks({11, 3});
    std::vector<SignalPartition> p;
    pol.partitionsForTick(0, p);
    const bool chunkFirst = p.size() == 2 && p[0].kind == PropagationGranularity::PerChunk;
    pol.setGranularity(PropagationGranularity::WholeWorld);
    pol.partitionsForTick(0, p);
    const bool worldNext = p.size() == 1 && p[0].kind == PropagationGranularity::WholeWorld &&
                           p[0].id == kWholeWorldPartitionId;
    pol.setGranularity(PropagationGranularity::PerNetwork);
    pol.partitionsForTick(0, p);
    const bool netLast = p.size() == 2 && p[0].kind == PropagationGranularity::PerNetwork &&
                         p[0].id == 3 && p[1].id == 11;
    check(chunkFirst && worldNext && netLast,
          "signal-propagate: switching granularity at runtime takes effect on the next call");
  }

  // --- partitionsAreSorted: rejects mixed kinds and unsorted ids, accepts empty ---
  {
    const std::vector<SignalPartition> mixed = {{PropagationGranularity::PerChunk, 5},
                                                {PropagationGranularity::PerNetwork, 3}};
    const std::vector<SignalPartition> unsorted = {{PropagationGranularity::PerChunk, 9},
                                                   {PropagationGranularity::PerChunk, 2}};
    const std::vector<SignalPartition> empty;
    const bool ok = !partitionsAreSorted(mixed) && !partitionsAreSorted(unsorted) &&
                    partitionsAreSorted(empty);
    check(ok, "signal-propagate: partitionsAreSorted rejects mixed kinds / unsorted ids, accepts empty");
  }

  // --- scheduler mapping: whole-world -> kSignalNetworkRes; finer grains fold deterministically ---
  {
    const SignalPartition w{PropagationGranularity::WholeWorld, kWholeWorldPartitionId};
    const SignalPartition c{PropagationGranularity::PerChunk, chunkPartitionId(4, -1)};
    const SignalPartition n{PropagationGranularity::PerNetwork, 42};
    const uint32_t rc = partitionResourceId(c), rn = partitionResourceId(n);
    const bool ok = partitionResourceId(w) == kSignalNetworkRes && rc != kSignalNetworkRes &&
                    rn != kSignalNetworkRes && partitionResourceId(c) == rc &&
                    partitionResourceId(n) == rn && partitionAccess(w).write &&
                    partitionAccess(w).resource == kSignalNetworkRes &&
                    partitionAccess(c).resource == rc;
    check(ok, "signal-propagate: whole-world maps to kSignalNetworkRes; finer grains fold to distinct, "
              "deterministic resource ids (Write access)");
  }
}

void testBlockNames() {
  std::printf("storage: block name table (namespaced ids, remap, legacy migration)\n");
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ll_blocknames_test";
  fs::remove_all(dir);
  BlockRegistry a;
  const BlockId rockA = a.add({"Rock", kSolid, Shape::Cube, "rock", {}, 0, 0, 0, 0.8f, false, 0, "test:rock"});
  const BlockId plantA = a.add({"Plant", kTransparent, Shape::Cross, "plant", {}, 0, 0, 0, 0.8f, false, 0, "test:plant"});
  const BlockId goneA = a.add({"Gone", kSolid, Shape::Cube, "gone", {}, 0, 0, 0, 0.8f, false, 0, "test:gone"});
  BlockRegistry b;  // a later version: new block first, order changed, "gone" removed
  const BlockId newB = b.add({"New", kSolid, Shape::Cube, "new", {}, 0, 0, 0, 0.8f, false, 0, "test:new"});
  const BlockId plantB = b.add({"Plant", kTransparent, Shape::Cross, "plant", {}, 0, 0, 0, 0.8f, false, 0, "test:plant"});
  const BlockId rockB = b.add({"Rock", kSolid, Shape::Cube, "rock", {}, 0, 0, 0, 0.8f, false, 0, "test:rock"});
  std::string err;
  const fs::path f = dir / "blocks.names";
  const bool wrote = storage::writeBlockNames(f, a, &err);
  const auto r = storage::readStableRemap(f, b, &err);
  ChunkColumn c;
  c.subs[0] = std::make_unique<SubChunk>();
  // Simulate BLKS decode output: save-time BlockIds (session A order: rock=1, plant=2, gone=3)
  // parked in loadScratch; remapColumn installs them as the new session's StableIds.
  c.subs[0]->loadScratch = std::vector<uint16_t>(kSubChunkVolume, 0);
  (*c.subs[0]->loadScratch)[0] = 1;  // rockA
  (*c.subs[0]->loadScratch)[1] = 2;  // plantA
  (*c.subs[0]->loadScratch)[2] = 3;  // goneA
  const bool remapped = r && storage::remapColumn(c, *r, &err);
  check(wrote && remapped && !r->legacy && !r->identity && r->unknown.size() == 1 && r->unknown[0] == "test:gone" &&
            c.subs[0]->at(0, 0, 0) == b.stableIdOf(rockB) && c.subs[0]->at(1, 0, 0) == b.stableIdOf(plantB) &&
            c.subs[0]->at(2, 0, 0) == kAirStableId && c.subs[0]->nonAir == 2 && newB != rockB,
        "block names: ids remap by namespaced name across a reordered registry; removed blocks load as air");
  const auto same = storage::readStableRemap(f, a, &err);
  check(same && same->identity && a.idOf(kAir) == "litholattice:air" && a.findById("test:plant") == plantA,
        "block names: same registry -> identity (no rewrite); air is litholattice:air");
  const auto legacy = storage::readStableRemap(dir / "missing.names", b, &err);
  check(legacy && legacy->legacy && legacy->map.size() == b.size(), "block names: no table = legacy save, migrated as the current order");
  {
    std::fstream io(f, std::ios::in | std::ios::out | std::ios::binary);
    io.seekp(14); io.put('\x55');
  }
  err.clear();
  check(!storage::readBlockRemap(f, b, &err) && !err.empty(), "block names: a damaged table is detected (CRC), not misread");
  fs::remove_all(dir);
}

void testAttachables() {
  std::printf("attachables: box models, attach faces, pop-off\n");
  std::string err;
  const auto ok = BoxModel::parse(R"({"asset":{"version":"ll-box-1"},"meshes":[{"boxes":[{"min":[7,0,7],"max":[9,10,9]}]}],
    "nodes":[{"mesh":0,"translation":[1,0,0],"rotation":[0,0.70710678,0,0.70710678]}]})", &err);
  const auto bad1 = BoxModel::parse(R"({"asset":{"version":"ll-box-1"},"meshes":[{"boxes":[{"min":[0,0,0],"max":[1,1,1]}]}],"nodes":[{"mesh":0,"scale":[2,2,2]}]})", &err);
  const auto bad2 = BoxModel::parse(R"({"asset":{"version":"ll-box-1"},"meshes":[{"boxes":[{"min":[0,0,0],"max":[1,1,1]}]}],"nodes":[{"mesh":0,"rotation":[0,0.3826834,0,0.9238795]}]})", &err);
  const auto bad3 = BoxModel::parse(R"({"asset":{"version":"ll-box-1"},"meshes":[{"boxes":[{"min":[4,0,0],"max":[4,1,1]}]}],"nodes":[{"mesh":0}]})", &err);
  bool parsed = ok && ok->nodes.size() == 1 && ok->nodes[0].quarterTurnsY == 1 && ok->nodes[0].translation[0] == 1;
  check(parsed && !bad1 && !bad2 && !bad3, "box model: glTF-subset JSON parses; scale, non-quarter rotations and empty boxes are rejected");
  BoxModel m;
  m.materials.push_back({"m", FaceSlot::Top});
  m.meshes.push_back({"stick", {{{7, 0, 7}, {9, 10, 9}, 0}}});
  m.nodes.push_back({0, {0, 0, 0}, 0});
  const auto top = m.orientedBoxes(1), ceil = m.orientedBoxes(2), north = m.orientedBoxes(3), east = m.orientedBoxes(6);
  check(top[0].min[1] == 0 && top[0].max[1] == 10 && ceil[0].min[1] == 6 && ceil[0].max[1] == 16 && north[0].min[2] == 0 &&
            north[0].max[2] == 10 && east[0].min[0] == 6 && east[0].max[0] == 16,
        "box model: oriented toward the support (top, ceiling, -Z wall, +X wall)");
  check(attachFaceFor(0, -1, 0) == 1 && attachFaceFor(1, 0, 0) == 6 && stAttach(withAttach(makeState(5, 2, 1, true), 4)) == 4 &&
            stValue(withAttach(makeState(5, 2, 1, true), 4)) == 5 && stOn(withAttach(makeState(5, 2, 1, true), 4)),
        "attach face: lives in state bits 13-15 without touching value / facing / on");

  BlockRegistry reg;
  const BlockId rock = reg.add({"Rock", kSolid | kOpaque, Shape::Cube, "rock"});
  reg.add({"Pad", kSolid, Shape::Cube, "pad"});  // keeps the knob's id clear of FixtureSim's vetoed kHalf
  reg.add({"Pad2", kSolid, Shape::Cube, "pad2"});
  const BlockId knob = reg.add({"Knob", kSolid | kTransparent | kAttachable, Shape::Cube, "knob"});
  ChunkMap w = worldWithColumns(1, &reg);
  w.set(8, 10, 8, rock);
  // The game removes blocks through command() here (a script / sim removal, not a player break).
  struct RemoverSim final : IWorldSimulation {
    void scan(const ChunkMap&) override {}
    void onBlockChanged(ChunkMap&, int, int, int, BlockId) override {}
    void tick(ChunkMap&, const TickArea&, const DirtyFn&) override {}
    void command(ChunkMap& w, const SimCommand& c, const DirtyFn& dirty) override { w.set(c.x, c.y, c.z, kAir); dirty(c.x, c.y, c.z); }
  };
  w.set(20, 10, 8, rock);
  w.set(20, 11, 8, knob); w.setState(20, 11, 8, withAttach(0, 1));  // on top of the second rock
  w.set(20, 12, 8, knob); w.setState(20, 12, 8, withAttach(0, 1));  // mounted on the first knob (cascade)
  SimConfig cfg; cfg.tickRate = 64; cfg.radiusChunks = 2;
  cfg.domains.push_back({"fixture", 64, true});
  SimThread st(w, reg, std::make_unique<RemoverSim>(), std::move(cfg), [](std::shared_ptr<MeshInput>) {});
  st.setPlayerChunk(0, 0);
  st.setRay({4.5f, 10.5f, 8.5f, 1, 0, 0});  // looking +X at the rock's -X face
  st.start();
  st.waitLoops(3);
  EditCmd place{EditCmd::Kind::Place}; place.block = knob;
  st.edit(place);
  st.waitLoops(4);
  const bool placed = w.get(7, 10, 8) == knob && stAttach(w.getState(7, 10, 8)) == 6;
  st.setRay({8.5f, 14.5f, 8.5f, 0, -1, 0});  // from above: break the rock (the support)
  st.waitLoops(4);
  st.edit(EditCmd{EditCmd::Kind::Break});
  st.waitLoops(4);
  EditCmd remove{EditCmd::Kind::Command}; remove.command = SimCommand{1, 20, 10, 8, 0, 0};
  st.edit(remove);
  st.waitLoops(4);
  st.stop();
  check(placed && w.get(8, 10, 8) == kAir && w.get(7, 10, 8) == kAir,
        "attachables: placed on the clicked face with its attach face; pops off when the support breaks");
  check(w.get(20, 10, 8) == kAir && w.get(20, 11, 8) == kAir && w.get(20, 12, 8) == kAir,
        "attachables: pop off when the game / a script removes the support too, cascading to mounted ones");
}

void testSim() {
  std::printf("sim: SimThread with a fixture IWorldSimulation\n");
  ChunkMap w = worldWithColumns(1, &fixtureBlocks());
  w.set(8, 10, 8, kRock);
  std::atomic<int> scans{0}, ticks{0}, changes{0}, commands{0}, meshes{0};
  auto sim = std::make_unique<FixtureSim>();
  sim->scans = &scans; sim->ticks = &ticks; sim->changes = &changes; sim->commands = &commands;
  SimConfig cfg;
  cfg.tickRate = 64; cfg.radiusChunks = 2; cfg.epoch = 3;
  cfg.domains.push_back({"fixture", 128, true});
  SimThread st(w, fixtureBlocks(), std::move(sim), std::move(cfg), [&](std::shared_ptr<MeshInput> in) { if (in->epoch == 3) ++meshes; });
  st.setPlayerChunk(0, 0);
  st.setRay({8.5f, 14.5f, 8.5f, 0, -1, 0});
  st.start();
  st.start();  // idempotent
  st.waitLoops(4);
  EditCmd place{EditCmd::Kind::Place}; place.block = kRock;
  st.edit(place);
  EditCmd refused{EditCmd::Kind::Place}; refused.block = kHalf;
  st.waitLoops(4);
  st.edit(refused);
  EditCmd cmd{EditCmd::Kind::Command}; cmd.command = SimCommand{7, 1, 2, 3, 99, 0};
  st.edit(cmd);
  st.waitLoops(4);
  st.toggleDomain(0);
  waitUntil([&] { const SimStats s = st.stats(); return ticks > 20 && s.domainCount == 1 && !s.domains[0].enabled && s.active == 42; });
  const SimStats stats = st.stats();
  std::vector<SimEvent> ev;
  st.drainEvents(ev);
  st.stop();
  st.stop();
  bool placeEv = false, cmdEv = false;
  for (const SimEvent& e : ev) { placeEv = placeEv || (e.kind == 1 && e.y == 11); cmdEv = cmdEv || (e.kind == 2 && e.value == 99); }
  check(scans == 1 && ticks > 20 && changes == 1 && commands == 1 && w.get(8, 11, 8) == kRock && w.get(8, 12, 8) == kAir,
        "SimThread: scan once, ticks, ray-targeted place, canPlace veto, game command routed");
  check(placeEv && cmdEv && meshes > 0, "SimThread: simulation events reach the game thread, remesh snapshots carry the epoch");
  check(stats.active == 42 && stats.domainCount == 1 && !stats.domains[0].enabled && stats.domains[0].hz == 128,
        "SimThread: stats report the simulation and the game's clock domain (toggled off)");
}

void testAnvilRegion() {
  namespace st = litholattice::storage;
  std::printf("storage: anvil-inverted region layer (sector alloc, commit protocol, crash safety)\n");
  const auto dir = std::filesystem::temp_directory_path() / "litholattice_anvilregion_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  std::mt19937_64 rng(0x5EEDBEEF);
  auto rnd = [&](size_t n) {
    std::vector<uint8_t> b(n);
    for (auto& x : b) x = uint8_t(rng());
    return b;
  };
  const uint8_t codecs[] = {st::AnvilRegion::kCodecNone, st::AnvilRegion::kCodecLZ4,
                            st::AnvilRegion::kCodecCustom, 9};
  {  // 1. round trip: 200 random payloads, 4 codec tags, byte-identical after reopen
    std::vector<std::vector<uint8_t>> payloads(200);
    std::vector<uint8_t> codecOf(200);
    {
      st::AnvilRegion r(dir / "r.0.0.llar", 0, 0);
      std::string err;
      bool ok = r.open(&err);
      std::ifstream mf(dir / "r.0.0.llar", std::ios::binary);
      char magic[4] = {};
      mf.read(magic, 4);
      check(ok && std::memcmp(magic, "LLAR", 4) == 0, "anvil region: open creates the file with the LLAR preamble");
      for (int i = 0; i < 200 && ok; ++i) {
        payloads[i] = rnd(1 + size_t(rng() % 6000));
        codecOf[i] = codecs[i % 4];
        ok = r.put(i % 32, i / 32, codecOf[i], payloads[i], &err);
      }
      check(ok, "anvil region: 200 puts across 200 slots");
    }
    {
      st::AnvilRegion r(dir / "r.0.0.llar", 0, 0);
      std::string err;
      check(r.open(&err) && r.count() == 200, "anvil region: reopen finds all 200 slots indexed");
      bool all = true;
      for (int i = 0; i < 200; ++i) {
        std::string re;
        const auto rec = r.read(i % 32, i / 32, &re);
        all = all && rec && rec->codec == codecOf[i] && rec->payload == payloads[i] && rec->generation > 0;
      }
      std::string e2;
      const auto absent = r.read(31, 31, &e2);
      check(all && !absent && e2.empty(),
            "anvil region: 200 random payloads round-trip byte-identical; a never-written slot reads absent, not corrupt");
    }
  }
  {  // 2. CRC32 catches a single flipped bit; neighbours unaffected
    const auto p = dir / "r.1.0.llar";
    uint32_t sector = 0;
    uint8_t count = 0;
    {
      st::AnvilRegion r(p, 1, 0);
      std::string err;
      r.open(&err);
      r.put(7, 9, st::AnvilRegion::kCodecLZ4, rnd(3000), &err);
      r.put(8, 9, st::AnvilRegion::kCodecNone, rnd(100), &err);
      check(r.locate(7, 9, &sector, &count) && count == 6, "anvil region: locate reports the index entry");
    }
    {
      std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
      const auto at = std::streamoff(sector) * 512 + 6 + 1500;  // inside the payload
      f.seekg(at);
      char c = 0;
      f.get(c);
      c ^= 0x01;
      f.clear();
      f.seekp(at);
      f.put(c);
    }
    {
      st::AnvilRegion r(p, 1, 0);
      std::string err;
      r.open(&err);
      std::string re, ne;
      const auto bad = r.read(7, 9, &re);
      const auto neighbour = r.read(8, 9, &ne);
      check(!bad && re.find("CRC") != std::string::npos && neighbour && neighbour->payload.size() == 100,
            "anvil region: a single flipped bit fails the CRC; the neighbouring record still reads");
    }
  }
  {  // 3. torn-write simulation: payload staged, crash before the index commit
    const auto p = dir / "r.2.0.llar";
    const auto v1 = rnd(2000), v2 = rnd(2500);
    {
      st::AnvilRegion r(p, 2, 0);
      std::string err;
      r.open(&err);
      check(r.put(3, 3, 3, v1, &err), "anvil region: baseline write commits");
      const auto s = r.stage(3, 3, 3, v2, &err);  // payload bytes flushed, index untouched...
      check(s.sector != 0 && err.empty(), "anvil region: replacement payload staged without commit");
      r.stage(4, 4, 3, v2, &err);  // ...and a brand-new slot staged, never committed
    }  // "crash": the region is destroyed with commits pending
    {
      st::AnvilRegion r(p, 2, 0);
      std::string err;
      r.open(&err);
      std::string re, re2;
      const auto rec = r.read(3, 3, &re);
      const auto missing = r.read(4, 4, &re2);
      check(rec && rec->payload == v1 && !missing && re2.empty(),
            "anvil region: torn write leaves the old record (stale, never garbage); the uncommitted slot reads absent");
    }
    {  // an index entry hand-pointed past EOF is normalized to absent, never garbage
      std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(16 + 4 * (4 * 32 + 3));  // location entry for slot (lx=3, lz=4)
      const uint8_t evil[4] = {0xFF, 0xFF, 0xFE, 0x01};  // sector 0xFFFFFE, 1 sector
      f.write(reinterpret_cast<const char*>(evil), 4);
    }
    {
      st::AnvilRegion r(p, 2, 0);
      std::string err;
      r.open(&err);
      std::string re;
      check(!r.read(3, 4, &re), "anvil region: an index entry past EOF reads absent, never garbage");
    }
  }
  {  // 4. sub-4KiB packing: 100 small records stay under 1 KiB/chunk (Anvil needs >= 4)
    const auto p = dir / "r.3.0.llar";
    st::AnvilRegion r(p, 3, 0);
    std::string err;
    r.open(&err);
    for (int i = 0; i < 100; ++i) r.put(i % 32, i / 32, 3, rnd(100), &err);
    const uint64_t bytes = uint64_t(r.fileSectors()) * st::AnvilRegion::kSector;
    // Anvil's 4 KiB sectors would need at least (2 header + 100) * 4096 = 417792 bytes.
    check(bytes < 100 * 1024, "anvil region: 100 x ~109-byte records pack into under 1 KiB/chunk all-in");
  }
  {  // 5. 32x32 lookup: every slot addressed directly through the index
    const auto p = dir / "r.4.0.llar";
    {
      st::AnvilRegion r(p, 4, 0);
      std::string err;
      r.open(&err);
      for (int lz = 0; lz < 32; ++lz)
        for (int lx = 0; lx < 32; ++lx) {
          std::vector<uint8_t> pay(size_t(16 + (lx + lz) % 64));
          pay[0] = uint8_t(lx);
          pay[1] = uint8_t(lz);
          for (size_t k = 2; k < pay.size(); ++k) pay[k] = uint8_t(k * 7 + lx);
          r.put(lx, lz, 3, pay, &err);
        }
      check(r.count() == 1024, "anvil region: all 1024 slots indexed");
    }
    {
      st::AnvilRegion r(p, 4, 0);
      std::string err;
      r.open(&err);
      bool all = true;
      for (int lz = 0; lz < 32 && all; ++lz)
        for (int lx = 0; lx < 32 && all; ++lx) {
          std::string re;
          const auto rec = r.read(lx, lz, &re);
          all = all && rec && rec->payload.size() == size_t(16 + (lx + lz) % 64) &&
                rec->payload[0] == uint8_t(lx) && rec->payload[1] == uint8_t(lz);
        }
      check(all, "anvil region: all 1024 slots read back correctly via direct index lookup");
    }
  }
  {  // 6. free bitmap is rebuilt from the index on open: freed sectors are reused
    const auto p = dir / "r.5.0.llar";
    size_t sectorsBefore = 0;
    {
      st::AnvilRegion r(p, 5, 0);
      std::string err;
      r.open(&err);
      for (int i = 0; i < 10; ++i) r.put(i, 0, 3, rnd(4000), &err);  // 4009 B -> 8 sectors each
      sectorsBefore = r.fileSectors();
      for (int i = 0; i < 5; ++i) r.erase(i, 0, &err);
      check(r.count() == 5 && r.fileSectors() == sectorsBefore, "anvil region: erase drops entries without growing the file");
    }
    {
      st::AnvilRegion r(p, 5, 0);  // reopen: bitmap derived purely from the surviving index
      std::string err;
      r.open(&err);
      for (int i = 10; i < 15; ++i) r.put(i, 0, 3, rnd(4000), &err);
      bool all = r.fileSectors() == sectorsBefore;  // freed sectors reused, file did not grow
      for (int i = 5; i < 15 && all; ++i) {
        std::string re;
        const auto rec = r.read(i, 0, &re);
        all = all && rec && rec->payload.size() > 3900;
      }
      std::string re;
      const auto gone = r.read(0, 0, &re);
      check(all && !gone && re.empty() && r.count() == 10,
            "anvil region: reopened bitmap reuses freed sectors (size unchanged); erased slots stay absent");
    }
  }
  {  // 7. oversized records spill to a sidecar; erase removes it
    const auto p = dir / "r.6.0.llar";
    const auto pay = rnd(st::AnvilRegion::kMaxInlineBytes + 1000);
    const auto sp = st::AnvilRegion::sidecarPath(p, 6 * 32, 0);
    {
      st::AnvilRegion r(p, 6, 0);
      std::string err;
      r.open(&err);
      check(r.put(0, 0, st::AnvilRegion::kCodecCustom, pay, &err) && std::filesystem::exists(sp),
            "anvil region: a record past the 255-sector cap spills to a sidecar file");
    }
    {
      st::AnvilRegion r(p, 6, 0);
      std::string err;
      r.open(&err);
      std::string re;
      const auto rec = r.read(0, 0, &re);
      const bool roundTrips = rec && rec->codec == st::AnvilRegion::kCodecCustom && rec->payload == pay;
      std::string ee;
      const bool erased = r.erase(0, 0, &ee);
      std::string re2;
      check(roundTrips && erased && !std::filesystem::exists(sp) && !r.read(0, 0, &re2) && re2.empty(),
            "anvil region: sidecar record round-trips; erase deletes the sidecar and the slot reads absent");
    }
  }
  {  // 8. timestamp table works as a dirty-generation vector
    const auto p = dir / "r.7.0.llar";
    st::AnvilRegion r(p, 7, 0);
    std::string err;
    r.open(&err);
    r.put(1, 1, 3, rnd(50), &err);
    const uint32_t g1 = r.generation(1, 1);
    r.put(1, 1, 3, rnd(50), &err);
    const uint32_t g2 = r.generation(1, 1);
    r.put(2, 2, 3, rnd(50), &err);
    const uint32_t g3 = r.generation(2, 2);
    check(g1 > 0 && g2 > g1 && g3 > g2 && r.generation(9, 9) == 0,
          "anvil region: dirty generations bump monotonically per write; untouched slots read 0");
  }
  {  // 9. a damaged file fails open and is never overwritten in place
    const auto p = dir / "r.9.0.llar";
    { std::ofstream f(p, std::ios::binary); f << "garbage"; }
    st::AnvilRegion r(p, 9, 0);
    std::string err;
    check(!r.open(&err) && !err.empty(), "anvil region: a damaged file fails open, kept for recovery");
  }
  std::filesystem::remove_all(dir);
}

void testFileContract() {
  namespace st = litholattice::storage;
  std::printf("storage: file contract (atomic writes, torn-write recovery, backups, framing, quarantine)\n");
  const auto dir = std::filesystem::temp_directory_path() / "litholattice_filecontract_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto p = dir / "data.bin";
  std::string err;
  st::FileWriteOptions opts;  // fsync on
  opts.keepBackups = 0;       // explicitly no backups for these cases
  const st::Bytes v1 = {1, 2, 3, 4, 5};
  check(st::writeFileAtomic(p, v1, &err, opts) && err.empty(), "file contract: atomic write succeeds");
  st::Bytes back;
  check(st::readFileAll(p, &back, &err) && back == v1, "file contract: read back byte-identical");
  {  // simulate a crash: a killed writer's garbage temp file must never become the target
    std::ofstream t(p.string() + st::kAtomicTmpSuffix, std::ios::binary);
    t << "torn garbage";
  }
  const st::Bytes v2 = {9, 9, 9};
  check(st::writeFileAtomic(p, v2, &err, opts), "file contract: write succeeds over a stale crash temp");
  check(st::readFileAll(p, &back, &err) && back == v2, "file contract: stale temp never became the target");
  check(!std::filesystem::exists(p.string() + st::kAtomicTmpSuffix), "file contract: no temp left behind");
  {  // a failed replacement leaves the old version intact and cleans its temp
    std::ofstream f(dir / "not_a_dir", std::ios::binary);
    f << "x";
    f.close();
    const auto bad = dir / "not_a_dir" / "x.bin";
    check(!st::writeFileAtomic(bad, v1, &err, opts) && !err.empty(), "file contract: failed write reports");
    check(st::readFileAll(p, &back, &err) && back == v2, "file contract: old version survives a failed write");
    check(!std::filesystem::exists(bad.string() + st::kAtomicTmpSuffix), "file contract: failed write cleans its temp");
  }
  {  // backup rotation: the N previous versions, rotated only on success
    const auto bp = dir / "critical.bin";
    st::FileWriteOptions bopts;
    bopts.keepBackups = 3;
    auto wv = [&](uint8_t v) { return st::writeFileAtomic(bp, st::Bytes{v}, &err, bopts); };
    auto rv = [&](const std::filesystem::path& q) {
      st::Bytes b;
      return st::readFileAll(q, &b, &err) ? (b.empty() ? -1 : int(b[0])) : -2;
    };
    check(wv(1) && wv(2) && wv(3) && wv(4), "file contract: four versioned writes succeed");
    check(rv(bp) == 4 && rv(bp.string() + ".bak.1") == 3 && rv(bp.string() + ".bak.2") == 2 &&
              rv(bp.string() + ".bak.3") == 1 && !std::filesystem::exists(bp.string() + ".bak.4"),
          "file contract: rotation keeps the 3 previous versions, drops the oldest");
    const auto bad = dir / "not_a_dir" / "y.bin";
    check(!st::writeFileAtomic(bad, st::Bytes{7}, &err, bopts), "file contract: versioned write can fail");
    check(rv(bp) == 4 && rv(bp.string() + ".bak.1") == 3, "file contract: a failed write rotates nothing");
  }
  {  // CRC32 framing: corruption is detected, quarantined (logged + counted), never returned
    const auto fp = dir / "rec.fram";
    const st::Bytes payload = {10, 20, 30, 40, 50};
    check(st::writeFileAtomic(fp, st::frameRecord(payload), &err, opts), "file contract: framed write");
    st::Quarantine q;
    check(st::readValidatedRecord(fp, &q, &err) == payload && q.count() == 0,
          "file contract: framed record validates");
    {  // flip one payload byte
      std::fstream f(fp, std::ios::in | std::ios::out | std::ios::binary);
      f.seekp(10);
      char c = 0;
      f.get(c);
      f.clear();
      f.seekp(10);
      f.put(char(c ^ 0x01));
    }
    std::string qerr;
    check(!st::readValidatedRecord(fp, &q, &qerr) && qerr.find("CRC") != std::string::npos && q.count() == 1,
          "file contract: CRC mismatch quarantines the file, never returns garbage");
    check(q.entries().size() == 1 && q.entries()[0].find("rec.fram") != std::string::npos,
          "file contract: quarantine entry names the file");
    std::string e2;
    check(!st::readValidatedRecord(dir / "nope.fram", &q, &e2) && e2.empty() && q.count() == 1,
          "file contract: a missing file reads absent (no error, no quarantine)");
    std::ofstream g(dir / "badmagic.fram", std::ios::binary);
    g << "not a framed record at all, just text........";
    g.close();
    std::string e3;
    check(!st::readValidatedRecord(dir / "badmagic.fram", &q, &e3) && q.count() == 2,
          "file contract: bad magic quarantines too");
  }
  std::filesystem::remove_all(dir);
}

void testBackupDepth() {
  namespace st = litholattice::storage;
  std::printf("storage: backup depth option (powers of two, 2..32, default 8)\n");
  check(st::kBackupDepthMin == 2 && st::kBackupDepthMax == 32 && st::kBackupDepthDefault == 8,
        "backup depth: option range is 2..32, default 8");
  for (unsigned d : {2u, 4u, 8u, 16u, 32u})
    check(st::isValidBackupDepth(d), "backup depth: every power of two in range is a valid option");
  for (unsigned d : {1u, 3u, 5u, 6u, 7u, 9u, 12u, 33u, 64u, 100u})
    check(!st::isValidBackupDepth(d), "backup depth: non-powers-of-two and out-of-range rejected");
  check(!st::isValidBackupDepth(0),
        "backup depth: 0 is not an option (it is the documented opt-out, not a dial position)");
  st::FileWriteOptions def;
  check(def.keepBackups == st::kBackupDepthDefault,
        "backup depth: FileWriteOptions defaults to 8 backups");
  {  // rotation honors the depth: 6 writes at depth 4 keep .bak.1..4, drop .bak.5
    const auto dir = std::filesystem::temp_directory_path() / "litholattice_backupdepth_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto bp = dir / "v.dat";
    std::string err;
    st::FileWriteOptions bopts;
    bopts.keepBackups = 4;
    auto wv = [&](uint8_t v) { return st::writeFileAtomic(bp, st::Bytes{v}, &err, bopts); };
    auto rv = [&](const std::filesystem::path& q) {
      st::Bytes b;
      return st::readFileAll(q, &b, &err) ? (b.empty() ? -1 : int(b[0])) : -2;
    };
    bool wok = true;
    for (uint8_t v = 1; v <= 6; ++v) wok = wok && wv(v);
    check(wok, "backup depth: six versioned writes succeed");
    check(rv(bp) == 6 && rv(bp.string() + ".bak.1") == 5 && rv(bp.string() + ".bak.2") == 4 &&
                rv(bp.string() + ".bak.3") == 3 && rv(bp.string() + ".bak.4") == 2 &&
                !std::filesystem::exists(bp.string() + ".bak.5"),
          "backup depth: depth 4 keeps 4 generations, drops the 5th");
    std::filesystem::remove_all(dir);
  }
}

void testAsyncIo() {
  namespace st = litholattice::storage;
  std::printf("storage: async I/O on the JobSystem (completion contract, strands, fences)\n");
  const auto dir = std::filesystem::temp_directory_path() / "litholattice_asyncio_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  JobSystem::Config cfg;
  cfg.workerThreads = 2;
  cfg.ioThreads = 2;
  JobSystem jobs(cfg);
  st::IoScheduler io(jobs);
  st::FileWriteOptions opts;
  {  // completions run on an I/O thread, never the submitting (sim stand-in) thread
    std::atomic<bool> cbOnJobThread{false}, cbOnSimThread{false};
    const std::thread::id simId = std::this_thread::get_id();
    st::IoFence fence(io);
    fence.submitWrite(dir / "a.bin", st::Bytes{1, 2, 3}, opts, [&](bool ok, std::string) {
      cbOnJobThread = JobSystem::onJobThread();
      cbOnSimThread = std::this_thread::get_id() == simId;
      if (!ok) throw std::runtime_error("write failed");
    });
    check(fence.wait() == 0, "async io: write completes through the fence");
    check(cbOnJobThread && !cbOnSimThread, "async io: completion runs on an I/O thread, never the submitter");
  }
  {  // per-path FIFO: same-path ops complete in submission order
    std::vector<int> order;
    std::mutex om;
    st::IoFence fence(io);
    const auto fp = dir / "fifo.bin";
    for (int i = 0; i < 20; ++i)
      fence.submitWrite(fp, st::Bytes{uint8_t(i)}, opts, [&, i](bool, std::string) {
        std::lock_guard<std::mutex> l(om);
        order.push_back(i);
      });
    check(fence.wait() == 0, "async io: 20 same-path writes all complete");
    bool fifo = order.size() == 20;
    for (size_t i = 0; i < order.size() && fifo; ++i) fifo = order[i] == int(i);
    st::Bytes back;
    std::string err;
    check(fifo && st::readFileAll(fp, &back, &err) && back == st::Bytes{19},
          "async io: same-path ops complete in submission order (last write wins)");
  }
  {  // submit() never blocks, even while the I/O threads are occupied
    std::atomic<bool> release{false};
    io.submit([&] { while (!release.load()) std::this_thread::yield(); }, [] {});
    const auto t0 = std::chrono::steady_clock::now();
    std::atomic<int> done{0};
    for (int i = 0; i < 8; ++i)
      io.submitWrite(dir / ("nb" + std::to_string(i) + ".bin"), st::Bytes{7}, opts,
                     [&](bool, std::string) { ++done; });
    const auto dt = std::chrono::steady_clock::now() - t0;
    check(dt < std::chrono::seconds(5), "async io: submits return at once while I/O threads are busy");
    release.store(true);
    jobs.waitIdle();
    check(done == 8, "async io: every submitted op completes");
  }
  {  // framed write + read round trip on one path; a missing file is a reported error, not a fence failure
    st::IoFence fence(io);
    std::string seen = "unset";
    fence.submitWriteRecord(dir / "rec.bin", st::Bytes{4, 5, 6}, opts, [](bool, std::string) {});
    fence.submitRead(dir / "rec.bin", [&](st::Bytes d, std::string e) {
      seen = e.empty() && d == st::frameRecord(st::Bytes{4, 5, 6}) ? "ok" : e;
    });
    fence.submitRead(dir / "missing.bin", [](st::Bytes, std::string) {});
    check(fence.wait() == 0, "async io: framed write + read round trip; missing file is not a fence failure");
    check(seen == "ok", "async io: read-after-write on the same path sees the write");
    st::Quarantine q;
    std::string err;
    check(st::readValidatedRecord(dir / "rec.bin", &q, &err).value() == st::Bytes({4, 5, 6}),
          "async io: framed record written async validates");
  }
  {  // a throwing callback counts as a failed op on the fence
    st::IoFence fence(io);
    io.submit([] {}, [] { throw std::runtime_error("boom"); }, &fence);
    check(fence.wait() == 1, "async io: a throwing callback counts as one failed op");
  }
  {  // WorldStore on a JobSystem: saves/loads/region writes go through submitIo()
    const auto wdir = dir / "world";
    st::PayloadRegistry reg;
    auto mkcol = [](int cx, int cz) {
      auto c = std::make_unique<ChunkColumn>();
      c->cx = cx;
      c->cz = cz;
      auto s = std::make_unique<SubChunk>();
      s->blocks.fill(BlockId(3));
      s->nonAir = kSubChunkVolume;
      c->subs[1] = std::move(s);
      return c;
    };
    {
      st::WorldStore store(wdir, reg, &jobs);
      for (int i = 0; i < 10; ++i) store.saveAsync(*mkcol(i, -i));
      store.flush();
      check(store.quarantinedCount() == 0, "async io: JobSystem-backed store saves with no quarantines");
    }
    {
      st::WorldStore store(wdir, reg, &jobs);
      std::string err;
      bool all = true;
      for (int i = 0; i < 10; ++i) {
        const auto c = store.load(i, -i, &err);
        all = all && c && c->cx == i && c->cz == -i && c->subs[1] && c->subs[1]->nonAir == kSubChunkVolume;
      }
      check(all, "async io: JobSystem-backed store round-trips 10 columns through submitIo()");
      std::ofstream f(st::WorldStore::regionPath(wdir, 9, 9), std::ios::binary | std::ios::trunc);
      f << "garbage";
      f.close();
    }
    {
      st::WorldStore store(wdir, reg, &jobs);
      std::string err;
      const auto c = store.load(9 * 32, 9 * 32, &err);  // region (9,9) is garbage now
      check(!c && store.quarantinedCount() == 1, "async io: corrupt region quarantined + counted, load skips it");
    }
  }
  std::filesystem::remove_all(dir);
}

void testModSandbox() {
  namespace st = litholattice::storage;
  std::printf("storage: mod sandbox (path traversal attacks)\n");
  const auto dir = std::filesystem::temp_directory_path() / "litholattice_sandbox_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "mod_a" / "data");
  {  // a file the mod is legitimately allowed to read
    std::ofstream f(dir / "mod_a" / "data" / "ok.txt", std::ios::binary);
    f << "hi";
  }
  const st::ModSandbox sb(dir / "mod_a");
  std::string err;
  auto resolves = [&](const std::string& p) {
    err.clear();
    return sb.resolve(p, &err).has_value();
  };
  {  // benign paths resolve to absolute in-root paths
    err.clear();
    const auto r = sb.resolve("data/ok.txt", &err);
    check(r && r->is_absolute() && r->filename() == "ok.txt", "sandbox: a plain relative path resolves");
    check(sb.resolve("data/../data/ok.txt", &err).has_value(), "sandbox: inner .. that stays inside resolves");
  }
  check(!resolves("../evil.txt") && !err.empty(), "sandbox: ../ escape rejected");
  check(!resolves("data/../../evil.txt"), "sandbox: nested ../ escape rejected");
  check(!resolves("/etc/passwd"), "sandbox: absolute path rejected");
  check(!resolves("C:/Windows/x"), "sandbox: drive-letter path rejected");
  check(!resolves(""), "sandbox: empty path rejected");
  {  // NUL byte would truncate the path at the OS layer
    err.clear();
    check(!sb.resolve(std::string("a\0b", 3), &err), "sandbox: NUL byte rejected");
  }
  if (!test_env::symlinksAvailable()) {
    test_env::skip("sandbox: symlink escape, prefix-sibling and loop checks", test_env::symlinkSkipReason());
  } else {
    std::error_code ec;
    std::filesystem::create_directory_symlink(dir, dir / "mod_a" / "link_out", ec);
    std::filesystem::create_directory_symlink(dir / "mod_a" / "data", dir / "mod_a" / "link_in", ec);
    check(!ec, "sandbox: test symlinks created");
    check(!resolves("link_out/evil.txt"), "sandbox: symlink pointing outside the root rejected");
    check(resolves("link_in/ok.txt"), "sandbox: symlink staying inside the root allowed");
    std::filesystem::create_directories(dir / "mod_a_evil");  // sibling sharing the root's name prefix
    std::filesystem::create_directory_symlink(dir / "mod_a_evil", dir / "mod_a" / "sib", ec);
    check(!resolves("sib/x"), "sandbox: symlink to a prefix-sharing sibling rejected");
    std::filesystem::create_symlink("loop_b", dir / "mod_a" / "loop_a", ec);
    std::filesystem::create_symlink("loop_a", dir / "mod_a" / "loop_b", ec);
    check(!resolves("loop_a/x"), "sandbox: symlink loop rejected");
  }
  std::filesystem::remove_all(dir);
}

void testSandboxedOpen() {
  namespace st = litholattice::storage;
  std::printf("storage: sandboxed open (O_NOFOLLOW + fd re-validation vs symlink races)\n");
  const auto dir = std::filesystem::temp_directory_path() / "litholattice_sandboxopen_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "mod" / "data");
  std::filesystem::create_directories(dir / "outside");
  {  // a file the mod may read, and attacker content outside the root
    std::ofstream f(dir / "mod" / "data" / "ok.txt", std::ios::binary);
    f << "hello";
  }
  {
    std::ofstream f(dir / "outside" / "evil.txt", std::ios::binary);
    f << "evil";
  }
  const st::ModSandbox sb(dir / "mod");
  std::string err;
  auto asString = [](const st::Bytes& b) { return std::string(b.begin(), b.end()); };

  {  // 1. the benign path: resolve + hardened open + read
    err.clear();
    const auto data = st::readSandboxedFile(sb, "data/ok.txt", &err);
    check(data && asString(*data) == "hello", "sandboxed open: normal read works");
  }
  const bool links = test_env::symlinksAvailable();
  if (!links)
    test_env::skip("sandboxed open: symlink-swap race checks (cases 2 and 3)", test_env::symlinkSkipReason());
  if (links) {  // 2. trailing-component swap: resolve a real file, then the attacker replaces
      // the validated file itself with a symlink pointing outside. (resolve()
      // canonicalizes, so a swapped *link* never survives in the returned path —
      // the dangerous race is the validated file becoming a link.)
    err.clear();
    const auto resolved = sb.resolve("data/ok.txt", &err);
    check(resolved.has_value(), "sandboxed open: real file resolves");
    // the race: swap the validated file for a symlink to the outside
    std::error_code ec;
    std::filesystem::remove(dir / "mod" / "data" / "ok.txt", ec);
    std::filesystem::create_symlink(dir / "outside" / "evil.txt", dir / "mod" / "data" / "ok.txt", ec);
    check(!ec, "sandboxed open: attacker swapped the file for a symlink");
    err.clear();
    const auto f = st::SandboxedFile::openRead(*resolved, sb, &err);
#if defined(__linux__)
    check(!f, "sandboxed open: swapped trailing symlink refused (O_NOFOLLOW)");
    check(!err.empty(), "sandboxed open: refusal explains itself");
#else
    (void)f;  // fallback path documents the gap; nothing asserted
#endif
    // restore the legit file for the remaining tests
    std::filesystem::remove(dir / "mod" / "data" / "ok.txt", ec);
    {
      std::ofstream rf(dir / "mod" / "data" / "ok.txt", std::ios::binary);
      rf << "hello";
    }
    err.clear();
    const auto again = st::readSandboxedFile(sb, "data/ok.txt", &err);
    check(again && asString(*again) == "hello",
          "sandboxed open: legit file readable after the attack");
  }
  if (links) {  // 3. intermediate-component swap: O_NOFOLLOW cannot see this one; the fd
      // containment re-check must catch it.
    std::filesystem::create_directories(dir / "mod" / "sub");
    std::filesystem::create_directories(dir / "outside" / "sub");
    {
      std::ofstream f(dir / "mod" / "sub" / "file.txt", std::ios::binary);
      f << "inner";
    }
    err.clear();
    const auto resolved = sb.resolve("sub/file.txt", &err);
    check(resolved.has_value(), "sandboxed open: nested path resolves while benign");
    // the race: replace the intermediate directory with a symlink outside
    std::error_code ec;
    std::filesystem::remove_all(dir / "mod" / "sub", ec);
    {
      std::ofstream f(dir / "outside" / "sub" / "file.txt", std::ios::binary);
      f << "evil-inner";
    }
    std::filesystem::create_symlink(dir / "outside" / "sub", dir / "mod" / "sub", ec);
    check(!ec, "sandboxed open: attacker swapped the intermediate dir");
    err.clear();
    const auto f = st::SandboxedFile::openRead(*resolved, sb, &err);
#if defined(__linux__)
    check(!f, "sandboxed open: intermediate-component swap caught by fd re-check");
    check(err.find("sandbox") != std::string::npos,
          "sandboxed open: refusal names the sandbox");
#else
    (void)f;  // fallback path documents the gap; nothing asserted
#endif
  }
  {  // 4. non-regular files are refused even when they resolve inside the root
    err.clear();
    const auto d = st::SandboxedFile::openRead(dir / "mod" / "data", sb, &err);
    check(!d, "sandboxed open: directory refused (not a regular file)");
#if defined(__linux__)
    const auto fifo = dir / "mod" / "data" / "pipe";
    ::mkfifo(fifo.c_str(), 0600);
    err.clear();
    const auto p = st::SandboxedFile::openRead(fifo, sb, &err);
    check(!p, "sandboxed open: FIFO refused (not a regular file)");
#endif
  }
  {  // 5. traversal never reaches the open path
    err.clear();
    check(!st::readSandboxedFile(sb, "../outside/evil.txt", &err) && !err.empty(),
          "sandboxed open: traversal rejected end-to-end");
  }
  std::filesystem::remove_all(dir);
}

void testStorage() {
  namespace st = litholattice::storage;
  std::printf("storage: codec, region files, payload migration, sparse world\n");
  const auto dir = std::filesystem::temp_directory_path() / "litholattice_storage_test";
  std::filesystem::remove_all(dir);
  const BlockRegistry& reg = fixtureBlocks();
  // Two-phase load contract (same as the game): decode parks the voxels, remapColumn installs them.
  const storage::StableRemap idremap = identityStableRemap(reg);
  auto column = [&](int cx, int cz) {
    auto c = std::make_unique<ChunkColumn>(); c->cx = cx; c->cz = cz;
    auto s = std::make_unique<SubChunk>();
    for (int i = 0; i < kSubChunkVolume; ++i) if (i % 7 == 0) {
      s->blocks.set(size_t(i), reg.stableIdOf(BlockId(1 + i % 3)));  // StableId currency
      s->state[size_t(i)] = CellState(i & 63); ++s->nonAir;
    }
    c->subs[3] = std::move(s);
    auto u = std::make_unique<SubChunk>(); u->blocks.fill(reg.stableIdOf(kRock)); u->nonAir = kSubChunkVolume; c->subs[0] = std::move(u);
    return c;
  };
  auto same = [](const ChunkColumn& a, const ChunkColumn& b) {
    for (int s = 0; s < kSubChunks; ++s) {
      const bool ea = !a.subs[size_t(s)] || !a.subs[size_t(s)]->nonAir, eb = !b.subs[size_t(s)];
      if (ea != eb) return false;
      if (!ea && (a.subs[size_t(s)]->blocks != b.subs[size_t(s)]->blocks || a.subs[size_t(s)]->state != b.subs[size_t(s)]->state ||
                  a.subs[size_t(s)]->nonAir != b.subs[size_t(s)]->nonAir)) return false;
    }
    return a.cx == b.cx && a.cz == b.cz;
  };
  // Game payload, version 2 (v1 stored one byte, v2 two): SDK-style registration + migration.
  int loadedSignal = -1;
  auto signalType = [&](uint16_t version) {
    st::PayloadType t; t.tag = st::fourcc("TEST"); t.version = version;
    t.save = [version](const ChunkColumn&) { return version == 1 ? st::Bytes{7} : st::Bytes{7, 9}; };
    t.load = [&loadedSignal](const st::Bytes& b, ChunkColumn&) { if (b.size() != 2) return false; loadedSignal = b[0] * 100 + b[1]; return true; };
    t.migrate[1] = [](const st::Bytes& b) -> std::optional<st::Bytes> { st::Bytes o = b; o.push_back(9); return o; };
    return t;
  };
  st::PayloadRegistry v1, v2;
  v1.setRegistry(&reg); v2.setRegistry(&reg);  // encode translates StableId -> save-time BlockId
  v1.add(signalType(1)); v2.add(signalType(2));
  {
    const auto a = column(5, -3);
    const st::Bytes rec = v2.encode(*a);
    std::string err;
    const auto b = v2.decode(rec, &err);
    const bool bremap = b && st::remapColumn(*b, idremap, &err);
    check(b && bremap && same(*a, *b) && loadedSignal == 709 && rec.size() < 6000, "record round trip (palette + bit-packed: 2 sub-chunks in under 6 KB instead of 32 KB raw)");
    loadedSignal = -1;
    const auto m = v2.decode(v1.encode(*a), &err);
    check(m && loadedSignal == 709, "old payload version is migrated up the chain on load");
    st::PayloadRegistry old; old.add(signalType(1));
    const auto newer = old.decode(rec, &err);
    check(!newer && !err.empty(), "a payload newer than the build is refused, not misread");
    st::Bytes bad = rec; bad[bad.size() / 2] ^= 0xFF; bad.resize(bad.size() - 3);
    check(!v2.decode(bad, &err), "a truncated / damaged record is rejected");
  }
  {
    st::WorldStore store(dir, v2);
    for (int i = 0; i < 40; ++i) store.saveAsync(*column(i * 37 - 700, -i * 53));
    store.saveAsync(*column(1000000, -1000000));
    store.saveAsync(*column(-2000000000 / 16, 2000000000 / 16));
    store.flush();
  }
  size_t regionFiles = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir)) regionFiles += e.path().extension() == ".llr";
  {
    st::WorldStore store(dir, v2);
    std::string err;
    bool all = true;
    for (int i = 0; i < 40; ++i) {
      const auto c = store.load(i * 37 - 700, -i * 53, &err);
      all = all && c && st::remapColumn(*c, idremap, &err) && same(*c, *column(i * 37 - 700, -i * 53));
    }
    const auto far = store.load(-2000000000 / 16, 2000000000 / 16, &err);
    const auto none = store.load(3, 3, &err);
    std::atomic<int> async{0};
    store.loadAsync(1000000, -1000000, [&](std::unique_ptr<ChunkColumn> c, std::string) { if (c && c->cx == 1000000) ++async; });
    store.flush();
    check(all && far && !none && async == 1 && regionFiles == 42, "sparse world: 42 columns up to 2e9 blocks out, one region file each, async load");
  }
  {  // flip a byte inside a stored record: the CRC catches it, neighbours still load
    const int cx = -700, cz = 0;  // the only column in region (-22, 0)
    const auto p = st::WorldStore::regionPath(dir, -22, 0);
    std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(0, std::ios::end); const auto size = f.tellg();
    f.seekp(size - std::streamoff(20)); f.put(char(0x5A)); f.close();
    st::WorldStore store(dir, v2);
    std::string err;
    const auto c = store.load(cx, cz, &err);
    check(!c && err.find("CRC") != std::string::npos, "corruption detected by the record CRC");
    std::ofstream(st::WorldStore::regionPath(dir, 77, 77), std::ios::binary) << "garbage";
    store.saveAsync(*column(77 * 32, 77 * 32));
    store.flush();
    std::ifstream g(st::WorldStore::regionPath(dir, 77, 77), std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(g)), std::istreambuf_iterator<char>());
    check(content == "garbage" && !std::filesystem::exists(st::WorldStore::regionPath(dir, 77, 77).string() + ".tmp"),
          "an unreadable region file is never overwritten (kept for recovery)");
  }
  testAnvilRegion();
  testFileContract();
  testBackupDepth();
  testAsyncIo();
  testModSandbox();
  testSandboxedOpen();
  std::filesystem::remove_all(dir);
}

void testMods() {
  std::printf("mods: mod.json schema, validation, dependency order\n");
  std::string err;
  auto mk = [&](const std::string& j) { return ModManifest::parse(j, &err); };
  const auto content = mk(R"({"id":"lithokraft","version":"0.1.0","deps":[{"id":"litholattice","version":">=0.1.0"}]})");
  const auto script = mk(R"({"id":"doors","version":"1.2.0","type":"script","deps":[{"id":"lithokraft","version":"^0.1.0"}],
    "permissions":[{"api":"world.write","reason":"Opens doors"}]})");
  check(content && content->type == ModType::Content && !content->needsPermissionPrompt() && script && script->needsPermissionPrompt() &&
            script->permissions[0].reason == "Opens doors",
        "mod.json: content mods load silently; script mods carry permissions with reasons");
  const bool rejects = !mk(R"({"id":"Bad-Id","version":"1.0.0"})") && !mk(R"({"id":"a","version":"1.0"})") &&
                       !mk(R"({"id":"a","version":"1.0.0","type":"dll"})") &&
                       !mk(R"({"id":"a","version":"1.0.0","permissions":[{"api":"world.write","reason":"x"}]})") &&
                       !mk(R"({"id":"a","version":"1.0.0","type":"script","permissions":[{"api":"world.write"}]})") &&
                       !mk(R"({"id":"a","version":"1.0.0","type":"script","permissions":[{"api":"rootkit","reason":"x"}]})") &&
                       !mk(R"({"id":"a","version":"1.0.0","schema":2})");
  check(rejects, "mod.json: bad id, version, type, permission on content, missing reason, unknown api, schema are rejected");
  const auto hs = mk(R"({"id":"proto","version":"1.0.0","hot_swap":true})");
  const auto sealed = mk(R"({"id":"locked","version":"1.0.0","sealed":true,"hot_swap":true})");
  const auto plain = mk(R"({"id":"plain","version":"1.0.0"})");
  check(hs && hs->hotSwap && !hs->sealed && sealed && sealed->sealed && sealed->hotSwap && plain && !plain->hotSwap && !plain->sealed,
        "mod.json: hot_swap and sealed flags parse; sealed wins documented when both set");
  check(versionSatisfies({1, 4, 0}, "^1.2.0") && !versionSatisfies({2, 0, 0}, "^1.2.0") && !versionSatisfies({0, 2, 0}, "^0.1.0") &&
            versionSatisfies({0, 1, 5}, "^0.1.0") && versionSatisfies({3, 0, 0}, ">=1.0.0") && versionSatisfies({1, 0, 0}, "*"),
        "mod.json: version ranges (*, =, >=, ^)");
  ModManifest engine = *mk(R"({"id":"litholattice","version":"0.1.0"})");
  std::vector<ModManifest> mods = {*script, *content, *mk(R"({"id":"needs_new","version":"1.0.0","deps":[{"id":"lithokraft","version":">=2.0.0"}]})"),
                                   *mk(R"({"id":"cyc_a","version":"1.0.0","deps":[{"id":"cyc_b"}]})"),
                                   *mk(R"({"id":"cyc_b","version":"1.0.0","deps":[{"id":"cyc_a"}]})"), *content};
  const ModSet set = resolveMods(mods, {engine});
  const bool order = set.ordered.size() == 2 && set.ordered[0].id == "lithokraft" && set.ordered[1].id == "doors";
  check(order && set.errors.size() >= 3, "mods: dependencies load first; out-of-range deps, cycles and duplicates are left out with errors");
  const auto dir = std::filesystem::temp_directory_path() / "ll_mod_test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir / "b_pack");
  std::filesystem::create_directories(dir / "a_broken");
  std::ofstream(dir / "b_pack" / "mod.json") << R"({"id":"pack","version":"1.0.0"})";
  std::ofstream(dir / "a_broken" / "mod.json") << "{nope";
  const ModSet loaded = loadModDirectory(dir, {engine});
  check(loaded.ordered.size() == 1 && loaded.ordered[0].id == "pack" && loaded.errors.size() == 1 && loaded.ordered[0].dir == dir / "b_pack",
        "mods: loadModDirectory reads <dir>/*/mod.json and reports malformed ones");
  ModSet mixed = set;
  for (ModManifest& m : mixed.ordered) m.dir = dir / m.id;
  const auto packs = contentModPacks(mixed);
  check(packs.size() == 1 && packs[0] == dir / "lithokraft" && contentModPacks(loaded).size() == 1,
        "mods: content mods (only) are mounted into the pack stack, in load order");
  std::filesystem::remove_all(dir);
}

void testAi() {
  using namespace litholattice::ai;
  std::printf("ai: walk cache, movement modes, A* hops, senses, brains, tick budget / LOD / cap, determinism\n");
  BlockRegistry reg;
  const BlockId rock = reg.add({"Rock", kSolid | kOpaque, Shape::Cube, "rock"});
  const BlockId wire = reg.add({"Track", kTransparent, Shape::Custom, "track"});
  ChunkMap w = worldWithColumns(8, &reg);  // 8x8 columns: x, z in 0..127
  for (int z = 0; z < 128; ++z) for (int x = 0; x < 128; ++x) w.set(x, 9, z, rock);  // floor; walk at y = 10
  for (int z = 0; z < 30; ++z) { w.set(20, 10, z, rock); w.set(20, 11, z, rock); }  // a 2-high wall with a gap at z >= 30
  w.set(5, 10, 5, rock);                                                             // a 1-high step
  for (int x = 40; x < 60; ++x) w.set(x, 10, 70, wire);                               // a wire track
  NavRules rules;
  rules.track = [&](BlockId b) { return b == wire; };
  WalkCache nav(w, reg, rules);
  check((nav.bits(3, 10, 3) & kNavStandable) && !(nav.bits(20, 10, 3) & kNavPassable) && (nav.bits(5, 11, 5) & kNavStandable),
        "ai: walk cache: floor standable, wall blocked, step top standable");
  const size_t cols = nav.cachedColumns();
  nav.invalidate(20, 3);
  check(cols >= 1 && nav.cachedColumns() == cols - 1, "ai: walk cache is per chunk column and invalidates on edit");

  auto walk = makeWalk(), fly = makeFly(), track = makeWireFollow(), climb = makeClimb(), swim = makeSwim();
  const Path around = findPath(nav, {{10, 10, 5}, {30, 10, 5}, walk.get(), 48});
  bool legal = around.complete && around.cells.front() == (Cell{10, 10, 5}) && around.cells.back() == (Cell{30, 10, 5});
  for (const Cell& c : around.cells) legal = legal && (nav.bits(c.x, c.y, c.z) & kNavStandable);
  check(legal && around.cells.size() > 40, "ai: A* walks around the wall (through the gap), every cell standable");
  const Path up = findPath(nav, {{4, 10, 5}, {6, 10, 5}, walk.get(), 48});
  bool stepped = up.complete;
  for (const Cell& c : up.cells) stepped = stepped || c.y == 11;
  check(up.complete && std::any_of(up.cells.begin(), up.cells.end(), [](const Cell& c) { return c.y == 11; }) && stepped,
        "ai: walk steps up one block (over the step rather than around)");
  const Path hop = findPath(nav, {{2, 10, 2}, hopToward({2, 10, 2}, {120, 10, 120}, 40), walk.get(), 48});
  check(hop.complete && hop.cells.back().x <= 42 && hopToward({0, 0, 0}, {100, 0, 0}, 48).x == 48,
        "ai: long trips plan in hops (<= 48 blocks per plan)");
  const Path over = findPath(nav, {{18, 10, 5}, {22, 10, 5}, fly.get(), 48});
  check(over.complete && over.cells.size() < 12, "ai: fly goes over the wall");
  const Path rail = findPath(nav, {{40, 10, 70}, {59, 10, 70}, track.get(), 48});
  const Path offRail = findPath(nav, {{40, 10, 70}, {40, 10, 75}, track.get(), 48});
  check(rail.complete && rail.cells.size() == 20 && !offRail.complete, "ai: wire-follow stays on the track");
  check(climb->name() == "climb" && swim->name() == "swim", "ai: climb and swim modes registered");

  auto opaque = [&](BlockId b) { return reg.has(b, kOpaque); };
  check(!canSee(w, opaque, {18.5f, 10.5f, 5.5f}, {1, 0, 0}, {22.5f, 10.5f, 5.5f}) &&
            canSee(w, opaque, {18.5f, 10.5f, 40.5f}, {1, 0, 0}, {22.5f, 10.5f, 40.5f}) &&
            !canSee(w, opaque, {18.5f, 10.5f, 40.5f}, {-1, 0, 0}, {22.5f, 10.5f, 40.5f}, {24.f, 0.5f}),
        "ai: sight is occluded by walls (no wallhacks), clear in the open, limited by the view cone");
  const float open = hear(w, opaque, {18.5f, 10.5f, 40.5f}, {{22.5f, 10.5f, 40.5f}, 16.f}),
              walled = hear(w, opaque, {18.5f, 10.5f, 5.5f}, {{22.5f, 10.5f, 5.5f}, 16.f});
  check(open > 0.7f && walled > 0 && walled < open * 0.6f, "ai: hearing falls off with distance and is muffled by walls");
  const FieldHit heat = senseField([](int x, int y, int z) { return x == 3 && y == 1 && z == -2 ? 5.f : 0.f; }, {0, 0, 0}, 4);
  check(heat.value == 5.f && heat.at == (Cell{3, 1, -2}), "ai: field sense (signal / heat) finds the strongest cell");

  // Brains: utility pick with a learned-scorer override, BT composition.
  struct Flip final : ILearnedScorer {
    std::optional<float> score(const AiContext&, const UtilityAction& a) const override { return a.name == "flee" ? 1.f : 0.f; }
  };
  AiWorld world(nav, {}, nullptr);
  const uint32_t a = *world.spawn({2, 10, 2}, walk.get());
  AiContext ctx{world, *world.mob(a), AiRng{1}, 0, 1.f, {}};
  std::vector<UtilityAction> acts = {{"eat", {[](const AiContext&) { return 0.9f; }}, 1.f, {}},
                                     {"flee", {[](const AiContext&) { return 0.2f; }}, 1.f, {}}};
  Flip flip;
  NullLearnedScorer none;
  check(pickUtility(ctx, acts) == 0 && pickUtility(ctx, acts, &none) == 0 && pickUtility(ctx, acts, &flip) == 1,
        "ai: utility AI picks the best score; the learned-model hook can override it (stub = no change)");
  int ran = 0;
  std::vector<BtPtr> seq;
  seq.push_back(btCondition([](const AiContext&) { return true; }));
  seq.push_back(btAction([&](AiContext&) { ++ran; return BtStatus::Success; }));
  std::vector<BtPtr> sel;
  sel.push_back(btInverter(btCondition([](const AiContext&) { return true; })));
  sel.push_back(btSequence(std::move(seq)));
  BtPtr tree = btSelector(std::move(sel));
  check(tree->tick(ctx) == BtStatus::Success && ran == 1, "ai: behaviour tree selector / sequence / inverter / condition / action");

  // Cap, waypoints, boss hooks, LOD, budget, determinism across thread counts.
  AiConfig cap; cap.mobCap = 500;
  AiWorld big(nav, cap);
  int spawned = 0;
  while (big.spawn({1, 10, 1}, walk.get())) ++spawned;
  AiWorld small(nav, {});
  int spawned64 = 0;
  while (small.spawn({1, 10, 1}, walk.get())) ++spawned64;
  check(spawned == 128 && spawned64 == 64, "ai: mob cap 64 by default, configurable up to 128");
  struct NoDiamonds final : IBossSlot {
    bool allowDrop(const Mob&, uint32_t item) const override { return item != 7; }
  };
  NoDiamonds gate;
  world.setBossSlot(&gate);
  SurgeSchedule surge(10, 3, 2.f);
  check(!world.requestDrop(a, 7) && world.requestDrop(a, 3) && surge.surge(1) == 2.f && surge.surge(5) == 1.f,
        "ai: final-boss slot: drop gating and surge schedule hooks");
  world.setWaypoint("arena", {30, 10, 40});
  check(world.waypoint("arena") && !world.waypoint("nowhere"), "ai: named waypoint targets");

  auto run = [&](JobPool* pool, std::vector<Cell>& end) {
    AiConfig c; c.pathsPerTick = 8; c.seed = 42;
    AiWorld aw(nav, c, pool);
    aw.setWaypoint("arena", {100, 10, 100});
    for (int i = 0; i < 64; ++i) {
      std::vector<UtilityAction> brainActs = {
          {"wander", {[](const AiContext&) { return 0.5f; }}, 1.f, [](AiContext& x) {
             if (!x.mob.goal) x.mob.goal = Cell{int(x.rng.next() % 120) + 2, 10, int(x.rng.next() % 120) + 2};
           }},
          {"arena", {[](const AiContext& x) { return x.mob.id % 4 == 0 ? 0.9f : 0.f; }}, 1.f, [](AiContext& x) {
             if (!x.mob.goal) x.mob.goal = x.world.waypoint("arena");
           }}};
      aw.spawn({2 + (i % 8) * 3, 10, 2 + (i / 8) * 3}, walk.get(), btUtility(std::move(brainActs)));
    }
    aw.setObservers({{0, 10, 0}});
    int deferred = 0;
    for (int t = 0; t < 60; ++t) { aw.tick(); deferred = std::max(deferred, aw.stats().pathsDeferred); }
    for (const auto& [id, m] : aw.mobs()) end.push_back(m.pos);
    return deferred;
  };
  std::vector<Cell> e1, e4;
  const int deferred = run(nullptr, e1);
  JobPool pool(4);
  run(&pool, e4);
  bool moved = false;
  for (size_t i = 0; i < e1.size(); ++i) moved = moved || !(e1[i] == (Cell{2 + int(i % 8) * 3, 10, 2 + int(i / 8) * 3}));
  check(e1 == e4 && moved, "ai: deterministic: 64 mobs end in identical cells with 1 or 4 path threads");
  check(deferred > 0, "ai: path budget: requests over the per-tick budget wait their turn");
  AiConfig hz; hz.hz = 40;
  AiWorld clamped(nav, hz);
  clamped.advance(64, 64);
  AiWorld lod(nav, {});
  const uint32_t far = *lod.spawn({120, 10, 120}, walk.get(), btAction([](AiContext&) { return BtStatus::Success; }));
  const uint32_t near = *lod.spawn({1, 10, 1}, walk.get(), btAction([](AiContext&) { return BtStatus::Success; }));
  lod.setObservers({{0, 10, 0}});
  for (int t = 0; t < 8; ++t) lod.tick();
  check(clamped.config().hz == 16 && clamped.stats().ticks == 16 && lod.mob(near)->thoughts == 8 && lod.mob(far)->thoughts == 2,
        "ai: tick rate clamped to 8-16 Hz; distance LOD thinks far mobs every 4th tick");

  // H-AI-1 regression pin: a brain that despawns ITSELF mid-tick must not
  // invalidate the tick loop (the fix defers the erase to the end of tick).
  // The world survives, the mob is gone after the tick, its tick-mate is
  // untouched, duplicate despawn requests are harmless, and the world keeps
  // ticking afterwards.
  {
    AiWorld selfKill(nav, {});
    uint32_t selfId = 0;
    BtPtr suicide = btAction([&](AiContext& x) {
      x.world.despawn(x.mob.id);
      x.world.despawn(x.mob.id);  // duplicate request: harmless (deferred list)
      return BtStatus::Success;
    });
    selfId = *selfKill.spawn({3, 10, 3}, walk.get(), std::move(suicide));
    const uint32_t bystander = *selfKill.spawn({4, 10, 4}, walk.get(),
                                               btAction([](AiContext&) { return BtStatus::Success; }));
    selfKill.tick();
    check(selfKill.mob(selfId) == nullptr && selfKill.mob(bystander) != nullptr,
          "ai: self-despawn during tick is deferred and safe (H-AI-1)");
    selfKill.tick();
    check(selfKill.mob(bystander) != nullptr && selfKill.stats().mobs == 1,
          "ai: world keeps ticking after a self-despawn");
  }

  // ---- P1 (2026-10-14): fixed-point costs, step tags, diagonals, Leap ----
  {
    auto leap = makeLeap();
    check(leap->name() == "leap", "ai: leap mode registered");
    // Diagonals: an open-floor 4+4 trip is 4 diagonal steps (5 cells), and
    // the step tags stay parallel to the cells.
    const Path diag = findPath(nav, {{90, 10, 90}, {94, 10, 94}, walk.get(), 48});
    check(diag.complete && diag.cells.size() == 5 && diag.steps.size() == diag.cells.size() &&
              diag.steps.front() == StepTag::kStepStart,
          "ai: diagonal: open-floor trip uses diagonals (5 cells, tags parallel)");
    // Corner-cut guard: two rocks touching diagonally forbid the squeeze.
    w.set(95, 10, 95, rock);
    w.set(94, 10, 96, rock);
    nav.invalidate(95, 95);
    nav.invalidate(94, 96);
    const Path sq = findPath(nav, {{94, 10, 95}, {95, 10, 96}, walk.get(), 48});
    bool squeezes = false;
    for (size_t i = 1; i < sq.cells.size(); ++i)
      squeezes = squeezes || (sq.cells[i - 1] == (Cell{94, 10, 95}) && sq.cells[i] == (Cell{95, 10, 96}));
    check(sq.complete && !squeezes, "ai: diagonal corner-cut guard: no squeezing between diagonal rocks");
    w.set(95, 10, 95, kAir);
    w.set(94, 10, 96, kAir);
    nav.invalidate(95, 95);
    nav.invalidate(94, 96);
  }
  {
    // Leap fixture: its own small world (floor y=9, walk at y=10).
    BlockRegistry reg2;
    const BlockId rock2 = reg2.add({"Rock", kSolid | kOpaque, Shape::Cube, "rock"});
    const BlockId ladder = reg2.add({"Ladder", kTransparent, Shape::Cross, "ladder"});
    ChunkMap lw = worldWithColumns(4, &reg2);  // x, z in 0..63
    for (int z = 0; z < 64; ++z)
      for (int x = 0; x < 64; ++x) lw.set(x, 9, z, rock2);
    for (int z = 4; z < 28; ++z) { lw.set(8, 10, z, rock2); lw.set(8, 11, z, rock2); }       // 2-high wall, no gap in range
    for (int z = 25; z < 46; ++z)
      for (int x = 20; x < 23; ++x) lw.set(x, 9, z, kAir);                                   // 3-wide ditch (floor removed)
    lw.set(40, 16, 35, rock2);                                                               // pillar top (stand at y=17)
    for (int z = 44; z < 49; ++z)
      for (int y = 10; y <= 14; ++y) lw.set(50, y, z, rock2);                                // 5-high wall
    for (int y = 10; y <= 13; ++y) lw.set(49, y, 45, ladder);                                 // ladder column on its face
    NavRules r2;
    r2.climbable = [&](BlockId b) { return b == ladder; };
    WalkCache lnav(lw, reg2, r2);
    auto leap = makeLeap(), walk2 = makeWalk(), climb2 = makeClimb();
    auto hasTag = [](const Path& p, StepTag t) {
      return std::any_of(p.steps.begin(), p.steps.end(), [&](StepTag s) { return s == t; });
    };

    const Path j = findPath(lnav, {{7, 10, 15}, {9, 10, 15}, leap.get(), 6});
    check(j.complete && hasTag(j, StepTag::kJump) &&
              std::any_of(j.cells.begin(), j.cells.end(), [](const Cell& c) { return c == (Cell{8, 12, 15}); }),
          "ai: leap jumps a 2-high wall (tagged kJump, over the top cell)");
    check(!findPath(lnav, {{7, 10, 15}, {9, 10, 15}, walk2.get(), 6}).complete,
          "ai: walk cannot cross the 2-high wall within range 6");
    for (int z = 4; z < 28; ++z) lw.set(8, 12, z, rock2);  // wall becomes 3-high
    lnav.invalidate(8, 15);  // the wall spans chunk columns (0,0) and (0,1):
    lnav.invalidate(8, 23);  // every touched column must be invalidated
    check(!findPath(lnav, {{7, 10, 15}, {9, 10, 15}, leap.get(), 6}).complete,
          "ai: leap cannot jump a 3-high wall");

    const Path g = findPath(lnav, {{19, 10, 35}, {24, 10, 35}, leap.get(), 8});
    check(g.complete && hasTag(g, StepTag::kGap), "ai: leap gap-jumps the 3-wide ditch (tagged kGap)");
    check(!findPath(lnav, {{19, 10, 35}, {24, 10, 35}, walk2.get(), 8}).complete,
          "ai: walk cannot cross the ditch within range 8");
    for (int z = 25; z < 46; ++z) lw.set(23, 9, z, kAir);  // ditch becomes 4 wide: no landing inside 4
    lnav.invalidate(23, 27);  // ditch columns (1,1) and (1,2)
    lnav.invalidate(23, 35);
    check(!findPath(lnav, {{19, 10, 35}, {24, 10, 35}, leap.get(), 8}).complete,
          "ai: leap refuses a gap with no landing inside 4");

    const Path d = findPath(lnav, {{40, 17, 35}, {44, 10, 35}, leap.get(), 8});
    bool fell7 = false;
    for (size_t i = 1; i < d.cells.size(); ++i) fell7 = fell7 || (d.cells[i - 1].y - d.cells[i].y == 7);
    check(d.complete && hasTag(d, StepTag::kDrop) && fell7, "ai: leap long-drops 7 off the pillar (tagged kDrop)");
    check(!findPath(lnav, {{40, 17, 35}, {44, 10, 35}, walk2.get(), 8}).complete,
          "ai: walk cannot leave the pillar top");

    const Path cl = findPath(lnav, {{48, 10, 45}, {49, 14, 45}, climb2.get(), 8});
    check(cl.complete && hasTag(cl, StepTag::kClimb), "ai: climb ascends the ladder column (rules predicate)");
    check(!findPath(lnav, {{48, 10, 45}, {49, 14, 45}, walk2.get(), 8}).complete,
          "ai: walk cannot climb the ladder column");

    // Determinism, double-run: identical request -> byte-identical result.
    const Path r1 = findPath(lnav, {{7, 10, 15}, {30, 10, 35}, leap.get(), 48});
    const Path r2p = findPath(lnav, {{7, 10, 15}, {30, 10, 35}, leap.get(), 48});
    check(r1.complete && r1.cells == r2p.cells && r1.steps == r2p.steps,
          "ai: determinism: repeated leap search is byte-identical (cells + tags)");

    // Smoothing is opt-in and string-pulls a flat corridor to its endpoints.
    const Path sm = findPath(lnav, {{2, 10, 2}, {14, 10, 2}, walk2.get(), 48, 20000, true});
    check(sm.complete && sm.cells.size() == 2 && sm.steps.size() == 2,
          "ai: smoothing string-pulls a flat corridor to its endpoints");
    check(findPath(lnav, {{2, 10, 2}, {14, 10, 2}, walk2.get(), 48}).cells.size() > 2,
          "ai: smoothing is opt-in (default path keeps its steps)");

    // P2 feed: a live AiWorld mob carries the tags parallel to its path.
    AiWorld lw2(lnav, {}, nullptr);
    const uint32_t lm = *lw2.spawn({2, 10, 2}, leap.get());
    lw2.setGoal(lm, {14, 10, 2});
    lw2.tick();
    const Mob* lmob = lw2.mob(lm);
    check(lmob && !lmob->path.empty() && lmob->pathSteps.size() == lmob->path.size() &&
              lmob->pathSteps.front() == StepTag::kStepStart,
          "ai: mob path carries parallel step tags (P2 presentation feed)");

    // ---- P3: bit-plane storage, LRU cap, surface-span sidecar ----
    check(lnav.topStandable(7, 15) == 10, "ai: P3 spans: open floor tops at y=10");
    check(lnav.topStandable(8, 15) == 13 && lnav.standableSpans(8, 15).size() == 1,
          "ai: P3 spans: the 3-high wall column tops at y=13, exactly one span");
    check(lnav.topStandable(21, 35) == -1 && lnav.standableSpans(21, 35).empty(),
          "ai: P3 spans: the ditch column has no standable span");
    WalkCache mc(lw, reg2, r2);
    mc.prewarm(0, 0);
    const size_t mcBytes = mc.cachedBytes();
    check(mc.cachedColumns() == 1 && mcBytes >= 122880 && mcBytes < 160000,
          "ai: P3 storage: a cached column is ~120 KiB of bit-planes + sidecar (was 192 KiB)");
    WalkCache lc(lw, reg2, r2, 2);
    const uint8_t wantBits = lc.bits(2, 10, 2);  // column (0,0)
    lc.bits(18, 10, 2);                          // column (1,0)
    lc.bits(34, 10, 2);                          // column (2,0) -> evicts (0,0)
    check(lc.cachedColumns() == 2, "ai: P3 LRU: the cap holds at 2 columns");
    check(lc.bits(2, 10, 2) == wantBits && lc.cachedColumns() == 2,
          "ai: P3 LRU: an evicted column recomputes to identical bits");
    WalkCache tiny(lw, reg2, r2, 3);
    const Path t1 = findPath(tiny, {{7, 10, 15}, {30, 10, 35}, leap.get(), 48});
    check(t1.complete && t1.cells == r1.cells && t1.steps == r1.steps,
          "ai: P3 LRU: a search through a too-small cache is byte-identical");
  }

  // ---- P4: flow fields, HPA*-lite, portal-pair cache, AiWorld flags ----
  {
    FlowField ff;
    check(ff.build(nav, {30, 10, 40}), "ai: P4 flow: field builds toward a standable goal");
    check(ff.dist({30, 10, 40}) == 0 && ff.dist({26, 10, 40}) == 4 * 1024,
          "ai: P4 flow: distances are exact fixed-point (goal 0, 4 straight steps = 4096)");
    check(ff.dist({500, 10, 500}) == FlowField::kUnreached, "ai: P4 flow: outside the field reads kUnreached");
    check(!ff.build(nav, {20, 10, 10}), "ai: P4 flow: a non-standable goal (inside the wall) is refused");
    ff.build(nav, {30, 10, 40});
    auto descends = [&](Cell from, int budget) {
      Cell cur = from;
      for (int i = 0; i < budget && !(cur == (Cell{30, 10, 40})); ++i) {
        const std::optional<Cell> nx = ff.next(cur);
        if (!nx) return false;
        cur = *nx;
      }
      return cur == (Cell{30, 10, 40});
    };
    check(descends({10, 10, 40}, 64), "ai: P4 flow: descent reaches the goal across open floor");
    check(descends({10, 10, 10}, 300), "ai: P4 flow: descent routes around the wall (through the gap)");

    HpaGraph hpa(nav);
    const auto hp = hpa.findPath({2, 10, 2}, {100, 10, 100}, walk.get());
    check(hp && hp->complete && hp->cells.front() == (Cell{2, 10, 2}) && hp->cells.back() == (Cell{100, 10, 100}),
          "ai: P4 HPA: a cross-map route refines to a complete path");
    bool legalSteps = hp.has_value();
    if (hp)
      for (size_t i = 1; i < hp->cells.size(); ++i) {
        const int dx = std::abs(hp->cells[i].x - hp->cells[i - 1].x), dz = std::abs(hp->cells[i].z - hp->cells[i - 1].z),
                  dy = hp->cells[i].y - hp->cells[i - 1].y;
        legalSteps = legalSteps && dx <= 1 && dz <= 1 && dy >= -3 && dy <= 1 &&
                     (nav.bits(hp->cells[i].x, hp->cells[i].y, hp->cells[i].z) & kNavStandable);
      }
    check(legalSteps, "ai: P4 HPA: every refined step is a legal walk step onto a standable cell");
    check(hpa.clustersBuilt() >= 2, "ai: P4 HPA: clusters build lazily along the route");
    const size_t hits0 = hpa.portalHits();
    const auto hp2 = hpa.findPath({2, 10, 2}, {100, 10, 100}, walk.get());
    check(hp2 && hp && hp2->cells == hp->cells && hpa.portalHits() > hits0,
          "ai: P4 HPA: a repeat query is identical and hits the portal-pair cache");
    HpaGraph cold(nav);
    const auto hp3 = cold.findPath({2, 10, 2}, {100, 10, 100}, walk.get());
    check(hp3 && hp && hp3->cells == hp->cells, "ai: P4 HPA: a cold-cache route is identical (history-independent)");
    check(!hpa.findPath({2, 10, 2}, {20, 10, 10}, walk.get()), "ai: P4 HPA: an unreachable goal returns no route");

    // Repair: wall off the gap region on the short route and the graph
    // must reroute (the column versions move -> clusters rebuild).
    const auto shortRoute = hpa.findPath({10, 10, 40}, {30, 10, 40}, walk.get());
    for (int z = 30; z < 61; ++z) {
      w.set(20, 10, z, rock);
      w.set(20, 11, z, rock);
    }
    nav.invalidate(20, 40);
    const auto rerouted = hpa.findPath({10, 10, 40}, {30, 10, 40}, walk.get());
    check(shortRoute && rerouted && rerouted->complete && rerouted->cells != shortRoute->cells,
          "ai: P4 HPA: an edit on the route repairs the graph and reroutes");
    for (int z = 30; z < 61; ++z) {
      w.set(20, 10, z, kAir);
      w.set(20, 11, z, kAir);
    }
    nav.invalidate(20, 40);

    // AiWorld consumption, behind flags (default off -- every pre-existing
    // AiWorld check above already proves the off behaviour is unchanged).
    AiConfig hc;
    hc.useHpa = true;
    AiWorld hw(nav, hc, nullptr);
    const uint32_t hm = *hw.spawn({2, 10, 2}, walk.get());
    hw.setGoal(hm, {100, 10, 100});
    hw.tick();
    const Mob* hmob = hw.mob(hm);
    check(hmob && hmob->path.size() > 40, "ai: P4 AiWorld: useHpa plans a full cross-map route in one tick");
    AiConfig fc;
    fc.useFlowField = true;
    AiWorld fwl(nav, fc, nullptr);
    const uint32_t fm = *fwl.spawn({10, 10, 40}, walk.get());
    fwl.setGoal(fm, {30, 10, 40});
    for (int t = 0; t < 30 && fwl.mob(fm) && !(fwl.mob(fm)->pos == (Cell{30, 10, 40})); ++t) fwl.tick();
    check(fwl.mob(fm) && (fwl.mob(fm)->pos == (Cell{30, 10, 40})),
          "ai: P4 AiWorld: useFlowField walks the mob down the field to its goal");
  }

  // ==== Worker2 fix-run 2026-10-15: B-C1 / B-C2 / B-H2 regression ====
  // Delimited section: everything below until the end of testAi belongs to
  // the HPA/nav fix run; other workers append elsewhere.
  {
    // ---- B-H2: A* must return the Dijkstra-optimal cost ----
    // Fixture (own flat world, floor y=9, walk at y=10): a 2-high wall row
    // at z=5, x=4..7 and x=10..11, leaving gaps at x<=3, x=8..9, x>=12.
    // Start (2,10,7), goal (11,10,3) (offsets of the searched 10x10 case).
    // Pre-fix (Manhattan x1024) this fixture returns 12112 against the
    // Dijkstra optimum 11512 (verified against the pre-fix heuristic in a
    // reference search; the 600 overcharge is the diagonal overestimate);
    // the post-fix octile heuristic must match Dijkstra exactly.
    BlockRegistry regH;
    const BlockId rockH = regH.add({"Rock", kSolid | kOpaque, Shape::Cube, "rock"});
    ChunkMap hw = worldWithColumns(2, &regH);  // x, z in 0..31
    for (int z = 0; z < 32; ++z)
      for (int x = 0; x < 32; ++x) hw.set(x, 9, z, rockH);
    for (int x : {4, 5, 6, 7, 10, 11}) { hw.set(x, 10, 5, rockH); hw.set(x, 11, 5, rockH); }
    WalkCache hnav(hw, regH, {});
    auto hwk = makeWalk();
    auto dijkstraRef = [&](Cell from, Cell to, const IMovementMode* mode, int maxRange) {
      NavView nv(hnav);
      std::unordered_map<Cell, NavCost, CellHash> dist;
      std::unordered_map<Cell, bool, CellHash> done;
      dist[from] = 0;
      std::vector<NavStep> nb;
      for (;;) {
        Cell cur{};
        NavCost best = ~NavCost(0);
        bool any = false;
        for (const auto& [c, d] : dist)
          if (!done[c] && d < best) { best = d; cur = c; any = true; }
        if (!any) break;
        done[cur] = true;
        if (cur == to) break;
        nb.clear();
        mode->neighbours(nv, cur, nb);
        for (const NavStep& s : nb) {
          if (std::abs(s.to.x - from.x) > maxRange || std::abs(s.to.z - from.z) > maxRange ||
              std::abs(s.to.y - from.y) > maxRange)
            continue;
          const NavCost nd = best + s.cost;
          if (auto it = dist.find(s.to); it == dist.end() || nd < it->second) dist[s.to] = nd;
        }
      }
      const auto it = dist.find(to);
      return it == dist.end() ? ~NavCost(0) : it->second;
    };
    const Path opt = findPath(hnav, {{2, 10, 7}, {11, 10, 3}, hwk.get(), 24});
    const NavCost ref = dijkstraRef({2, 10, 7}, {11, 10, 3}, hwk.get(), 24);
    check(ref == 11512, "ai: B-H2 fixture: Dijkstra reference cost is the hand-verified 11512");
    check(opt.complete && opt.totalCost == ref,
          "ai: B-H2: findPath returns the Dijkstra-optimal cost (admissible octile heuristic)");
    // Exact-cost pins on primitives the heuristic must price tightly:
    const Path pureDiag = findPath(hnav, {{20, 10, 20}, {24, 10, 24}, hwk.get(), 24});
    check(pureDiag.complete && pureDiag.totalCost == 4 * 1448,
          "ai: B-H2: a pure diagonal trip costs exactly 4 x 1448");
    // A 1-high ridge crossed straight: step-up (1536) + drop (1536) = 3072.
    // (A heuristic pricing |dy| at a flat 1024/step on top of horizontal
    // travel overestimates exactly this bundled step.)
    for (int z = 18; z <= 22; ++z) hw.set(26, 10, z, rockH);
    hnav.invalidate(26, 20);
    const Path ridge = findPath(hnav, {{25, 10, 20}, {27, 10, 20}, hwk.get(), 24});
    check(ridge.complete && ridge.totalCost == 3072 &&
              std::any_of(ridge.cells.begin(), ridge.cells.end(),
                          [](const Cell& c) { return c == (Cell{26, 11, 20}); }),
          "ai: B-H2: step-up + drop over a ridge costs exactly 1536 + 1536");

    // ---- B-C1: leg cache must not serve a leg walled after it was cached ----
    // Straight 3-cluster route along z=8. The middle cluster's interior
    // leg (16,10,8) -> (31,10,8) is cached by the first query; then ONLY
    // that leg's corridor is walled (one interior column at x=24 --
    // portals and the winning portal route are untouched).
    BlockRegistry regC;
    const BlockId rockC = regC.add({"Rock", kSolid | kOpaque, Shape::Cube, "rock"});
    ChunkMap cw2 = worldWithColumns(3, &regC);  // x, z in 0..47
    for (int z = 0; z < 48; ++z)
      for (int x = 0; x < 48; ++x) cw2.set(x, 9, z, rockC);
    WalkCache cnav(cw2, regC, {});
    auto cwk = makeWalk();
    HpaGraph cg(cnav);
    const auto cr1 = cg.findPath({2, 10, 8}, {40, 10, 8}, cwk.get());
    check(cr1 && cr1->complete && cg.portalCacheSize() > 0 &&
              std::any_of(cr1->cells.begin(), cr1->cells.end(),
                          [](const Cell& c) { return c == (Cell{24, 10, 8}); }),
          "ai: B-C1 fixture: first route is straight, caches the middle leg through (24,10,8)");
    cw2.set(24, 10, 8, rockC);
    cw2.set(24, 11, 8, rockC);  // 2-high: the corridor cell is no longer standable
    cnav.invalidate(24, 8);
    const auto cr2 = cg.findPath({2, 10, 8}, {40, 10, 8}, cwk.get());
    bool legOk = cr2 && cr2->complete && cr2->cells != cr1->cells;
    if (cr2)
      for (const Cell& c : cr2->cells)
        legOk = legOk && !(c == (Cell{24, 10, 8})) &&
                (cnav.bits(c.x, c.y, c.z) & kNavStandable);
    check(legOk, "ai: B-C1: after walling the cached leg's corridor, the refined path avoids the wall");
    check(cr2 && std::any_of(cr2->cells.begin(), cr2->cells.end(),
                             [](const Cell& c) { return c == (Cell{16, 10, 8}); }) &&
              std::any_of(cr2->cells.begin(), cr2->cells.end(),
                          [](const Cell& c) { return c == (Cell{31, 10, 8}); }),
          "ai: B-C1: the winning portal route is unchanged (same entrances, leg re-refined)");

    // ---- B-C2: a portal that is a legal drop down must refuse the climb up ----
    // Cliff border at x=15/16 across the whole map: west plateau standable
    // at y=13 (floor y=12), east lowland standable at y=10 (floor y=9) --
    // dy = -3 in the canonical west -> east direction.
    BlockRegistry regD;
    const BlockId rockD = regD.add({"Rock", kSolid | kOpaque, Shape::Cube, "rock"});
    ChunkMap dw = worldWithColumns(2, &regD);  // x, z in 0..31
    for (int z = 0; z < 32; ++z) {
      for (int x = 0; x < 16; ++x) dw.set(x, 12, z, rockD);
      for (int x = 16; x < 32; ++x) dw.set(x, 9, z, rockD);
    }
    WalkCache dnav(dw, regD, {});
    auto dwk = makeWalk();
    HpaGraph dg(dnav);
    const auto down = dg.findPath({8, 13, 8}, {24, 10, 8}, dwk.get());
    bool downOk = down && down->complete && down->cells.back() == (Cell{24, 10, 8});
    bool usesDropPortal = false;
    if (down)
      for (size_t i = 1; i < down->cells.size(); ++i) {
        const int dy = down->cells[i].y - down->cells[i - 1].y;
        downOk = downOk && dy >= -3 && dy <= 1 &&
                 (dnav.bits(down->cells[i].x, down->cells[i].y, down->cells[i].z) & kNavStandable);
        usesDropPortal = usesDropPortal ||
                         (down->cells[i - 1] == (Cell{15, 13, 8}) && down->cells[i] == (Cell{16, 10, 8}));
      }
    check(downOk && usesDropPortal,
          "ai: B-C2: routing DOWN the cliff uses the drop portal (legal -3 step, all steps legal)");
    const auto up = dg.findPath({24, 10, 8}, {8, 13, 8}, dwk.get());
    check(!up, "ai: B-C2: routing UP the cliff refuses the drop portal (no +3 step is ever served)");
    // Leap must not be broken by the directed-portal fix: its ordinary
    // searches keep working (2-high jump fixture geometry, flat world).
    auto dlp = makeLeap();
    const Path leapFlat = findPath(dnav, {{24, 10, 8}, {28, 10, 8}, dlp.get(), 24});
    check(leapFlat.complete && leapFlat.totalCost == 4 * 1024,
          "ai: B-C2: Leap still paths normally on flat ground after the portal fix");
  }
  // ==== End Worker2 fix-run section ====
}

void testNet() {
  std::printf("net: snapshot/interpolation model\n");
  using namespace net;

  auto ent = [](uint32_t id, int32_t x, int32_t y, int32_t z) {
    SnapshotEntity e;
    e.id = id;
    e.x = x;
    e.y = y;
    e.z = z;
    return e;
  };

  // --- varint codec ---------------------------------------------------------
  {
    const int32_t vals[] = {0, 1, -1, 63, 64, -64, 65, -65, 8191, -8192,
                            123456789, -123456789, INT32_MAX, INT32_MIN};
    bool ok = true;
    for (int32_t v : vals) {
      quantize::ByteWriter w;
      w.putVarint(v);
      quantize::ByteReader r(w.data(), w.size());
      int32_t back = 0;
      ok = ok && r.getVarint(back) && back == v && r.eof();
    }
    quantize::ByteWriter w0, w63, w64;
    w0.putVarint(0);
    w63.putVarint(63);
    w64.putVarint(64);
    ok = ok && w0.size() == 1 && w63.size() == 1 && w64.size() == 2;  // canonical
    const uint32_t uvals[] = {0, 127, 128, 300, UINT32_MAX};
    for (uint32_t v : uvals) {
      quantize::ByteWriter w;
      w.putUVarint(v);
      quantize::ByteReader r(w.data(), w.size());
      uint32_t back = 0;
      ok = ok && r.getUVarint(back) && back == v && r.eof();
    }
    const uint8_t bad[] = {0x80};  // truncated continuation
    quantize::ByteReader rb(bad, sizeof bad);
    int32_t bv = 0;
    ok = ok && !rb.getVarint(bv);
    const uint8_t bad6[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x00};  // 6 bytes
    quantize::ByteReader rb6(bad6, sizeof bad6);
    uint32_t bu = 0;
    ok = ok && !rb6.getUVarint(bu);
    check(ok, "net: varint round-trip incl INT32_MIN/MAX, canonical lengths, rejects malformed");
  }

  // --- packet header ----------------------------------------------------------
  {
    PacketHeader h;
    h.flags = kFlagTruncated;
    h.seq = 0xABCD;
    h.ack = 42;
    h.ackBits = 0x1234;
    h.type = PayloadType::Snapshot;
    h.payloadLen = 900;
    quantize::ByteWriter w;
    encodeHeader(h, w);
    bool ok = w.size() == kHeaderLen;
    quantize::ByteReader r(w.data(), w.size());
    PacketHeader back;
    ok = ok && decodeHeader(r, back) && back == h;
    w.buf[0] ^= 0xFF;  // corrupt magic
    quantize::ByteReader r2(w.data(), w.size());
    PacketHeader h2;
    ok = ok && !decodeHeader(r2, h2);
    quantize::ByteReader r3(w.data(), 5);  // truncated header
    ok = ok && !decodeHeader(r3, h2);
    check(ok, "net: 14-byte header round-trip; rejects bad magic and truncation");
    check(seqGreater(1, 0) && !seqGreater(0, 1) && seqGreater(0, 0xFFFF) &&
              !seqGreater(0xFFFF, 0) && !seqGreater(7, 7) && seqGreaterEq(7, 7),
          "net: RFC-1982 sequence arithmetic across the u16 wrap");
  }

  // --- fixed-point quantization ------------------------------------------------
  {
    bool ok = true;
    for (int i = -40; i <= 40; ++i) {
      const double d = i * 6553.6 + 0.0007;  // +/-262144 with fractions
      const int32_t q = quantize::posToFixed(d);
      ok = ok && quantize::posToFixed(d) == q;  // deterministic
      ok = ok && std::fabs(quantize::fixedToPos(q) - d) <= 1.0 / 2048 + 1e-9;
      const int32_t vq = quantize::velToFixed(d / 512.0);
      ok = ok && std::fabs(quantize::fixedToVel(vq) - d / 512.0) <= 1.0 / 2048 + 1e-9;
    }
    check(ok, "net: Q10 quantization error bounded by 1/2048 block, deterministic");
  }

  // --- smallest-three quaternion -------------------------------------------------
  {
    struct Q {
      double x, y, z, w;
    };
    auto norm = [](Q q) {
      const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
      return Q{q.x / n, q.y / n, q.z / n, q.w / n};
    };
    const Q qs[] = {
        norm({0, 0, 0, 1}), norm({1, 0, 0, 0}),
        norm({0, 0.7071067811865476, 0, 0.7071067811865476}),
        norm({0.5, 0.5, 0.5, 0.5}), norm({0.27, -0.35, 0.41, 0.8}),
        norm({-0.1, 0.9, -0.2, 0.37}),
    };
    bool ok = true;
    for (const Q& q : qs) {
      const uint32_t p = quantize::packQuat(q.x, q.y, q.z, q.w);
      ok = ok && p == quantize::packQuat(-q.x, -q.y, -q.z, -q.w);  // q == -q
      ok = ok && (p >> 29) == 0;                                  // fits in 29 bits
      double x, y, z, w;
      quantize::unpackQuat(p, x, y, z, w);
      // Sign-agnostic max per-component error.
      const double e1 = std::max({std::fabs(x - q.x), std::fabs(y - q.y),
                                  std::fabs(z - q.z), std::fabs(w - q.w)});
      const double e2 = std::max({std::fabs(x + q.x), std::fabs(y + q.y),
                                  std::fabs(z + q.z), std::fabs(w + q.w)});
      ok = ok && std::min(e1, e2) < 0.01;
    }
    check(ok, "net: smallest-three quaternion: 29 bits, q==-q, error < 0.01");
  }

  // --- full snapshot round-trip, bit-exact -----------------------------------------
  {
    Snapshot s;
    s.tick = 12345;
    SnapshotEntity a = ent(7, 1024, -2048, 0);
    a.atRest = true;
    a.vx = 99;  // garbage velocity: the codec normalizes it to zero while atRest
    SnapshotEntity b = ent(3, -5, 6, 7);
    b.vx = 10;
    b.vy = -20;
    b.vz = 30;
    b.flags = kEntHasOrient;
    b.hasOrient = true;
    b.orient = quantize::packQuat(0, 0, 0, 1);
    SnapshotEntity c = ent(100, 0, 0, 0);
    c.flags = 0xF0;
    c.atRest = true;
    s.entities = {a, b, c};  // deliberately unsorted
    quantize::ByteWriter w;
    bool trunc = true;
    bool ok = encodeSnapshot(s, nullptr, 0, 1200, w, trunc) && !trunc;
    quantize::ByteReader r(w.data(), w.size());
    Snapshot back;
    ok = ok && decodeSnapshot(r, nullptr, back);
    ok = ok && back.tick == 12345 && back.entities.size() == 3;
    ok = ok && back.entities[0].id == 3 && back.entities[1].id == 7 &&
         back.entities[2].id == 100;  // sorted by id
    ok = ok && back.entities[0] == b;
    SnapshotEntity aExp = a;
    aExp.vx = aExp.vy = aExp.vz = 0;  // atRest normalization
    ok = ok && back.entities[1] == aExp && back.entities[2] == c;
    check(ok, "net: full snapshot encode->decode round-trip is bit-exact");
  }

  // --- delta vs baseline: smaller and exact ------------------------------------------
  {
    Snapshot base, cur;
    base.tick = 0;
    for (uint32_t i = 0; i < 100; ++i) {
      SnapshotEntity e = ent(i, int32_t(i * 1024), 0, 0);
      e.atRest = true;
      base.entities.push_back(e);
    }
    base.seq = 7;
    cur = base;
    cur.tick = 16;
    cur.entities[3].x += 5;
    cur.entities[3].atRest = false;
    cur.entities[3].vx = 2;
    cur.entities[40].y -= 3;
    cur.entities[99].flags = 0x10;
    quantize::ByteWriter wf, wd;
    bool t1 = true, t2 = true;
    encodeSnapshot(cur, nullptr, 0, 65536, wf, t1);
    encodeSnapshot(cur, &base, 7, 65536, wd, t2);
    bool ok = !t1 && !t2 && wd.size() < wf.size();
    std::printf("  [info] net: full=%zu bytes, delta=%zu bytes (3 changed of 100)\n",
                wf.size(), wd.size());
    quantize::ByteReader rd(wd.data(), wd.size());
    Snapshot back;
    // Note: decode fills tick + entities; seq comes from the packet header,
    // so it is compared separately (here: left at 0 by the raw codec).
    ok = ok && decodeSnapshot(rd, &base, back) && back.tick == cur.tick &&
         back.entities == cur.entities;
    check(ok, "net: delta vs acked baseline decodes exactly and is smaller than full");
  }

  // --- spawn + despawn + move in one delta ----------------------------------------------
  {
    Snapshot base, cur;
    base.tick = 0;
    base.seq = 9;
    base.entities = {ent(1, 0, 0, 0), ent(2, 0, 0, 0), ent(3, 0, 0, 0)};
    for (auto& e : base.entities) e.atRest = true;
    cur.tick = 16;
    SnapshotEntity e2 = ent(2, 10 * 1024, 0, 0);
    e2.atRest = true;
    SnapshotEntity e3 = ent(3, 0, 0, 0);
    e3.atRest = true;
    SnapshotEntity e4 = ent(4, 5, 6, 7);
    e4.flags = kEntHasOrient | 0x20;
    e4.hasOrient = true;
    e4.orient = quantize::packQuat(0.5, 0.5, 0.5, 0.5);
    e4.vx = 1;
    e4.vy = 2;
    e4.vz = 3;
    cur.entities = {e2, e3, e4};
    quantize::ByteWriter w;
    bool tr = true;
    bool ok = encodeSnapshot(cur, &base, 9, 1200, w, tr) && !tr;
    quantize::ByteReader r(w.data(), w.size());
    Snapshot back;
    ok = ok && decodeSnapshot(r, &base, back);
    ok = ok && back.tick == 16 && back.entities == cur.entities;
    check(ok, "net: delta handles spawn, despawn, and move in one packet");
  }

  // --- snapshotter: cadence, baseline, acks ------------------------------------------------
  {
    SnapshotClient c;  // 512 TPS / 32 Hz -> divisor 16
    bool ok = c.divisor() == 16 && c.isSnapshotTick(0) && c.isSnapshotTick(16) &&
              !c.isSnapshotTick(15) && c.baselineSeq() == 0;
    std::vector<SnapshotEntity> e1 = {ent(1, 0, 0, 0)};
    e1[0].atRest = true;
    const auto p1 = c.build(0, e1);   // seq 1, full
    const auto p2 = c.build(16, e1);  // seq 2, full (no ack yet)
    PacketHeader h1, h2;
    {
      quantize::ByteReader r(p1.data(), p1.size());
      ok = ok && decodeHeader(r, h1);
    }
    {
      quantize::ByteReader r(p2.data(), p2.size());
      ok = ok && decodeHeader(r, h2);
    }
    ok = ok && h1.seq == 1 && h2.seq == 2;
    auto baselineOf = [&](const std::vector<uint8_t>& p) {
      quantize::ByteReader r(p.data() + kHeaderLen, p.size() - kHeaderLen);
      uint32_t t, b;
      return decodeSnapshotPrefix(r, t, b) ? b : 0xFFFFFFFFu;
    };
    ok = ok && baselineOf(p1) == 0 && baselineOf(p2) == 0;
    c.onAck(h1.seq, 0);
    ok = ok && c.baselineSeq() == 1;
    c.onAck(h1.seq, 0);  // duplicate ack: must not regress
    ok = ok && c.baselineSeq() == 1;
    std::vector<SnapshotEntity> e2 = {ent(1, 1024, 0, 0)};
    e2[0].atRest = true;
    const auto p3 = c.build(32, e2);  // seq 3, delta vs 1
    ok = ok && baselineOf(p3) == 1;
    c.onAck(h2.seq, 0);
    ok = ok && c.baselineSeq() == 2 && c.historySize() == 2;
    check(ok, "net: snapshotter cadence 32 Hz, per-client baseline, acks never regress");
  }

  // --- receiver: out-of-order / duplicate / malformed ------------------------------------------
  {
    SnapshotClient srv;
    SnapshotReceiver cli;
    std::vector<SnapshotEntity> e = {ent(1, 0, 0, 0)};
    e[0].atRest = true;
    const auto p1 = srv.build(0, e);
    const auto p2 = srv.build(16, e);
    const auto p3 = srv.build(32, e);
    const auto p4 = srv.build(48, e);
    (void)p1;
    bool ok = true;
    ok = ok && cli.receive(p3.data(), p3.size()) == SnapshotReceiver::Result::Applied;
    ok = ok && cli.receive(p2.data(), p2.size()) == SnapshotReceiver::Result::Dropped;  // old
    ok = ok && cli.receive(p3.data(), p3.size()) == SnapshotReceiver::Result::Dropped;  // dup
    ok = ok && cli.receive(p4.data(), p4.size()) == SnapshotReceiver::Result::Applied;  // gap ok
    ok = ok && cli.ackSeq() == 4;
    // ackBits: bit0 <-> seq 3 (seen), bits 1-2 <-> seqs 2,1 (unseen)
    ok = ok && (cli.ackBits() & 1u) && !(cli.ackBits() & 6u);
    ok = ok && cli.receive(p4.data(), 10) == SnapshotReceiver::Result::Dropped;  // truncated
    auto bad = p4;
    bad[0] ^= 0xFF;  // corrupt magic
    ok = ok && cli.receive(bad.data(), bad.size()) == SnapshotReceiver::Result::Dropped;
    check(ok, "net: receiver drops dups/old/malformed, accepts gaps, selective acks");
  }

  // --- lost baseline -> needFull -> resync ---------------------------------------------------------
  {
    SnapshotClient srv;
    SnapshotReceiver cli;
    std::vector<SnapshotEntity> e1 = {ent(1, 0, 0, 0)};
    e1[0].atRest = true;
    std::vector<SnapshotEntity> e2 = {ent(1, 2048, 0, 0)};
    e2[0].atRest = true;
    const auto p1 = srv.build(0, e1);  // seq 1, full -- "lost" (never delivered)
    (void)p1;
    srv.onAck(1, 0);  // server believes the client holds seq 1
    const auto p2 = srv.build(16, e2);  // seq 2, delta vs 1
    bool ok = cli.receive(p2.data(), p2.size()) == SnapshotReceiver::Result::NeedBaseline;
    ok = ok && cli.needFull();
    srv.requestFull();  // game honors the client's needFull flag
    const auto p3 = srv.build(32, e2);  // seq 3, full
    ok = ok && cli.receive(p3.data(), p3.size()) == SnapshotReceiver::Result::Applied;
    ok = ok && !cli.needFull() && cli.latest().entities[0].x == 2048;
    check(ok, "net: lost baseline -> NeedBaseline/needFull -> full-snapshot resync");
  }

  // --- fixed-point interpolation between snapshots -----------------------------------------------------
  {
    Snapshot a, b;
    a.tick = 0;
    b.tick = 16;
    SnapshotEntity ea = ent(5, 0, 100 * 1024, 0);
    SnapshotEntity eb = ent(5, 16 * 1024, 100 * 1024, 0);
    eb.vx = 1024;
    a.entities = {ea};
    b.entities = {eb};
    Snapshot out;
    bool ok = true;
    interpolateSnapshots(a, b, 8, out);
    ok = ok && out.entities.size() == 1 && out.entities[0].x == 8 * 1024 &&
         out.entities[0].vx == 512 && out.tick == 8;
    interpolateSnapshots(a, b, 0, out);
    ok = ok && out.entities[0].x == 0;
    interpolateSnapshots(a, b, 16, out);
    ok = ok && out.entities[0].x == 16 * 1024;
    interpolateSnapshots(a, b, 4, out);
    ok = ok && out.entities[0].x == 4 * 1024;  // alpha = 1/4 exact
    // Spawned between snapshots: held at spawn state.
    SnapshotEntity spawn = ent(9, 3 * 1024, 0, 0);
    spawn.atRest = true;
    Snapshot b2 = b;
    b2.entities.push_back(spawn);  // ids stay sorted (5 < 9)
    interpolateSnapshots(a, b2, 8, out);
    ok = ok && out.entities.size() == 2 && out.entities[1].id == 9 &&
         out.entities[1].x == 3 * 1024;
    // Despawned between snapshots: omitted.
    Snapshot a2 = a;
    a2.entities.push_back(spawn);
    interpolateSnapshots(a2, b, 8, out);
    ok = ok && out.entities.size() == 1 && out.entities[0].id == 5;
    check(ok, "net: fixed-point interpolation exact at intermediate ticks; spawn/despawn handled");
  }

  // --- receiver sample(): render state between buffered snapshots ------------------------------------------
  {
    SnapshotClient srv;
    SnapshotReceiver cli;
    bool ok = true;
    for (int i = 0; i < 3; ++i) {
      SnapshotEntity e = ent(1, int32_t(i * 16 * 1024), 0, 0);
      e.atRest = true;
      const auto p = srv.build(uint64_t(i * 16), {e});
      ok = ok && cli.receive(p.data(), p.size()) == SnapshotReceiver::Result::Applied;
    }
    Snapshot out;
    ok = ok && cli.sample(24, out) && out.entities[0].x == 24 * 1024;
    ok = ok && cli.sample(0, out) && out.entities[0].x == 0;          // clamp: hold oldest
    ok = ok && cli.sample(100, out) && out.entities[0].x == 32 * 1024;  // clamp: hold newest
    check(ok, "net: receiver interpolates render state, clamps at buffer edges");
  }

  // --- spiral of death: missed slots are skipped, never caught up ----------------------------------------------
  {
    SnapshotClient c;
    std::vector<SnapshotEntity> e = {ent(1, 0, 0, 0)};
    e[0].atRest = true;
    const auto p1 = c.build(0, e);
    (void)p1;
    // The game misses the tick-16 and tick-32 slots entirely (slowdown, not
    // catch-up): one build at tick 48, no burst.
    std::vector<SnapshotEntity> e2 = {ent(1, 48 * 1024, 0, 0)};
    e2[0].atRest = true;
    const auto p2 = c.build(48, e2);
    PacketHeader h;
    quantize::ByteReader r(p2.data(), p2.size());
    bool ok = decodeHeader(r, h) && h.seq == 2;  // one packet, not a burst
    ok = ok && !(h.flags & kFlagTruncated);
    quantize::ByteReader pr(p2.data() + kHeaderLen, p2.size() - kHeaderLen);
    Snapshot back;
    ok = ok && decodeSnapshot(pr, nullptr, back) && back.entities[0].x == 48 * 1024;
    check(ok, "net: missed snapshot slots are skipped; the delta covers the gap");
  }

  // --- byte budget: truncation flag, recovery on the next delta ------------------------------------------------------
  {
    SnapshotClient c;
    std::vector<SnapshotEntity> ents;
    for (uint32_t i = 0; i < 50; ++i) {
      auto e = ent(i, int32_t(i), 0, 0);
      e.atRest = true;  // small records: 50 entities fit one full budget
      ents.push_back(e);
    }
    const auto p1 = c.build(0, ents, 64);  // tiny budget
    PacketHeader h;
    quantize::ByteReader r(p1.data(), p1.size());
    bool ok = decodeHeader(r, h) && (h.flags & kFlagTruncated);
    quantize::ByteReader pr(p1.data() + kHeaderLen, p1.size() - kHeaderLen);
    Snapshot part;
    ok = ok && decodeSnapshot(pr, nullptr, part) && !part.entities.empty() &&
         part.entities.size() < 50;
    // Next slot with a real budget: the delta vs the (advanced) baseline
    // re-includes everything the budget cut.
    c.onAck(h.seq, 0);
    const auto p2 = c.build(16, ents);
    PacketHeader h2;
    quantize::ByteReader r2(p2.data(), p2.size());
    ok = ok && decodeHeader(r2, h2) && !(h2.flags & kFlagTruncated);
    SnapshotReceiver cli;
    ok = ok && cli.receive(p1.data(), p1.size()) == SnapshotReceiver::Result::Applied;
    ok = ok && cli.receive(p2.data(), p2.size()) == SnapshotReceiver::Result::Applied;
    ok = ok && cli.latest().entities.size() == 50;
    check(ok, "net: byte budget truncates with flag; next delta recovers cut entities");
  }

  // --- PrioManager --------------------------------------------------------------------------------------
  {
    PrioManager pm;
    pm.addStream({streams::kSnapshot, StreamPriority::Critical, 800});
    pm.addStream({streams::kChunk, StreamPriority::High, 200});
    pm.addStream({streams::kEvent, StreamPriority::Normal, 100});
    const auto g = pm.allocate(2000);
    // Floors: 800+200+100 = 1100; leftover 900 goes to Critical first.
    bool ok = g.size() == 3 && g[0].streamId == streams::kSnapshot && g[0].bytes == 1700 &&
              g[1].bytes == 200 && g[2].bytes == 100;
    // Leftover splits evenly within one priority level; remainder to lowest id.
    PrioManager pm2;
    pm2.addStream({20, StreamPriority::High, 0});
    pm2.addStream({10, StreamPriority::High, 0});
    const auto g2 = pm2.allocate(101);
    ok = ok && g2.size() == 2 && g2[0].streamId == 10 && g2[0].bytes == 51 &&
         g2[1].bytes == 50;
    ok = ok && pm.allocate(2000) == g;  // deterministic
    const auto g4 = pm.allocate(0);
    ok = ok && g4[0].bytes == 0 && g4[1].bytes == 0 && g4[2].bytes == 0;
    check(ok, "net: PrioManager: guaranteed floors, priority-ordered leftover, deterministic");
  }
  testInfiniteRegistryNet();
}

// ---- Infinite Registry (phase 1): LEB128 uvarint64 on the wire, BlockUpdate, protocol v3 ----

void testInfiniteRegistryNet() {
  std::printf("net: infinite registry wire (uvarint64, block update, protocol v3)\n");
  using namespace net::quantize;

  // Boundary byte-lengths: 0->1, 127->1, 128->2, 2^32-1->5, 2^64-1->10.
  {
    const std::pair<uint64_t, size_t> cases[] = {
        {0, 1}, {1, 1}, {127, 1}, {128, 2}, {255, 2}, {16383, 2}, {16384, 3},
        {0xFFFFFFFFULL, 5}, {0x100000000ULL, 5}, {0x1FFFFFFFFFFFFFULL, 8},
        {0x7FFFFFFFFFFFFFFFULL, 9}, {0x8000000000000000ULL, 10}, {0xFFFFFFFFFFFFFFFFULL, 10}};
    bool ok = true;
    for (const auto& [v, want] : cases) {
      ByteWriter w;
      w.putUVarint64(v);
      if (w.buf.size() != want) { ok = false; break; }
      ByteReader r(w.buf.data(), w.buf.size());
      uint64_t back = 0xDEAD;
      if (!r.getUVarint64(back) || back != v || r.remaining() != 0) { ok = false; break; }
    }
    check(ok, "net: uvarint64 boundary byte-lengths and round-trips (pack the zeros)");
  }

  // Strictness: truncated, overlong, and overflowing inputs all fail.
  {
    ByteWriter w;
    w.putUVarint64(0xFFFFFFFFFFFFFFFFULL);  // 10 bytes
    bool ok = true;
    for (size_t cut = 0; cut < 10; ++cut) {
      ByteReader r(w.buf.data(), cut);
      uint64_t v = 0;
      if (r.getUVarint64(v)) { ok = false; break; }
    }
    // 11 continuation bytes: malformed.
    uint8_t eleven[11];
    for (int i = 0; i < 11; ++i) eleven[i] = 0x80;
    ByteReader r11(eleven, 11);
    uint64_t v11 = 0;
    ok = ok && !r11.getUVarint64(v11);
    // 10th byte carrying bits beyond bit 63 (0x02, and 0x80 continuation): malformed.
    uint8_t bad10[10] = {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x02};
    ByteReader rb(bad10, 10);
    uint64_t vb = 0;
    ok = ok && !rb.getUVarint64(vb) && vb == 0;
    uint8_t bad10c[10] = {0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x80,0x81};
    ByteReader rc(bad10c, 10);
    uint64_t vc = 0;
    ok = ok && !rc.getUVarint64(vc);
    check(ok, "net: uvarint64 rejects truncated, overlong, and overflowing encodings");
  }

  // Non-minimal encodings are rejected (review F-L2): a final zero group in
  // a multi-byte form is not canonical, even though the value is in range.
  {
    bool ok = true;
    const uint8_t nm5[] = {0x85, 0x00};   // 5 in 2 bytes
    ByteReader r1(nm5, sizeof nm5);
    uint64_t v = 0xDEAD;
    ok = ok && !r1.getUVarint64(v) && v == 0xDEAD;  // rejected, out untouched
    const uint8_t nm0[] = {0x80, 0x00};   // 0 in 2 bytes
    ByteReader r2(nm0, sizeof nm0);
    ok = ok && !r2.getUVarint64(v);
    const uint8_t nm10[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00};  // 0 in 10 bytes
    ByteReader r3(nm10, sizeof nm10);
    ok = ok && !r3.getUVarint64(v);
    const uint8_t canon128[] = {0x80, 0x01};  // 128, minimal: final group nonzero
    ByteReader r4(canon128, sizeof canon128);
    ok = ok && r4.getUVarint64(v) && v == 128;
    check(ok, "net: uvarint64 rejects non-minimal encodings (final zero group), accepts canonical ones");
  }

  // BlockUpdate: round-trip incl. negative coords and extreme ids; truncation fails clean.
  {
    using namespace litholattice::net;
    bool ok = true;
    const BlockUpdate cases[] = {
        {0, 0, 0, 0}, {-5, 300, -70000, 1}, {2147483647, -2147483648, 12345, 0xFFFFFFFFFFFFFFFFULL},
        {1, 2, 3, 0x82ca3801816336c9ULL}};
    for (const auto& u : cases) {
      ByteWriter w;
      encodeBlockUpdate(u, w);
      ByteReader r(w.buf.data(), w.buf.size());
      BlockUpdate back;
      if (!decodeBlockUpdate(r, back) || !(back == u) || r.remaining() != 0) { ok = false; break; }
    }
    check(ok, "net: BlockUpdate round-trips coords + stable id");
    ByteWriter w;
    encodeBlockUpdate({-5, 300, -70000, 42}, w);
    bool truncOk = true;
    for (size_t cut = 0; cut < w.buf.size(); ++cut) {
      ByteReader r(w.buf.data(), cut);
      BlockUpdate back{9, 9, 9, 9};
      if (decodeBlockUpdate(r, back) || back.x != 9 || back.stableId != 9) { truncOk = false; break; }
    }
    check(truncOk, "net: truncated BlockUpdate fails and leaves the out-param untouched");
  }

  // Protocol v3: version bump, handshake lists, codec selection.
  {
    using namespace litholattice::net;
    const auto known = knownProtocolVersions();
    bool listed = false;
    for (uint8_t v : known) listed = listed || v == LITHOLATTICE_PROTOCOL_V3;
    check(kProtocolVersion == 3 && LITHOLATTICE_PROTOCOL_V3 == 3 &&
              maxProtocolVersion() == LITHOLATTICE_PROTOCOL_V3 && listed,
          "net: protocol v3 is the current version and is offered by the handshake");
    check(codecForProtocol(LITHOLATTICE_PROTOCOL_V3) == CodecKind::V3StableIds &&
              codecForProtocol(LITHOLATTICE_PROTOCOL_V2) == CodecKind::V2Delta &&
              codecForProtocol(99) == CodecKind::Unknown,
          "net: codecForProtocol selects the v3 stable-id codec, v2 still maps to delta");
    const uint8_t v3[] = {LITHOLATTICE_PROTOCOL_V1, LITHOLATTICE_PROTOCOL_V2, LITHOLATTICE_PROTOCOL_V3};
    const uint8_t v2[] = {LITHOLATTICE_PROTOCOL_V1, LITHOLATTICE_PROTOCOL_V2};
    const auto both3 = negotiate(std::span<const uint8_t>(v3, 3), std::span<const uint8_t>(v3, 3));
    const auto mixed = negotiate(std::span<const uint8_t>(v2, 2), std::span<const uint8_t>(v3, 3));
    check(both3.ok && both3.version == LITHOLATTICE_PROTOCOL_V3 &&
              mixed.ok && mixed.version == LITHOLATTICE_PROTOCOL_V2,
          "net: v3/v3 negotiate v3; a v2 binary and a v3 binary fall back to v2");
  }
}


void testNetChannels() {
  std::printf("net: reliable/unreliable channels\n");
  using namespace net;

  // 4-byte id + 32 deterministic filler bytes: proves EVERY byte arrives.
  auto payloadFor = [](uint32_t id) {
    std::vector<uint8_t> p = {
        static_cast<uint8_t>(id & 0xFF), static_cast<uint8_t>((id >> 8) & 0xFF),
        static_cast<uint8_t>((id >> 16) & 0xFF), static_cast<uint8_t>((id >> 24) & 0xFF)};
    for (int i = 0; i < 32; ++i) p.push_back(static_cast<uint8_t>((id * 31u + uint32_t(i)) & 0xFF));
    return p;
  };
  auto idOf = [](const std::vector<uint8_t>& p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[3]) << 24);
  };

  // --- channel bit round-trips in the flags byte; header stays 14 bytes -------------------------------
  {
    PacketHeader h;
    h.flags = kFlagReliable | kFlagTruncated;
    h.seq = 7;
    h.ack = 3;
    h.ackBits = 0x000F;
    h.type = PayloadType::Event;
    h.payloadLen = 12;
    quantize::ByteWriter w;
    encodeHeader(h, w);
    bool ok = w.size() == kHeaderLen;  // header did NOT grow
    PacketHeader back;
    quantize::ByteReader r(w.data(), w.size());
    ok = ok && decodeHeader(r, back) && back == h && (back.flags & kFlagReliable);
    h.flags = 0;  // clear = unreliable / legacy snapshot passthrough
    quantize::ByteWriter w2;
    encodeHeader(h, w2);
    PacketHeader back2;
    quantize::ByteReader r2(w2.data(), w2.size());
    ok = ok && decodeHeader(r2, back2) && !(back2.flags & kFlagReliable);
    check(ok, "net: channel bit round-trips in header flags; 14-byte header unchanged");
  }

  // --- ChannelMap: which traffic uses which channel is config -------------------------------------------
  {
    ChannelMap m;
    bool ok = m.channelFor(PayloadType::Snapshot) == ChannelId::Unreliable &&
              m.channelFor(PayloadType::Ack) == ChannelId::Unreliable &&
              m.channelFor(PayloadType::ChunkBatch) == ChannelId::Unreliable &&
              m.channelFor(PayloadType::Event) == ChannelId::Reliable;
    m.event = ChannelId::Unreliable;
    m.chunkBatch = ChannelId::Reliable;
    ok = ok && m.channelFor(PayloadType::Event) == ChannelId::Unreliable &&
         m.channelFor(PayloadType::ChunkBatch) == ChannelId::Reliable &&
         m.channelFor(PayloadType::Snapshot) == ChannelId::Unreliable;  // default preserved
    check(ok,
          "net: ChannelMap defaults (snapshots keep today's wire behavior; events reliable) + override");
  }

  // --- reliable delivery under loss: every byte exactly once, in order ------------------------------------
  {
    ReliableChannel a, b;  // default config: RTO 200 ms
    const int kMsgs = 120;
    std::mt19937 rng(0xC4A4E1u);  // fixed seed: deterministic
    struct Flight {
      std::vector<uint8_t> bytes;
      int deliverAt = 0;
      bool toB = true;
    };
    std::vector<Flight> air;
    auto launch = [&](std::vector<uint8_t> d, bool toB, int now) {
      if (rng() % 100 < 25) return;           // 25% loss
      const int delay = int(rng() % 4) * 10;  // 0-30 ms jitter: reorders
      air.push_back(Flight{std::move(d), now + delay, toB});
      if (rng() % 100 < 5)  // 5% duplicates, on top of RTO retransmits
        air.push_back(Flight{air.back().bytes, now + delay + 10, toB});
    };

    bool ok = true;
    int sentA = 0, sentB = 0, now = 0;
    std::vector<uint32_t> gotA, gotB;
    auto drain = [&](ReliableChannel& ch, std::vector<uint32_t>& got) {
      std::vector<uint8_t> p;
      PayloadType t = PayloadType::Ack;
      while (ch.pollDelivery(p, t)) {
        if (p.empty()) {
          ok = ok && t == PayloadType::Ack;  // heartbeat: ack-only, no message id
          continue;
        }
        ok = ok && p.size() >= 4 && p == payloadFor(idOf(p));  // every byte exact
        got.push_back(idOf(p));
      }
    };

    const int kStepMs = 10;
    int step = 0;
    for (; step < 60000; ++step) {
      now = step * kStepMs;
      const bool draining = sentA == kMsgs && sentB == kMsgs &&
                            gotA.size() == size_t(kMsgs) && gotB.size() == size_t(kMsgs);
      if (!draining) {
        // Each side injects up to 2 new messages per step (backpressure retries).
        for (int i = 0; i < 2 && sentA < kMsgs; ++i) {
          std::vector<uint8_t> d;
          if (a.send(payloadFor(uint32_t(sentA)), PayloadType::Event, uint64_t(now), d)) {
            launch(std::move(d), true, now);
            ++sentA;
          }
        }
        for (int i = 0; i < 2 && sentB < kMsgs; ++i) {
          std::vector<uint8_t> d;
          if (b.send(payloadFor(uint32_t(sentB)), PayloadType::Event, uint64_t(now), d)) {
            launch(std::move(d), false, now);
            ++sentB;
          }
        }
      } else if (a.unackedCount() == 0 && b.unackedCount() == 0 && air.empty()) {
        break;  // quiescent: heartbeats are ack-only, so this state is stable
      }
      // Ack timer (the game's job too): ack-only heartbeats keep acks flowing
      // past the last data packet. They are not sequenced and never pollute
      // the unacked window, which counts data only.
      if (step % 5 == 0) {
        std::vector<uint8_t> d;
        if (a.heartbeat(uint64_t(now), d)) launch(std::move(d), true, now);
        if (b.heartbeat(uint64_t(now), d)) launch(std::move(d), false, now);
      }
      for (auto& d : a.poll(uint64_t(now))) launch(std::move(d), true, now);
      for (auto& d : b.poll(uint64_t(now))) launch(std::move(d), false, now);
      // Deliver what's due.
      for (size_t i = 0; i < air.size();) {
        if (air[i].deliverAt <= now) {
          Flight f = std::move(air[i]);
          if (i + 1 < air.size()) air[i] = std::move(air.back());  // no self-move
          air.pop_back();
          ok = ok && (f.toB ? b : a).receive(f.bytes.data(), f.bytes.size());
        } else {
          ++i;
        }
      }
      drain(a, gotA);
      drain(b, gotB);
    }
    ok = ok && step < 60000;
    // Exactly once, in order, both directions.
    for (int i = 0; i < kMsgs && ok; ++i)
      ok = gotA[size_t(i)] == uint32_t(i) && gotB[size_t(i)] == uint32_t(i);
    ok = ok && gotA.size() == size_t(kMsgs) && gotB.size() == size_t(kMsgs);
    check(ok,
          "net: reliable delivers every byte exactly once, in order, under 25% loss + reorder + dups");
  }

  // --- retransmit timing: RTO default 200 ms; ack stops retransmits ---------------------------------------
  {
    ReliableChannel a;
    std::vector<uint8_t> d;
    bool ok = a.send(payloadFor(1), PayloadType::Event, 1000, d);
    ok = ok && a.unackedCount() == 1;
    ok = ok && a.poll(1000).empty() && a.poll(1199).empty();  // not yet
    auto due = a.poll(1200);
    ok = ok && due.size() == 1;  // RTO fires at exactly 200 ms
    PacketHeader h;
    quantize::ByteReader r(due[0].data(), due[0].size());
    ok = ok && decodeHeader(r, h) && (h.flags & kFlagReliable);
    quantize::ByteReader r0(d.data(), d.size());  // same seq retransmitted...
    PacketHeader h0;
    ok = ok && decodeHeader(r0, h0) && h0.seq == h.seq;
    a.onAck(h.seq, 0);  // ...until acked, then silence.
    ok = ok && a.unackedCount() == 0 && a.poll(1000000).empty();
    // Backpressure: the in-flight window caps sends.
    ReliableConfig small;
    small.maxInFlight = 4;
    ReliableChannel c(small);
    bool filled = true;
    for (int i = 0; i < 4; ++i)
      filled = filled && c.send(payloadFor(uint32_t(i)), PayloadType::Event, 0, d);
    ok = ok && filled && !c.send(payloadFor(9), PayloadType::Event, 0, d) && c.unackedCount() == 4;
    // Oversize payload refused (MTU discipline), never silently truncated.
    ok = ok && !c.send(std::vector<uint8_t>(1201, 0), PayloadType::Event, 0, d);
    check(ok, "net: RTO retransmits at 200 ms default, same seq; ack stops it; window + MTU backpressure");
  }

  // --- unreliable: loss tolerated, old/dups dropped, never blocks -----------------------------------------
  {
    UnreliableChannel tx, rx;
    std::mt19937 rng(0x9E3779B9u);
    const int kN = 60;
    std::vector<uint32_t> got;
    std::vector<std::vector<uint8_t>> sent;
    bool ok = true;
    for (int i = 0; i < kN; ++i) {
      std::vector<uint8_t> d;
      ok = ok && tx.send(payloadFor(uint32_t(i)), PayloadType::ChunkBatch, d);
      sent.push_back(d);
      if (rng() % 2 == 0) {  // 50% loss
        std::vector<uint8_t> p;
        PayloadType t = PayloadType::Ack;
        if (rx.receive(d.data(), d.size(), p, t)) {
          ok = ok && t == PayloadType::ChunkBatch && p == payloadFor(uint32_t(i));
          got.push_back(uint32_t(i));
        }
      }
    }
    // Accepted seqs are a strictly-increasing subsequence: drops tolerated,
    // never reordered, never duplicated.
    for (size_t i = 1; i < got.size(); ++i) ok = ok && got[i] > got[i - 1];
    ok = ok && got.size() < size_t(kN) && !got.empty();
    // Redelivering an already-accepted packet is dropped (latest-wins).
    std::vector<uint8_t> p;
    PayloadType t = PayloadType::Ack;
    ok = ok && !rx.receive(sent[got[0]].data(), sent[got[0]].size(), p, t);
    // A reliable-channel datagram is refused here (the demux routes those).
    ReliableChannel rc;
    std::vector<uint8_t> rd;
    ok = ok && rc.send(payloadFor(7), PayloadType::Event, 0, rd) &&
         !rx.receive(rd.data(), rd.size(), p, t);
    check(ok, "net: unreliable latest-wins: drops tolerated, old/dups refused, never blocks");
  }

  // --- demux: one socket, channel bit routes ---------------------------------------------------------------
  {
    ChannelDemux demux;
    int reliableHits = 0, unreliableHits = 0;
    demux.onReliable([&](const uint8_t*, size_t) { ++reliableHits; });
    demux.onUnreliable([&](const uint8_t*, size_t) { ++unreliableHits; });
    ReliableChannel rc;
    UnreliableChannel uc;
    std::vector<uint8_t> rd, ud;
    bool ok = rc.send(payloadFor(3), PayloadType::Event, 0, rd) &&
              uc.send(payloadFor(4), PayloadType::ChunkBatch, ud);
    // Legacy snapshot datagram: framed by SnapshotClient, bit clear.
    PacketHeader h;
    h.type = PayloadType::Snapshot;
    h.seq = 41;
    quantize::ByteWriter w;
    encodeHeader(h, w);
    const std::vector<uint8_t> legacy = w.buf;
    ok = ok && demux.receive(rd.data(), rd.size()) && reliableHits == 1 && unreliableHits == 0;
    ok = ok && demux.receive(ud.data(), ud.size()) && reliableHits == 1 && unreliableHits == 1;
    ok = ok && demux.receive(legacy.data(), legacy.size()) && unreliableHits == 2;  // legacy: unreliable side
    ok = ok && !demux.receive(rd.data(), 5);                                        // malformed
    ChannelDemux bare;  // no handlers installed
    ok = ok && !bare.receive(rd.data(), rd.size());
    check(ok, "net: demux routes by channel bit; legacy snapshot datagrams stay on the unreliable side");
  }

  // --- mux: frames per ChannelMap; snapshots pass through --------------------------------------------------
  {
    ChannelMux mux;  // defaults
    std::vector<uint8_t> d;
    bool ok = mux.send(PayloadType::Event, payloadFor(1).data(), 36, 0, d);
    PacketHeader h;
    quantize::ByteReader r(d.data(), d.size());
    ok = ok && decodeHeader(r, h) && (h.flags & kFlagReliable);  // events -> reliable
    ok = ok && mux.send(PayloadType::ChunkBatch, payloadFor(2).data(), 36, 0, d);
    quantize::ByteReader r2(d.data(), d.size());
    ok = ok && decodeHeader(r2, h) && !(h.flags & kFlagReliable);  // chunks -> unreliable
    ok = ok && !mux.send(PayloadType::Snapshot, payloadFor(3).data(), 36, 0, d);  // passthrough
    ChannelMap m;
    m.chunkBatch = ChannelId::Reliable;  // game overrides: chunks are critical now
    mux.setMap(m);
    ok = ok && mux.send(PayloadType::ChunkBatch, payloadFor(4).data(), 36, 0, d);
    quantize::ByteReader r3(d.data(), d.size());
    ok = ok && decodeHeader(r3, h) && (h.flags & kFlagReliable);
    check(ok, "net: mux frames per ChannelMap; snapshots pass through (SnapshotClient owns their framing)");
  }
}

// ---- net confinement: mod rate limiting + send-path guard (mod hardening) ----

void testNetConfinement() {
  std::printf("net: mod network confinement (origin tagging, rate limits, suspension)\n");
  g_fails += net::runNetConfinementSelfTest();
}

// ---- script sandbox: binding allowlist + per-mod quotas (mod hardening) ----

// Binding stubs for the allowlist tests (signatures only need to match the decls).
static void stubPwn(const std::string&) {}
static int stubSetBlock(int, int, int, uint16_t) { return 0; }
static uint64_t stubWorldTick() { return 42; }
static uint64_t stubThrowTick() { throw std::runtime_error("binding boom"); }

static bool isPow2(uint64_t v) { return v && !(v & (v - 1)); }

void testScriptSandbox() {
  std::printf("script: binding allowlist + per-mod CPU/memory quotas\n");

  // --- allowlist: the table is deny-by-default --------------------------------
  ScriptBindingAllowlist::registerBuiltins();
  check(ScriptBindingAllowlist::isAllowlisted("void log(const string &in)") &&
            !ScriptBindingAllowlist::isAllowlisted("void system(const string &in)") &&
            !ScriptBindingAllowlist::isAllowlisted("int open(const string &in, int)"),
        "allowlist: core bindings listed; system()/open() are not");
  check(ScriptBindingAllowlist::requiredPermissions("int setBlock(int, int, int, uint16)") == uint32_t(kPermWorldWrite) &&
            ScriptBindingAllowlist::requiredPermissions("void log(const string &in)") == 0,
        "allowlist: entries carry their permission bits (setBlock needs world-write, log needs none)");

  // --- allowlist: the script host refuses unlisted / unpermitted bindings ----
  {
    ScriptEngine se;
    se.init();
    se.setModPermissions(0xFFFFFFFFu);  // all permissions: only the allowlist can refuse
    check(se.bindGlobalFunction("void pwn(const string &in)", asFUNCTION(stubPwn), asCALL_CDECL) < 0,
          "allowlist: script attempting an unlisted binding is refused");
    se.setModPermissions(0);  // no permissions granted
    check(se.bindGlobalFunction("int setBlock(int, int, int, uint16)", asFUNCTION(stubSetBlock), asCALL_CDECL) < 0,
          "allowlist: binding refused when the mod lacks the required permission bit");
    check(se.bindGlobalFunction("void log(const string &in)", asFUNCTION(stubPwn), asCALL_CDECL_OBJLAST, &se) < 0,
          "allowlist: duplicate registration of a builtin is refused");
    se.setModPermissions(kPermWorldWrite);
    check(se.bindGlobalFunction("int setBlock(int, int, int, uint16)", asFUNCTION(stubSetBlock), asCALL_CDECL) >= 0,
          "allowlist: binding succeeds with the required permission bit");
    se.setModPermissions(kPermWorldRead);
    check(se.bindGlobalFunction("uint64 worldTick()", asFUNCTION(stubWorldTick), asCALL_CDECL) >= 0,
          "allowlist: worldTick binds with world-read permission");
  }

  // --- allowlist: runtime registration is guarded too -------------------------
  {
    ScriptBindingAllowlist::registerBuiltins();
    check(!ScriptBindingAllowlist::registerBinding(
              {"void exfiltrateFile(const string &in)", kPermWorldRead, "evil"}),
          "allowlist: runtime binding with a file-access token is refused");
    check(!ScriptBindingAllowlist::registerBinding({"void sysExec(const string &in)", 0, "evil"}),
          "allowlist: runtime binding with a process token is refused");
    check(ScriptBindingAllowlist::registerBinding({"string modVersion()", 0, "test-only custom binding"}) &&
              ScriptBindingAllowlist::isAllowlisted("string modVersion()"),
          "allowlist: legit runtime binding is accepted");
    check(!ScriptBindingAllowlist::registerBinding({"string modVersion()", 0, "duplicate"}),
          "allowlist: duplicate runtime binding is refused");
  }

  // --- syncBindings: revocation truly unbinds (sandbox rebuild) ----------------
  {
    ScriptEngine se;
    se.init();
    se.setModPermissions(0);  // nothing granted: the refusal is recorded
    check(se.bindGlobalFunction("int setBlock(int, int, int, uint16)", asFUNCTION(stubSetBlock),
                                asCALL_CDECL) < 0,
          "syncBindings: setBlock refused with no permissions (refusal recorded)");
    se.setModPermissions(kPermWorldWrite);  // widen: syncBindings registers the refused decl
    check(se.loadSource("sync", "void onLoad() { setBlock(1, 2, 3, 4); }\n") &&
              se.callHook("void onLoad()"),
          "syncBindings: widened mask binds the previously refused decl");
    se.setModPermissions(0);  // narrow: revocation rebuilds the sandbox
    check(!se.loadSource("sync2", "void onLoad() { setBlock(1, 2, 3, 4); }\n"),
          "syncBindings: revoked binding is truly gone (a reload cannot re-expose it)");
    se.setModPermissions(kPermWorldWrite);  // grant again after a revocation
    check(se.loadSource("sync3", "void onLoad() { setBlock(1, 2, 3, 4); }\n") &&
              se.callHook("void onLoad()"),
          "syncBindings: re-grant re-binds after a revocation");
    se.unbindAll();
    check(!se.loadSource("sync4", "void onLoad() { setBlock(1, 2, 3, 4); }\n"),
          "syncBindings: unbindAll drops every binding");
  }

  // --- quotas: defaults are powers of two -------------------------------------
  {
    ScriptEngine se;
    check(isPow2(se.modBudgetUs()) && isPow2(se.modMemoryQuotaBytes()) && isPow2(se.throttleTicks()) &&
              isPow2(se.maxThrottles()),
          "quotas: defaults are powers of two (1024 us CPU, 1 MiB memory, 8-tick throttle, 4 throttles)");
    check(se.modBudgetUs() == ScriptEngine::kDefaultModBudgetUs &&
              se.modMemoryQuotaBytes() == ScriptEngine::kDefaultModMemoryQuotaBytes,
          "quotas: defaults match the kDefault* constants");
  }

  // --- quotas: CPU overrun -> throttle (skip ticks) -> suspend ----------------
  {
    ScriptEngine se;
    se.init();
    se.setModBudgetUs(64);  // tiny budget: the busy hook always exceeds it
    se.setThrottleTicks(2);
    se.setMaxThrottles(2);
    se.loadSource("quota",
                  "int g = 0;\n"
                  "void onTick() { int s = 0; for (int i = 0; i < 200000; i++) s += i; g = s; }\n");
    se.beginTick();
    const bool ran = se.callHook("void onTick()");
    check(ran && se.throttled() && !se.suspended(),
          "cpu quota: overrun throttles the mod (hook still ran this tick)");
    const int ticksBefore = se.tickCpuUs() >= 64 ? 1 : 0;
    const bool skipped = se.callHook("void onTick()");  // throttled: skipped, not an error
    check(ticksBefore == 1 && skipped && se.throttled(),
          "cpu quota: throttled mod's hooks are skipped for the throttle window");
    se.beginTick();
    se.beginTick();  // 2 ticks pass: throttle lifts
    check(!se.throttled(), "cpu quota: throttle lifts after the window with no new overrun");
    se.beginTick();
    se.callHook("void onTick()");  // second consecutive overrun -> suspend
    check(se.suspended() && !se.suspendReason().empty(),
          "cpu quota: repeated overruns suspend the mod with a logged reason");
    check(!se.callHook("void onTick()"), "cpu quota: suspended mod never executes hooks");
    se.resume();
    se.beginTick();
    check(!se.suspended() && !se.throttled(), "cpu quota: resume() clears quota state");
  }

  // --- quotas: memory overrun -> suspend --------------------------------------
  {
    ScriptEngine se;
    se.init();
    se.setModMemoryQuotaBytes(4096);  // tiny quota
    // A chain of 500 script objects: script class instances are allocated
    // through AngelScript's memory functions, so the per-mod accounting shim
    // sees them (the chain keeps them alive for the whole hook).
    se.loadSource("mem",
                  "class Node { Node@ next; int x; };\n"
                  "Node@ g;\n"
                  "void onLoad() {\n"
                  "  @g = Node();\n"
                  "  Node@ head = g;\n"
                  "  for (int i = 0; i < 500; i++) { @head.next = Node(); @head = head.next; }\n"
                  "}\n");
    const bool firstRan = se.callHook("void onLoad()");
    check(!firstRan && se.suspended() && se.suspendReason().find("memory") != std::string::npos,
          "memory quota: the in-flight script is ABORTED on quota trip (Execute does not run on), "
          "and the mod is suspended with a logged reason");
    check(!se.callHook("void onLoad()"), "memory quota: suspended mod never executes hooks");
  }

  // --- run(): a C++ exception from a bound callback is contained (F-L10) ------
  {
    ScriptEngine se;
    se.init();
    se.setModPermissions(kPermWorldRead);
    check(se.bindGlobalFunction("uint64 worldTick()", asFUNCTION(stubThrowTick), asCALL_CDECL) >= 0,
          "script run: throwing binding registers for the exception-guard check");
    se.loadSource("thrower", "void onLoad() { worldTick(); }\n");
    check(!se.callHook("void onLoad()"),
          "script run: a C++ exception escaping Execute() is caught and reported as a failed call (no terminate, no skipped restore)");
    // The engine is still usable afterwards: context/attribution were restored.
    se.loadSource("calm", "void onLoad() { log(\"calm\"); }\n");
    check(se.callHook("void onLoad()") && se.lastLog() == "calm",
          "script run: the engine executes normally after a contained binding exception");
  }

  // --- quotas: std::string heap buffers count (string blind spot closed) ------
  {
    ScriptEngine se;
    se.init();
    se.setModMemoryQuotaBytes(4096);  // tiny quota
    // 64-char chunks defeat SSO (libstdc++ SSO capacity is 15): every +=
    // grows the heap buffer through the global operator new hook, which
    // attributes it to the executing mod. Before the hook, these allocations
    // bypassed the AngelScript memory functions entirely (unbounded strings
    // escaped the quota).
    se.loadSource("strmem",
                  "void onLoad() {\n"
                  "  string chunk = \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\";\n"
                  "  string s = \"\";\n"
                  "  for (int i = 0; i < 200; i++) { s += chunk; }\n"
                  "}\n");
    const bool strRan = se.callHook("void onLoad()");
    check(!strRan && se.suspended() && se.suspendReason().find("memory") != std::string::npos,
          "memory quota: script string heap growth trips the quota (blind spot closed) and aborts "
          "the in-flight script");
    check(!se.callHook("void onLoad()"), "memory quota: string-suspended mod never executes hooks");
  }

  // --- quotas: the operator-new hook leaves engine allocations alone ---------
  {
    ScriptEngine se;
    se.init();
    check(se.modMemoryBytes() == 0, "quotas: fresh engine has nothing attributed");
    {
      // Heavy STL use OUTSIDE any mod execution: everything below goes
      // through the hooked operators, none of it is attributed, and all of
      // it must behave exactly like the default operators.
      std::string big(100000, 'x');
      std::vector<int> v(10000, 7);
      std::string* np = new std::string(5000, 'y');
      std::string* arr = new std::string[4];
      struct alignas(64) OverAligned { char pad[64]; };
      OverAligned* oa = new OverAligned;
      std::string* nt = new (std::nothrow) std::string(100, 'z');
      check(nt != nullptr, "quotas: nothrow new still allocates");
      volatile size_t sink = big.size() + v.size() + np->size() + arr[3].size() + nt->size();
      (void)sink;
      check(big[99999] == 'x' && v[9999] == 7 && (*np)[4999] == 'y' && (*nt)[99] == 'z',
            "quotas: hooked new/delete keeps engine allocation contents intact");
      check(reinterpret_cast<std::uintptr_t>(oa) % 64 == 0,
            "quotas: hooked operator new honors align_val_t alignment");
      check(reinterpret_cast<std::uintptr_t>(np) % alignof(std::string) == 0,
            "quotas: hooked operator new keeps default alignment");
      delete np;
      delete[] arr;
      delete oa;
      delete nt;
    }
    check(se.modMemoryBytes() == 0,
          "quotas: engine allocations outside mod execution are unattributed");
  }

  // --- quotas: alloc/free cycles balance (no leak, no double-count) ----------
  {
    ScriptEngine se;
    se.init();
    se.setModMemoryQuotaBytes(1 << 20);  // roomy: nothing here should suspend
    // 400 iterations of heap-string alloc + realloc + free inside one hook.
    // Locals die each iteration, so the attributed counter must return to
    // its pre-hook level when the hook returns: exercises new/new[]/delete,
    // sized/unsized delete, and the header bookkeeping under churn.
    // Note: the first Execute in the process leaves a small one-time
    // AngelScript lazy allocation (~56 bytes, freed at engine shutdown)
    // attributed to the mod, so the test warms up once and then asserts
    // exact no-drift across runs rather than absolute zero.
    se.loadSource("fuzz",
                  "void onLoad() {\n"
                  "  string chunk = \"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\";\n"
                  "  for (int i = 0; i < 400; i++) {\n"
                  "    string t = chunk;\n"
                  "    t += chunk;\n"
                  "    t += chunk;\n"
                  "  }\n"
                  "}\n");
    check(se.callHook("void onLoad()") && !se.suspended(),
          "memory quota: alloc/free fuzz hook runs clean inside quota");
    const int64_t baseline = se.modMemoryBytes();
    check(baseline >= 0 && baseline < 1024,
          "memory quota: fuzz hook leaves only the one-time engine lazy allocation");
    check(se.callHook("void onLoad()") && se.modMemoryBytes() == baseline,
          "memory quota: alloc/free cycles balance exactly (no leak, no double-count)");
    check(se.callHook("void onLoad()") && se.modMemoryBytes() == baseline,
          "memory quota: repeated fuzz runs show no accounting drift");
  }

  // --- quotas: a well-behaved mod is untouched --------------------------------
  {
    ScriptEngine se;
    se.init();
    // Ample CPU budget: this test is about "inside its budgets", not about
    // racing the wall clock — accountCpu() measures wall-clock time, so a
    // loaded machine can push even a trivial hook past the 1024 us default.
    se.setModBudgetUs(1 << 20);
    se.loadSource("nice", "void onLoad() { log(\"hello\"); }\n");
    se.beginTick();
    check(se.callHook("void onLoad()") && !se.throttled() && !se.suspended() && se.logCount() == 1,
          "quotas: a mod inside its budgets runs normally");
  }

  // --- suspension policy: backoff schedule is doubling, capped ----------------
  {
    using P = SuspensionPolicy;
    check(P::backoffTicksFor(1) == 64 && P::backoffTicksFor(2) == 128 &&
              P::backoffTicksFor(3) == 256 && P::backoffTicksFor(4) == 512 &&
              P::backoffTicksFor(5) == 1024 && P::backoffTicksFor(6) == 2048 &&
              P::backoffTicksFor(7) == 4096,
          "suspension policy: backoff doubles per consecutive suspension (64..4096 ticks)");
    check(P::backoffTicksFor(8) == 4096 && P::backoffTicksFor(1000) == 4096,
          "suspension policy: backoff capped at 4096 ticks");
    check(isPow2(P::kBackoffBaseTicks) && isPow2(P::kBackoffMaxTicks) &&
              isPow2(P::kMaxConsecutiveSuspensions) && isPow2(P::kProbationTicks) &&
              isPow2(P::kCleanTicksPerDecay),
          "suspension policy: all constants are powers of two (64/4096/8/64/256)");
  }

  // --- suspension policy: cooldown -> auto-recovery -> probation -------------
  {
    ScriptEngine se;
    se.init();
    se.setModId("policy-probe");
    se.setModBudgetUs(64);  // tiny budget: the busy hook always exceeds it
    se.setMaxThrottles(1);  // first overrun suspends
    se.loadSource("busy",
                  "int g = 0;\n"
                  "void onTick() { int s = 0; for (int i = 0; i < 200000; i++) s += i; g = s; }\n");
    se.beginTick();
    se.callHook("void onTick()");
    check(se.suspended() && se.suspensionPolicy().consecutiveSuspensions() == 1,
          "suspension policy: first suspension opens the consecutive count at 1");
    check(se.suspensionPolicy().cooldownRemaining() == 64,
          "suspension policy: first cooldown is the 64-tick base");
    for (uint32_t i = 0; i < 63; ++i) se.beginTick();
    check(se.suspended() && se.suspensionPolicy().cooldownRemaining() == 1,
          "suspension policy: mod stays suspended until the cooldown elapses");
    se.beginTick();  // 64th tick: cooldown hits zero
    const SuspensionPolicy& pol = se.suspensionPolicy();
    check(!se.suspended() && pol.probationActive() && pol.probationRemaining() == 64,
          "suspension policy: cooldown elapses -> auto-recovery onto 64-tick probation");
    check(pol.effectiveCpuBudgetUs(1024) == 512 && pol.effectiveCpuBudgetUs(1) == 1,
          "suspension policy: probation halves the CPU budget (min 1 us)");
    // Zero tolerance on probation: one overrun suspends immediately.
    se.beginTick();
    se.callHook("void onTick()");
    check(se.suspended() && !se.throttled() && pol.consecutiveSuspensions() == 2,
          "suspension policy: probation overrun suspends immediately (throttle ladder bypassed)");
    check(pol.cooldownRemaining() == 128,
          "suspension policy: second consecutive suspension cools down 128 ticks");
  }

  // --- suspension policy: throttling alone opens no record -------------------
  {
    ScriptEngine se;
    se.init();
    se.setModBudgetUs(64);
    se.setMaxThrottles(4);  // ladder: overruns throttle before suspending
    se.setThrottleTicks(2);
    se.loadSource("busy",
                  "int g = 0;\n"
                  "void onTick() { int s = 0; for (int i = 0; i < 200000; i++) s += i; g = s; }\n");
    se.beginTick();
    se.callHook("void onTick()");
    check(se.throttled() && !se.suspended() && se.suspensionPolicy().consecutiveSuspensions() == 0 &&
              se.suspensionPolicy().cooldownRemaining() == 0,
          "suspension policy: throttling alone opens no suspension record");
  }

  // --- suspension policy: clean ticks decay the consecutive count ------------
  {
    ScriptEngine se;
    se.init();
    se.setModId("policy-decay");
    se.setModBudgetUs(64);
    se.setMaxThrottles(1);
    se.loadSource("busy",
                  "int g = 0;\n"
                  "void onTick() { int s = 0; for (int i = 0; i < 200000; i++) s += i; g = s; }\n");
    se.beginTick();
    se.callHook("void onTick()");  // suspend #1
    check(se.suspensionPolicy().effectiveCpuBudgetUs(1024) == 1024,
          "suspension policy: outside probation the full budget applies");
    for (uint32_t i = 0; i < 64; ++i) se.beginTick();  // cooldown elapses, no hooks run
    check(!se.suspended() && se.suspensionPolicy().probationActive(),
          "suspension policy: idle cooldown still auto-recovers");
    // 256 clean ticks (64 probation + 192 monitoring) decay consecutive 1 -> 0.
    for (uint32_t i = 0; i < 256; ++i) se.beginTick();
    const SuspensionPolicy& pol = se.suspensionPolicy();
    check(pol.consecutiveSuspensions() == 0 && !pol.probationActive() && !se.suspended(),
          "suspension policy: 256 clean ticks decay the consecutive count to 0");
  }

  // --- suspension policy: 8 consecutive suspensions escalate to manual -------
  {
    ScriptEngine se;
    se.init();
    se.setModId("policy-escalate");
    se.setModBudgetUs(64);
    se.setMaxThrottles(1);
    se.loadSource("busy",
                  "int g = 0;\n"
                  "void onTick() { int s = 0; for (int i = 0; i < 200000; i++) s += i; g = s; }\n");
    for (uint32_t n = 1; n <= 8; ++n) {
      se.beginTick();
      se.callHook("void onTick()");  // overrun -> suspend (immediate on probation)
      check(se.suspended(), "suspension policy: overrun suspends the mod");
      const SuspensionPolicy& pol = se.suspensionPolicy();
      check(pol.consecutiveSuspensions() == n, "suspension policy: consecutive count tracks");
      if (n < 8) {
        const uint32_t cd = pol.cooldownRemaining();
        for (uint32_t i = 0; i < cd; ++i) se.beginTick();  // auto-recover onto probation
        check(!se.suspended() && pol.probationActive(),
              "suspension policy: cooldown elapses -> auto-recovery");
      }
    }
    check(se.suspensionPolicy().escalated(),
          "suspension policy: 8th consecutive suspension escalates (no more auto-recovery)");
    for (uint32_t i = 0; i < 9000; ++i) se.beginTick();
    check(se.suspended() && se.suspensionPolicy().escalated(),
          "suspension policy: escalated mod stays suspended without manual re-enable");
    se.resume();
    const SuspensionPolicy& pol = se.suspensionPolicy();
    check(!se.suspended() && !pol.escalated() && pol.consecutiveSuspensions() == 0 &&
              pol.cooldownRemaining() == 0 && !pol.probationActive(),
          "suspension policy: manual resume() clears escalation and the whole record");
    se.beginTick();
    se.callHook("void onTick()");
    check(se.suspended() && pol.cooldownRemaining() == 64,
          "suspension policy: post-resume suspension restarts at the 64-tick base");
  }

  // --- suspension policy: resume() clears mid-record state -------------------
  {
    ScriptEngine se;
    se.init();
    se.setModId("policy-resume");
    se.setModBudgetUs(64);
    se.setMaxThrottles(1);
    se.loadSource("busy",
                  "int g = 0;\n"
                  "void onTick() { int s = 0; for (int i = 0; i < 200000; i++) s += i; g = s; }\n");
    se.beginTick();
    se.callHook("void onTick()");                      // suspend #1
    for (uint32_t i = 0; i < 64; ++i) se.beginTick();  // auto-recover onto probation
    se.beginTick();
    se.callHook("void onTick()");  // probation: immediate suspend #2
    check(se.suspensionPolicy().consecutiveSuspensions() == 2,
          "suspension policy: two consecutive suspensions on the record");
    se.resume();
    const SuspensionPolicy& pol = se.suspensionPolicy();
    check(!se.suspended() && pol.consecutiveSuspensions() == 0 && pol.cooldownRemaining() == 0 &&
              !pol.probationActive() && !pol.escalated(),
          "suspension policy: resume() clears consecutive count, cooldown, and probation");
  }

  // --- suspension policy: every transition lands in the engine log tap -------
  {
    engineLogTap().clear();
    ScriptEngine se;
    se.init();
    se.setModId("policy-tap");
    se.setModBudgetUs(64);
    se.setMaxThrottles(1);
    se.loadSource("busy",
                  "int g = 0;\n"
                  "void onTick() { int s = 0; for (int i = 0; i < 200000; i++) s += i; g = s; }\n");
    auto tapHas = [](const char* needle, LogLevel lvl) {
      for (const LogRecord& r : engineLogTap().snapshot())
        if (r.level == lvl && r.text.find(needle) != std::string::npos) return true;
      return false;
    };
    se.beginTick();
    se.callHook("void onTick()");  // suspend
    check(tapHas("modsys: suspended mod 'policy-tap'", LogLevel::Warn),
          "suspension policy: suspension is in the tap at Warn with reason + numbers");
    check(tapHas("auto-recovery in 64 ticks (consecutive suspension #1 of 8)", LogLevel::Info),
          "suspension policy: cooldown decision is in the tap");
    for (uint32_t i = 0; i < 64; ++i) se.beginTick();  // cooldown elapses
    check(tapHas("modsys: auto-recovered mod 'policy-tap' after 64 ticks", LogLevel::Info),
          "suspension policy: recovery is in the tap at Info");
    // Escalation is loud: drive to 8 consecutive (7 more suspend/recover cycles).
    for (uint32_t n = 2; n <= 8; ++n) {
      se.beginTick();
      se.callHook("void onTick()");
      const uint32_t cd = se.suspensionPolicy().cooldownRemaining();
      for (uint32_t i = 0; i < cd; ++i) se.beginTick();
    }
    check(tapHas("AUTO-RECOVERY DISABLED", LogLevel::Error),
          "suspension policy: escalation is in the tap at Error (loud)");
    se.resume();
    check(tapHas("manually re-enabled; suspension record cleared", LogLevel::Info),
          "suspension policy: manual resume is in the tap");
  }
}

// ---- mods: ModLoader scan + SDK version enforcement ----

void testModLoader() {
  std::printf("mods: ModLoader scan + SDK enforcement (refuse major, warn minor)\n");
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "ll_modloader_test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  auto mkmod = [&](const char* name, const std::string& json) {
    fs::create_directories(dir / name, ec);
    std::ofstream(dir / name / "mod.json") << json;
  };
  mkmod("good", R"({"id":"good","version":"1.0.0","sdk":"1","type":"script","permissions":["world_read","tick","net"]})");
  mkmod("numsdk", R"({"id":"numsdk","version":"1.0.0","sdk":1,"type":"content"})");
  mkmod("newmajor", R"({"id":"newmajor","version":"1.0.0","sdk":"2","type":"script"})");
  mkmod("newminor", R"({"id":"newminor","version":"1.0.0","sdk":"1.1.0","type":"content"})");
  mkmod("rangeok", R"({"id":"rangeok","version":"1.0.0","sdk":">=1.0.0 <2.0.0","type":"content"})");
  mkmod("rangebad", R"({"id":"rangebad","version":"1.0.0","sdk":">=1.0.0 <1.0.0","type":"content"})");
  mkmod("broken", "{nope");
  mkmod("nosdk", R"({"id":"nosdk","version":"1.0.0"})");
  // Review F-M7/F-L8 fixtures: duplicate id (two folders), invalid id
  // format, and a numeric sdk outside uint32 range (old code hit UB on the
  // double->uint32 cast).
  mkmod("dupa", R"({"id":"dupmod","version":"1.0.0","sdk":"1","type":"content"})");
  mkmod("dupb", R"({"id":"dupmod","version":"1.0.0","sdk":"1","type":"content"})");
  mkmod("badid", R"({"id":"Bad Id!","version":"1.0.0","sdk":"1","type":"content"})");
  mkmod("hugesdk", R"({"id":"hugesdk","version":"1.0.0","sdk":1e30,"type":"content"})");
  mkmod("negsdk", R"({"id":"negsdk","version":"1.0.0","sdk":-2,"type":"content"})");

  ModLoader loader;
  const std::vector<ModInfo> scanned = loader.scan(dir.string());
  auto find = [](const std::vector<ModInfo>& v, const char* id) -> const ModInfo* {
    for (const auto& m : v)
      if (m.id == id) return &m;
    return nullptr;
  };
  check(scanned.size() == 8 && !find(scanned, "broken") && !find(scanned, "nosdk") &&
            !find(scanned, "Bad Id!") && !find(scanned, "hugesdk") && !find(scanned, "negsdk"),
        "modloader: scan finds the valid mods (string or numeric sdk) and skips malformed ones, invalid ids, and out-of-range numeric sdk (F-M7/F-L8)");
  const ModInfo* good = find(scanned, "good");
  check(good && good->permissions == uint32_t(kPermWorldRead | kPermTick | kPermNet) && good->path == (dir / "good").string() &&
            loader.needsPermissionPrompt(*good),
        "modloader: permissions parse to the bitmask (incl. \"net\" -> kPermNet); script mods need the prompt");

  std::string reason;
  check(good && loader.checkCompatibility(*good, reason) && reason.empty(),
        "modloader: matching SDK major loads cleanly");
  const ModInfo* numsdk = find(scanned, "numsdk");
  check(numsdk && loader.checkCompatibility(*numsdk, reason) && reason.empty(),
        "modloader: numeric sdk form loads cleanly");
  const ModInfo* newmajor = find(scanned, "newmajor");
  check(newmajor && !loader.checkCompatibility(*newmajor, reason) && !reason.empty(),
        "modloader: SDK major mismatch is refused with a reason");
  const ModInfo* newminor = find(scanned, "newminor");
  check(newminor && loader.checkCompatibility(*newminor, reason) && !reason.empty(),
        "modloader: SDK minor mismatch warns and continues");
  const ModInfo* rangeok = find(scanned, "rangeok");
  check(rangeok && loader.checkCompatibility(*rangeok, reason) && reason.empty(),
        "modloader: in-range semver range loads cleanly");
  const ModInfo* rangebad = find(scanned, "rangebad");
  check(rangebad && !loader.checkCompatibility(*rangebad, reason),
        "modloader: out-of-range semver range is refused");

  const std::vector<ModInfo> loaded = loader.load(dir.string());
  check(loaded.size() == 5 && find(loaded, "good") && find(loaded, "numsdk") && find(loaded, "newminor") &&
            find(loaded, "rangeok") && !find(loaded, "newmajor") && !find(loaded, "rangebad"),
        "modloader: load() enforces compatibility (refuse major, warn-and-continue minor)");
  {
    int dupCount = 0;
    for (const auto& m : loaded)
      if (m.id == "dupmod") ++dupCount;
    check(dupCount == 1, "modloader: duplicate mod ids are rejected in load() — the first folder wins (F-M7)");
  }
  fs::remove_all(dir, ec);
}

// ---- net handshake: protocol version negotiation at connect ----

void testNetHandshake() {
  std::printf("net: connect handshake (protocol version negotiation)\n");
  using namespace net;

  // --- negotiate(): the matrix ------------------------------------------------
  {
    bool ok = true;
    const uint8_t v1[] = {1}, v2[] = {2}, v12[] = {1, 2}, v212[] = {2, 1, 2}, v21[] = {2, 1};
    auto same = negotiate(v2, v2);
    ok = ok && same.ok && same.version == 2;  // same version
    auto hi = negotiate(v12, v12);
    ok = ok && hi.ok && hi.version == 2;  // highest common wins
    auto lo = negotiate(v12, v1);
    ok = ok && lo.ok && lo.version == 1;  // highest common is 1
    auto none = negotiate(v1, v2);
    ok = ok && !none.ok;  // incompatible: no common version
    auto noOffer = negotiate(std::span<const uint8_t>{}, v12);
    ok = ok && !noOffer.ok;  // empty offer list: incompatible
    auto messy = negotiate(v212, v21);  // unsorted + dupes: harmless
    ok = ok && messy.ok && messy.version == 2;
    check(ok, "net: negotiate() matrix: same version, highest-common, incompatible-major -> clean fail");
  }

  // --- protocolCompatible(): the real same-major rule -------------------------
  {
    bool ok = protocolCompatible(1, 1) && protocolCompatible(2, 2);
    ok = ok && !protocolCompatible(1, 2) && !protocolCompatible(2, 1);
    check(ok, "net: protocolCompatible() is the same-major rule (equal versions only)");
  }

  // --- engineCompatible(): minor/patch drift within a major is OK -------------
  {
    bool ok = engineCompatible(EngineVersion{0, 1, 0}, EngineVersion{0, 9, 7});  // higher minor each side
    ok = ok && engineCompatible(EngineVersion{0, 1, 0}, EngineVersion{0, 1, 3});  // patch drift
    ok = ok && !engineCompatible(EngineVersion{0, 1, 0}, EngineVersion{1, 0, 0});  // major differs
    check(ok, "net: engineCompatible(): major must match, minor/patch differences OK");
  }

  // --- message codec round-trips ----------------------------------------------
  {
    bool ok = true;
    const uint8_t offer[] = {2, 1, 2};
    const auto hello = encodeClientHello(EngineVersion{0, 1, 0}, LITHOLATTICE_SDK_VERSION, offer);
    ClientHelloMsg hm;
    ok = ok && !hello.empty() && hello.size() <= 6 + kHsMaxProtoList &&
         parseClientHello(hello.data(), hello.size(), hm) && hm.engine == EngineVersion{0, 1, 0} &&
         hm.sdkVersion == LITHOLATTICE_SDK_VERSION &&
         hm.protocols == std::vector<uint8_t>{1, 2};  // sorted, deduped
    const auto sh = encodeServerHello(2, EngineVersion{0, 2, 0}, LITHOLATTICE_SDK_VERSION);
    ServerHelloMsg sm;
    ok = ok && parseServerHello(sh.data(), sh.size(), sm) && sm.chosenProtocol == 2 &&
         sm.engine == EngineVersion{0, 2, 0} && sm.sdkVersion == LITHOLATTICE_SDK_VERSION;
    const auto rj = encodeReject(RejectReason::NoCommonProtocol, 2);
    RejectMsg rm;
    ok = ok && parseReject(rj.data(), rj.size(), rm) &&
         rm.reason == RejectReason::NoCommonProtocol && rm.serverMaxProtocol == 2;
    // malformed inputs are refused, never half-parsed
    ClientHelloMsg bad;
    ok = ok && !parseClientHello(nullptr, 0, bad);
    const uint8_t badId[] = {9, 0, 1, 0, 1, 1, 2};
    ok = ok && !parseClientHello(badId, sizeof badId, bad);
    const uint8_t noProto[] = {kHsClientHello, 0, 1, 0, 1, 0};
    ok = ok && !parseClientHello(noProto, sizeof noProto, bad);  // nProto = 0
    const uint8_t shortHello[] = {kHsClientHello, 0, 1, 0, 1, 1};
    ok = ok && !parseClientHello(shortHello, sizeof shortHello, bad);  // truncated list
    const uint8_t longHello[] = {kHsClientHello, 0, 1, 0, 1, 2, 1};  // nProto=2, only 1 present
    ok = ok && !parseClientHello(longHello, sizeof longHello, bad);
    const uint8_t shortSh[] = {kHsServerHello, 2, 0, 1, 0};  // 5 bytes: pre-SDK shape
    ServerHelloMsg badS;
    ok = ok && !parseServerHello(shortSh, sizeof shortSh, badS);
    const uint8_t noReason[] = {kHsReject, 0, 2};
    RejectMsg badR;
    ok = ok && !parseReject(noReason, sizeof noReason, badR);  // reason None is malformed
    // unset SDK line is a programmer error: no hello is built
    ok = ok && encodeClientHello(EngineVersion{0, 1, 0}, kSdkVersionUnset, offer).empty();
    check(ok, "net: handshake messages encode/parse round-trip (SDK line rides along); malformed input refused");
  }

  // --- codec dispatch ----------------------------------------------------------
  {
    bool ok = codecForProtocol(1) == CodecKind::V1Baseline;
    ok = ok && codecForProtocol(2) == CodecKind::V2Delta;
    ok = ok && codecForProtocol(0) == CodecKind::Unknown;
    ok = ok && codecForProtocol(3) == CodecKind::V3StableIds;
    ok = ok && codecForProtocol(99) == CodecKind::Unknown;
    check(ok, "net: codecForProtocol() dispatches v1->baseline, v2->delta, v3->stable-ids, unknown->Unknown");
  }

  // --- happy path over a fake reliable transport (with one dropped hello) ------
  {
    bool ok = true;
    ReliableChannel cc, sc;  // client / server channel endpoints
    for (auto* c : {&cc, &sc}) {
      c->setAcceptedVersions(knownProtocolVersions());  // pre-handshake: accept {1,2,3}
      c->setVersion(maxProtocolVersion());              // stamp own highest
    }
    ChannelDemux cdemux, sdemux;  // route by channel bit, like the game loop
    cdemux.onReliable([&](const uint8_t* d, size_t n) { cc.receive(d, n); });
    sdemux.onReliable([&](const uint8_t* d, size_t n) { sc.receive(d, n); });
    cdemux.setAcceptedVersions(knownProtocolVersions());
    sdemux.setAcceptedVersions(knownProtocolVersions());

    ConnectHandshake chs(HandshakeConfig{HandshakeRole::Client, {1, 2, 3}, {0, 1, 0}, LITHOLATTICE_SDK_VERSION, 5000});
    ConnectHandshake shs(HandshakeConfig{HandshakeRole::Server, {1, 2, 3}, {0, 1, 0}, LITHOLATTICE_SDK_VERSION, 5000});

    std::deque<std::vector<uint8_t>> c2s, s2c;  // the fake wire
    uint64_t now = 1000;
    std::vector<uint8_t> hello;
    ok = ok && chs.start(now, hello) && chs.state() == HandshakeState::HelloSent;
    ok = ok && !chs.start(now, hello);  // second start: refused
    std::vector<uint8_t> dg;
    ok = ok && cc.send(hello, PayloadType::Handshake, now, dg);
    {  // the wire shape: reliable bit, Handshake type, own-max version byte
      PacketHeader h;
      quantize::ByteReader r(dg.data(), dg.size());
      ok = ok && decodeHeader(r, h) && (h.flags & kFlagReliable) &&
           h.type == PayloadType::Handshake && h.version == 3;
    }
    // drop the first hello on the wire: the reliable channel must recover it
    now += 250;  // past the 200 ms RTO
    const auto rtx = cc.poll(now);
    ok = ok && rtx.size() == 1 && rtx[0].size() == dg.size();  // one retransmit, same seq
    c2s.push_back(rtx[0]);
    while (!c2s.empty()) {  // deliver to the server side
      auto d = std::move(c2s.front());
      c2s.pop_front();
      ok = ok && sdemux.receive(d.data(), d.size());
    }
    std::vector<uint8_t> payload;
    PayloadType t = PayloadType::Ack;
    while (sc.pollDelivery(payload, t)) {
      ok = ok && t == PayloadType::Handshake;
      shs.onPayload(payload, now);
    }
    ok = ok && shs.state() == HandshakeState::Agreed && shs.agreedProtocol() == 3 &&
         shs.peerHelloParsed() && shs.peerEngine() == EngineVersion{0, 1, 0} &&
         shs.peerSdkVersion() == LITHOLATTICE_SDK_VERSION;
    std::vector<uint8_t> reply;
    ok = ok && shs.pending(reply) && !shs.pending(reply);  // exactly one reply
    std::vector<uint8_t> rdg;
    ok = ok && sc.send(reply, PayloadType::Handshake, now, rdg);
    s2c.push_back(std::move(rdg));
    while (!s2c.empty()) {
      auto d = std::move(s2c.front());
      s2c.pop_front();
      ok = ok && cdemux.receive(d.data(), d.size());
    }
    while (cc.pollDelivery(payload, t)) chs.onPayload(payload, now);
    ok = ok && chs.state() == HandshakeState::Agreed && chs.agreedProtocol() == 3 &&
         chs.peerHelloParsed() && chs.peerEngine() == EngineVersion{0, 1, 0} &&
         chs.peerSdkVersion() == LITHOLATTICE_SDK_VERSION;
    // post-handshake: narrow to the agreed version; anything else is rejected
    for (auto* c : {&cc, &sc}) {
      c->setVersion(3);
      c->setAcceptedVersions({3});
    }
    ReliableChannel legacy;  // a v1-stamped datagram must not slip through now
    legacy.setVersion(1);
    std::vector<uint8_t> ldg;
    const uint8_t pl[] = {7};
    ok = ok && legacy.send(pl, sizeof pl, PayloadType::Event, now, ldg);
    ok = ok && !cc.receive(ldg.data(), ldg.size()) && cc.lastReject().versionMismatch &&
         cc.lastReject().got == 1;
    check(ok, "net: handshake completes over the reliable channel (survives a dropped hello); "
              "both sides agree on v3; post-agreement narrowing rejects v1");
  }

  // --- incompatible protocol versions: clean reject with a reason -------------
  {
    bool ok = true;
    // client only speaks v1, server only speaks v2
    ConnectHandshake chs(HandshakeConfig{HandshakeRole::Client, {1}, {0, 1, 0}, LITHOLATTICE_SDK_VERSION, 5000});
    ConnectHandshake shs(HandshakeConfig{HandshakeRole::Server, {2}, {0, 1, 0}, LITHOLATTICE_SDK_VERSION, 5000});
    std::vector<uint8_t> hello;
    ok = ok && chs.start(0, hello);
    shs.onPayload(hello, 0);
    ok = ok && shs.state() == HandshakeState::Rejected &&
         shs.rejectReason() == RejectReason::NoCommonProtocol;
    std::vector<uint8_t> reply;
    ok = ok && shs.pending(reply);
    RejectMsg rm;
    ok = ok && parseReject(reply.data(), reply.size(), rm) &&
         rm.reason == RejectReason::NoCommonProtocol && rm.serverMaxProtocol == 2;
    chs.onPayload(reply, 0);
    ok = ok && chs.state() == HandshakeState::Rejected &&
         chs.rejectReason() == RejectReason::NoCommonProtocol;
    // terminal states never move
    shs.onPayload(hello, 0);
    ok = ok && shs.state() == HandshakeState::Rejected;
    check(ok, "net: no common protocol version -> clean Reject(NoCommonProtocol) both sides");
  }

  // --- incompatible engine major: clean reject ---------------------------------
  {
    bool ok = true;
    ConnectHandshake chs(HandshakeConfig{HandshakeRole::Client, {1, 2}, {9, 0, 0}, LITHOLATTICE_SDK_VERSION, 5000});
    ConnectHandshake shs(HandshakeConfig{HandshakeRole::Server, {1, 2}, {0, 1, 0}, LITHOLATTICE_SDK_VERSION, 5000});
    std::vector<uint8_t> hello;
    ok = ok && chs.start(0, hello);
    shs.onPayload(hello, 0);
    ok = ok && shs.state() == HandshakeState::Rejected &&
         shs.rejectReason() == RejectReason::IncompatibleEngineMajor &&
         shs.peerHelloParsed() && shs.peerEngine() == EngineVersion{9, 0, 0};
    std::vector<uint8_t> reply;
    ok = ok && shs.pending(reply);
    chs.onPayload(reply, 0);
    ok = ok && chs.state() == HandshakeState::Rejected &&
         chs.rejectReason() == RejectReason::IncompatibleEngineMajor;
    check(ok, "net: engine major mismatch -> clean Reject(IncompatibleEngineMajor) both sides");
  }

  // --- server answers with a version the client never offered -------------------
  {
    bool ok = true;
    ConnectHandshake chs(HandshakeConfig{HandshakeRole::Client, {2}, {0, 1, 0}, LITHOLATTICE_SDK_VERSION, 5000});
    std::vector<uint8_t> hello;
    ok = ok && chs.start(0, hello);
    const auto bogus = encodeServerHello(1, EngineVersion{0, 1, 0}, LITHOLATTICE_SDK_VERSION);  // v1 was not offered
    chs.onPayload(bogus, 0);
    ok = ok && chs.state() == HandshakeState::Rejected &&
         chs.rejectReason() == RejectReason::MalformedHello;
    check(ok, "net: client rejects a ServerHello naming a version it never offered");
  }

  // --- sdkCompatible(): the SDK line is exact-match, never negotiable ---------
  {
    bool ok = sdkCompatible(1, 1);
    ok = ok && !sdkCompatible(1, 2) && !sdkCompatible(2, 1);
    check(ok, "net: sdkCompatible(): SDK major lines must match exactly (no cross-line downgrade)");
  }

  // --- runtime version accessors ------------------------------------------------
  {
    bool ok = runtimeEngineVersion() == EngineVersion{LITHOLATTICE_ENGINE_VERSION_MAJOR,
                                                      LITHOLATTICE_ENGINE_VERSION_MINOR,
                                                      LITHOLATTICE_ENGINE_VERSION_PATCH};
    ok = ok && runtimeProtocolVersion() == kProtocolVersion;
    check(ok, "net: runtime engine/protocol versions are exposed (handshake stamps these)");
  }

  // --- SDK major mismatch: clean reject, no silent downgrade ---------------------
  {
    bool ok = true;
    // server side: client on SDK line 1, server on line 2
    ConnectHandshake chs(HandshakeConfig{HandshakeRole::Client, {1, 2}, {0, 1, 0}, 1, 5000});
    ConnectHandshake shs(HandshakeConfig{HandshakeRole::Server, {1, 2}, {0, 1, 0}, 2, 5000});
    std::vector<uint8_t> hello;
    ok = ok && chs.start(0, hello);
    shs.onPayload(hello, 0);
    ok = ok && shs.state() == HandshakeState::Rejected &&
         shs.rejectReason() == RejectReason::IncompatibleSdkMajor &&
         shs.peerSdkVersion() == 1;
    std::vector<uint8_t> reply;
    ok = ok && shs.pending(reply);
    RejectMsg rm;
    ok = ok && parseReject(reply.data(), reply.size(), rm) &&
         rm.reason == RejectReason::IncompatibleSdkMajor;
    chs.onPayload(reply, 0);
    ok = ok && chs.state() == HandshakeState::Rejected &&
         chs.rejectReason() == RejectReason::IncompatibleSdkMajor;
    // client side: server answers with a different SDK line than the client's
    ConnectHandshake chs2(HandshakeConfig{HandshakeRole::Client, {2}, {0, 1, 0}, 1, 5000});
    std::vector<uint8_t> hello2;
    ok = ok && chs2.start(0, hello2);
    const auto foreign = encodeServerHello(2, EngineVersion{0, 1, 0}, 2);  // SDK line 2
    chs2.onPayload(foreign, 0);
    ok = ok && chs2.state() == HandshakeState::Rejected &&
         chs2.rejectReason() == RejectReason::IncompatibleSdkMajor;
    check(ok, "net: SDK major mismatch -> clean Reject(IncompatibleSdkMajor) both directions");
  }

  // --- unset SDK line: fail-closed before any comparison --------------------------
  {
    bool ok = true;
    HandshakeConfig noSdk{HandshakeRole::Client, {2}, {0, 1, 0}, kSdkVersionUnset, 5000};
    ConnectHandshake chs(noSdk);
    std::vector<uint8_t> hello;
    ok = ok && !chs.start(0, hello);  // client refuses to start without its SDK line
    ok = ok && chs.state() == HandshakeState::Idle;
    // server with an unset SDK line rejects instead of comparing
    HandshakeConfig srvNoSdk{HandshakeRole::Server, {2}, {0, 1, 0}, kSdkVersionUnset, 5000};
    ConnectHandshake shs(srvNoSdk);
    const uint8_t v2only[] = {2};
    const auto good = encodeClientHello(EngineVersion{0, 1, 0}, LITHOLATTICE_SDK_VERSION, v2only);
    ok = ok && !good.empty();
    shs.onPayload(good, 0);
    ok = ok && shs.state() == HandshakeState::Rejected &&
         shs.rejectReason() == RejectReason::MalformedHello;
    std::vector<uint8_t> reply;
    ok = ok && shs.pending(reply);  // the peer still gets a reject to log
    check(ok, "net: unset SDK line fails closed (no hello built, no comparison made)");
  }

  // --- subset offers: highest COMMON version wins (graceful downgrade) -------------
  {
    bool ok = true;
    // client only knows v1, server knows both: agree on 1 (legitimate downgrade)
    ConnectHandshake chs(HandshakeConfig{HandshakeRole::Client, {1}, {0, 1, 0},
                                         LITHOLATTICE_SDK_VERSION, 5000});
    ConnectHandshake shs(HandshakeConfig{HandshakeRole::Server, {1, 2}, {0, 1, 0},
                                         LITHOLATTICE_SDK_VERSION, 5000});
    std::vector<uint8_t> hello;
    ok = ok && chs.start(0, hello);
    shs.onPayload(hello, 0);
    ok = ok && shs.state() == HandshakeState::Agreed && shs.agreedProtocol() == 1;
    std::vector<uint8_t> reply;
    ok = ok && shs.pending(reply);
    chs.onPayload(reply, 0);
    ok = ok && chs.state() == HandshakeState::Agreed && chs.agreedProtocol() == 1;
    // ... and the codec dispatch tells the game what agreeing on 1 means
    ok = ok && codecForProtocol(chs.agreedProtocol()) == CodecKind::V1Baseline;
    // mirrored: server only knows v2, client offers both: agree on 2
    ConnectHandshake chs2(HandshakeConfig{HandshakeRole::Client, {1, 2}, {0, 1, 0},
                                          LITHOLATTICE_SDK_VERSION, 5000});
    ConnectHandshake shs2(HandshakeConfig{HandshakeRole::Server, {2}, {0, 1, 0},
                                          LITHOLATTICE_SDK_VERSION, 5000});
    std::vector<uint8_t> hello2;
    ok = ok && chs2.start(0, hello2);
    shs2.onPayload(hello2, 0);
    ok = ok && shs2.state() == HandshakeState::Agreed && shs2.agreedProtocol() == 2;
    check(ok, "net: subset offers agree on the highest common version (graceful downgrade, not a refusal)");
  }

  // --- timeouts: clean failure, never a hang ------------------------------------
  {
    bool ok = true;
    ConnectHandshake chs(HandshakeConfig{HandshakeRole::Client, {2}, {0, 1, 0}, LITHOLATTICE_SDK_VERSION, 5000});
    std::vector<uint8_t> hello;
    ok = ok && chs.start(0, hello);
    ok = ok && chs.pollTimeout(4999) == HandshakeState::HelloSent;  // not yet
    ok = ok && chs.pollTimeout(5000) == HandshakeState::TimedOut;   // deadline
    ok = ok && chs.pollTimeout(10'000'000) == HandshakeState::TimedOut;  // terminal: never moves
    ok = ok && chs.rejectReason() == RejectReason::None;  // no peer fault: just silence
    ConnectHandshake idle(HandshakeConfig{HandshakeRole::Server, {2}, {0, 1, 0}, LITHOLATTICE_SDK_VERSION, 5000});
    ok = ok && idle.pollTimeout(10'000'000) == HandshakeState::Idle;  // Idle never times out
    check(ok, "net: handshake timeout fails clean (TimedOut), never hangs; Idle is exempt");
  }

  // --- version byte round-trip in headers ----------------------------------------
  {
    bool ok = true;
    PacketHeader h;
    h.version = 1;
    h.flags = kFlagReliable;
    h.seq = 42;
    h.type = PayloadType::Event;
    h.payloadLen = 0;
    quantize::ByteWriter w;
    ok = ok && encodeHeader(h, w);
    PacketHeader o;
    HeaderReject rej;
    quantize::ByteReader r1(w.buf.data(), w.buf.size());
    ok = ok && decodeHeader(r1, o, 1, &rej) && o == h;  // accepted version: fine
    quantize::ByteReader r2(w.buf.data(), w.buf.size());
    ok = ok && !decodeHeader(r2, o, &rej) && rej.versionMismatch && rej.got == 1;  // default rejects v1, loudly
    const uint8_t badMagic[kHeaderLen] = {};  // bad magic is NOT a version mismatch
    quantize::ByteReader r3(badMagic, sizeof badMagic);
    HeaderReject rej3;
    ok = ok && !decodeHeader(r3, o, 1, &rej3) && !rej3.versionMismatch;
    check(ok, "net: header version byte round-trips; mismatch rejected with a clear reason");
  }

  // --- negotiated version on the snapshot path ------------------------------------
  {
    bool ok = true;
    SnapshotClient srv;
    srv.setVersion(1);  // post-handshake: agreed on v1
    std::vector<SnapshotEntity> e(1);
    e[0].id = 7;
    e[0].x = 1024;
    e[0].atRest = true;
    const auto pkt = srv.build(0, e);
    ok = ok && pkt.size() > kHeaderLen && pkt[2] == 1;  // version byte on the wire
    SnapshotReceiver cli;
    cli.setAcceptedVersions({1});
    ok = ok && cli.receive(pkt.data(), pkt.size()) == SnapshotReceiver::Result::Applied;
    SnapshotReceiver strict;  // default: v2 only
    ok = ok && strict.receive(pkt.data(), pkt.size()) == SnapshotReceiver::Result::Dropped &&
         strict.lastReject().versionMismatch && strict.lastReject().got == 1;
    check(ok, "net: SnapshotClient stamps the negotiated version; receiver validates it");
  }
}

void testNetEncryption() {
  std::printf("net: encryption stubs (deliberate plaintext v1, replay window, KEX shape)\n");
  using namespace net;

  // --- overhead budget ---------------------------------------------------------
  {
    bool ok = kCryptoTagBytes == 16 && kCryptoNonceBytes == 12;
    ok = ok && kCryptoWireOverheadBytes == 16;  // implicit nonce: tag only on the wire
    ok = ok && kCryptoWireOverheadBytes <= kCryptoMaxOverheadBytes && kCryptoMaxOverheadBytes == 28;
    check(ok, "net: crypto overhead budget: 16 B/pkt on the wire (tag only), 28 B never-exceed");
  }

  // --- NullCipher: refuses, never silently plaintext ----------------------------
  {
    bool ok = true;
    NullCipher nc;
    ok = ok && nc.isPlaintext();
    uint8_t nonce[kCryptoNonceBytes] = {};
    const uint8_t plain[] = {1, 2, 3, 4};
    uint8_t out[64];
    std::fill(std::begin(out), std::end(out), 0xAA);
    const auto sr = nc.seal(plain, sizeof plain, nonce, out, sizeof out);
    ok = ok && sr.error == CryptoError::NotImplemented && sr.bytes == 0;
    ok = ok && out[0] == 0xAA;  // buffers untouched on failure
    const auto or_ = nc.open(plain, sizeof plain, nonce, out, sizeof out);
    ok = ok && or_.error == CryptoError::NotImplemented && or_.bytes == 0;
    ok = ok && out[0] == 0xAA;
    check(ok, "net: NullCipher seal/open return NotImplemented and touch nothing (never silently plaintext)");
  }

  // --- CryptoSession: arming, requireEncryption, deriveKeys stub ------------------
  {
    bool ok = true;
    CryptoSession s(CryptoConfig{false, 0x12345678});
    ok = ok && s.isPlaintext() && s.state() == CryptoSessionState::Plaintext;
    // arming the null cipher is allowed in v1 (deliberate plaintext), still plaintext
    ok = ok && s.armCipher(std::make_unique<NullCipher>()).error == CryptoError::None;
    ok = ok && s.isPlaintext() && s.state() == CryptoSessionState::Plaintext;
    // seal/open while plaintext: NotImplemented (the game sends raw payloads itself)
    uint8_t buf[64];
    const uint8_t pl[] = {9, 9};
    ok = ok && s.sealPacket(pl, sizeof pl, buf, sizeof buf).error == CryptoError::NotImplemented;
    ok = ok && s.openPacket(pl, sizeof pl, 0, buf, sizeof buf).error == CryptoError::NotImplemented;
    // deriveKeys is a stub until the backend lands
    KeyExchangeMsg a, b;
    ok = ok && s.deriveKeys(a, b).error == CryptoError::NotImplemented;
    // requireEncryption: arming a plaintext cipher fails closed, old cipher kept
    CryptoSession hard(CryptoConfig{true, 0x9});
    ok = ok && hard.armCipher(std::make_unique<NullCipher>()).error == CryptoError::NotImplemented;
    ok = ok && hard.isPlaintext() && hard.state() == CryptoSessionState::Plaintext;
    ok = ok && hard.armCipher(nullptr).error == CryptoError::BadKey;  // null backend refused
    check(ok, "net: CryptoSession arms NullCipher as deliberate plaintext; requireEncryption refuses it");
  }

  // --- makeNonce: deterministic, distinct, LE layout --------------------------------
  {
    bool ok = true;
    uint8_t n1[kCryptoNonceBytes], n2[kCryptoNonceBytes], n3[kCryptoNonceBytes];
    makeNonce(0x12345678, 42, n1);
    makeNonce(0x12345678, 42, n2);
    makeNonce(0x12345678, 43, n3);
    ok = ok && std::equal(std::begin(n1), std::end(n1), std::begin(n2));  // deterministic
    ok = ok && !std::equal(std::begin(n1), std::end(n1), std::begin(n3));  // counter matters
    ok = ok && n1[0] == 0x78 && n1[1] == 0x56 && n1[2] == 0x34 && n1[3] == 0x12;  // LE session id
    ok = ok && n1[4] == 42 && n1[5] == 0;  // LE counter
    CryptoSession s(CryptoConfig{false, 0xAABBCCDD});
    uint8_t a[kCryptoNonceBytes], b[kCryptoNonceBytes];
    s.nextNonce(a);
    s.nextNonce(b);
    ok = ok && s.sendCounter() == 2 && !std::equal(std::begin(a), std::end(a), std::begin(b));
    ok = ok && a[0] == 0xDD && a[1] == 0xCC && a[2] == 0xBB && a[3] == 0xAA && a[4] == 0 && b[4] == 1;
    check(ok, "net: makeNonce packs sessionId||counter (LE, implicit on the wire); send counter advances");
  }

  // --- ReplayWindow: in-order, reorder, duplicates, too-old ---------------------------
  {
    bool ok = true;
    ReplayWindow w;
    ok = ok && w.accept(0) && w.accept(1) && w.accept(2);
    ok = ok && !w.accept(1) && !w.accept(2) && !w.accept(0);  // duplicates rejected
    ok = ok && w.accept(5) && w.accept(3) && w.accept(4);     // reorder inside window OK
    ok = ok && !w.accept(3);                                  // ...but not twice
    ok = ok && w.highest() == 5;
    // jump far ahead: window slides, the old range is forgotten
    ok = ok && w.accept(200);
    ok = ok && !w.accept(5);    // 195 behind: too old
    ok = ok && w.accept(199) && w.accept(137);  // within 64 behind: fine
    ok = ok && !w.accept(199);  // duplicate
    ok = ok && !w.accept(136);  // exactly 64 behind: outside
    ok = ok && !w.accept(137 + 63);  // == 200 again: duplicate of highest, rejected
    check(ok, "net: ReplayWindow accepts fresh/reordered, rejects duplicates and too-old counters");
  }

  // --- openPacket: replay window commits ONLY after successful auth (review F-L1) -----
  {
    // Fake ciphers: isPlaintext() false so the session leaves the v1 stub
    // path. OkCipher copies input to output; FailCipher always AuthFailed.
    struct OkCipher final : ICipher {
      CryptoResult seal(const uint8_t* p, size_t n, const uint8_t*, uint8_t* o, size_t cap) override {
        if (cap < n) return {CryptoError::BadLength, 0};
        std::memcpy(o, p, n);
        return {CryptoError::None, n};
      }
      CryptoResult open(const uint8_t* p, size_t n, const uint8_t*, uint8_t* o, size_t cap) override {
        if (cap < n) return {CryptoError::BadLength, 0};
        std::memcpy(o, p, n);
        return {CryptoError::None, n};
      }
      bool isPlaintext() const override { return false; }
      const char* name() const override { return "fake-ok"; }
    };
    struct FailCipher final : ICipher {
      CryptoResult seal(const uint8_t*, size_t, const uint8_t*, uint8_t*, size_t) override {
        return {CryptoError::AuthFailed, 0};
      }
      CryptoResult open(const uint8_t*, size_t, const uint8_t*, uint8_t*, size_t) override {
        return {CryptoError::AuthFailed, 0};
      }
      bool isPlaintext() const override { return false; }
      const char* name() const override { return "fake-fail"; }
    };
    bool ok = true;
    CryptoSession s(CryptoConfig{false, 0x77});
    ok = ok && s.armCipher(std::make_unique<FailCipher>()).error == CryptoError::None;
    const uint8_t pl[] = {1, 2, 3};
    uint8_t buf[64];
    // A forged packet claiming a far-future counter fails auth...
    ok = ok && s.openPacket(pl, sizeof pl, 1000000, buf, sizeof buf).error == CryptoError::AuthFailed;
    // ...and must NOT have shifted the replay window (the F-L1 poison).
    ok = ok && s.replayWindow().highest() == 0;
    // The legitimate low counters still open afterwards, and a successful
    // open commits the window (the same counter then replays-fails).
    ok = ok && s.armCipher(std::make_unique<OkCipher>()).error == CryptoError::None;
    ok = ok && s.openPacket(pl, sizeof pl, 5, buf, sizeof buf).error == CryptoError::None;
    ok = ok && s.replayWindow().highest() == 5;
    ok = ok && s.openPacket(pl, sizeof pl, 5, buf, sizeof buf).error == CryptoError::AuthFailed;
    ok = ok && s.openPacket(pl, sizeof pl, 1000000, buf, sizeof buf).error == CryptoError::None;
    check(ok, "net: openPacket commits the replay window only after successful auth (forged far-future counter cannot poison the session)");
  }

  // --- KeyExchange message: shape round-trips, malformed refused -----------------------
  {
    bool ok = true;
    KeyExchangeMsg m;
    m.initiator = true;
    m.algorithm = KexAlgorithm::X25519;
    m.ephemeralKey = std::vector<uint8_t>(32, 0x5A);  // X25519 pubkey size
    const auto enc = encodeKeyExchange(m);
    ok = ok && !enc.empty() && enc[0] == kHsKeyExchange;
    KeyExchangeMsg p;
    ok = ok && parseKeyExchange(enc.data(), enc.size(), p) && p.initiator &&
         p.algorithm == KexAlgorithm::X25519 && p.ephemeralKey == m.ephemeralKey;
    // malformed shapes are refused
    KeyExchangeMsg bad;
    ok = ok && !parseKeyExchange(nullptr, 0, bad);
    const uint8_t badId[] = {kHsClientHello, 1, 1, 0, 0};
    ok = ok && !parseKeyExchange(badId, sizeof badId, bad);
    const uint8_t badInit[] = {kHsKeyExchange, 2, 1, 0, 0};  // initiator not 0/1
    ok = ok && !parseKeyExchange(badInit, sizeof badInit, bad);
    const uint8_t badAlg[] = {kHsKeyExchange, 1, 9, 0, 0};  // unknown algorithm
    ok = ok && !parseKeyExchange(badAlg, sizeof badAlg, bad);
    const uint8_t trunc[] = {kHsKeyExchange, 1, 1, 4, 0, 0xAA};  // ephLen=4, 1 byte present
    ok = ok && !parseKeyExchange(trunc, sizeof trunc, bad);
    const uint8_t huge[] = {kHsKeyExchange, 1, 1, 0xFF, 0xFF};  // ephLen over the cap
    ok = ok && !parseKeyExchange(huge, sizeof huge, bad);
    // oversized ephemeral key is refused at encode time
    KeyExchangeMsg big = m;
    big.ephemeralKey.assign(kKexMaxKeyBytes + 1, 0);
    ok = ok && encodeKeyExchange(big).empty();
    KeyExchangeMsg noAlg = m;
    noAlg.algorithm = KexAlgorithm::Unknown;
    ok = ok && encodeKeyExchange(noAlg).empty();  // unspecified algorithm: no message
    check(ok, "net: KeyExchange message encodes/parses (size-first eph key); malformed refused");
  }
}

int runModule(const std::string& m) {
  const int before = g_fails;
  if (m == "core") { testCore(); testFailFast(); }
  else if (m == "world") testWorld();
  else if (m == "render") { testRender(); g_fails += testSoftwareRenderer(); g_fails += testVkSpriteConversion(); }
  else if (m == "sim") { testSim(); testChangedColumns(); testRandomTicks(); testStepTicks(); testHandshakeGroups(); testDetSched(); testTraceJitCompose(); testSignalPropagate(); testBlockNames(); testAttachables(); }
  else if (m == "ai") testAi();
  else if (m == "anim") g_fails += anim::runAnimSelfTest();
  else if (m == "storage") testStorage();
  else if (m == "procgen") { JobPool pool(2); g_fails += procgen::runProcgenSelfTest(pool); }
  else if (m == "trees") g_fails += procgen::runTreeGenSelfTest();
  else if (m == "shrubs") g_fails += procgen::runShrubsSelfTest();
  else if (m == "water") g_fails += procgen::runWaterSelfTest();
  else if (m == "litho_formula") {
    g_fails += procgen::litho_formula::runLithoFormulaSelfTest();
    g_fails += procgen::litho_formula::runLithoFormula2DSelfTest();
    g_fails += procgen::litho_formula::runLithoFormulaNodeSelfTest();
    g_fails += procgen::litho_formula::runLithoStampFeatureSelfTest();
  }
  else if (m == "ui") g_fails += ui::runUiSelfTest();
  else if (m == "assets") { g_fails += runAssetsSelfTest(); testMods(); }
  else if (m == "audio") g_fails += runAudioSelfTest();
  else if (m == "input") g_fails += runInputSelfTest();
  else if (m == "mods") { g_fails += mods::runModsSelfTest(); testModLoader(); }
  else if (m == "net") { testNet(); testNetChannels(); testNetConfinement(); testNetHandshake(); testNetEncryption(); }
  else if (m == "script") testScriptSandbox();
  else if (m == "console") g_fails += console::runConsoleSelfTest();
  else if (m == "crypto") g_fails += crypto::runCryptoSelfTest();
  else if (m == "index") g_fails += index::runIndexSelfTest();
  else if (m == "mcp") g_fails += mcp::runMcpSelfTest();
  else if (m == "defrag") g_fails += defrag::runDefragSelfTest();
  else if (m == "subdiv") g_fails += subdiv::runSubdivSelfTest();
  else if (m == "texture") g_fails += runTextureSelfTest();
  else { std::printf("unknown module '%s'\n", m.c_str()); return 1; }
  return g_fails - before;
}

}  // namespace

int main(int argc, char** argv) {
  // Unbuffered stdout: when a check crashes the process, CTest still shows
  // every line printed up to the crash, which pinpoints the failing check.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const char* all[] = {"core", "world", "render", "sim", "anim", "storage", "procgen", "trees", "shrubs", "litho_formula", "ui", "assets", "audio", "input", "net", "script", "mods", "console", "index", "mcp", "defrag", "subdiv", "texture"};
  int fails = 0;
  if (argc > 1) for (int i = 1; i < argc; ++i) fails += runModule(argv[i]);
  else for (const char* m : all) fails += runModule(m);
  std::printf(fails ? "ENGINE TESTS FAILED (%d)\n" : "ENGINE TESTS OK\n", fails);
  if (fails) return 1;
  // Nothing failed, but a section needed an OS capability this machine lacks.
  // Report that as a CTest skip rather than a pass (see core/test_env.h).
  if (const int skipped = test_env::skipCount()) {
    std::printf("ENGINE TESTS SKIPPED %d SECTION(S); see the [SKIP] lines above\n", skipped);
    return test_env::kSkipReturnCode;
  }
  return 0;
}
