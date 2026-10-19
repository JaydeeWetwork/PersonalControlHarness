// Actions: the input vocabulary game code asks about. The engine defines none; the game declares
// its actions in an ActionSet (id = index, usually an enum of its own) and passes enum values or
// ids to ActionMap / InputSystem.
#pragma once
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace litholattice {

using ActionId = uint16_t;

enum class ActionKind : uint8_t { Button, Axis2D };

struct ActionInfo {
  std::string_view name;  ///< stable name used in the bindings file
  ActionKind kind = ActionKind::Button;
};

/// Anything that converts to an action / context id: the game's enum or a raw id.
template <class T>
concept InputId = std::is_enum_v<T> || std::is_integral_v<T>;
template <InputId T>
constexpr ActionId actionId(T a) { return ActionId(a); }

/// The game's actions, in id order. Immutable once built; keep it alive as long as the maps.
class ActionSet {
 public:
  /// `mayShare`: pairs of actions that are meant to share an input (not reported as conflicts).
  explicit ActionSet(std::vector<ActionInfo> actions, std::vector<std::pair<ActionId, ActionId>> mayShare = {})
      : actions_(std::move(actions)), mayShare_(std::move(mayShare)) {}
  size_t size() const { return actions_.size(); }
  std::string_view name(ActionId a) const { return a < actions_.size() ? actions_[a].name : std::string_view{}; }
  ActionKind kind(ActionId a) const { return a < actions_.size() ? actions_[a].kind : ActionKind::Button; }
  std::optional<ActionId> find(std::string_view name) const {
    for (size_t i = 0; i < actions_.size(); ++i)
      if (actions_[i].name == name) return ActionId(i);
    return std::nullopt;
  }
  bool mayShareBinding(ActionId a, ActionId b) const {
    for (const auto& [x, y] : mayShare_)
      if ((x == a && y == b) || (x == b && y == a)) return true;
    return false;
  }

 private:
  std::vector<ActionInfo> actions_;
  std::vector<std::pair<ActionId, ActionId>> mayShare_;
};

}  // namespace litholattice
