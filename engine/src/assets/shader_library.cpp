#include "litholattice/assets/shader_library.h"

namespace litholattice {
namespace {

constexpr int kMaxIncludeDepth = 8;

/// The quoted name of an `#include "name"` line, or an empty view if the line is not one.
std::string_view includeTarget(std::string_view line) {
  const size_t first = line.find_first_not_of(" \t");
  if (first == std::string_view::npos) return {};
  line.remove_prefix(first);
  constexpr std::string_view kDirective = "#include";
  if (!line.starts_with(kDirective)) return {};
  const size_t open = line.find('"'), close = line.rfind('"');
  if (open == std::string_view::npos || close <= open) return {};
  return line.substr(open + 1, close - open - 1);
}

bool expandInto(std::string_view source, const ShaderIncludeResolver& resolve, int depth, std::string& out,
                std::string* error) {
  size_t pos = 0;
  while (pos < source.size()) {
    size_t end = source.find('\n', pos);
    const bool lastLine = end == std::string_view::npos;
    if (lastLine) end = source.size();
    const std::string_view line = source.substr(pos, end - pos);
    if (const std::string_view name = includeTarget(line); !name.empty()) {
      if (depth >= kMaxIncludeDepth) {
        if (error) *error = "include depth exceeded at \"" + std::string(name) + "\" (cycle?)";
        return false;
      }
      const std::optional<std::string> text = resolve(name);
      if (!text) {
        if (error) *error = "include \"" + std::string(name) + "\" not found";
        return false;
      }
      if (!expandInto(*text, resolve, depth + 1, out, error)) return false;
      if (!out.empty() && out.back() != '\n') out += '\n';
    } else {
      out.append(line);
      if (!lastLine) out += '\n';
    }
    pos = end + 1;
  }
  return true;
}

}  // namespace

std::optional<std::string> expandShaderIncludes(std::string_view source, const ShaderIncludeResolver& resolve,
                                                std::string* error) {
  std::string out;
  if (!expandInto(source, resolve, 0, out, error)) return std::nullopt;
  return out;
}

ShaderLibrary ShaderLibrary::load(const PackStack& stack, std::span<const std::string_view> programs,
                                  std::vector<std::string>* warnings) {
  ShaderLibrary lib;
  const ShaderIncludeResolver fromStack = [&stack](std::string_view name) {
    return stack.readText("shaders/" + std::string(name));
  };
  for (const std::string_view name : programs) {
    const std::string base = "shaders/" + std::string(name);
    std::string vo, fo;
    const auto vs = stack.readText(base + ".vert", &vo);
    const auto fs = stack.readText(base + ".frag", &fo);
    if (!vs && !fs) continue;
    if (!vs || !fs) {
      if (warnings) warnings->push_back(base + (vs ? ".frag" : ".vert") + " missing; program '" + std::string(name) + "' skipped");
      continue;
    }
    std::string err;
    auto vertex = expandShaderIncludes(*vs, fromStack, &err);
    auto fragment = vertex ? expandShaderIncludes(*fs, fromStack, &err) : std::nullopt;
    if (!fragment) {
      if (warnings) warnings->push_back("program '" + std::string(name) + "' skipped: " + err);
      continue;
    }
    ShaderSource s;
    s.vertex = std::move(*vertex);
    s.fragment = std::move(*fragment);
    s.origin = vo == fo ? vo : vo + " + " + fo;
    lib.programs_.emplace(std::string(name), std::move(s));
  }
  return lib;
}

const ShaderSource* ShaderLibrary::find(std::string_view program) const {
  const auto it = programs_.find(program);
  return it == programs_.end() ? nullptr : &it->second;
}

}  // namespace litholattice
