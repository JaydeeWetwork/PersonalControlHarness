// Bindings for every action (several per action, across keyboard, mouse and gamepad) plus the
// analog settings. Plain data: loaded from / saved to config/input.json.
#pragma once
#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "litholattice/input/action.h"
#include "litholattice/input/analog.h"
#include "litholattice/input/binding.h"

namespace litholattice {

class ActionMap {
 public:
  /// No bindings yet for the actions of `actions` (which must outlive the map).
  explicit ActionMap(const ActionSet& actions) : actions_(&actions), binds_(actions.size()) {}
  const ActionSet& actions() const { return *actions_; }
  template <InputId A>
  const std::vector<Binding>& bindings(A a) const { return binds_[slot(a)]; }
  template <InputId A>
  void setBindings(A a, std::vector<Binding> b) { binds_[slot(a)] = std::move(b); }
  template <InputId A>
  void addBinding(A a, Binding b) { binds_[slot(a)].push_back(b); }

  const AnalogSettings& analog() const { return analog_; }
  AnalogSettings& analog() { return analog_; }

  /// Pairs of actions sharing an input, except pairs the ActionSet marks as meant to share.
  std::vector<std::pair<ActionId, ActionId>> conflicts() const;

  /// JSON text: {"version":1, "analog":{...}, "bindings":{"MoveForward":["key:W", ...], ...}}.
  std::string toJson() const;
  /// Parses JSON over `defaults`: actions missing from the file keep their defaults (so new
  /// actions appear after an update). Unknown actions / bindings are skipped and reported in
  /// `warnings`. Returns nullopt on a syntax error (`err` set).
  static std::optional<ActionMap> fromJson(const ActionMap& defaults, const std::string& text, std::string* err,
                                           std::vector<std::string>* warnings = nullptr);

  bool operator==(const ActionMap&) const = default;

 private:
  template <InputId A>
  size_t slot(A a) const {
    const size_t i = size_t(a);
    return i < binds_.size() ? i : binds_.size() - 1;  // out-of-range ids clamp (sets are never empty)
  }
  const ActionSet* actions_;
  std::vector<std::vector<Binding>> binds_;
  AnalogSettings analog_{};
};

/// Result of loading the bindings file.
struct BindingsFile {
  enum class Status { Loaded, Created, Invalid };
  ActionMap map;
  Status status = Status::Loaded;
  std::string message{};              ///< error / creation note for the log
  std::vector<std::string> warnings{};  ///< skipped entries
};

/// Loads `path` over `defaults`. A missing file is created from the defaults; an unreadable or
/// invalid file is left untouched and the defaults are used (status Invalid).
BindingsFile loadOrCreateBindings(const std::filesystem::path& path, const ActionMap& defaults);
/// Writes `map` to `path` (creating the directory). False + `err` on failure.
bool saveBindings(const ActionMap& map, const std::filesystem::path& path, std::string* err);

}  // namespace litholattice
