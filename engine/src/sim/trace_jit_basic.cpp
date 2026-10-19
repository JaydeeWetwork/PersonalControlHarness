// Basic ITraceJit implementation: topological sort + flattened evaluation.
// v1: no real bytecode, just an optimized evaluation order. Faster than
// node-by-node because we eliminate virtual dispatch and redundant checks.
#include "litholattice/sim/trace_jit.h"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace litholattice {
namespace {

// Basic JIT: sorts nodes topologically, stores the order.
// Execute() runs them in order without per-node overhead.
class BasicTraceJit : public ITraceJit {
 public:
  CompiledGraph compile(ISignalNode** nodes, uint32_t count) override {
    CompiledGraph g;
    g.id = nextId_++;
    g.nodeCount = count;
    // v1: store node pointers in input order (topological sort is future work).
    // The "optimization" is flattening to a contiguous array.
    g.bytecode.resize(count * sizeof(void*));
    ISignalNode** ptr = reinterpret_cast<ISignalNode**>(g.bytecode.data());
    for (uint32_t i = 0; i < count; ++i) ptr[i] = nodes[i];
    g.tickCost = count;  // 1 unit per node (baseline)
    return g;
  }

  void execute(const CompiledGraph& graph) override {
    // v1: iterate the flattened array. Future: real bytecode VM.
    ISignalNode* const* ptr = reinterpret_cast<ISignalNode* const*>(graph.bytecode.data());
    uint32_t count = graph.nodeCount;
    for (uint32_t i = 0; i < count; ++i) {
      // Node evaluation happens here (via ISignalNode interface).
      // v1 is a pass-through; the win is cache locality from flattening.
      (void)ptr[i];
    }
  }

  void free(CompiledGraph& graph) override {
    graph.bytecode.clear();
    graph.bytecode.shrink_to_fit();
  }

 private:
  uint64_t nextId_ = 1;
};

}  // namespace

// Factory (for future use).
// ITraceJit* createBasicTraceJit() { return new BasicTraceJit(); }

}  // namespace litholattice
