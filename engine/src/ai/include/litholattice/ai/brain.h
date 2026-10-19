// Decision making: utility AI (scored actions, with an optional learned scorer hook) and
// behaviour-tree nodes. Both are pure functions of the context they are given + its RNG.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace litholattice::ai {

struct Mob;
struct AiContext;

/// Deterministic per-mob RNG (splitmix64 of world seed, mob id and AI tick).
struct AiRng {
  uint64_t s = 0;
  uint64_t next();
  float unit() { return float(next() >> 40) / float(1u << 24); }
};

// ---- utility AI ----
struct UtilityAction {
  std::string name;
  std::vector<std::function<float(const AiContext&)>> considerations;  ///< each 0..1; multiplied
  float weight = 1.f;
  std::function<void(AiContext&)> run;
};
/// Learned-model hook: may override an action's score (nullopt = use the hand-written one).
/// Stub today; the final-boss slot is meant to plug a trained model in here.
class ILearnedScorer {
 public:
  virtual ~ILearnedScorer() = default;
  virtual std::optional<float> score(const AiContext& ctx, const UtilityAction& action) const = 0;
};
class NullLearnedScorer final : public ILearnedScorer {
 public:
  std::optional<float> score(const AiContext&, const UtilityAction&) const override { return std::nullopt; }
};
/// Index of the best action (ties -> lowest index), or -1 if all score 0.
int pickUtility(const AiContext& ctx, const std::vector<UtilityAction>& actions, const ILearnedScorer* learned = nullptr);

// ---- behaviour trees ----
enum class BtStatus : uint8_t { Success, Failure, Running };
class BtNode {
 public:
  virtual ~BtNode() = default;
  virtual BtStatus tick(AiContext& ctx) = 0;
};
using BtPtr = std::unique_ptr<BtNode>;
BtPtr btSequence(std::vector<BtPtr> children);  ///< fails on first failure; resumes a Running child
BtPtr btSelector(std::vector<BtPtr> children);  ///< succeeds on first success
BtPtr btInverter(BtPtr child);
BtPtr btCondition(std::function<bool(const AiContext&)> pred);
BtPtr btAction(std::function<BtStatus(AiContext&)> act);
/// Runs a utility pick as a BT leaf (Success if an action ran).
BtPtr btUtility(std::vector<UtilityAction> actions, const ILearnedScorer* learned = nullptr);

}  // namespace litholattice::ai
