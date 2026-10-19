// Per-mod save-data integrity (mod hardening item 3, 2026-10-11).
//
// Every mod gets its own save-data section in the world directory:
//   <world>/moddata/<modid>.llms
// Each section is checksummed (CRC32) and versioned (mod id + mod version +
// mod data-schema version recorded at write time). On world load every section
// is verified; a corrupt or version-incompatible section is QUARANTINED — the
// mod is disabled for that world (logged reason) and the world loads without
// it. Bad mod data never kills the world.
//
// Framing is the storage layer's own CRC32 framing (storage/file_contract.h:
// frameRecord / readValidatedRecord / "LFRM"), so mod sections get the same
// torn-write and bit-rot protection as every other engine file. Writes go
// through writeFileAtomic (temp + fsync + atomic rename, backup rotation), so
// a crash leaves either the old or the new section, never a torn one.
//
// Load outcomes (ModSaveStatus):
//   Ok                 section verified, mod runs with its data.
//   Fresh              mod is installed but has no section yet (fresh install
//                      into an existing world): starts with an empty payload.
//   Corrupt            checksum / framing / id mismatch: the file is
//                      quarantined (left on disk for recovery tooling, never
//                      overwritten or deleted by the engine) and the mod is
//                      disabled for this world.
//   VersionIncompatible the stored mod version cannot run against the loaded
//                      mod (major-version mismatch, or a minor/patch change
//                      with no registered migration): quarantined like Corrupt,
//                      mod disabled, logged reason.
//   Orphaned           a section exists for a mod that is not installed: kept
//                      on disk (never deleted), logged, ignored.
//
// Version migration hook (for the builder): when the stored version differs
// from the running mod's version on the same major line, the registry calls
// the registered ModSaveMigrateFn to upgrade the payload. No function
// registered, or the function declines (nullopt) -> the section is treated
// as VersionIncompatible (quarantined, mod disabled). The default posture is
// fail-closed: the world never runs a mod on data it cannot prove it
// understands.
//
// THREAT MODEL: a mod's bytes are untrusted input even when written by the
// engine itself — a crash, a disk fault, or a hand-edited file must not let
// garbage flow into the sim. Every section is validated before the mod's
// onWorldStart runs; onWorldStop/onUnload never see unverified data. Mod ids
// are validated ([a-z0-9_]{1,64}, ExtensionRegistry::isValidId) before they
// touch the filesystem, so a malicious id cannot escape <world>/moddata.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "litholattice/storage/file_contract.h"

namespace litholattice::mods {

/// On-disk format version of the mod-save inner payload (the outer framing is
/// the storage layer's "LFRM" frame; see file_contract.h).
inline constexpr uint16_t kModSaveFormatVersion = 1;

/// Subdirectory of the world dir holding per-mod sections.
inline constexpr const char* kModSaveDirName = "moddata";
/// Section file suffix: "<modid>.llms".
inline constexpr const char* kModSaveFileSuffix = ".llms";

/// Outcome of loading one mod's save section.
enum class ModSaveStatus : uint8_t {
  Ok = 0,               ///< verified; payload is the mod's data.
  Fresh = 1,            ///< no section on disk; mod starts with empty data.
  Corrupt = 2,          ///< failed verification; quarantined, mod disabled.
  VersionIncompatible,  ///< stored version unusable; quarantined, mod disabled.
  Orphaned = 4,         ///< section without an installed mod; kept, ignored.
};

/// A running mod the registry should load a section for.
struct RunningMod {
  std::string id;
  std::string version;  ///< semver, e.g. "1.2.0"
};

/// Result of loading one section.
struct ModSaveResult {
  ModSaveStatus status = ModSaveStatus::Fresh;
  std::string modId;
  std::string storedVersion;  ///< version recorded in the section ("" when Fresh)
  std::vector<uint8_t> payload;  ///< verified (or migrated) data; empty when Fresh
  std::string reason;  ///< human-readable reason for Corrupt/VersionIncompatible/Orphaned
  /// True when the mod must NOT run for this world (Corrupt/VersionIncompatible).
  bool disabled() const {
    return status == ModSaveStatus::Corrupt || status == ModSaveStatus::VersionIncompatible;
  }
};

/// Migration hook (builder seam): upgrade `payload` written by `fromVersion`
/// so the mod at `toVersion` can load it. Return the upgraded bytes, or
/// nullopt to decline — declining quarantines the section (VersionIncompatible)
/// and disables the mod for the world. `fromDataVersion` is the mod's own
/// schema version recorded with the payload; the running mod's current schema
/// version is known to the mod's own migrate implementation, not the registry.
using ModSaveMigrateFn = std::function<std::optional<std::vector<uint8_t>>(
    const std::string& modId, const std::string& fromVersion, const std::string& toVersion,
    uint16_t fromDataVersion, const std::vector<uint8_t>& payload)>;

/// Owns the <world>/moddata directory: atomic writes and verified loads of
/// per-mod sections. Not thread-safe; the game calls it from the world
/// load/save path (or the storage I/O thread) only.
class ModSaveRegistry {
 public:
  explicit ModSaveRegistry(std::filesystem::path worldDir);

  /// Register the migration hook (builder seam). Unset = every version change
  /// on a differing semver quarantines the section.
  void setMigrateFn(ModSaveMigrateFn fn) { migrate_ = std::move(fn); }

  /// Write one mod's section (atomic: temp + fsync + rename, backup rotation
  /// per opts). False + err on I/O failure or an invalid mod id. Never throws.
  bool save(const std::string& modId, const std::string& modVersion, uint16_t dataVersion,
            const std::vector<uint8_t>& payload, std::string* err,
            const storage::FileWriteOptions& opts = {});

  /// Load and verify every section for `mods`. Also reports Orphaned sections
  /// (kept on disk, never deleted). Corrupt / version-incompatible sections
  /// are quarantined via storage::Quarantine and flagged disabled — the caller
  /// skips those mods' onWorldStart. Never throws.
  std::vector<ModSaveResult> loadAll(const std::vector<RunningMod>& mods);

  /// Sections quarantined by the last loadAll (path + reason, oldest first).
  const storage::Quarantine& quarantine() const { return quarantine_; }

  /// Path of one mod's section file (id-validated; empty path when invalid).
  std::filesystem::path sectionPath(const std::string& modId) const;

 private:
  std::filesystem::path worldDir_;
  std::filesystem::path saveDir_;
  ModSaveMigrateFn migrate_;
  storage::Quarantine quarantine_;
};

/// Split "1.2.0" into {1,2,0}; false when the string is not a plain semver.
bool parseModSaveVersion(const std::string& s, uint32_t& major, uint32_t& minor,
                         uint32_t& patch);

}  // namespace litholattice::mods
