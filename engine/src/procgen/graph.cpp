#include "litholattice/procgen/graph.h"
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include "graph_nodes.h"
#include "litholattice/procgen/seed.h"

namespace litholattice::procgen {
namespace {

using detail::NodeType;

// ---- normalization (GraphDesc::fromJson)

class Normalizer {
 public:
  explicit Normalizer(GraphDesc& out) : out_(out) {}

  /// Adds node `name` (and any inline input nodes, hoisted as "<name>/<input>").
  bool addNode(const std::string& name, const Json& node, std::string& err) {
    if (node.type != Json::Type::Object) return fail(err, name, "a node must be an object");
    const std::string type = node.getStr("type");
    const NodeType* t = detail::findNodeType(type);
    if (!t) return fail(err, name, "unknown node type '" + type + "'");
    if (!names_.insert(name).second) return fail(err, name, "duplicate node name");
    const size_t index = out_.nodes.size();
    out_.nodes.push_back({name, type, Json::object()});
    for (const auto& [key, value] : node.obj) {
      if (key == "type") continue;
      Json param = value;
      if (std::find(t->inputs.begin(), t->inputs.end(), key) != t->inputs.end()) {
        if (!normalizeInput(name + "/" + key, param, err)) return false;
      } else if (t->variadic && key == "inputs") {
        if (param.type != Json::Type::Array || param.arr.empty())
          return fail(err, name, "'inputs' must be a non-empty array");
        for (size_t i = 0; i < param.arr.size(); ++i)
          if (!normalizeInput(name + "/" + std::to_string(i), param.arr[i], err)) return false;
      } else if (std::find(t->params.begin(), t->params.end(), key) == t->params.end()) {
        return fail(err, name, "unknown parameter '" + key + "' for type '" + type + "'");
      }
      out_.nodes[index].params.add(key, std::move(param));
    }
    for (const std::string_view in : t->inputs)
      if (!out_.nodes[index].params.get(std::string(in)))
        return fail(err, name, "missing input '" + std::string(in) + "'");
    if (t->variadic && !out_.nodes[index].params.get("inputs")) return fail(err, name, "missing 'inputs'");
    return true;
  }

  /// Input values: node name (kept), number (kept, a constant), or inline node (hoisted).
  bool normalizeInput(const std::string& hoistName, Json& value, std::string& err) {
    if (value.type == Json::Type::String || value.type == Json::Type::Number) return true;
    if (value.type != Json::Type::Object) return fail(err, hoistName, "an input must be a node name, number or node");
    if (!addNode(hoistName, value, err)) return false;
    value = Json::string(hoistName);
    return true;
  }

  bool has(const std::string& name) const { return names_.contains(name); }

