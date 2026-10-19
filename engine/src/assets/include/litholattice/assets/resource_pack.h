// Resource packs: a folder with an optional pack.json (display name) and an optional
// resources.json (texture / palette / animation / block-model definitions, see
// docs/RESOURCE_PACKS.md). A pack may also contain only files that override other packs.
#pragma once
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "litholattice/core/json.h"

namespace litholattice {

class ResourcePack {
 public:
  /// Opens `dir`. Fails (nullopt + `err`) if the folder is missing or resources.json has a
  /// syntax error; a pack without resources.json is valid (file overrides only).
  static std::optional<ResourcePack> open(const std::filesystem::path& dir, std::string* err);

  /// pack.json "name", else the folder name.
  const std::string& name() const { return name_; }
  const std::filesystem::path& dir() const { return dir_; }
  /// resources.json, or nullptr if the pack has none.
  const Json* manifest() const { return hasManifest_ ? &manifest_ : nullptr; }

  /// True if the pack contains `rel` ("textures/blocks/stone.png"). Paths that are absolute or
  /// contain ".." are rejected, so a pack cannot reach outside its folder.
  bool has(std::string_view rel) const;
  /// UNCHECKED lexical path (dir_/rel): no containment or existence check.
  /// Prefer checkedPath() for opening files (P3-3, Appendix E L6): the old
  /// pattern has(rel) -> open(path(rel)) is a TOCTOU pair — a symlink
  /// swapped between the check and the open escapes the check. path()
  /// remains for display/diagnostics and for callers that resolve through
  /// checkedPath() themselves.
  std::filesystem::path path(std::string_view rel) const { return dir_ / std::filesystem::path(rel); }
  /// Checked accessor: canonicalises `rel` inside the pack, verifies
  /// component-wise containment in the canonical pack root and that the
  /// target is a regular file, and returns the RESOLVED path to open.
  /// Opening the returned path (instead of path(rel)) closes the
  /// check->open TOCTOU window for the path itself. Returns nullopt when
  /// the file is absent, unsafe, or escapes the pack.
  std::optional<std::filesystem::path> checkedPath(std::string_view rel) const;

 private:
  std::string name_;
  std::filesystem::path dir_;
  Json manifest_;
  bool hasManifest_ = false;
};

/// True if `rel` is a safe pack-relative path (relative, no "..", not empty).
bool isSafePackPath(std::string_view rel);

/// Ordered packs, lowest priority first: engine base, then art packs and mods, then the
/// player's pack. A file or definition resolves to the highest pack that has it.
class PackStack {
 public:
  /// Adds `pack` above every pack already in the stack.
  void push(ResourcePack pack) { packs_.push_back(std::move(pack)); }

  /// Highest pack containing `rel`, or nullptr.
  const ResourcePack* owner(std::string_view rel) const;
  /// Full path of `rel` in its highest pack.
  std::optional<std::filesystem::path> find(std::string_view rel) const;
  /// Reads `rel` from its highest pack; `origin` receives that pack's name.
  std::optional<std::string> readText(std::string_view rel, std::string* origin = nullptr) const;

  std::span<const ResourcePack> packs() const { return packs_; }
  bool empty() const { return packs_.empty(); }

 private:
  std::vector<ResourcePack> packs_;
};

}  // namespace litholattice
