#pragma once
#include "lithokraft/game/engine.h"
#include <filesystem>
#include <string>
#include <vector>
namespace lithokraft {
/// `packDirs`: the shipped pack stack (engine base, game pack, pack_64); empty if not found.
int runSelfTests(const std::string& scriptsDir, const std::vector<std::filesystem::path>& packDirs, bool timingHandshake = true);
}
