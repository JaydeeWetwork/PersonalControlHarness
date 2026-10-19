#include "litholattice/world/dimension.h"
#include "litholattice/core/log.h"

namespace litholattice {

Spawn Dimension::load(JobPool& pool, int sizeChunks) {
  unload();
  HandoffQueue<std::unique_ptr<ChunkColumn>> out;
  int failed = 0;
  if (!generator) {
    logError("dimension '%s': no terrain generator; loading empty", id.c_str());
  } else {
    const std::shared_ptr<const ColumnGenerator> gen = generator;
    JobBatch batch(pool);
    for (int cz = 0; cz < sizeChunks; ++cz)
      for (int cx = 0; cx < sizeChunks; ++cx)
        batch.submit([&out, gen, cx, cz] {
          if (auto col = gen->generate(cx, cz)) out.push(std::move(col));
        });
    failed = batch.wait();  // blocks on a condition variable; failed columns are logged and skipped
  }
  std::vector<std::unique_ptr<ChunkColumn>> cols;
  out.drainInto(cols);
  for (auto& c : cols) chunks.insert(std::move(c));
  if (failed) logError("dimension '%s': %d of %d columns failed to generate (left empty)", id.c_str(), failed, sizeChunks * sizeChunks);
  lastLoadFailures = failed;
  const Spawn sp = setup ? setup(chunks, sizeChunks) : Spawn{8.f, 100.f, 8.f, 0.785f, -0.3f};
  loaded = true;
  return sp;
}

}  // namespace litholattice
