// Per-mod save-data integrity: atomic writes, CRC32-framed sections, verified
// loads with quarantine. See mod_save_data.h for the design and threat model.
#include "litholattice/mods/mod_save_data.h"

#include <cctype>

#include "litholattice/core/log.h"
#include "litholattice/mods/extension_registry.h"  // isValidId (path safety)
#include "litholattice/mods/mod_interface.h"       // LITHOLATTICE_SDK_VERSION
#include "litholattice/storage/storage.h"         // ByteWriter/ByteReader (LE)

namespace litholattice::mods {
namespace {

using storage::ByteReader;
using storage::ByteWriter;

// Inner payload layout (ByteWriter, little-endian):
//   u16  kModSaveFormatVersion
//   bytes modId
//   bytes modVersion (semver string)
//   u16  dataVersion (the mod's own schema version)
//   u32  sdkMajor at write time (diagnostic)
//   bytes modData
void encodeSection(const std::string& modId, const std::string& modVersion, uint16_t dataVersion,
                   const std::vector<uint8_t>& data, ByteWriter& w) {
  w.u16(kModSaveFormatVersion);
  w.bytes(storage::Bytes(modId.begin(), modId.end()));
  w.bytes(storage::Bytes(modVersion.begin(), modVersion.end()));
  w.u16(dataVersion);
  w.u32(LITHOLATTICE_SDK_VERSION);
  w.bytes(storage::Bytes(data.begin(), data.end()));
}

bool decodeSection(const uint8_t* p, size_t n, std::string& modId, std::string& modVersion,
                   uint16_t& dataVersion, std::vector<uint8_t>& data, std::string& err) {
  ByteReader r(p, n);
  if (r.u16() != kModSaveFormatVersion || !r.ok()) {
    err = "unknown mod-save format version";
    return false;
  }
  const storage::Bytes idB = r.bytes();
  const storage::Bytes verB = r.bytes();
  dataVersion = r.u16();
  (void)r.u32();  // sdkMajor: diagnostic only
  const storage::Bytes dataB = r.bytes();
  if (!r.ok() || !r.done()) {
    err = "malformed mod-save section";
    return false;
  }
  modId.assign(idB.begin(), idB.end());
  modVersion.assign(verB.begin(), verB.end());
  data.assign(dataB.begin(), dataB.end());
  return true;
}

}  // namespace

bool parseModSaveVersion(const std::string& s, uint32_t& major, uint32_t& minor,
                         uint32_t& patch) {
  unsigned long parts[3] = {0, 0, 0};
  size_t pos = 0;
  for (int i = 0; i < 3; ++i) {
    const size_t dot = s.find('.', pos);
    const std::string tok = (dot == std::string::npos) ? s.substr(pos) : s.substr(pos, dot - pos);
    if (tok.empty() || tok.size() > 9) return false;
    for (char ch : tok)
      if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
    parts[i] = std::stoul(tok);
    if (dot == std::string::npos) {
      if (i != 2) return false;  // "1.2" is not a semver here
      pos = s.size();            // fully consumed
      break;
    }
    pos = dot + 1;
  }
  if (pos != s.size()) return false;  // trailing garbage ("1.2.0.4") is not a semver
  major = static_cast<uint32_t>(parts[0]);
  minor = static_cast<uint32_t>(parts[1]);
  patch = static_cast<uint32_t>(parts[2]);
  return true;
}

ModSaveRegistry::ModSaveRegistry(std::filesystem::path worldDir)
    : worldDir_(std::move(worldDir)), saveDir_(worldDir_ / kModSaveDirName) {}

std::filesystem::path ModSaveRegistry::sectionPath(const std::string& modId) const {
  if (!ExtensionRegistry::isValidId(modId)) return {};
  return saveDir_ / (modId + kModSaveFileSuffix);
}

bool ModSaveRegistry::save(const std::string& modId, const std::string& modVersion,
                           uint16_t dataVersion, const std::vector<uint8_t>& payload,
                           std::string* err, const storage::FileWriteOptions& opts) {
  const std::filesystem::path path = sectionPath(modId);
  if (path.empty()) {
    if (err) *err = "invalid mod id '" + modId + "'";
    logError("modsave: refusing save for invalid mod id '%s'", modId.c_str());
    return false;
  }
  // A quarantined section is a recovery artifact: the engine never overwrites
  // one (file_contract.h quarantine contract). The game must surface this and
  // the user/tooling moves the file aside before the mod can save again.
  for (const std::string& e : quarantine_.entries()) {
    if (e.rfind(path.string() + ":", 0) == 0) {
      if (err) *err = "section is quarantined (" + e + "); move it aside before saving";
      logError("modsave: refusing to overwrite quarantined section '%s'", path.string().c_str());
      return false;
    }
  }
  ByteWriter w;
  encodeSection(modId, modVersion, dataVersion, payload, w);
  const storage::Bytes framed = storage::frameRecord(w.out);
  if (!storage::writeFileAtomic(path, framed, err, opts)) {
    logError("modsave: failed to write section '%s'", path.string().c_str());
    return false;
  }
  logInfo("modsave: wrote section '%s' (%zu bytes payload, version %s)", modId.c_str(),
          payload.size(), modVersion.c_str());
  return true;
}

std::vector<ModSaveResult> ModSaveRegistry::loadAll(const std::vector<RunningMod>& mods) {
  std::vector<ModSaveResult> results;
  results.reserve(mods.size());

  for (const RunningMod& mod : mods) {
    ModSaveResult res;
    res.modId = mod.id;
    const std::filesystem::path path = sectionPath(mod.id);
    if (path.empty()) {
      // Invalid id: cannot have a valid section; treat as Fresh and log loudly.
      res.status = ModSaveStatus::Fresh;
      res.reason = "invalid mod id; no section can exist";
      logError("modsave: mod id '%s' is invalid; treating as fresh (no save data)",
               mod.id.c_str());
      results.push_back(std::move(res));
      continue;
    }
    std::string err;
    std::optional<storage::Bytes> framed =
        storage::readValidatedRecord(path, &quarantine_, &err);
    if (!framed) {
      if (!err.empty()) {
        // Corrupt framing/CRC (already quarantined + logged by readValidatedRecord).
        res.status = ModSaveStatus::Corrupt;
        res.reason = err;
        logError("modsave: mod '%s' save section is corrupt (%s); mod disabled for this world",
                 mod.id.c_str(), err.c_str());
      } else {
        // Absent: fresh install of this mod into an existing world.
        res.status = ModSaveStatus::Fresh;
        logInfo("modsave: no save section for mod '%s'; starting fresh", mod.id.c_str());
      }
      results.push_back(std::move(res));
      continue;
    }

    std::string storedId, storedVersion;
    uint16_t storedDataVersion = 0;
    std::vector<uint8_t> data;
    std::string why;
    if (!decodeSection(framed->data(), framed->size(), storedId, storedVersion, storedDataVersion,
                       data, why) ||
        storedId != mod.id) {
      if (why.empty()) why = "mod id mismatch (section for '" + storedId + "')";
      quarantine_.quarantine(path, why);
      res.status = ModSaveStatus::Corrupt;
      res.reason = why;
      logError("modsave: mod '%s' save section failed inner validation (%s); mod disabled",
               mod.id.c_str(), why.c_str());
      results.push_back(std::move(res));
      continue;
    }
    res.storedVersion = storedVersion;

    uint32_t sMaj = 0, sMin = 0, sPat = 0, rMaj = 0, rMin = 0, rPat = 0;
    const bool sOk = parseModSaveVersion(storedVersion, sMaj, sMin, sPat);
    const bool rOk = parseModSaveVersion(mod.version, rMaj, rMin, rPat);
    if (!sOk || !rOk) {
      why = "unparseable version (stored '" + storedVersion + "', running '" + mod.version + "')";
      quarantine_.quarantine(path, why);
      res.status = ModSaveStatus::VersionIncompatible;
      res.reason = why;
      logError("modsave: mod '%s' %s; mod disabled for this world", mod.id.c_str(), why.c_str());
      results.push_back(std::move(res));
      continue;
    }
    if (sMaj != rMaj) {
      why = "major version mismatch (stored '" + storedVersion + "', running '" + mod.version +
            "'); no cross-major migration";
      quarantine_.quarantine(path, why);
      res.status = ModSaveStatus::VersionIncompatible;
      res.reason = why;
      logError("modsave: mod '%s' %s; mod disabled for this world", mod.id.c_str(), why.c_str());
      results.push_back(std::move(res));
      continue;
    }
    if (storedVersion == mod.version) {
      res.status = ModSaveStatus::Ok;
      res.payload = std::move(data);
      logInfo("modsave: loaded section for mod '%s' (version %s, %zu bytes)", mod.id.c_str(),
              storedVersion.c_str(), res.payload.size());
      results.push_back(std::move(res));
      continue;
    }
    // Same major, different minor/patch: the migration hook point (builder seam).
    if (migrate_) {
      std::optional<std::vector<uint8_t>> migrated =
          migrate_(mod.id, storedVersion, mod.version, storedDataVersion, data);
      if (migrated) {
        res.status = ModSaveStatus::Ok;
        res.payload = std::move(*migrated);
        logInfo("modsave: migrated mod '%s' save data %s -> %s", mod.id.c_str(),
                storedVersion.c_str(), mod.version.c_str());
        results.push_back(std::move(res));
        continue;
      }
      why = "migration " + storedVersion + " -> " + mod.version + " declined by migrate hook";
    } else {
      why = "version changed " + storedVersion + " -> " + mod.version +
            " with no migration registered";
    }
    quarantine_.quarantine(path, why);
    res.status = ModSaveStatus::VersionIncompatible;
    res.reason = why;
    logError("modsave: mod '%s' %s; mod disabled for this world", mod.id.c_str(), why.c_str());
    results.push_back(std::move(res));
  }

  // Orphaned sections: a section on disk for a mod that is not installed.
  // Kept — never deleted — and logged, so reinstalling the mod later finds
  // its data (subject to the version checks above).
  std::error_code ec;
  if (std::filesystem::is_directory(saveDir_, ec)) {
    for (const auto& e : std::filesystem::directory_iterator(saveDir_, ec)) {
      if (!e.is_regular_file(ec)) continue;
      const std::string name = e.path().filename().string();
      const std::string suffix = kModSaveFileSuffix;
      if (name.size() <= suffix.size() ||
          name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
        continue;
      const std::string id = name.substr(0, name.size() - suffix.size());
      if (!ExtensionRegistry::isValidId(id)) {
        logError("modsave: ignoring file with invalid mod id '%s' (left on disk)",
                 name.c_str());
        continue;
      }
      bool installed = false;
      for (const RunningMod& m : mods)
        if (m.id == id) {
          installed = true;
          break;
        }
      if (!installed) {
        ModSaveResult res;
        res.modId = id;
        res.status = ModSaveStatus::Orphaned;
        res.reason = "mod not installed; section kept on disk";
        logInfo("modsave: orphaned save section for removed mod '%s' (kept, not deleted)",
                id.c_str());
        results.push_back(std::move(res));
      }
    }
  }
  return results;
}

}  // namespace litholattice::mods
