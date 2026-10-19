// Mod UI confinement + mod-content badging (mod hardening item 5, 2026-10-11).
//
// Mods build UI ONLY through the engine widget system, and ONLY through the
// confined factory below. The rules, enforced in code (not convention):
//
//   1. No raw draw calls: the mod API surface (LithoLatticeModApi in
//      litholattice/mods/mod_interface.h) hands mods no Painter, no
//      IUiRenderer, no Widget*, and no paint hook. The only UI entry point
//      is registerUiPanel(mod, id, title) — strings in, void out — which the
//      engine binds to ModUiFactory::createPanel after checking kPermUI.
//      Mod-supplied layout is JSON text, built by the widget factory's closed
//      type table (no dialog/modal/popup types exist there at all). Widget
//      event handlers from mod JSON are script functions only (Widget::on,
//      the C++-callable overload, is never wired by the factory). Audited in
//      mods_selftest.cpp (testModApiUiInputAudit).
//   2. Badging: every mod panel root is a ModPanel (final). Its badge strip —
//      the amber MOD tag plus the owning mod's id — is painted by the engine
//      with FIXED colours (not theme-dependent, so no pack or mod can hide
//      it), the panel arranges children strictly below the strip, and the
//      strip is painted AFTER the children: a mod child can never cover,
//      remove, restyle, or opt out of the badge. Mods never receive a
//      ModPanel*, so there is nothing to downcast or override.
//   3. No spoofing the system look: the confined factory rejects the reserved
//      style classes "dialog" (system modal look), "tooltip" (system hint
//      look), and the engine-applied "mod_panel"/"mod_badge". Mod panels are
//      attached to the UiRoot main layer only — never as modals or popups —
//      so a mod cannot block the screen with a fake system prompt.
//   4. DoS guards: mod JSON is pre-scanned (iterative, no recursion) for
//      nesting depth and node count before the recursive factory runs; the
//      per-panel widget budget is capped.
//
// THREAT MODEL (fake system prompt / credential phishing): a mod wants the
// player to believe a system dialog is asking for a password, a key, or a
// confirmation. Defences: (a) every mod-owned pixel lives under a ModPanel
// whose MOD badge is always visible and always names the mod — attribution
// is inescapable; (b) mods cannot use the system dialog/tooltip frame
// treatment; (c) mods cannot open modals, so they cannot seize the screen;
// (d) keystrokes typed while a system modal/text field has focus never reach
// mods at all (see input/mod_input.h: ModInputRouter::shouldDeliverToMod) —
// even the REAL password dialog is unreadable to mods. Honest scope, as with
// the net confinement: for native (C++) mods the API surface is the real
// control — in-process code could in principle call engine internals it links
// against, and only OS sandboxing (future work, builder note) changes that.
// What this layer guarantees: the engine never builds mod UI except through
// the confined factory, and everything it builds is badged.
#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "litholattice/ui/controls.h"
#include "litholattice/ui/widget_factory.h"

namespace litholattice::ui {

class UiRoot;

/// Style class the engine applies to every mod panel root: the distinct
/// frame treatment for mod content (amber-tinted border in the built-in
/// theme; packs may recolour it, mods cannot — mods ship no theme packs).
/// Reserved: the confined factory rejects it in mod JSON.
inline constexpr const char* kModPanelStyleClass = "mod_panel";

/// Style classes a mod may never select in its JSON. "dialog" is the system
/// modal look, "tooltip" the system floating-hint look; "mod_panel" and
/// "mod_badge" are engine-applied and only ever set by the engine itself.
inline constexpr const char* kModReservedStyles[] = {
    "dialog", "tooltip", "mod_panel", "mod_badge",
};

/// Hard limits on mod-supplied layout JSON (DoS guard; the factory itself
/// recurses, so depth is pre-scanned iteratively before it runs).
inline constexpr size_t kModUiMaxJsonDepth = 32;
inline constexpr size_t kModUiMaxJsonNodes = 1024;
/// Max widgets in one mod panel (factory output, counted post-build).
/// 2^8: comfortably above any legitimate config panel, and reachable under
/// the JSON node budget above (which would otherwise make this cap dead).
inline constexpr size_t kModUiMaxWidgets = 256;
/// Max title length (registerUiPanel title).
inline constexpr size_t kModUiMaxTitleLen = 128;

/// A Panel that is always visibly badged as mod content. `final`: neither a
/// subclass nor a mod can override or skip the badge painting. The badge
/// strip is engine-drawn with fixed colours, the panel arranges its children
/// strictly below the strip, and the strip paints after the children — a mod
/// child can never cover it. Mods never receive a ModPanel*.
class ModPanel final : public Panel {
 public:
  ModPanel(std::string modId, std::string title);

  const std::string& modId() const { return modId_; }
  const std::string& title() const { return title_; }

  /// The mod's content goes here (filled by the confined factory from JSON).
  StackPanel& body() { return *body_; }
  const StackPanel& body() const { return *body_; }

  /// Height of the engine badge strip, in pixels at the current theme scale.
  float badgeHeight() const;
  /// The badge strip rect in panel-local coordinates (valid after place()).
  Rect badgeRect() const;

 protected:
  void arrange(const Theme& theme) override;
  void paint(Painter& p) const override;

 private:
  void paintBadge(Painter& p) const;

  std::string modId_, title_;
  StackPanel* body_ = nullptr;
};

/// The confined widget factory for mods: the ONLY sanctioned path for mod
/// UI. The engine binds LithoLatticeModApi::registerUiPanel here (after
/// checking kPermUI); script layouts for mod panels go through here too.
class ModUiFactory {
 public:
  ModUiFactory() = delete;

  /// Builds a badged ModPanel for `modId` (must be [a-z0-9_]{1,64}, the
  /// extension-registry rule). `panelId` becomes the widget id; `title` is
  /// shown in the badge strip (capped at kModUiMaxTitleLen). `bodyJson` may
  /// be empty (empty body) or a widget-factory JSON object describing the
  /// panel's content; it is pre-scanned for depth/node count, built by the
  /// closed factory type table, and rejected when it names a reserved style
  /// class. Returns nullptr + `err` on any violation. The caller (the game)
  /// owns the panel; the mod never sees the pointer.
  static std::unique_ptr<ModPanel> createPanel(const std::string& modId,
                                              const std::string& panelId,
                                              const std::string& title,
                                              const std::string& bodyJson, std::string& err);

  /// True when `styleClass` is reserved (rejected in mod JSON).
  static bool isReservedStyle(std::string_view styleClass);

  /// Validates a mod/panel id: [a-z0-9_]{1,64}.
  static bool isValidModId(std::string_view id);

  /// Attaches a built panel to the UiRoot MAIN layer (never as a modal or
  /// popup: mods cannot open modals/popups, so a fake system prompt can
  /// never seize the screen). Returns the attached panel.
  static ModPanel& attachToRoot(UiRoot& ui, std::unique_ptr<ModPanel> panel);
};

}  // namespace litholattice::ui
