// mod.json: the Workshop-ready mod manifest (see docs/SDK.md, "Mods").
//   {"schema": 1, "id": "lithokraft", "name": "LithoKraft", "version": "0.1.0", "type": "content",
//    "deps": [{"id": "litholattice", "version": ">=0.1.0"}],
//    "permissions": [{"api": "filesystem.save", "reason": "Writes world saves"}]}
// type: content (data/assets only; loads silently), script (AngelScript), native (DLL, future).
// Script and native mods must explain every permission; the prompt UI comes later (decision Q6).
//
// CANONICAL MANIFEST (F-L12, review 2026-10-13): THIS system -- assets/
// mod_manifest (schema-1 JSON, permissions as {api, reason} pairs, the
// signature/trust model in mod_signature.h) -- is the canonical mod-manifest
// system: it is the one the game actually uses (content packs, boot seal /
// hot-swap decisions in main.cpp). The OTHER system, engine/src/mods'
// ModLoader (different schema: "sdk" + permission NAMES -> kPerm bits), is
// STAGED/UNWIRED -- no game code loads mods through it; it exists for the
// future script/native loading work and is exercised by selftests only.
// Do not wire new features to the ModLoader schema; if script/native
// loading is ever wired, it migrates onto THIS manifest first.
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace litholattice {

struct SemVer {
  int major = 0, minor = 0, patch = 0;
  static std::optional<SemVer> parse(const std::string& s);
  auto operator<=>(const SemVer&) const = default;
  std::string str() const;
};

enum class ModType : uint8_t { Content, Script, Native };

/// Mod signature verification outcome (see mod_signature.h for the verifier).
/// Certificates answer WHO vouches for the mod; sandboxing answers WHAT it can do.
enum class SignatureStatus : uint8_t {
  Verified,  ///< signed by JD's baked-in key, signature valid - full trust
  Unsigned,  ///< no signature.json - loads sandboxed, clearly labeled
  Invalid,   ///< signature present but bad - refused in all builds (fail closed)
};

struct ModManifest {
  struct Dep { std::string id; std::string range; };          ///< range: "*", "=x.y.z", ">=x.y.z" or "^x.y.z"
  struct Permission { std::string api; std::string reason; };
  int schema = 1;
  std::string id, name;
  SemVer version;
  ModType type = ModType::Content;
  std::vector<Dep> deps;
  std::vector<Permission> permissions;
  std::filesystem::path dir;  ///< folder it was loaded from (empty when parsed from text)
  /// Hot-swap prototyping: the mod opts into runtime block registration and
  /// definition updates while the engine runs. Ignored when sealed is true.
  /// In release builds the registry stays open ONLY for the verified live-ops
  /// case (all mods Verified + a verified hot_swap mod); otherwise release
  /// seals (see main.cpp boot seal decision).
  bool hotSwap = false;
  /// Sealed registry: no runtime registration or hot-swap updates, even in debug.
  /// Sealed wins over hotSwap when both are set. First-party mods seal after boot.
  bool sealed = false;
  /// Signature verification outcome, set by loadModDirectory().
  SignatureStatus signatureStatus = SignatureStatus::Unsigned;

  /// Parse + validate. Errors name the field. Ids are [a-z0-9_]{1,64}; content mods may not
  /// request permissions; every permission needs a known API and a non-empty reason.
  static std::optional<ModManifest> parse(const std::string& json, std::string* err);
  bool needsPermissionPrompt() const { return type != ModType::Content && !permissions.empty(); }
};

/// The APIs a permission may name.
const std::vector<std::string>& knownModApis();
/// Does `v` satisfy `range`?
bool versionSatisfies(const SemVer& v, const std::string& range);

struct ModSet {
  std::vector<ModManifest> ordered;  ///< dependencies first
  std::vector<std::string> errors;   ///< mods left out and why
};
/// Orders `mods` so dependencies load first. A mod with a missing or out-of-range dependency, a
/// duplicate id or a dependency cycle is left out with an error; `provided` lists ids that are
/// always present (e.g. the engine, "litholattice").
ModSet resolveMods(std::vector<ModManifest> mods, const std::vector<ModManifest>& provided = {});
/// Folders of the content mods in `set`, in load order: mounted into the resource pack stack
/// (a content mod's folder is laid out like a resource pack). Script / native mods are skipped
/// until the permission prompt exists.
std::vector<std::filesystem::path> contentModPacks(const ModSet& set);
/// Reads every `<dir>/<mod>/mod.json` (sorted by folder name) and resolves them. Malformed
/// manifests are reported in errors.
ModSet loadModDirectory(const std::filesystem::path& dir, const std::vector<ModManifest>& provided = {});

}  // namespace litholattice
