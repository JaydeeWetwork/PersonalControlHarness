// Authoring tool: (re)generates the eight .lthm model files under
// game/LithoKraft/assets/models/ from the author functions in models.cpp.
//
// Usage: author_models [outdir]
//   outdir defaults to the compiled-in LITHOKRAFT_MODELS_SRC_DIR
//   (the source tree's assets/models/).
//
// The pipeline is deterministic: running this twice produces byte-identical
// files (verified by authored_models_selftest).

#include <cstdio>
#include <filesystem>
#include <string>

#include "models.h"

namespace fs = std::filesystem;
using lithokraft::model::authored::allAuthoredModels;
using lithokraft::model::Model;
using lithokraft::model::saveModel;

int main(int argc, char** argv) {
  const fs::path out =
      (argc > 1) ? fs::path(argv[1]) : fs::path(LITHOKRAFT_MODELS_SRC_DIR);
  std::error_code ec;
  fs::create_directories(out, ec);
  if (ec) {
    std::printf("author_models: cannot create %s: %s\n", out.string().c_str(),
                ec.message().c_str());
    return 1;
  }

  int failures = 0;
  for (const auto& am : allAuthoredModels()) {
    const Model m = am.author();
    const fs::path p = out / (std::string(am.fileStem) + ".lthm");
    std::string err;
    if (!saveModel(p.string(), m, &err)) {
      std::printf("FAIL %-10s: %s\n", am.fileStem, err.c_str());
      ++failures;
    } else {
      std::printf("wrote %-42s %6zu leaves  maxLevel %d\n", p.string().c_str(),
                  m.geometry.leafCount(), m.maxLevel);
    }
  }
  std::printf(failures ? "author_models: %d FAILURE(S)\n"
                       : "author_models: all models written\n",
              failures);
  return failures ? 1 : 0;
}
