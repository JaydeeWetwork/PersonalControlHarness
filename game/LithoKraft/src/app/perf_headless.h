#pragma once
#include <string>
namespace lithokraft {
/// Config for --bench-scene when used with --perf-headless.
struct BenchSceneConfig {
  std::string scene = "circuits";  ///< circuits|display|framebuffer|redirector|wall-pixel|pattern-wall|sawtooth|wire-torture
  int size = 64;                   ///< --scene-size
  int rows = 16;                   ///< --scene-rows
  std::string pattern = "circles"; ///< --pattern (checkers|circles)
  bool timingHandshake = true;     ///< bench mode A/B: default follows the standard (handshake ON since 2026-10-14); --no-timing-handshake selects legacy
  int handshakePhases = 4;         ///< --handshake-phases
};
/// --perf-headless [file.json]: thread scaling, save / load timing, stress circuits.
/// If scene.scene != "default", benchmarks that scene instead of the built-in world.
int runPerfHeadless(const std::string& jsonPath, const BenchSceneConfig& scene = {});
}  // namespace lithokraft
