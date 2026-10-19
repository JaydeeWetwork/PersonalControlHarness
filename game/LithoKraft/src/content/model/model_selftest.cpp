// model_selftest: self-tests for the .lthm model format + save/place pipeline.
// Exit code 0 = all pass. Each check prints "ok"/"not ok" (TAP-ish).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <litholattice/subdiv/subdiv_store.h>

#include "model.h"
#include <set>
#include <tuple>
#include "litho_stamp_models.h"

namespace {

namespace fs = std::filesystem;
namespace mdl = lithokraft::model;
namespace subdiv = litholattice::subdiv;

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* name) {
  ++g_checks;
  if (cond) {
    std::printf("ok - %s\n", name);
  } else {
    ++g_failures;
    std::printf("not ok - %s\n", name);
  }
}

std::string tmpPath(const char* name) {
  return (fs::temp_directory_path() / name).string();
}

std::vector<uint8_t> readBytes(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                              std::istreambuf_iterator<char>());
}

void writeBytes(const std::string& path, const std::vector<uint8_t>& data) {
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(data.data()),
          static_cast<std::streamsize>(data.size()));
}

// The real workstream-1 SubdivStore stands in for itself here.
using Store = subdiv::SubdivStore;

mdl::Model makeTestModel() {
  mdl::ModelBuilder b("Test Cube", "Elfie");
  b.fillBox(1, 0, 0, 0, 1, 1, 1, 3);  // full block at level 1, material 3
  b.setCell(3, 5, 5, 5, 7);           // one detailed cell at level 3
  b.setCell(2, 0, 0, 0, 9);           // overwrite one level-1 octant's corner
  return b.build();
}

void testRoundTrip() {
  const std::string path = tmpPath("lthm_roundtrip.lthm");
  mdl::Model m = makeTestModel();
  std::string err;
  check(mdl::saveModel(path, m, &err), "save succeeds");
  mdl::LoadResult r = mdl::loadModel(path);
  check(r.error == mdl::LoadError::Ok, "load succeeds");
  check(mdl::modelEquals(m, r.model), "round-trip modelEquals");
  check(r.model.name == "Test Cube", "name preserved");
  check(r.model.author == "Elfie", "author preserved");
  check(r.model.maxLevel == 3, "maxLevel preserved");
  check(r.model.geometry.leafCount() == m.geometry.leafCount(),
        "leaf count preserved");
}

void testDeterminism() {
  const std::string p1 = tmpPath("lthm_det1.lthm");
  const std::string p2 = tmpPath("lthm_det2.lthm");
  check(mdl::saveModel(p1, makeTestModel()), "determinism save 1");
  check(mdl::saveModel(p2, makeTestModel()), "determinism save 2");
  check(readBytes(p1) == readBytes(p2), "same builder calls => byte-identical");
}

void testEmptyModel() {
  const std::string path = tmpPath("lthm_empty.lthm");
  mdl::ModelBuilder b("Empty");
  mdl::Model m = b.build();
  check(m.geometry.empty(), "empty builder => empty geometry");
  check(m.maxLevel == 0, "empty model maxLevel 0");
  check(mdl::saveModel(path, m), "save empty model");
  mdl::LoadResult r = mdl::loadModel(path);
  check(r.error == mdl::LoadError::Ok && mdl::modelEquals(m, r.model),
        "empty model round-trips");
}

void testCrcCorruption() {
  const std::string path = tmpPath("lthm_crc.lthm");
  check(mdl::saveModel(path, makeTestModel()), "crc fixture save");
  // Corrupt one payload byte (last byte before the CRC field).
  std::vector<uint8_t> data = readBytes(path);
  check(data.size() > 16, "crc fixture big enough");
  data[data.size() - 5] ^= 0xFF;
  writeBytes(path, data);
  mdl::LoadResult r = mdl::loadModel(path);
  check(r.error == mdl::LoadError::BadCrc, "payload corruption => BadCrc");
  check(!r.message.empty(), "BadCrc carries a message");

  // Corrupt the CRC field itself.
  check(mdl::saveModel(path, makeTestModel()), "crc fixture re-save");
  data = readBytes(path);
  data[data.size() - 1] ^= 0x01;
  writeBytes(path, data);
  r = mdl::loadModel(path);
  check(r.error == mdl::LoadError::BadCrc, "crc field corruption => BadCrc");
}

