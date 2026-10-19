// litholattice::storage::ModSandbox — the mod file-access security boundary.
//
// Mods never touch the real filesystem directly: every mod-supplied path (asset
// loads, config reads, data writes) is resolved through a sandbox rooted at the
// mod's own directory (or the game's mod-data directory). resolve() is the single
// choke point; anything it rejects never reaches open().
//
// What it stops:
//   - `..` traversal that escapes the root ("../evil", "a/../../evil")
//   - absolute paths ("/etc/passwd", and drive-letter forms like "C:/x" so a mod
//     written on Windows cannot escape on Linux either)
//   - symlink escapes: any symlink in the resolved chain that points outside the
//     root is rejected, including symlink loops. A symlink that stays inside the
//     root is allowed.
//   - NUL bytes (would truncate the path at the OS layer) and empty paths.
//
// What it does NOT promise: TOCTOU-proofing against a mod that is actively
// racing the filesystem (swapping a symlink between resolve() and use). A native
// (DLL) mod can already call the OS directly, so a mod that hostile is outside
// the sandbox's threat model — the boundary stops accidental escapes, confused
// paths, and casual traversal attacks, which is the realistic attack surface for
// script/content mods going through engine APIs. Callers that need harder
// guarantees should open resolved paths with O_NOFOLLOW and re-check containment
// on the open fd.
//
// Usage: the engine resolves the mod's path once, then uses the returned absolute
// path for the actual I/O. Never concatenate the mod's raw string with the root
// yourself — that is exactly the bug this class exists to prevent.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace litholattice::storage {

class ModSandbox {
 public:
  /// `root`: the directory the mod is jailed to. Stored weakly-canonicalized.
  explicit ModSandbox(std::filesystem::path root) {
    std::error_code ec;
    root_ = std::filesystem::weakly_canonical(root, ec);
    if (ec) root_ = root.lexically_normal();
    rootStr_ = root_.string();
  }

  const std::filesystem::path& root() const { return root_; }

  /// True if the absolute path `p` is inside the root (the root itself or
  /// below it). Used to re-check containment on an already-open fd's true
  /// path (see sandboxed_io.h) — the descriptor can't be raced, only the
  /// path string could have been, so this closes the check-vs-use window.
  bool contains(const std::filesystem::path& p) const { return withinRoot(p); }

  /// Resolve a mod-supplied path against the sandbox root. Returns the absolute,
  /// symlink-resolved path on success; nullopt + err on rejection (see above).
  /// The returned path is guaranteed inside the root at resolve() time.
  std::optional<std::filesystem::path> resolve(std::string_view modPath,
                                               std::string* err) const {
    auto reject = [&](const std::string& m) -> std::optional<std::filesystem::path> {
      if (err) *err = m;
      return std::nullopt;
    };
    if (modPath.empty()) return reject("empty mod path");
    if (modPath.find('\0') != std::string_view::npos) return reject("NUL byte in mod path");

    std::filesystem::path p(modPath);
    if (p.is_absolute()) return reject("absolute mod path rejected: " + std::string(modPath));
    if (modPath.size() >= 2 && modPath[1] == ':' &&
        ((modPath[0] >= 'A' && modPath[0] <= 'Z') || (modPath[0] >= 'a' && modPath[0] <= 'z')))
      return reject("drive-letter mod path rejected: " + std::string(modPath));

    // Lexical pre-check: no ".." may pop above the root. (weakly_canonical below
    // resolves symlinks, but this cheap pass rejects the obvious attacks even
    // when nothing on disk exists yet.)
    int depth = 0;
    for (const auto& comp : p) {
      const std::string c = comp.string();
      if (c == "..") {
        if (--depth < 0) return reject("path escapes the sandbox root: " + std::string(modPath));
      } else if (c != "." && !c.empty()) {
        ++depth;
      }
    }

    // Resolve symlinks (of the portions that exist) and normalize the rest.
    std::error_code ec;
    const std::filesystem::path canon =
        std::filesystem::weakly_canonical(root_ / p, ec);
    if (ec) return reject("cannot resolve mod path (symlink loop?): " + std::string(modPath));

    if (!withinRoot(canon))
      return reject("path escapes the sandbox root: " + std::string(modPath));
    return canon;
  }

 private:
  bool withinRoot(const std::filesystem::path& canon) const {
    const std::string s = canon.string();
    if (s.size() < rootStr_.size()) return false;
    if (s.compare(0, rootStr_.size(), rootStr_) != 0) return false;
    // Exact root, or root followed by a separator — never a sibling whose name
    // merely shares the prefix ("/game/mod" vs "/game/modevil").
    return s.size() == rootStr_.size() ||
           s[rootStr_.size()] == std::filesystem::path::preferred_separator;
  }

  std::filesystem::path root_;
  std::string rootStr_;
};

}  // namespace litholattice::storage
