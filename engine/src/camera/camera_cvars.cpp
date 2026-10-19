// camera.* cvar registration + read-back. The engine owns no registry; the
// game calls registerCameraCvars() during its cvar setup and syncs values
// into CameraConfigs with readCameraCvars()/applyCameraCvars() — the same
// register-in-game / read-per-frame pattern as mcp.* and render.* knobs.
#include "litholattice/camera/camera.h"

#include <cstdlib>

#include "litholattice/console/concmd.h"

namespace litholattice::camera {
namespace {

float cvarFloat(const console::ConsoleRegistry& reg, const char* name, float fallback) {
  const console::CVar* v = reg.findCVar(name);
  if (!v) return fallback;
  const float f = std::strtof(v->value.c_str(), nullptr);
  return f == 0.f && v->value != "0" && v->value != "0.0" ? fallback : f;
}
int cvarInt(const console::ConsoleRegistry& reg, const char* name, int fallback) {
  const console::CVar* v = reg.findCVar(name);
  if (!v) return fallback;
  return static_cast<int>(std::strtol(v->value.c_str(), nullptr, 10));
}
bool cvarBool(const console::ConsoleRegistry& reg, const char* name, bool fallback) {
  const console::CVar* v = reg.findCVar(name);
  if (!v) return fallback;
  return v->value == "1" || v->value == "true";
}

}  // namespace

bool registerCameraCvars(console::ConsoleRegistry& reg, std::string* err) {
  bool ok = true;
  std::string e;
  auto add = [&](const char* name, console::CVarType type, const char* def, const char* help) {
    if (!reg.registerCVar(name, type, def, /*archived=*/true, help, &e)) {
      ok = false;
      if (err && err->empty()) *err = e;
    }
  };
  add("camera.chunk_radius", console::CVarType::Int, "4",
      "Chunks a camera keeps loaded around itself (limited radius; JD 2026-10-11)");
  add("camera.default_fov", console::CVarType::Float, "70", "Default camera field of view, degrees [10..120]");
  add("camera.third_person_distance", console::CVarType::Float, "6", "Default third-person boom length, blocks");
  add("camera.shake_scale", console::CVarType::Float, "1", "Master camera shake multiplier (0 = no shake)");
  add("camera.headbob", console::CVarType::Bool, "0", "First-person head-bob (off by default)");
  add("camera.fov_kick", console::CVarType::Bool, "1", "Speed-based FOV kick on/off");
  return ok;
}

CameraCvarValues readCameraCvars(const console::ConsoleRegistry& reg) {
  CameraCvarValues v;
  v.chunkRadiusChunks = cvarInt(reg, "camera.chunk_radius", v.chunkRadiusChunks);
  v.defaultFovDeg = cvarFloat(reg, "camera.default_fov", v.defaultFovDeg);
  v.thirdPersonDistance = cvarFloat(reg, "camera.third_person_distance", v.thirdPersonDistance);
  v.shakeScale = cvarFloat(reg, "camera.shake_scale", v.shakeScale);
  v.headBob = cvarBool(reg, "camera.headbob", v.headBob);
  v.fovKick = cvarBool(reg, "camera.fov_kick", v.fovKick);
  return v;
}

void applyCameraCvars(const CameraCvarValues& v, CameraConfig& cfg) {
  cfg.chunkRadiusChunks = v.chunkRadiusChunks < 0 ? 0 : v.chunkRadiusChunks;
  cfg.fovDeg = v.defaultFovDeg;
  cfg.orbitDistance = v.thirdPersonDistance;
  cfg.shakeScale = v.shakeScale;
  cfg.headBob = v.headBob;
  cfg.fovKick = v.fovKick;
}

}  // namespace litholattice::camera