void testBadMagic() {
  const std::string path = tmpPath("lthm_magic.lthm");
  check(mdl::saveModel(path, makeTestModel()), "magic fixture save");
  std::vector<uint8_t> data = readBytes(path);
  data[0] = 'X';
  writeBytes(path, data);
  mdl::LoadResult r = mdl::loadModel(path);
  check(r.error == mdl::LoadError::BadMagic, "bad magic rejected");
}

void testVersionRejection() {
  const std::string path = tmpPath("lthm_version.lthm");
  check(mdl::saveModel(path, makeTestModel()), "version fixture save");
  for (uint16_t v : {uint16_t(2), uint16_t(99)}) {
    std::vector<uint8_t> data = readBytes(path);
    // Bump the version field, then re-stamp a valid CRC so the failure is
    // purely about the version.
    data[4] = static_cast<uint8_t>(v & 0xFF);
    data[5] = static_cast<uint8_t>((v >> 8) & 0xFF);
    // Recompute CRC32 over the payload (duplicated here to keep the test
    // independent of model.cpp internals).
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i + 4 < data.size(); ++i) {
      // (simple bit-by-bit CRC to avoid a table in the test)
      c ^= data[i];
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    c ^= 0xFFFFFFFFu;
    const size_t n = data.size();
    data[n - 4] = static_cast<uint8_t>(c & 0xFF);
    data[n - 3] = static_cast<uint8_t>((c >> 8) & 0xFF);
    data[n - 2] = static_cast<uint8_t>((c >> 16) & 0xFF);
    data[n - 1] = static_cast<uint8_t>((c >> 24) & 0xFF);
    writeBytes(path, data);
    mdl::LoadResult r = mdl::loadModel(path);
    char name[64];
    std::snprintf(name, sizeof name, "version %u => UnsupportedVersion", v);
    check(r.error == mdl::LoadError::UnsupportedVersion, name);
    check(r.message.find("unsupported") != std::string::npos,
          "version error message is clear");
  }
}

void testTruncated() {
  const std::string path = tmpPath("lthm_trunc.lthm");
  check(mdl::saveModel(path, makeTestModel()), "trunc fixture save");
  std::vector<uint8_t> data = readBytes(path);
  data.resize(data.size() / 2);
  writeBytes(path, data);
  mdl::LoadResult r = mdl::loadModel(path);
  check(r.error == mdl::LoadError::Truncated, "half file => Truncated");

  writeBytes(path, std::vector<uint8_t>{'L', 'T'});
  r = mdl::loadModel(path);
  check(r.error == mdl::LoadError::Truncated, "2-byte file => Truncated");
}

void testMissingFile() {
  mdl::LoadResult r = mdl::loadModel(tmpPath("lthm_no_such_file_xyz.lthm"));
  check(r.error == mdl::LoadError::IoError, "missing file => IoError");
}

void testBuilderSemantics() {
  mdl::ModelBuilder b("Semantics");
  b.fillBox(1, 0, 0, 0, 1, 1, 1, 1);
  b.fillBox(1, 0, 0, 0, 0, 0, 0, 2);  // overwrite one octant
  // Reversed bounds must behave like normalized bounds.
  b.fillBox(2, 3, 3, 3, 2, 2, 2, 4);
  // Out-of-range input is ignored, never crashes.
  b.setCell(9, 0, 0, 0, 5);
  b.setCell(1, 5, 0, 0, 5);
  b.fillBox(9, 0, 0, 0, 1, 1, 1, 5);
  mdl::Model m = b.build();
  check(m.geometry.get(1, 0, 0, 0) == 2, "fillBox overwrite wins");
  check(m.geometry.get(1, 1, 0, 0) == 1, "untouched octant keeps material");
  check(m.geometry.get(2, 2, 2, 2) == 4, "reversed bounds fill works");
  check(m.geometry.get(2, 3, 3, 3) == 4, "reversed bounds fill covers box");
  check(m.maxLevel == 2, "maxLevel tracks finest level");

  // clearCell removes the cell.
  mdl::ModelBuilder c("Clear");
  c.setCell(2, 1, 1, 1, 6);
  c.clearCell(2, 1, 1, 1);
  mdl::Model cm = c.build();
  check(cm.geometry.empty(), "clearCell empties the cell");
}

