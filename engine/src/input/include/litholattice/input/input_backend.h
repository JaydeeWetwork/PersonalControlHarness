// Input backends: the only code that talks to an OS / library input API. The GLFW backend lives
// in the platform module; XInput, SDL or console backends would implement the same interface.
#pragma once
#include <string_view>
#include <vector>
#include "litholattice/input/input_event.h"

namespace litholattice {

class IInputBackend {
 public:
  virtual ~IInputBackend() = default;
  /// Backend name for the log ("glfw", "replay", ...).
  virtual std::string_view name() const = 0;
  /// Main thread, once per frame: pumps the OS queue and appends every event since the last
  /// call to `out`, oldest first.
  virtual void poll(std::vector<InputEvent>& out) = 0;
};

/// Plays back a recorded (or scripted) event stream: poll() returns the events with
/// time <= the current replay time. Used by the self-test and for reproducing bugs.
class ReplayInputBackend final : public IInputBackend {
 public:
  ReplayInputBackend() = default;
  explicit ReplayInputBackend(std::vector<InputEvent> events) : events_(std::move(events)) {}

  std::string_view name() const override { return "replay"; }
  void poll(std::vector<InputEvent>& out) override;

  /// Appends an event to the stream (scripted input).
  void push(const InputEvent& e) { events_.push_back(e); }
  /// Moves the replay clock; the next poll() delivers everything up to `time`.
  void advanceTo(double time) { now_ = time; }
  bool finished() const { return next_ >= events_.size(); }

 private:
  std::vector<InputEvent> events_;
  size_t next_ = 0;
  double now_ = 0.0;
};

}  // namespace litholattice
