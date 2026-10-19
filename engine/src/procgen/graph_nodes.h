// Private to procgen: the node type table and node implementations behind NoiseGraph.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "litholattice/core/json.h"
#include "litholattice/procgen/noise.h"

namespace litholattice::procgen::detail {

/// A compiled node: computes out[i] from its input buffers at points[i].
class Node {
 public:
  virtual ~Node() = default;
  virtual void eval(const PointsView& points, std::span<const std::span<const float>> inputs,
                    std::span<float> out) const = 0;
};

/// Node type description: input parameters (node references), plain parameters, factory.
struct NodeType {
  using Factory = std::unique_ptr<Node> (*)(const Json& params, uint64_t seed, std::string& err);
  std::string_view name;
  std::vector<std::string_view> inputs;  ///< required single-input parameters
  bool variadic = false;                 ///< takes an "inputs" array (one or more)
  std::vector<std::string_view> params;  ///< allowed plain parameters
  Factory make = nullptr;
};

/// The node type named `name`, or nullptr.
const NodeType* findNodeType(std::string_view name);
/// Identity node used for "cache" (the graph evaluator keeps its values between bindings).
inline constexpr std::string_view kCacheType = "cache";
/// A constant node for inline numeric inputs.
std::unique_ptr<Node> makeConstant(float value);
/// Factory for the "lithoform" generator node (litho_formula_node.cpp).
std::unique_ptr<Node> makeLithoform(const Json& params, uint64_t seed, std::string& err);

}  // namespace litholattice::procgen::detail
