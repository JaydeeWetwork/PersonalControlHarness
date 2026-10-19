// Mod input confinement (mod hardening item 6, 2026-10-11).
//
// Mods receive GAME input events only — the action/event layer — never raw
// OS keystrokes or device state. The rules, enforced in code:
//
//   1. No raw device handles: the mod API surface (LithoLatticeModApi in
//      litholattice/mods/mod_interface.h) exposes no InputSystem*, no
//      IInputBackend*, no DeviceState*, no key/mouse/gamepad state query at
//      all. The only input-related entry points are registerInputAction
//      (strings in, void out) and the engine->mod onInputEvent hook.
//      Audited in mods_selftest.cpp (testModApiUiInputAudit).
//   2. One-directional flow: this router is the engine-side record of mod
//      input actions and the delivery-policy check. It holds no InputPump*,
//      no InputSystem*, no IInputBackend*, no DeviceState* — there is no
//      method here that can place an event into the engine's input pipeline,
//      by construction. Engine -> mod is a call the GAME makes; mod -> engine
//      input does not exist.
//   3. Delivery policy: a key event reaches a mod's onInputEvent only when
//      the UI does NOT want the keyboard (UiRoot::wantsKeyboard: no modal
//      open, no popup open, no text field focused). Keystrokes typed into a
//      system dialog — passwords included — never reach mods.
//   4. No text exfiltration by construction: the onInputEvent hook signature
//      is (mod, key, down) — no Text-event codepoints, no device ids. A mod
//      cannot keylog typed text and cannot tell which device produced a key.
//
// ANTI-CHEAT: mods must not be able to synthesize fake player input. The
// engine's input pipeline has exactly two event-injection points, both
// engine-internal and both unreachable from mod code:
//   a. ReplayInputBackend::push() — the replay/testing seam. The backend
//      object is owned by InputPump (private unique_ptr); the mod API
//      surface exposes no path to it.
//   b. InputSystem::processFrame() — replay/tests. Mods never receive an
//      InputSystem*.
// The script binding allowlist (Worker A) additionally contains no
// input-injection binding, so script mods cannot reach these either.
// Honest scope, as with the net confinement: for native (C++) mods the API
// surface is the real control — in-process code could in principle call any
// engine symbol it links against, and only OS sandboxing (future work,
// builder note) changes that. What this layer guarantees: the engine never
// hands mod code a handle to the input pipeline, the API struct has no
// injection entry (audit-pinned), and this router — the only engine->mod
// input path — cannot inject by construction.
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace litholattice {

/// Engine-side registry of mod input actions + the delivery-policy check.
/// Owned by the game (which also owns the InputSystem); the mod API's
/// registerInputAction binds here AFTER the engine checks kPermInput.
class ModInputRouter {
 public:
  struct ModAction {
    std::string modId;        ///< owning mod
    std::string actionId;     ///< namespaced: "modid:action"
    std::string displayName;  ///< player-facing name (remapping UI)
  };

  /// Registers one mod input action. False + `err` on an invalid id or a
  /// duplicate. Ids are [a-z0-9_]{1,64} (the extension-registry rule); the
  /// stored id is namespaced "modid:action" so a mod can never shadow or
  /// collide with an engine action or another mod's action.
  bool registerAction(const std::string& modId, const std::string& actionId,
                      const std::string& displayName, std::string& err);

  /// Drops every action of `modId` (mod unload). Returns the count removed.
  size_t unregisterMod(const std::string& modId);

  /// All registered actions, in registration order (for the remapping UI).
  std::vector<ModAction> actions() const;
  bool hasAction(const std::string& namespacedId) const;
  size_t actionCount() const { return order_.size(); }

  /// Delivery policy: the game calls this before forwarding a key event to a
  /// mod's onInputEvent hook. False when the event must NOT reach the mod —
  /// i.e. when `uiWantsKeyboard` (UiRoot::wantsKeyboard(): a system modal is
  /// open, a popup is open, or a text field has focus). Keystrokes typed into
  /// system dialogs never reach mods.
  static bool shouldDeliverToMod(bool uiWantsKeyboard) { return !uiWantsKeyboard; }

  // NOTE (anti-cheat): there is deliberately no inject/push/send method on
  // this class. The only engine->mod input path is the game's explicit call
  // of the mod's onInputEvent hook; this router keeps the books and the
  // policy, and it cannot place events into the engine pipeline — it holds
  // no InputPump*, InputSystem*, IInputBackend*, or DeviceState*.

 private:
  static bool isValidId(std::string_view id);

  std::map<std::string, ModAction> actions_;  // namespaced id -> action
  std::vector<std::string> order_;            // registration order
};

}  // namespace litholattice