void testPlaceModelMerge() {
  Store store;
  // Pre-existing detail at the target block: one cell the model will
  // overwrite, one the model does not touch (outside every model leaf),
  // one finer cell UNDER a model leaf (correctly cleared by the stamp),
  // plus a cell at a neighbor block.
  {
    subdiv::SparseOctree t;
    t.set(2, 0, 0, 0, 11);
    t.set(3, 0, 0, 7, 12);
    t.set(3, 7, 7, 7, 14);
    store.setBlockOctree(4, 5, 6, t);
    subdiv::SparseOctree n;
    n.set(1, 0, 0, 0, 13);
    store.setBlockOctree(7, 5, 6, n);
  }

  mdl::ModelBuilder b("Stamp");
  b.setCell(2, 0, 0, 0, 21);  // overwrites the pre-existing 11
  b.setCell(1, 1, 1, 1, 22);  // new cell
  mdl::Model m = b.build();

  mdl::placeModel(store, 4, 5, 6, m);

  const subdiv::SparseOctree* t = store.getBlockOctree(4, 5, 6);
  check(t != nullptr, "target block has an entry after place");
  check(t->get(2, 0, 0, 0) == 21, "model cell overwrites target cell");
  check(t->get(1, 1, 1, 1) == 22, "new model cell stamped");
  check(t->get(3, 0, 0, 7) == 12, "untouched target cell preserved");
  check(t->get(3, 7, 7, 7) == 22,
        "finer target cells under a stamped cell are cleared");
  const subdiv::SparseOctree* n = store.getBlockOctree(7, 5, 6);
  check(n != nullptr && n->get(1, 0, 0, 0) == 13, "neighbor block untouched");

  // Empty model is a no-op.
  mdl::ModelBuilder e("Empty");
  mdl::Model em = e.build();
  const size_t before = t->leafCount();
  mdl::placeModel(store, 4, 5, 6, em);
  check(store.getBlockOctree(4, 5, 6)->leafCount() == before,
        "empty model changes nothing");

  // Placing into a fresh block creates its octree.
  mdl::placeModel(store, 0, 0, 0, m);
  const subdiv::SparseOctree* fresh = store.getBlockOctree(0, 0, 0);
  check(fresh != nullptr && fresh->get(2, 0, 0, 0) == 21,
        "place creates missing block octree");

  // Out-of-range block coords are a no-op.
  const size_t blocksBefore = store.blockCount();
  mdl::placeModel(store, 16, 0, 0, m);
  check(store.blockCount() == blocksBefore, "out-of-range coords ignored");
}

void testCopyIsolation() {
  // Value semantics: mutating a copy must not affect the original.
  mdl::ModelBuilder b("Copy");
  b.setCell(2, 1, 1, 1, 5);
  mdl::Model a = b.build();
  mdl::Model c = a;  // copy
  c.geometry.set(2, 1, 1, 1, 6);
  check(a.geometry.get(2, 1, 1, 1) == 5, "copy is isolated from original");
  check(c.geometry.get(2, 1, 1, 1) == 6, "copy mutation took effect");
  mdl::Model d;
  d = a;  // copy-assign
  d.geometry.set(2, 1, 1, 1, 7);
  check(a.geometry.get(2, 1, 1, 1) == 5, "assign is isolated from original");
}

