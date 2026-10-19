// Which scripts/ folder the game loads and the F10 editor writes to.
//   dev build (CMake LITHOKRAFT_DEV_BUILD=ON, the default): the source tree's LithoKraft/scripts, so
//     in-game edits land in the repo and the next build's copy next to the exe can't overwrite them;
//   shipping build (-DLITHOKRAFT_DEV_BUILD=OFF, build.ps1 -Ship): the scripts/ copied next to the exe.
// `--scripts source|exe|<dir>` overrides either default at run time.
#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include "lithokraft/game/engine.h"

namespace lithokraft {

struct ScriptsDirChoice {
  std::string dir;     ///< empty: none found
  std::string reason;  ///< for the log: "dev build: source tree", "--scripts exe", ...
};

/// `mode`: "" (build default), "source", "exe" or a folder path. `sourceDir`: the source tree's
/// scripts/ (empty in shipping builds). `exeCandidates`: exe-local folders, best first.
ScriptsDirChoice resolveScriptsDir(std::string_view mode, const std::filesystem::path& sourceDir,
                                   const std::vector<std::filesystem::path>& exeCandidates);

/// resolveScriptsDir with this build's source dir and the folders around the running exe.
ScriptsDirChoice findScriptsDir(const char* argv0, std::string_view mode);

}  // namespace lithokraft
