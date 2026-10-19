// LithoLattice JIT: compiles ISignalNode graphs to optimized form.
// Players build Trace networks physically. The JIT analyzes the node graph
// and produces a faster executable version. The "chip" is just the physical
// item/block that holds the compiled graph.
#pragma once
#include <cstdint>
#include <vector>

namespace litholattice {

// Forward declare (defined in signal_node.h)
class ISignalNode;

/// A compiled Trace graph: optimized, faster to tick than the raw nodes.
struct CompiledGraph {
  uint64_t id = 0;
  uint32_t nodeCount = 0;      ///< nodes in the original graph
  uint32_t tickCost = 0;       ///< estimated ticks to evaluate (lower = faster)
  std::vector<uint8_t> bytecode;  ///< compiled form (opaque to callers)
};

/// JIT compiler: ISignalNode graph in, optimized CompiledGraph out.
class ITraceJit {
 public:
  virtual ~ITraceJit() = default;

  /// Compile a set of connected ISignalNodes into an optimized graph.
  /// @param nodes the nodes to compile (must form a connected graph)
  /// @param count number of nodes
  /// @return compiled graph (faster than ticking nodes individually)
  virtual CompiledGraph compile(ISignalNode** nodes, uint32_t count) = 0;

  /// Execute a compiled graph for one tick. Faster than node-by-node.
  virtual void execute(const CompiledGraph& graph) = 0;

  /// Free a compiled graph.
  virtual void free(CompiledGraph& graph) = 0;
};

}  // namespace litholattice
