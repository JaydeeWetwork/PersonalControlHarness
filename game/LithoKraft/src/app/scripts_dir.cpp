#include "scripts_dir.h"

#ifndef LITHOKRAFT_DEV_SCRIPTS_DIR
#define LITHOKRAFT_DEV_SCRIPTS_DIR ""  // shipping build: no source tree
#endif

namespace lithokraft {

namespace fs = std::filesystem;

ScriptsDirChoice resolveScriptsDir(std::string_view mode, const fs::path& sourceDir,
                                   const std::vector<fs::path>& exeCandidates) {
  std::error_code ec;
  const auto firstExe = [&]() -> std::string {
    for (const fs::path& p : exeCandidates)
      if (fs::is_directory(p, ec)) return p.lexically_normal().string();
    return {};
  };
  const bool haveSource = !sourceDir.empty() && fs::is_directory(sourceDir, ec);
  if (mode == "source") return {haveSource ? sourceDir.lexically_normal().string() : std::string(), "--scripts source"};
  if (mode == "exe") return {firstExe(), "--scripts exe"};
  if (!mode.empty()) {
    const fs::path p(mode);
    return {fs::is_directory(p, ec) ? p.lexically_normal().string() : std::string(), "--scripts <dir>"};
  }
  if (haveSource) return {sourceDir.lexically_normal().string(), "dev build: source tree"};
  return {firstExe(), sourceDir.empty() ? "shipping build: next to the exe" : "dev build, source tree missing: next to the exe"};
}

ScriptsDirChoice findScriptsDir(const char* argv0, std::string_view mode) {
  std::error_code ec;
  const fs::path exeDir = fs::absolute(fs::path(argv0), ec).parent_path();
  return resolveScriptsDir(mode, fs::path(LITHOKRAFT_DEV_SCRIPTS_DIR),
                           {exeDir / "scripts", exeDir / ".." / "scripts", exeDir / ".." / ".." / "scripts"});
}

}  // namespace lithokraft
