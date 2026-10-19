// ExtensionRegistry: tracks the mod/extension tree for IExtensible parents.
//
// Dotted identity paths: every extension is addressed by the full path from
// its top-level mod, e.g. "lithokraft.worldedit.brushes". Parent/child links
// are derived from the path (parent = path minus the last segment).
//
// Depth: a top-level mod sits at depth 0; each nesting level adds 1. The cap
// is kDefaultMaxExtensionDepth (8) by default and is a runtime setting, not
// hardcoded behavior: setMaxExtensionDepth() lets users raise it as hardware
// and the ecosystem grow. Registering at depth > max is refused.
//
// This registry tracks identity and depth only. Budgets, isolation, and
// privilege come from the facet (extensible_interface.h) and are worker 2/3's
// piece; they consume find()/childrenOf()/depthOf() here.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace litholattice {

/// Default extension nesting cap. A k-prefix constant, NOT hardcoded behavior:
/// ExtensionRegistry::setMaxExtensionDepth() overrides it at runtime.
inline constexpr uint32_t kDefaultMaxExtensionDepth = 8;

/// One node in the extension tree.
struct ExtensionNode {
  std::string id;          ///< leaf id, e.g. "brushes" ([a-z0-9_]{1,64})
  std::string path;        ///< dotted identity, e.g. "lithokraft.worldedit.brushes"
  std::string parentPath;  ///< dotted path of the parent ("" for top-level mods)
  uint32_t depth = 0;      ///< top-level mod = 0; each nesting level +1
};

class ExtensionRegistry {
 public:
  uint32_t maxExtensionDepth() const { return maxDepth_; }

  /// Overrides the depth cap. 0 resets to kDefaultMaxExtensionDepth.
  /// Lowering the cap does not evict already-registered nodes; it only
  /// refuses new registrations beyond the new cap.
  void setMaxExtensionDepth(uint32_t depth);

  /// Registers a top-level mod (depth 0). False on invalid id or duplicate.
  bool registerMod(const std::string& id);

  /// Registers an extension under parentPath ("mod" or "mod.child...").
  /// Returns the new dotted identity path, or "" on failure: unknown parent,
  /// invalid id, duplicate path, or the child would sit deeper than the cap.
  std::string registerExtension(const std::string& parentPath, const std::string& id);

  /// Removes a single node by dotted path. Does NOT cascade: the caller
  /// (ExtensionLoader::unloadMod) removes descendants first, children-first.
  /// Returns false when the path is unknown.
  bool unregister(const std::string& path);

  bool contains(const std::string& path) const;
  const ExtensionNode* find(const std::string& path) const;
  /// Depth of a registered path; 0 for top-level mods. Returns false when unknown.
  bool depthOf(const std::string& path, uint32_t& depth) const;
  /// Dotted paths of the direct children of parentPath, in registration order.
  std::vector<std::string> childrenOf(const std::string& parentPath) const;
  /// Every registered dotted path, parents before children (lexicographic
  /// order guarantees it: a parent path is always a strict prefix of its
  /// children's). Added for worker 3's budget-tree rebuild (ExtensionRuntime).
  std::vector<std::string> allPaths() const;

  /// "mod", "mod.child", ... — every segment [a-z0-9_]{1,64}.
  static bool isValidId(std::string_view id);
  static bool isValidPath(std::string_view path);
  /// joinPath("lithokraft.worldedit", "brushes") == "lithokraft.worldedit.brushes".
  static std::string joinPath(const std::string& parentPath, std::string_view id);
  /// splitPath("lithokraft.worldedit.brushes") == {"lithokraft","worldedit","brushes"}.
  /// Returns {} on invalid input.
  static std::vector<std::string> splitPath(std::string_view path);
  /// parentPath("lithokraft.worldedit.brushes") == "lithokraft.worldedit"; "" for top-level.
  static std::string parentPath(std::string_view path);

 private:
  uint32_t maxDepth_ = kDefaultMaxExtensionDepth;
  // Keyed by dotted path; std::map keeps childrenOf deterministic.
  std::map<std::string, ExtensionNode> nodes_;
  std::vector<std::string> order_;  // registration order
};

}  // namespace litholattice
