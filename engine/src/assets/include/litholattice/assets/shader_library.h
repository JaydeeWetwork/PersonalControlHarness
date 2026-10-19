// Shader sources read through the pack stack (shaders/<program>.vert / .frag), so packs can
// override them. Missing programs are simply absent: the renderer then uses its built-in copy.
// Sources may share code with `#include "file.glsl"` lines (resolved from shaders/, expanded
// here because GLSL 330 has no include directive).
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "litholattice/assets/resource_pack.h"

namespace litholattice {

struct ShaderSource {
  std::string vertex, fragment;
  std::string origin;  ///< pack(s) the two files came from
  bool operator==(const ShaderSource&) const = default;
};

/// Returns the text of an include file by name, or nullopt if it does not exist.
using ShaderIncludeResolver = std::function<std::optional<std::string>(std::string_view name)>;

/// Replaces each line of the form `#include "name"` with the resolved file (recursively, up to
/// 8 levels). Returns nullopt and sets `error` for a missing file or an include cycle.
std::optional<std::string> expandShaderIncludes(std::string_view source, const ShaderIncludeResolver& resolve,
                                                std::string* error);

class ShaderLibrary {
 public:
  /// Loads each program whose .vert and .frag both exist in the stack, with includes expanded;
  /// a program with only one stage or a broken include is skipped with a warning.
  static ShaderLibrary load(const PackStack& stack, std::span<const std::string_view> programs,
                            std::vector<std::string>* warnings);

  const ShaderSource* find(std::string_view program) const;
  size_t size() const { return programs_.size(); }
  bool operator==(const ShaderLibrary&) const = default;

 private:
  std::map<std::string, ShaderSource, std::less<>> programs_;
};

}  // namespace litholattice
