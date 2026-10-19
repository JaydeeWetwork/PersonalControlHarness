#include "lithokraft/game/settings.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "litholattice/core/json.h"

namespace lithokraft {

int nearestTps(int v) {
  int best = kSettingsTpsModes[0];
  for (int i = 1; i < kSettingsTpsModeCount; ++i)
    if (std::abs(v - kSettingsTpsModes[i]) < std::abs(v - best)) best = kSettingsTpsModes[i];
  return best;
}

namespace {

int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

}  // namespace

GameSettings GameSettings::load(const std::string& path) {
  GameSettings s;
  if (path.empty()) return s;
  std::ifstream in(path, std::ios::binary);
  if (!in) return s;  // first run: no file yet
  std::stringstream ss;
  ss << in.rdbuf();
  litholattice::Json json;
  std::string err;
  if (!litholattice::Json::parse(ss.str(), json, &err)) return s;  // corrupt: defaults
  s.masterVolume = std::clamp(float(json.getNum("masterVolume", s.masterVolume)), 0.f, 1.f);
  s.muted = json.getBool("muted", s.muted);
  s.hudScale = clampInt(int(json.getNum("hudScale", s.hudScale)), 0, 2);
  s.displayMode = clampInt(int(json.getNum("displayMode", s.displayMode)), 0, kSettingsDisplayModeCount - 1);
  s.vsync = json.getBool("vsync", s.vsync);
  s.tps = nearestTps(int(json.getNum("tps", s.tps)));
  s.tickRadius = clampInt(int(json.getNum("tickRadius", s.tickRadius)), 0, 512);
  s.euv = json.getBool("euv", s.euv);
  s.mesher = clampInt(int(json.getNum("mesher", s.mesher)), 0, kSettingsMesherCount - 1);
  s.pbrTier = clampInt(int(json.getNum("pbrTier", s.pbrTier)), 0, 3);
  s.ambientMode = clampInt(int(json.getNum("ambientMode", s.ambientMode)), 0, 1);
  s.fpsCap = clampInt(int(json.getNum("fpsCap", s.fpsCap)), 0, 1000);
  s.timingHandshake = json.getBool("timingHandshake", s.timingHandshake);
  s.handshakePhases = clampInt(int(json.getNum("handshakePhases", s.handshakePhases)), 1, 16);
  return s;
}

bool GameSettings::save(const std::string& path) const {
  if (path.empty()) return false;
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  litholattice::Json json = litholattice::Json::object();
  json.add("masterVolume", litholattice::Json::number(masterVolume));
  json.add("muted", litholattice::Json::boolean(muted));
  json.add("hudScale", litholattice::Json::number(hudScale));
  json.add("displayMode", litholattice::Json::number(displayMode));
  json.add("vsync", litholattice::Json::boolean(vsync));
  json.add("tps", litholattice::Json::number(tps));
  json.add("tickRadius", litholattice::Json::number(tickRadius));
  json.add("euv", litholattice::Json::boolean(euv));
  json.add("mesher", litholattice::Json::number(mesher));
  json.add("pbrTier", litholattice::Json::number(pbrTier));
  json.add("ambientMode", litholattice::Json::number(ambientMode));
  json.add("fpsCap", litholattice::Json::number(fpsCap));
  json.add("timingHandshake", litholattice::Json::boolean(timingHandshake));
  json.add("handshakePhases", litholattice::Json::number(handshakePhases));
  out << json.dump(2) << "\n";
  return bool(out);
}

}  // namespace lithokraft
