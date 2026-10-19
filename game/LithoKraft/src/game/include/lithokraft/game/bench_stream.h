// --bench-path stream: a contained streaming world for the bench. The camera flies +x out of the
// fixed loaded world; this streamer generates the columns around it on the job pool (procgen in
// real time), meshes them, uploads them and drops columns that fall behind. It owns its own
// ChunkMap on the main thread (columns outside the fixed world), so the sim thread's map is never
// touched. It also keeps the per-second and whole-run stats for the bench.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "lithokraft/game/engine.h"
#include "litholattice/core/threading.h"
#include "litholattice/render/block_materials.h"
#include "litholattice/render/mesher.h"
#include "litholattice/render/renderer.h"
#include "litholattice/world/chunk_map.h"
#include "litholattice/world/column_generator.h"

namespace lithokraft {

/// Timings of one kind of job (ms), per second and for the run.
struct MsSamples {
  std::vector<double> second, run;
  void add(double ms) { second.push_back(ms); run.push_back(ms); }
  static double avg(const std::vector<double>& v);
  static double pct(std::vector<double> v, double q);
};

class BenchStreamer {
 public:
  BenchStreamer(IRenderer& renderer, JobPool& pool, std::shared_ptr<const ColumnGenerator> gen, int worldChunks, BlockId belowWorld,
                int radiusChunks);
  ~BenchStreamer();  ///< waits for its jobs, removes its meshes
  /// Main thread, once per frame: request / drop columns around (x, z), take finished work,
  /// upload meshes. Returns bytes uploaded this frame.
  size_t update(float x, float z, const std::shared_ptr<const BlockMaterials>& materials);
  // Per-second counters (reset by takeSecond()) and run totals.
  struct Second { int generated = 0, meshed = 0, dropped = 0, loaded = 0; };
  Second takeSecond();
  /// Bookkeeping sizes (leak checks): requested keys, columns with meshes, columns loaded.
  size_t requestedCount() const { return requested_.size(); }
  size_t meshedColumnCount() const { return meshedSubs_.size(); }
  size_t loadedCount() const { return map_.columns().size(); }
  MsSamples genMs, meshMs;
  uint64_t totalGenerated = 0, totalMeshed = 0;

 private:
  struct GenOut { std::unique_ptr<ChunkColumn> col; double ms; };
  struct MeshOut { ChunkMesh mesh; double ms; };
  bool inFixedWorld(int cx, int cz) const { return cx >= 0 && cz >= 0 && cx < world_ && cz < world_; }
  IRenderer& renderer_;
  JobPool& pool_;
  std::shared_ptr<const ColumnGenerator> gen_;
  int world_, radius_;
  BlockId below_;
  ChunkMap map_;
  std::unordered_set<uint64_t> requested_;                      // generating or loaded
  std::unordered_map<uint64_t, std::vector<int>> meshedSubs_;   // column -> sub-chunks with a mesh
  std::shared_ptr<HandoffQueue<GenOut>> genDone_ = std::make_shared<HandoffQueue<GenOut>>();
  std::shared_ptr<HandoffQueue<MeshOut>> meshDone_ = std::make_shared<HandoffQueue<MeshOut>>();
  std::shared_ptr<std::atomic<int>> inFlight_ = std::make_shared<std::atomic<int>>(0);
  Second sec_;
};

}  // namespace lithokraft
