// Self-test for the eight authored models (workstream 5). Verifies the
// pipeline end to end, per model:
//   1. save -> load round-trips bit-identical (modelEquals on the leaf stream)
//   2. leaf counts are within sane bounds (non-degenerate, sparse: nowhere
//      near a full 64^3 fill)
//   3. bounding checks: every leaf inside the 64^3 volume, maxLevel in range,
//      non-degenerate extents
//   4. determinism: re-author => modelEquals, and re-save => byte-identical
//      files, matching the checked-in assets/models/*.lthm byte for byte
//
// Exit code = number of failed checks. ctest entry: authored_models_selftest.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "models.h"

namespace fs = std::filesystem;
using lithokraft::model::LoadError;
using lithokraft::model::LoadResult;
using lithokraft::model::MicroCell;
using lithokraft::model::Model;
using lithokraft::model::authored::AuthoredModel;
using lithokraft::model::authored::allAuthoredModels;
namespace palette = lithokraft::model::authored::palette;
using lithokraft::model::loadModel;
using lithokraft::model::modelEquals;
using lithokraft::model::saveModel;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++g_failures;
}

const char* loadErrorName(LoadError e) {
  switch (e) {
    case LoadError::Ok:
      return "Ok";
    case LoadError::IoError:
      return "IoError";
    case LoadError::BadMagic:
      return "BadMagic";
    case LoadError::UnsupportedVersion:
      return "UnsupportedVersion";
    case LoadError::Truncated:
      return "Truncated";
    case LoadError::BadData:
      return "BadData";
    case LoadError::BadCrc:
      return "BadCrc";
  }
  return "?";
}

std::vector<uint8_t> readBytes(const fs::path& p) {
  std::vector<uint8_t> out;
  FILE* f = std::fopen(p.string().c_str(), "rb");
  if (!f) return out;
  uint8_t buf[8192];
  size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
    out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return out;
}

// Sanity bounds for leaf counts: every model must be non-degenerate but
// sparse (a full 64^3 fill is 262144 leaves; nothing here should exceed
// a quarter of the volume).
constexpr size_t kMinLeaves = 200;
constexpr size_t kMaxLeaves = 64u * 64u * 64u / 4;

}  // namespace

int main() {
  const fs::path modelsDir(LITHOKRAFT_MODELS_SRC_DIR);
  const fs::path tmp = fs::temp_directory_path() / "lthm_authored_selftest";
  std::error_code ec;
  fs::create_directories(tmp, ec);
  if (ec) {
    std::printf("FAIL: cannot create temp dir: %s\n", ec.message().c_str());
    return 1;
  }

  for (const AuthoredModel& am : allAuthoredModels()) {
    std::printf("model %-10s (%s):\n", am.fileStem, am.displayName);
    const Model m1 = am.author();
    const size_t leaves = m1.geometry.leafCount();

    char buf[256];
    std::snprintf(buf, sizeof buf, "leaf count %zu within [%zu, %zu]", leaves,
                  kMinLeaves, kMaxLeaves);
    check(leaves >= kMinLeaves && leaves <= kMaxLeaves, buf);

    std::snprintf(buf, sizeof buf, "maxLevel %d in [1, 6]", m1.maxLevel);
    check(m1.maxLevel >= 1 && m1.maxLevel <= 6, buf);

    // Bounding box: every leaf inside the volume, extents non-degenerate.
    int minX = 64, minY = 64, minZ = 64, maxX = -1, maxY = -1, maxZ = -1;
    bool inRange = true;
    for (const MicroCell& c : m1.geometry.leaves()) {
      const int span = 1 << c.level;
      if (c.x >= span || c.y >= span || c.z >= span) inRange = false;
      if (c.x < minX) minX = c.x;
      if (c.y < minY) minY = c.y;
      if (c.z < minZ) minZ = c.z;
      if (c.x > maxX) maxX = c.x;
      if (c.y > maxY) maxY = c.y;
      if (c.z > maxZ) maxZ = c.z;
    }
    check(inRange, "all leaves inside the 64^3 volume");
    std::snprintf(buf, sizeof buf, "bbox x[%d,%d] y[%d,%d] z[%d,%d] non-degenerate",
                  minX, maxX, minY, maxY, minZ, maxZ);
    check(maxX - minX >= 3 && maxY - minY >= 3 && maxZ - minZ >= 1, buf);

    // Round-trip through disk.
    const fs::path t1 = tmp / (std::string(am.fileStem) + ".a.lthm");
    const fs::path t2 = tmp / (std::string(am.fileStem) + ".b.lthm");
    std::string err;
    check(saveModel(t1.string(), m1, &err), "saveModel ok");
    const LoadResult lr = loadModel(t1.string());
    std::snprintf(buf, sizeof buf, "loadModel ok (err=%s)",
                  loadErrorName(lr.error));
    check(lr.error == LoadError::Ok, buf);
    if (lr.error == LoadError::Ok)
      check(modelEquals(m1, lr.model), "round-trip modelEquals");

    // Determinism: re-author => equal model, byte-identical file.
    const Model m2 = am.author();
    check(modelEquals(m1, m2), "re-author modelEquals");
    check(saveModel(t2.string(), m2, &err), "saveModel (2nd) ok");
    check(readBytes(t1.string()) == readBytes(t2.string()),
          "re-author byte-identical file");

    // The checked-in file must match the authoring code exactly.
    const fs::path ref =
        modelsDir / (std::string(am.fileStem) + ".lthm");
    const LoadResult rr = loadModel(ref.string());
    std::snprintf(buf, sizeof buf, "checked-in %s loads (err=%s)",
                  ref.string().c_str(), loadErrorName(rr.error));
    check(rr.error == LoadError::Ok, buf);
    if (rr.error == LoadError::Ok) {
      check(modelEquals(m1, rr.model), "checked-in modelEquals author");
      check(readBytes(ref.string()) == readBytes(t1.string()),
            "checked-in byte-identical to re-author");
    }

    // Palette sanity: every used material id is a known palette entry.
    bool paletteOk = true;
    for (const MicroCell& c : m1.geometry.leaves()) {
      if (std::strcmp(palette::name(c.material), "?") == 0) paletteOk = false;
    }
    check(paletteOk, "all materials are known palette ids");

    std::printf("  leaves=%zu maxLevel=%d\n", leaves, m1.maxLevel);
  }

  std::printf(g_failures ? "authored_models_selftest: %d FAILURE(S)\n"
                         : "authored_models_selftest: ALL GREEN\n",
              g_failures);
  return g_failures ? 1 : 0;
}
