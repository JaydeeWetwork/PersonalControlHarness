#include "lithokraft/game/bench_stream.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include "lithokraft/content/appearance.h"
#include "lithokraft/content/blocks.h"
#include "litholattice/core/time.h"
#include "litholattice/world/mesh_input.h"

namespace lithokraft {

double MsSamples::avg(const std::vector<double>& v) {
  double s = 0; for (double x : v) s += x;
  return v.empty() ? 0.0 : s / double(v.size());
}
double MsSamples::pct(std::vector<double> v, double q) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
}

BenchStreamer::BenchStreamer(IRenderer& renderer, JobPool& pool, std::shared_ptr<const ColumnGenerator> gen, int worldChunks,
                             BlockId belowWorld, int radiusChunks)
    : renderer_(renderer), pool_(pool), gen_(std::move(gen)), world_(worldChunks), radius_(radiusChunks), below_(belowWorld) {}

BenchStreamer::~BenchStreamer() {
  while (inFlight_->load() > 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  for (const auto& [key, subs] : meshedSubs_)
    for (int sy : subs) {
      ChunkMesh empty; empty.cx = int(int32_t(key >> 32)); empty.cz = int(int32_t(key & 0xffffffffu)); empty.sy = sy;
      renderer_.uploadChunkMesh(empty);
    }
}

BenchStreamer::Second BenchStreamer::takeSecond() {
  Second s = sec_;
  s.loaded = int(map_.columns().size());
  sec_ = {};
  genMs.second.clear(); meshMs.second.clear();
  return s;
}

size_t BenchStreamer::update(float x, float z, const std::shared_ptr<const BlockMaterials>& materials) {
  const int pcx = int(std::floor(x / 16.f)), pcz = int(std::floor(z / 16.f));
  // 1) request missing columns in the radius (nearest first is not needed: the camera moves +x).
  constexpr int kMaxInFlight = 64;
  for (int dz = -radius_; dz <= radius_; ++dz)
    for (int dx = -radius_; dx <= radius_; ++dx) {
      if (inFlight_->load() >= kMaxInFlight) break;
      const int cx = pcx + dx, cz = pcz + dz;
      if (inFixedWorld(cx, cz)) continue;
      const uint64_t key = ChunkKey::make(cx, cz);
      if (!requested_.insert(key).second) continue;
      ++*inFlight_;
      pool_.submit([gen = gen_, out = genDone_, n = inFlight_, cx, cz] {
        const auto t0 = Clock::now();
        auto col = gen->generate(cx, cz);
        out->push({std::move(col), msBetween(t0, Clock::now())});
        --*n;
      });
    }
  // 2) generated columns -> map + mesh jobs
  std::vector<GenOut> gens;
  genDone_->drainInto(gens);
  for (GenOut& g : gens) {
    if (!g.col) continue;
    genMs.add(g.ms); ++sec_.generated; ++totalGenerated;
    const int cx = g.col->cx, cz = g.col->cz;
    map_.insert(std::move(g.col));
    const ChunkColumn* col = map_.column(cx, cz);
    for (int sy = 0; sy < kSubChunks; ++sy) {
      if (!col->subs[size_t(sy)]) continue;
      auto in = snapshotForMeshing(map_, blockRegistry(), cx, sy, cz, 0, 0, below_);
      ++*inFlight_;
      pool_.submit([in, materials, out = meshDone_, n = inFlight_] {
        const auto t0 = Clock::now();
        ChunkMesh m = meshSubChunk(*in, MeshContext{blockRegistry(), *materials, &blockAppearance()});
        out->push({std::move(m), msBetween(t0, Clock::now())});
        --*n;
      });
    }
  }
  // 3) upload finished meshes (columns dropped meanwhile are skipped)
  size_t bytes = 0;
  std::vector<MeshOut> meshes;
  meshDone_->drainInto(meshes);
  for (MeshOut& m : meshes) {
    meshMs.add(m.ms); ++sec_.meshed; ++totalMeshed;
    const uint64_t key = ChunkKey::make(m.mesh.cx, m.mesh.cz);
    if (!map_.column(m.mesh.cx, m.mesh.cz)) continue;
    bytes += m.mesh.verts.size() * sizeof(PackedVertex);
    meshedSubs_[key].push_back(m.mesh.sy);
    renderer_.uploadChunkMesh(m.mesh);
  }
  // 4) drop columns that fell behind (outside radius + 2)
  std::vector<uint64_t> drop;
  for (const auto& [key, col] : map_.columns())
    if (std::abs(col->cx - pcx) > radius_ + 2 || std::abs(col->cz - pcz) > radius_ + 2) drop.push_back(key);
  for (uint64_t key : drop) {
    const ChunkColumn* col = map_.columns().at(key).get();
    ChunkMesh empty; empty.cx = col->cx; empty.cz = col->cz;
    for (int sy : meshedSubs_[key]) { empty.sy = sy; renderer_.uploadChunkMesh(empty); }
    meshedSubs_.erase(key);
    requested_.erase(key);
    map_.columns().erase(key);
    ++sec_.dropped;
  }
  return bytes;
}

}  // namespace lithokraft
