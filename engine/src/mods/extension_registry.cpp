// ExtensionRegistry: dotted identity paths, parent/child links, depth cap.
#include "litholattice/mods/extension_registry.h"

#include <algorithm>

namespace litholattice {

void ExtensionRegistry::setMaxExtensionDepth(uint32_t depth) {
  maxDepth_ = (depth == 0) ? kDefaultMaxExtensionDepth : depth;
}

bool ExtensionRegistry::registerMod(const std::string& id) {
  if (!isValidId(id) || contains(id)) return false;
  ExtensionNode node;
  node.id = id;
  node.path = id;
  node.parentPath = "";
  node.depth = 0;
  nodes_[node.path] = node;
  order_.push_back(node.path);
  return true;
}

std::string ExtensionRegistry::registerExtension(const std::string& parentPath,
                                                const std::string& id) {
  const ExtensionNode* parent = find(parentPath);
  if (!parent || !isValidId(id)) return "";
  const uint32_t childDepth = parent->depth + 1;
  if (childDepth > maxDepth_) return "";  // depth cap: refused, not truncated
  const std::string path = joinPath(parentPath, id);
  if (contains(path)) return "";
  ExtensionNode node;
  node.id = id;
  node.path = path;
  node.parentPath = parentPath;
  node.depth = childDepth;
  nodes_[path] = node;
  order_.push_back(path);
  return path;
}

bool ExtensionRegistry::contains(const std::string& path) const {
  return nodes_.find(path) != nodes_.end();
}

bool ExtensionRegistry::unregister(const std::string& path) {
  const auto it = nodes_.find(path);
  if (it == nodes_.end()) return false;
  nodes_.erase(it);
  order_.erase(std::remove(order_.begin(), order_.end(), path), order_.end());
  return true;
}

const ExtensionNode* ExtensionRegistry::find(const std::string& path) const {
  const auto it = nodes_.find(path);
  return it == nodes_.end() ? nullptr : &it->second;
}

bool ExtensionRegistry::depthOf(const std::string& path, uint32_t& depth) const {
  const ExtensionNode* node = find(path);
  if (!node) return false;
  depth = node->depth;
  return true;
}

std::vector<std::string> ExtensionRegistry::childrenOf(const std::string& parentPath) const {
  std::vector<std::string> out;
  for (const std::string& path : order_) {
    const ExtensionNode& node = nodes_.at(path);
    if (node.parentPath == parentPath) out.push_back(path);
  }
  return out;
}

std::vector<std::string> ExtensionRegistry::allPaths() const {
  // nodes_ is a std::map keyed by dotted path: iteration is lexicographic,
  // and a parent path is always a strict prefix of its children's paths, so
  // parents come out before children.
  std::vector<std::string> out;
  out.reserve(nodes_.size());
  for (const auto& [path, node] : nodes_) out.push_back(path);
  return out;
}

bool ExtensionRegistry::isValidId(std::string_view id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

bool ExtensionRegistry::isValidPath(std::string_view path) {
  if (path.empty()) return false;
  size_t start = 0;
  bool any = false;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '.') {
      if (!isValidId(path.substr(start, i - start))) return false;
      any = true;
      start = i + 1;
    }
  }
  return any;
}

std::string ExtensionRegistry::joinPath(const std::string& parentPath, std::string_view id) {
  return parentPath.empty() ? std::string(id) : parentPath + "." + std::string(id);
}

std::vector<std::string> ExtensionRegistry::splitPath(std::string_view path) {
  std::vector<std::string> parts;
  if (!isValidPath(path)) return parts;
  size_t start = 0;
  for (size_t i = 0; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '.') {
      parts.emplace_back(path.substr(start, i - start));
      start = i + 1;
    }
  }
  return parts;
}

std::string ExtensionRegistry::parentPath(std::string_view path) {
  const std::vector<std::string> parts = splitPath(path);
  if (parts.size() < 2) return "";
  std::string out = parts[0];
  for (size_t i = 1; i + 1 < parts.size(); ++i) out += "." + parts[i];
  return out;
}

}  // namespace litholattice
