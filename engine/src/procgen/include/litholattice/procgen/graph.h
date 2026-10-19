// Composable noise node graph. A graph is described in JSON (see docs/PROCGEN.md), normalized
// into a GraphDesc, then compiled with a seed into an immutable NoiseGraph that evaluates its
// named outputs over batches of world positions (structure-of-arrays).
//
//   GraphDesc::parse(text) -> GraphDesc -> NoiseGraph::compile(desc, seed) -> evaluate(output, ctx)
//
// Node types: constant, coordinate, noise, add, multiply, min, max, clamp, curve, select, blend,
// terrace, scale_bias, cache, wave, abs, invert, power, fractal, warp (the last two are the
// standalone operator forms of the noise node's fractal/warp parameter blocks). Inputs reference
// other nodes by name, or are given inline
// as a number (constant) or a node object (hoisted into a named node on load).
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "litholattice/core/json.h"
#include "litholattice/procgen/noise.h"

namespace litholattice::procgen {

/// One named node: its type and its parameters. In normalized form every input parameter is a
/// node name (string) or a number; inline node objects have been hoisted into their own nodes.
struct NodeDesc {
  std::string name;
  std::string type;
  Json params;  ///< object, without the "type" member
};

/// Normalized graph description. Converting to JSON and back yields an equal description, so
/// tools can load, edit and save graphs losslessly.
struct GraphDesc {
  std::vector<NodeDesc> nodes;                                 ///< declaration order
  std::vector<std::pair<std::string, std::string>> outputs;    ///< output name -> node name

  /// Normalizes and validates `{"nodes": {...}, "outputs": {...}}` (node types, parameter
  /// names, input references). Returns false and sets `err` on any problem.
  static bool fromJson(const Json& json, GraphDesc& out, std::string& err);
  /// Parses JSON text, then fromJson.
  static bool parse(const std::string& text, GraphDesc& out, std::string& err);
  /// Canonical JSON form (nodes in order, inputs by name).
  Json toJson() const;
};

class NoiseGraph;

/// Per-thread evaluation scratch: one value buffer per node, reused while the same points stay
/// bound, plus the contents of cache nodes. Not thread-safe; give each worker its own.
class GraphContext {
 public:
  GraphContext() = default;
  GraphContext(const GraphContext&) = delete;
  GraphContext& operator=(const GraphContext&) = delete;

  /// Sets the positions for following evaluate() calls. The spans must outlive the binding.
  /// Node results are reused between outputs evaluated over the same binding.
  void bind(const PointsView& points);
  const PointsView& points() const { return points_; }

 private:
  friend class NoiseGraph;
  struct CacheEntry {
    uint64_t key = 0;
    std::vector<float> values;
  };
  PointsView points_;
  uint64_t pointsKey_ = 0;
  uint32_t epoch_ = 1;
  const NoiseGraph* graph_ = nullptr;
  std::vector<std::vector<float>> values_;
  std::vector<uint32_t> stamps_;
  std::vector<CacheEntry> caches_;
};

/// A compiled, immutable graph. evaluate() is const and thread-safe given distinct contexts.
class NoiseGraph {
 public:
  /// Builds the node objects; noise nodes seed from `seed` and their own seed parameter (or
  /// node name). Returns nullptr and sets `err` on unknown references or cycles.
  static std::shared_ptr<const NoiseGraph> compile(const GraphDesc& desc, uint64_t seed, std::string& err);
  /// Construction token: only NoiseGraph's own factory can make one, so it can use make_unique / make_shared.
  class Passkey {
    friend class NoiseGraph;
    Passkey() = default;
  };
  NoiseGraph(Passkey, GraphDesc desc, uint64_t seed);
  ~NoiseGraph();
  NoiseGraph(const NoiseGraph&) = delete;
  NoiseGraph& operator=(const NoiseGraph&) = delete;

  /// Index of a named output, or -1.
  int outputIndex(std::string_view name) const;
  std::vector<std::string> outputNames() const;
  const GraphDesc& desc() const { return desc_; }
  uint64_t seed() const { return seed_; }

  /// out[i] = output value at the context's bound point i. out.size() == points().size().
  void evaluate(int output, GraphContext& ctx, std::span<float> out) const;
  /// One point (convenience for tools; batch evaluation is much cheaper per point).
  float evaluateAt(int output, float x, float y, float z) const;

 private:
  struct Compiled;  // implementation (graph.cpp)
  const std::vector<float>& evalNode(size_t node, GraphContext& ctx) const;

  GraphDesc desc_;
  uint64_t seed_;
  std::unique_ptr<Compiled> impl_;
};

}  // namespace litholattice::procgen
