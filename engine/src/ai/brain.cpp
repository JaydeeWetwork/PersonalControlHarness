#include "litholattice/ai/brain.h"
#include "litholattice/ai/ai_world.h"
#include <algorithm>

namespace litholattice::ai {

uint64_t AiRng::next() {
  uint64_t z = (s += 0x9e3779b97f4a7c15ull);
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
  return z ^ (z >> 31);
}

int pickUtility(const AiContext& ctx, const std::vector<UtilityAction>& actions, const ILearnedScorer* learned) {
  int best = -1;
  float bestScore = 0.f;
  for (size_t i = 0; i < actions.size(); ++i) {
    float s = actions[i].weight;
    for (const auto& c : actions[i].considerations) s *= std::max(0.f, std::min(1.f, c(ctx)));
    if (learned)
      if (const auto o = learned->score(ctx, actions[i])) s = *o;
    if (s > bestScore) { bestScore = s; best = int(i); }
  }
  return best;
}

namespace {
class Sequence final : public BtNode {
 public:
  explicit Sequence(std::vector<BtPtr> c) : c_(std::move(c)) {}
  BtStatus tick(AiContext& ctx) override {
    for (; at_ < c_.size(); ++at_) {
      const BtStatus s = c_[at_]->tick(ctx);
      if (s == BtStatus::Running) return s;
      if (s == BtStatus::Failure) { at_ = 0; return s; }
    }
    at_ = 0;
    return BtStatus::Success;
  }
 private:
  std::vector<BtPtr> c_;
  size_t at_ = 0;
};
class Selector final : public BtNode {
 public:
  explicit Selector(std::vector<BtPtr> c) : c_(std::move(c)) {}
  BtStatus tick(AiContext& ctx) override {
    for (auto& n : c_)
      if (const BtStatus s = n->tick(ctx); s != BtStatus::Failure) return s;
    return BtStatus::Failure;
  }
 private:
  std::vector<BtPtr> c_;
};
class Inverter final : public BtNode {
 public:
  explicit Inverter(BtPtr c) : c_(std::move(c)) {}
  BtStatus tick(AiContext& ctx) override {
    const BtStatus s = c_->tick(ctx);
    return s == BtStatus::Running ? s : s == BtStatus::Success ? BtStatus::Failure : BtStatus::Success;
  }
 private:
  BtPtr c_;
};
class Condition final : public BtNode {
 public:
  explicit Condition(std::function<bool(const AiContext&)> p) : p_(std::move(p)) {}
  BtStatus tick(AiContext& ctx) override { return p_(ctx) ? BtStatus::Success : BtStatus::Failure; }
 private:
  std::function<bool(const AiContext&)> p_;
};
class Action final : public BtNode {
 public:
  explicit Action(std::function<BtStatus(AiContext&)> a) : a_(std::move(a)) {}
  BtStatus tick(AiContext& ctx) override { return a_(ctx); }
 private:
  std::function<BtStatus(AiContext&)> a_;
};
class Utility final : public BtNode {
 public:
  Utility(std::vector<UtilityAction> a, const ILearnedScorer* l) : a_(std::move(a)), l_(l) {}
  BtStatus tick(AiContext& ctx) override {
    const int i = pickUtility(ctx, a_, l_);
    if (i < 0) return BtStatus::Failure;
    if (a_[size_t(i)].run) a_[size_t(i)].run(ctx);
    ctx.lastAction = a_[size_t(i)].name;
    return BtStatus::Success;
  }
 private:
  std::vector<UtilityAction> a_;
  const ILearnedScorer* l_;
};
}  // namespace

BtPtr btSequence(std::vector<BtPtr> c) { return std::make_unique<Sequence>(std::move(c)); }
BtPtr btSelector(std::vector<BtPtr> c) { return std::make_unique<Selector>(std::move(c)); }
BtPtr btInverter(BtPtr c) { return std::make_unique<Inverter>(std::move(c)); }
BtPtr btCondition(std::function<bool(const AiContext&)> p) { return std::make_unique<Condition>(std::move(p)); }
BtPtr btAction(std::function<BtStatus(AiContext&)> a) { return std::make_unique<Action>(std::move(a)); }
BtPtr btUtility(std::vector<UtilityAction> a, const ILearnedScorer* l) { return std::make_unique<Utility>(std::move(a), l); }

}  // namespace litholattice::ai
