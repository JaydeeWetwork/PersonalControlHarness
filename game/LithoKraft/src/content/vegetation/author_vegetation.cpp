// Authoring tool: (re)generates the three vegetation family .lthm files under
// game/LithoKraft/assets/models/ from the author functions in
// vegetation_models.cpp.
//
// Usage: author_vegetation [outdir]
//   outdir defaults to the compiled-in LITHOKRAFT_MODELS_SRC_DIR
//   (the source tree's assets/models/).
//
// The pipeline is deterministic: running this twice produces byte-identical
// files (verified by vegetation_selftest against the checked-in assets).

#include <cstdio>
#include <filesystem>
#include <string>

#include "vegetation_models.h"

namespace fs = std::filesystem;
using lithokraft::model::Model;
using lithokraft::model::saveModel;
using lithokraft::vegetation::allVegetationModels;

int main(int argc, char** argv) {
  const fs::path out =
      (argc > 1) ? fs::path(argv[1]) : fs::path(LITHOKRAFT_MODELS_SRC_DIR);
  std::error_code ec;
  fs::create_directories(out, ec);
  if (ec) {
    std::printf("author_vegetation: cannot create %s: %s\n",
                out.string().c_str(), ec.message().c_str());
    return 1;
  }

  int failures = 0;
  for (const auto& vm : allVegetationModels()) {
    const Model m = vm.author();
    const fs::path p = out / (std::string(vm.fileStem) + ".lthm");
    std::string err;
    if (!saveModel(p.string(), m, &err)) {
      std::printf("FAIL %-20s: %s\n", vm.fileStem, err.c_str());
      ++failures;
    } else {
      std::printf("wrote %-42s %6zu leaves  maxLevel %d\n", p.string().c_str(),
                  m.geometry.leafCount(), m.maxLevel);
    }
  }
  std::printf(failures ? "author_vegetation: %d FAILURE(S)\n"
                       : "author_vegetation: all 3 models written\n",
              failures);
  return failures ? 1 : 0;
}