void testModelEquals() {  mdl::Model a = makeTestModel();
  mdl::Model b = makeTestModel();
  check(mdl::modelEquals(a, b), "identical models equal");
  b.name = "Different";
  check(!mdl::modelEquals(a, b), "name difference detected");
  b = makeTestModel();
  b.maxLevel = 99;
  check(!mdl::modelEquals(a, b), "maxLevel difference detected");
  b = makeTestModel();
  mdl::ModelBuilder extra("x");
  extra.setCell(0, 0, 0, 0, 1);
  b.geometry = extra.build().geometry;
  check(!mdl::modelEquals(a, b), "geometry difference detected");
}

void testUtf8Names() {
  const std::string path = tmpPath("lthm_utf8.lthm");
  mdl::ModelBuilder b("T\xc3\xabr\xc3\xa4ger-Modell", "J\xc3\xb6rg");
  b.setCell(0, 0, 0, 0, 1);
  mdl::Model m = b.build();
  check(mdl::saveModel(path, m), "utf8 save");
  mdl::LoadResult r = mdl::loadModel(path);
  check(r.error == mdl::LoadError::Ok && mdl::modelEquals(m, r.model),
        "utf8 names round-trip");
}

// testLithoStampModels: the ModelResolver adapter over the baked gallery.
void testLithoStampModels() {
#ifdef LITHOKRAFT_MODELS_DIR
  const std::string dir = LITHOKRAFT_MODELS_DIR;
  // 1. A real gallery preset loads: 32-cell grid, non-empty.
  auto star = mdl::loadLithoStampModel("starfish");
  check(star.has_value(), "stamp adapter: starfish loads");
  if (!star) return;
  check(star->size == 32, "stamp adapter: starfish grid is 32");
  check(!star->cells.empty(), "stamp adapter: starfish has cells");
  // 2. Stamping matches the baked bytes: expand the .lthm leaves independently and
  // compare the full cell set.
  mdl::LoadResult raw = mdl::loadModel(dir + "/starfish.lthm");
  check(raw.error == mdl::LoadError::Ok, "stamp adapter: raw starfish.lthm loads");
  if (raw.error != mdl::LoadError::Ok) return;
  std::set<std::tuple<int, int, int, uint8_t>> want;
  int maxLevel = 0;
  const auto leaves = raw.model.geometry.leaves();
  for (const auto& c : leaves) maxLevel = std::max(maxLevel, int(c.level));
  for (const auto& c : leaves) {
    if (c.material == 0) continue;
    const int s = 1 << (maxLevel - int(c.level));
    for (int y = int(c.y) * s; y < int(c.y + 1) * s; ++y)
      for (int z = int(c.z) * s; z < int(c.z + 1) * s; ++z)
        for (int x = int(c.x) * s; x < int(c.x + 1) * s; ++x)
          want.emplace(x, y, z, c.material);
  }
  std::set<std::tuple<int, int, int, uint8_t>> got;
  for (const auto& c : star->cells) got.emplace(c.x, c.y, c.z, c.palette);
  check(got == want, "stamp adapter: stamped cells match the baked .lthm bytes");
  // 3. Unknown and insane names resolve to nullopt.
  check(!mdl::loadLithoStampModel("no_such_preset").has_value(),
        "stamp adapter: unknown preset -> nullopt");
  check(!mdl::loadLithoStampModel("../evil").has_value(), "stamp adapter: path traversal -> nullopt");
  check(!mdl::loadLithoStampModel("").has_value(), "stamp adapter: empty name -> nullopt");
  // 4. The resolver wrapper agrees with the direct loader.
  auto resolver = mdl::makeLithoStampModelResolver();
  auto via = resolver("starfish");
  check(via.has_value() && via->cells.size() == star->cells.size(),
        "stamp adapter: resolver matches direct load");
#else
  check(!mdl::loadLithoStampModel("starfish").has_value(),
        "stamp adapter: no models dir -> nullopt");
#endif
}

}  // namespace

int main() {
  testRoundTrip();
  testDeterminism();
  testEmptyModel();
  testCrcCorruption();
  testBadMagic();
  testVersionRejection();
  testTruncated();
  testMissingFile();
  testBuilderSemantics();
  testPlaceModelMerge();
  testCopyIsolation();
  testModelEquals();
  testUtf8Names();
  testLithoStampModels();
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