 private:
  static bool fail(std::string& err, const std::string& node, const std::string& msg) {
    err = "node '" + node + "': " + msg;
    return false;
  }
  GraphDesc& out_;
  std::unordered_set<std::string> names_;
};

/// Calls f(inputJson) for every input parameter of a normalized node.
template <typename F>
void forEachInput(const NodeDesc& node, F&& f) {
  const NodeType* t = detail::findNodeType(node.type);
  for (const std::string_view in : t->inputs) f(*node.params.get(std::string(in)));
  if (t->variadic)
    for (const Json& v : node.params.get("inputs")->arr) f(v);
}

uint64_t hashPoints(const PointsView& p) {
  uint64_t h = mix64(p.size());
  for (const auto axis : {p.x, p.y, p.z, p.w})
    for (const float v : axis) {
      uint32_t bits;
      std::memcpy(&bits, &v, sizeof bits);
      h = mix64(h ^ bits);
    }
  return h;
}

}  // namespace

bool GraphDesc::fromJson(const Json& json, GraphDesc& out, std::string& err) {
  out = {};
  const Json* nodes = json.get("nodes");
  const Json* outputs = json.get("outputs");
  if (json.type != Json::Type::Object || !nodes || nodes->type != Json::Type::Object || !outputs ||
      outputs->type != Json::Type::Object) {
    err = "a graph needs \"nodes\" and \"outputs\" objects";
    return false;
  }
  Normalizer norm(out);
  for (const auto& [name, node] : nodes->obj)
    if (!norm.addNode(name, node, err)) return false;
  for (const auto& [name, value] : outputs->obj) {
    Json v = value;
    if (v.type == Json::Type::Number) {
      Json c = Json::object();
      c.add("type", Json::string("constant"));
      c.add("value", v);
      v = std::move(c);
    }
    if (!norm.normalizeInput("output/" + name, v, err)) return false;
    out.outputs.emplace_back(name, v.str);
  }
  // Every string input must name a node.
  for (const NodeDesc& n : out.nodes) {
    bool ok = true;
    forEachInput(n, [&](const Json& in) {
      if (ok && in.type == Json::Type::String && !norm.has(in.str)) {
        err = "node '" + n.name + "': unknown input node '" + in.str + "'";
        ok = false;
      }
    });
    if (!ok) return false;
  }
  for (const auto& [name, node] : out.outputs)
    if (!norm.has(node)) {
      err = "output '" + name + "': unknown node '" + node + "'";
      return false;
    }
  if (out.outputs.empty()) {
    err = "a graph needs at least one output";
    return false;
  }
  return true;
}

bool GraphDesc::parse(const std::string& text, GraphDesc& out, std::string& err) {
  Json j;
  if (!Json::parse(text, j, &err)) return false;
  return fromJson(j, out, err);
}

Json GraphDesc::toJson() const {
  Json root = Json::object();
  Json& nodesJson = root.add("nodes", Json::object());
  for (const NodeDesc& n : nodes) {
    Json node = Json::object();
    node.add("type", Json::string(n.type));
    for (const auto& [k, v] : n.params.obj) node.add(k, v);
    nodesJson.add(n.name, std::move(node));
  }
  Json& outputsJson = root.add("outputs", Json::object());
  for (const auto& [name, node] : outputs) outputsJson.add(name, Json::string(node));
  return root;
}

// ---- compiled graph

struct NoiseGraph::Compiled {
  struct Entry {
    std::unique_ptr<detail::Node> node;
    std::vector<size_t> inputs;
    int cacheSlot = -1;  ///< >= 0 for cache nodes
  };
  std::vector<Entry> entries;           ///< desc nodes first, then inline constants
  std::vector<size_t> outputs;          ///< entry per desc output
  size_t cacheCount = 0;
};

NoiseGraph::NoiseGraph(Passkey, GraphDesc desc, uint64_t seed) : desc_(std::move(desc)), seed_(seed) {}
NoiseGraph::~NoiseGraph() = default;

std::shared_ptr<const NoiseGraph> NoiseGraph::compile(const GraphDesc& desc, uint64_t seed, std::string& err) {
  auto g = std::make_shared<NoiseGraph>(Passkey{}, desc, seed);  // one allocation
  auto impl = std::make_unique<Compiled>();
  std::unordered_map<std::string, size_t> index;
  for (size_t i = 0; i < desc.nodes.size(); ++i) index.emplace(desc.nodes[i].name, i);
  impl->entries.resize(desc.nodes.size());
  for (size_t i = 0; i < desc.nodes.size(); ++i) {
    const NodeDesc& n = desc.nodes[i];
    const NodeType* t = detail::findNodeType(n.type);
    if (!t) {
      err = "node '" + n.name + "': unknown type '" + n.type + "'";
      return nullptr;
    }
    const Json* s = n.params.get("seed");
    const uint64_t salt = s && s->type == Json::Type::Number ? uint64_t(int64_t(s->num)) : nameHash(n.name);
    std::string nodeErr;
    impl->entries[i].node = t->make(n.params, deriveSeed(seed, salt), nodeErr);
    if (!impl->entries[i].node) {
      err = "node '" + n.name + "': " + nodeErr;
      return nullptr;
    }
    if (n.type == detail::kCacheType) impl->entries[i].cacheSlot = int(impl->cacheCount++);
    std::vector<size_t> inputs;
    bool ok = true;
    forEachInput(n, [&](const Json& in) {
      if (in.type == Json::Type::Number) {
        inputs.push_back(impl->entries.size());
        impl->entries.push_back({detail::makeConstant(float(in.num)), {}, -1});
      } else if (const auto it = index.find(in.str); it != index.end()) {
        inputs.push_back(it->second);
      } else {
        err = "node '" + n.name + "': unknown input node '" + in.str + "'";
        ok = false;
      }
    });
    if (!ok) return nullptr;
    impl->entries[i].inputs = std::move(inputs);
  }
  for (const auto& [name, node] : desc.outputs) {
    const auto it = index.find(node);
    if (it == index.end()) {
      err = "output '" + name + "': unknown node '" + node + "'";
      return nullptr;
    }
    impl->outputs.push_back(it->second);
  }
  // Cycle check: iterative DFS colouring (0 new, 1 on stack, 2 done).
  std::vector<uint8_t> colour(impl->entries.size(), 0);
  for (size_t root = 0; root < impl->entries.size(); ++root) {
    if (colour[root]) continue;
    std::vector<std::pair<size_t, size_t>> stack{{root, 0}};
    colour[root] = 1;
    while (!stack.empty()) {
      auto& [node, next] = stack.back();
      if (next < impl->entries[node].inputs.size()) {
        const size_t in = impl->entries[node].inputs[next++];
        if (colour[in] == 1) {
          err = "graph has a cycle through node '" + desc.nodes[in].name + "'";
          return nullptr;
        }
        if (colour[in] == 0) {
          colour[in] = 1;
          stack.emplace_back(in, 0);
        }
      } else {
        colour[node] = 2;
        stack.pop_back();
      }
    }
  }
  g->impl_ = std::move(impl);
  return g;
}

int NoiseGraph::outputIndex(std::string_view name) const {
  for (size_t i = 0; i < desc_.outputs.size(); ++i)
    if (desc_.outputs[i].first == name) return int(i);
  return -1;
}

std::vector<std::string> NoiseGraph::outputNames() const {
  std::vector<std::string> names;
  for (const auto& [name, node] : desc_.outputs) names.push_back(name);
  return names;
}

void GraphContext::bind(const PointsView& points) {
  points_ = points;
  pointsKey_ = hashPoints(points);
  if (++epoch_ == 0) {  // wrapped: invalidate every stamp explicitly
    std::fill(stamps_.begin(), stamps_.end(), 0u);
    epoch_ = 1;
  }
}

const std::vector<float>& NoiseGraph::evalNode(size_t node, GraphContext& ctx) const {
  std::vector<float>& out = ctx.values_[node];
  if (ctx.stamps_[node] == ctx.epoch_) return out;
  const Compiled::Entry& e = impl_->entries[node];
  const size_t n = ctx.points_.size();
  out.resize(n);
  if (e.cacheSlot >= 0) {
    GraphContext::CacheEntry& c = ctx.caches_[size_t(e.cacheSlot)];
    if (c.key != ctx.pointsKey_ || c.values.size() != n) {
      c.values = evalNode(e.inputs[0], ctx);
      c.key = ctx.pointsKey_;
    }
    std::copy(c.values.begin(), c.values.end(), out.begin());
  } else {
    std::vector<std::span<const float>> inputs;
    inputs.reserve(e.inputs.size());
    for (const size_t in : e.inputs) inputs.emplace_back(evalNode(in, ctx));
    e.node->eval(ctx.points_, inputs, out);
  }
  ctx.stamps_[node] = ctx.epoch_;
  return out;
}

void NoiseGraph::evaluate(int output, GraphContext& ctx, std::span<float> out) const {
  if (ctx.graph_ != this) {  // first use with this graph: size the scratch
    ctx.graph_ = this;
    ctx.values_.assign(impl_->entries.size(), {});
    ctx.stamps_.assign(impl_->entries.size(), 0u);
    ctx.caches_.assign(impl_->cacheCount, {});
  }
  const std::vector<float>& v = evalNode(impl_->outputs[size_t(output)], ctx);
  std::copy_n(v.begin(), std::min(v.size(), out.size()), out.begin());
}

float NoiseGraph::evaluateAt(int output, float x, float y, float z) const {
  GraphContext ctx;
  const float w = 0;
  ctx.bind({{&x, 1}, {&y, 1}, {&z, 1}, {&w, 1}});
  float v = 0;
  evaluate(output, ctx, {&v, 1});
  return v;
}

}  // namespace litholattice::procgen
