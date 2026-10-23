// lithoscript: the console scripting language (Quake / Half-Life 1 lineage).
// Console COMMANDS, typed CVARS (get/set, archived to a config file), `alias`
// (named command sequences), `exec` (runs .lth script files), `;`-separated
// command sequences per line, quoted/escaped argument tokenizing.
//
// THREE EXPOSURES (by design, one registry serves all three):
//   1. ENGINE: registerCommand / registerCVar / unregister* — the clean C++
//      registration API. The engine owns the ConsoleRegistry; mods register
//      through it only when granted kPermConsole (namespaced "modid:...";
//      the ModLoader wiring is builder follow-up).
//   2. USER: typed in the debug console, plus .lth lithoscript files players
//      write in the config dir (exec is sandboxed there; saveArchived writes
//      there too; .cfg files still execute as a legacy alias).
//   3. ANGELSCRIPT: consoleCmd / consoleCvar / consoleSetCvar bindings
//      (console/script_console.h), allowlist-gated on kPermConsole.
//
// SCRIPT TRUST MODEL: a .lth file carries no authority of its own — it runs
// with the permissions of the invoking context. `exec` typed by the player in
// the debug console runs as the player; `exec` (or any console line) issued
// through a mod's consoleCmd AngelScript binding runs with that mod's granted
// permissions, and the binding itself requires kPermConsole. There is no
// command that grants permissions, so a script can never widen its own
// authority. Deny-by-default everywhere: no kPermConsole, no console access.
//
// TRUST TIERS (JD-locked 2026-10-11):
//   UNTRUSTED (deny-by-default): mod AngelScript without kPermConsole gets no
//     console bindings at all.
//   SEMI-TRUSTED: Fragment-authored scripts — written by the player, the
//     proxy, or an agent and driven into the game through the ReadOR/keyboard
//     pipeline. Rationale (JD): these scripts are collaborative content that
//     players, proxies, and agents construct together — not hostile input.
//     What semi-trust GRANTS: the script may drive the console and the input
//     pipeline with the invoker's context permissions (cheats only where the
//     context allows them). What it does NOT grant: the hardening layer stays
//     FULLY ARMED — step budget, total-wait budget, wait clamp, alias/exec
//     caps, cooperative wait, fail-closed termination all apply unchanged.
//     Every hardening intervention prints to the console so the player AND the
//     agent can see what happened (exact strings below, stable for parsing).
//   TRUSTED: the player typing in the debug console; engine-internal calls.
// A semi-trusted script runs with the invoker's permissions and can never
// grant itself more: there is no command that widens authority.
//
// Hardening intervention messages (exact, stable for agent parsing):
//   "lithoscript: step budget exceeded (16384 commands); script terminated"
//   "lithoscript: total wait budget exceeded (4096 ticks); script terminated"
//   "lithoscript: too many suspended scripts (64); script terminated"
//   "wait: clamped to 512 ticks (1 second of game time at 512 TPS)"
//   "alias recursion too deep (expanding '<name>')"
//   "exec: refused '<file>': script nesting too deep (limit 8; possible cycle)"
//   "command '<name>' requires cheats (not allowed in this context)"
//   "command '<name>' requires session authority (not allowed in this context)"
//   "cvar '<name>' is read-only"
//   "cvar '<name>' requires cheats (not allowed in this context)"
//
// `wait N` — tick-based pacing (JD-locked 2026-10-11, reversing the v1
// omission): suspends the rest of the current script for N WORLD ticks and
// resumes it cooperatively. Tick-based and deterministic — never wall-clock:
// the game pumps tick() once per sim world tick, so `wait 512` is exactly
// one second of game time at 512 TPS, on any hardware, at any frame rate.
// While the sim is paused the world-tick counter does not advance, so waits
// freeze too (coherent: waits measure game time, not real time).
//
// COOPERATIVE, NEVER BLOCKING: `wait` does not sleep, spin, or block any
// thread. It captures the remaining commands as a continuation, queues it,
// and returns immediately; tick() resumes it later. The engine thread is
// never held — other commands run during a wait.
//
// HARDENING (why no script can hang or crash the engine):
//   * kMaxWaitTicks (512): one wait suspends at most 512 ticks. Larger
//     values are CLAMPED to 512 with a printed note (graceful, not an
//     error — JD's call). `wait 999999999` waits 512 ticks, not forever.
//   * kMaxTotalWaitTicks (4096): total suspended ticks per top-level
//     invocation, accumulated across every wait AND every suspension. A
//     wait-ping-pong (`alias a "wait 1; b"` / `alias b "wait 1; a"`) burns
//     >=1 tick per cycle and is terminated fail-closed at the cap.
//   * kMaxScriptSteps (16384): total commands dispatched per invocation
//     (alias expansions and exec'd lines included). Bounds per-tick CPU no
//     matter how clever the script.
//   * kMaxSuspendedScripts (64): the suspended queue itself is capped —
//     tick() scans it every tick, so the scan stays cheap. Machine fleets
//     should drive their own registry instance (see the ReadOR seam).
//   * The alias recursion cap (16) and exec depth cap (8) are preserved
//     ACROSS suspensions: a continuation resumes at the alias depth where
//     its wait fired, so `alias a "wait 1; a"` trips the alias cap after 16
//     cycles and wait-laced exec cycles trip the total-wait cap.
//   * Budget exceeded -> the script is terminated with a printed message;
//     the engine is unaffected (fail-closed). Termination cannot be caught
//     or suppressed from inside the language.
// `wait 0` is a documented no-op (no suspension, still counts as a step);
// bare `wait` waits 1 tick (HL1 muscle memory). Negative or non-numeric
// arguments are usage errors. There is deliberately no `delay`/`sleep` —
// one pacing command, hardened, is enough.
//
// FUTURE SEAM — Fragment journal / typewriter items / keyboard (design only,
// not built): this registry is drivable headlessly on purpose. The single
// entry point is executeLine(); output goes to the installed OutputFn sink;
// cvars are read/written via findCVar/setCVar without any UI. A future
// Fragment integration therefore needs no console changes: it constructs (or
// shares) a ConsoleRegistry, installs its own sink (journal page instead of
// the debug console's TextEditor), and feeds it lines typed on the typewriter
// item or tapped on the keyboard item. If the Fragment wants its own command
// namespace, it registers commands on its own registry instance. Suggested
// follow-up: an IConsoleDriver { submitLine(string_view), setSink(...) }
// interface so the debug console, the Fragment, and the typewriter are three
// drivers over one registry — do not add item code here.
//
// FUTURE SEAM — Fragment → lithoscript → world, via the ReadOR (design
// only, not built). JD's ReadOR (new -OR component, fits the family): the
// player slots their Fragment into a ReadOR, which reads the Fragment's
// lithoscript scripts and sends them to a KEYBOARD component; the keyboard
// feeds them into larger contraptions built by the user, the proxy, or the
// agent. This is the physical mechanism for the Fragment → lithoscript →
// world pipeline. The full design note (ReadOR-Design-Note-2026-10-11) is
// kept outside this repository.
//
// What the engine already provides for it (no changes needed): the single
// executeLine() entry point, the swappable OutputFn sink, and the cvar
// read/write API — the ReadOR/keyboard drive the same seams as the debug
// console, on their own registry instance (or the shared one), with no new
// console code. The Fragment becomes a script cartridge: write scripts in
// the book, slot it into a ReadOR, machines run them. JD's extension — the
// SERIALIZATION LINE: the ReadOR also serializes the Fragment's scripts and
// data directly out into the world, written into player-built memory chips
// (DRAM, SDRAM, hard drives). So the Fragment is both script cartridge AND
// data exporter: write in the book, slot it into a ReadOR, serialize out
// into your hardware. Same Fragment → world pipeline, extended to data —
// which means the ReadOR needs a defined serialization format for Fragment
// pages (script pages vs data pages vs the unlock log), and the memory-chip
// side needs a defined write interface; both are builder design work.
// Scripts the Fragment carries live inside the Fragment item's data (a
// per-player save section, backed up with the world); export/import to .lth
// files in the config dir is the bridge to `exec` and to sharing with
// other players.
//
// OPEN DESIGN QUESTIONS (for the builder — deliberately unanswered here):
//   1. The keyboard as a script-input component: how does it accept
//      lithoscript and route it into machine logic? (E.g. typed lines fed
//      to a registry; a per-machine command surface; something else.)
//   2. Permission/trust model for agent- and proxy-authored scripts: three
//      authors feed the pipeline (user hand-written, proxy, agent). Who can
//      write what, where do agent/proxy scripts live (Fragment pages? a
//      machine-local store?), and how are they attributed? Standing rule to
//      honor: agent scripts execute with the AGENT's permissions, never the
//      player's — an agent cannot grant itself kPermConsole or any other
//      permission through the Fragment/ReadOR (see SCRIPT TRUST MODEL
//      above); the registry instance an agent drives is constructed with
//      its granted permission set, deny-by-default, the same as mod
//      bindings today.
//   3. Multiplayer authority: when a script is read from a slotted
//      Fragment, whose context does it run under — the Fragment owner's,
//      the ReadOR placer's, or the reader's? A slotted Fragment must not
//      become a privilege-laundering channel (one player slotting another
//      player's Fragment to borrow their permissions).
// The remaining work is all item/UI/save-format design (ReadOR + keyboard
// components, Fragment page types) — not console-module code.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "litholattice/input/device_state.h"
#include "litholattice/console/iworldquery.h"

namespace litholattice::defrag {
class DefragService;  // engine defrag service (owned by the game, must outlive the registry)
}  // namespace litholattice::defrag

namespace litholattice::console {

/// Cvar value types. Stored as strings; coerced on set/get.
enum class CVarType : uint8_t { String, Int, Float, Bool };

/// Caller authority sentinel for setCallerPermissions(): "no restricted mask
/// installed" — the registry behaves exactly as before this seam existed
/// (cheat-gated commands/cvars run when cheatsAllowed() is true).
inline constexpr uint32_t kCallerFullAuthority = 0xFFFFFFFFu;

/// Per-command flags (Decision 8 extends to commands: give/kill are
/// cheat-gated, save/map are not).
struct CommandFlags {
  bool cheat = false;
  /// Session tier (P1b, 2026-10-13, Appendix F M6): session/destructive
  /// commands (quit, save, map, debug.defrag) that are NOT cheat-flagged —
  /// the local player uses them in ordinary play — but must still be
  /// unreachable by restricted callers (MCP agents, mods). A session
  /// command runs only under kCallerFullAuthority; installing any
  /// restricted caller mask refuses it, exactly like the cheat gate but
  /// independent of cheatsAllowed(). A full-authority caller is unaffected
  /// whether or not cheats are enabled.
  bool session = false;
};

class ConsoleRegistry;

/// Called synchronously inside setCVar after a successful set (covers
/// `name value` lines, `toggle`/`incrementvar`/`++`/`--`, and archived
/// loads). Keep it short and non-recursive: setting a cvar from inside a
/// callback re-enters setCVar, so guard if you do (the game's settings
/// bridge uses a syncing flag for exactly this).
using CVarChangeFn = std::function<void(ConsoleRegistry&, const struct CVar&)>;

struct CVar {
  std::string name;
  CVarType type = CVarType::String;
  std::string value;         ///< current value, always in canonical form
  std::string defaultValue;  ///< canonical form of the default
  bool archived = false;     ///< written by saveArchived; read back at boot via executeFile
  std::string help;
  CVarChangeFn onChange;     ///< optional: fired after every successful set
  // Per-cvar flags (Decision 8, JD-locked 2026-10-11): NOT HL1's single
  // sv_cheats sledgehammer — each cvar declares its own policy.
  bool readOnly = false;   ///< set refused (engine-internal telemetry, version strings)
  bool replicated = false; ///< server->client replication candidate (netcode consumes later)
  bool cheat = false;      ///< set requires cheatsAllowed on the registry (per-cvar authority)
};

/// Cvar flag set for registerCVar (Decision 8).
struct CVarFlags {
  bool readOnly = false;
  bool replicated = false;
  bool cheat = false;
};

class ConsoleRegistry;
/// argv[0] is the command name. Errors are reported through print().
using ConsoleArgs = std::vector<std::string>;
using ConsoleCommandFn = std::function<void(ConsoleRegistry&, const ConsoleArgs&)>;

/// Quake/HL1-style console registry + executor. Not thread-safe by itself:
/// the game drives it on the main thread; script bindings call into it on the
/// script thread. The game serializes those (scripts run on the main thread
/// in v1), so no internal locking — document, don't add.
class ConsoleRegistry {
 public:
  /// Where printed lines go (the debug console's log view, a test capture,
  /// a future Fragment journal page). Defaults to a no-op.
  using OutputFn = std::function<void(const std::string& line)>;
  void setOutput(OutputFn fn) { output_ = std::move(fn); }
  void print(const std::string& line) { if (output_) output_(line); }

  /// Alias recursion guard (HL1-style infinite-alias protection).
  static constexpr int kMaxAliasDepth = 16;
  /// Exec-chain nesting guard: a .lth file can `exec` another, which can
  /// `exec` back (a.lth -> b.lth -> a.lth). Nesting at or past this depth is
  /// refused, fail-closed, with a printed error. Mirrors the IExtensible
  /// depth cap (8); see executeFile().
  static constexpr int kMaxExecDepth = 8;
  /// `wait` caps (JD-locked 2026-10-11): a single `wait` suspends at most
  /// 512 ticks = 1 second of game time at 512 TPS. A named constant, not a
  /// magic number: raise it as hardware improves (scalable-fidelity
  /// philosophy — what is 1 second of pacing today stays 1 second of pacing
  /// as tick rates climb); the clamp keeps old scripts valid. Larger values
  /// clamp to this with a printed note instead of erroring.
  static constexpr int kMaxWaitTicks = 512;
  /// Total suspended ticks one top-level script invocation may accumulate
  /// across all of its waits (8 max-waits = 8 seconds of game time). Past
  /// this the invocation is terminated fail-closed: no wait-ping-pong may
  /// run forever.
  static constexpr int kMaxTotalWaitTicks = 4096;
  /// Total commands dispatched by one top-level invocation (alias
  /// expansions and exec'd lines included). Bounds per-tick CPU no matter
  /// how clever the script; past this the invocation is terminated
  /// fail-closed. Sized at 4x kMaxTotalWaitTicks: a wait-cycle costs at
  /// least 2 steps (the wait plus the resuming command), so wait-heavy
  /// scripts always hit the wait budget first — the step budget is the
  /// backstop for pure CPU spin, not the primary wait-loop killer.
  static constexpr int kMaxScriptSteps = 16384;
  /// Suspended-script queue bound: tick() scans the whole queue every tick,
  /// so the queue itself is capped (machine fleets should drive their own
  /// registry instance — see the ReadOR seam note above).
  static constexpr int kMaxSuspendedScripts = 64;

  // ---- ENGINE registration API ----
  /// Registers a command. False + `err` on an invalid name or a duplicate.
  /// Names: [a-z0-9_:.]{1,64}, lowercase (a cvar or alias with the same name
  /// is also a collision).
  bool registerCommand(std::string name, std::string help, ConsoleCommandFn fn, std::string* err = nullptr);
  /// Flagged overload: cheat-gated commands refuse to run unless
  /// setCheatsAllowed(true) was called on this registry.
  bool registerCommand(std::string name, std::string help, ConsoleCommandFn fn,
                       CommandFlags flags, std::string* err = nullptr);
  bool unregisterCommand(const std::string& name);
  bool hasCommand(const std::string& name) const;
  /// Registers a cvar. `defaultValue` is coerced to canonical form; false +
  /// `err` when it does not coerce or the name is taken/invalid.
  bool registerCVar(std::string name, CVarType type, std::string defaultValue, bool archived,
                    std::string help, std::string* err = nullptr);
  /// registerCVar with per-cvar flags (Decision 8).
  bool registerCVar(std::string name, CVarType type, std::string defaultValue, bool archived,
                    std::string help, CVarFlags flags, std::string* err = nullptr);
  bool unregisterCVar(const std::string& name);
  const CVar* findCVar(const std::string& name) const;
  /// Typed set. Returns false + `err` on unknown cvar, a value that does
  /// not coerce to the cvar's type (the old value is kept), a read-only
  /// cvar, or a cheat cvar when cheats are not allowed in this context.
  bool setCVar(const std::string& name, const std::string& rawValue, std::string* err = nullptr);
  /// Cheat authority for THIS registry instance (per-context, not global):
  /// the game enables it for the local single-player console; a server
  /// enables it only where granted. Default false (fail-closed, deny by
  /// default — same philosophy as the mod permissions).
  void setCheatsAllowed(bool allowed) { cheatsAllowed_ = allowed; }
  bool cheatsAllowed() const { return cheatsAllowed_; }
  /// Caller permission mask for agent-driven execution (MCP
  /// lithoscript_exec, Phase 4, 2026-10-11) — the minimal seam that makes
  /// "agent scripts execute with the AGENT's permissions, never the
  /// player's" enforceable. Default kCallerFullAuthority: everything behaves
  /// exactly as before (cheat-gated commands/cvars run when cheatsAllowed()
  /// is true). Installing any OTHER mask revokes cheat authority for
  /// cheat-gated commands/cvars — fail-closed: a restricted mask can never
  /// widen authority, it can only narrow it. The MCP server's IConsoleExec
  /// adapter installs the agent's mask for the duration of one executeLine
  /// and restores it afterwards. The mask is also stamped onto suspended
  /// scripts (see SuspendedScript::callerMask) so a `wait`-suspended line
  /// resumes under the same authority it started with.
  void setCallerPermissions(uint32_t mask) { callerPermissions_ = mask; }
  uint32_t callerPermissions() const { return callerPermissions_; }
  /// Installs the world-query backend (game-owned, must outlive the registry).
  /// The `query` command is part of the complete language; without a backend
  /// it reports "no world backend registered".
  void setWorldQuery(IWorldQuery* q) { worldQuery_ = q; }
  IWorldQuery* worldQuery() const { return worldQuery_; }
  /// Installs the engine defrag service (game-owned, must outlive the
  /// registry). Powers `debug.defrag` and is pumped once per tick() so a
  /// long defrag never stalls the sim. Null = the command reports unregistered.
  void setDefragService(defrag::DefragService* d) { defragService_ = d; }
  defrag::DefragService* defragService() const { return defragService_; }
  /// Installs/replaces the onChange hook for a cvar. False when unknown.
  bool setCVarChangeFn(const std::string& name, CVarChangeFn fn);
  std::vector<std::string> commandNames() const;
  std::vector<const CVar*> cvars() const;
  /// Help text of a command ("" when unknown). Added for the completer.
  std::string commandHelp(const std::string& name) const;

  // ---- aliases ----
  /// Defines `name` as the command sequence `body` ("cmd1; cmd2"). False +
  /// `err` on invalid name or collision with a command/cvar.
  /// ANTI-LAUNDERING (P1b, 2026-10-13, Appendix F H1 — mirrors the KeyBind
  /// fix): the defining caller's permission mask is stamped onto the
  /// alias, and alias expansion re-installs the DEFINER's mask (composed
  /// with the invoker's mask, see below) for the duration of the body, so
  /// an alias defined by a restricted caller (MCP agent, mod) can never
  /// execute at a later invoker's wider authority.
  /// MASK COMPOSITION RULE: when an alias body invokes further aliases or
  /// keybind-dispatched commands, the effective mask is the MOST
  /// RESTRICTIVE applicable one — the bitwise AND of the permission bits
  /// (kCallerFullAuthority is the identity element: AND with it yields the
  /// other mask). Authority can only narrow as expansion nests, never
  /// widen. The composed mask is also what a `wait` inside the body
  /// captures, so a suspended continuation resumes under the composed
  /// (definer-bounded) authority, not the invoker's.
  bool setAlias(std::string name, std::string body, std::string* err = nullptr);
  bool removeAlias(const std::string& name);
  bool hasAlias(const std::string& name) const;
  std::vector<std::string> aliasNames() const;
  /// Body of an alias ("" when unknown). Added for the completer.
  std::string aliasBody(const std::string& name) const;

  // ---- key binds: console owns the table, the input pump dispatches ----
  /// Records keyName -> command. `keyName` is validated with keyFromName
  /// (e.g. "F5", "GraveAccent"); false + `err` when unknown. The binder's
  /// callerPermissions() mask is stamped onto the entry: dispatchKeyBinds
  /// re-installs it for the call, so a bind planted by a restricted context
  /// (an MCP agent, a mod script) can never fire under wider authority than
  /// the binder held — fail-closed against permission laundering across
  /// time. Creation and overwrite are printed through the output sink.
  /// Dispatch is live: the game calls dispatchKeyBinds() once per frame,
  /// which runs the bound command for every pressed bound key.
  bool setKeyBind(std::string keyName, std::string command, std::string* err = nullptr);
  bool removeKeyBind(const std::string& keyName);
  void clearKeyBinds() { keyBinds_.clear(); }
  std::map<std::string, std::string> keyBinds() const;

  // ---- key-bind dispatch ----
  /// The console owns the binding table; the input pump (game) dispatches.
  /// Call once per frame after the input system updated: every bound key
  /// pressed this frame (edge-triggered, no key repeat) runs its command
  /// string through the executor. `textCapture` must be true while a text
  /// field/modal owns the keyboard (same rule as the tilde key): binds
  /// never fire then. Aliases, `;` sequences and `wait` inside bound
  /// commands behave exactly as typed (`wait` suspends cooperatively under
  /// the usual budgets).
  void dispatchKeyBinds(const DeviceState& devices, bool textCapture);

  // ---- execution ----
  /// Tokenizes, `;`-splits, alias-expands (recursion-capped) and dispatches
  /// one line. Errors (unknown command, bad cvar value, alias recursion)
  /// are printed through the output sink, never thrown. A line containing
  /// `wait` suspends cooperatively (see tick()): the remainder is queued,
  /// not run.
  void executeLine(std::string_view line);
  /// Advances script time by one WORLD tick: suspended `wait`
  /// continuations whose counters reach zero resume here, in FIFO order.
  /// The game calls this once per sim world tick — never per frame, never
  /// on a timer: script time is deterministic, not wall-clock. Cheap: one
  /// scan of the suspended queue. Re-entrant calls are ignored (a resumed
  /// command must not pump the queue it came from).
  void tick();
  /// Number of scripts currently suspended on `wait` (tests, debug output).
  size_t suspendedCount() const { return suspended_.size(); }
  /// `exec` target root: .lth script files live under here and ONLY here.
  void setConfigDir(std::filesystem::path dir) { configDir_ = std::move(dir); }
  const std::filesystem::path& configDir() const { return configDir_; }
  /// Runs the lithoscript file `<name>` from the config dir. Refuses absolute
  /// paths, "..", and names not ending in .lth (false + printed reason).
  /// `.cfg` names still execute as a legacy alias. `//` line comments are
  /// stripped per line; `/* ... */` block comments are stripped across the
  /// whole file first, so they may span lines. An unterminated block
  /// comment refuses the whole file fail-closed (prints the reason).
  /// Nesting: an executed file may itself `exec` another, but past
  /// kMaxExecDepth the inner exec is refused fail-closed (prints the reason)
  /// — a.lth -> b.lth -> a.lth cycles always terminate. Trust model: the file
  /// runs with the invoking context's permissions (see header comment).
  bool executeFile(std::string_view name);
  /// Writes archived cvars as `name "value"` lines (atomic temp+rename).
  /// Sandboxed like exec: the target must resolve under the config dir
  /// (setConfigDir); refuses fail-closed otherwise. Archived cvars boot back
  /// through the sandboxed executeFile path (config.lth) — there is no
  /// unsandboxed read twin.
  bool saveArchived(const std::filesystem::path& path, std::string* err = nullptr) const;

  /// Built-in commands: help, echo, alias, unalias, exec, wait, toggle,
  /// incrementvar, find, cvarlist, cmdlist, bind, unbind, unbindall, version.
  /// `debugBuild` adds the extra verbose debug-build-only commands
  /// (dbg_logtap, dbg_registry). (toggleconsole and clear are registered by
  /// DebugConsole::attach, not here.)
  void registerBuiltins(bool debugBuild);

  /// Tokenizes one line into `;`-separated commands (quotes, backslash
  /// escapes, `//` comments and `/* ... */` block comments outside quotes).
  /// Exposed for tests. On an unterminated block comment this form returns
  /// an empty command list (it cannot report the error); use the
  /// two-argument form when the error matters.
  static std::vector<std::vector<std::string>> tokenize(std::string_view line);
  /// Same as tokenize(line), but a tokenizer error is reported: on an
  /// unterminated `/*` block comment, `err` (if non-null) is set to
  /// "unterminated block comment" and an empty command list is returned —
  /// the input is never silently truncated. `err` is set only on error,
  /// never cleared.
  static std::vector<std::vector<std::string>> tokenize(std::string_view line, std::string* err);
  /// Extracts comment bodies from source text in source order: a `//`
  /// comment yields the rest of its line (a trailing `\r` is stripped for
  /// CRLF tolerance); a `/* ... */` comment yields its verbatim inner
  /// text, which may span lines. Bodies come back WITHOUT the `//`, `/*`
  /// and `*/` markers. The same lexical rules as tokenize() apply: markers
  /// inside double-quoted strings or right after a backslash escape are
  /// literal text, not comments; `//` inside a `/* ... */` block is just
  /// comment text. On an unterminated `/*`, `err` (if non-null) is set to
  /// "unterminated block comment" and the comments extracted so far are
  /// returned — the failure is reported, nothing is silently dropped.
  /// `err` is set only on error, never cleared.
  static std::vector<std::string> extractComments(std::string_view text, std::string* err);

 private:
  struct Command {
    std::string help;
    ConsoleCommandFn fn;
    CommandFlags flags;  ///< cheat=true: needs cheatsAllowed() (Decision 8, commands too);
                         ///< session=true: needs kCallerFullAuthority (P1b tier)
  };
  /// An alias definition: the body plus the authority it was defined with.
  /// The definer's caller mask is stamped at setAlias and re-installed
  /// (composed with the current mask) at expansion — see setAlias().
  struct Alias {
    std::string body;
    uint32_t callerMask = kCallerFullAuthority;
  };
  /// Most-restrictive composition of two caller masks (see setAlias):
  /// kCallerFullAuthority is the identity; otherwise bitwise AND.
  static uint32_t composeCallerMasks(uint32_t a, uint32_t b) {
    if (a == kCallerFullAuthority) return b;
    if (b == kCallerFullAuthority) return a;
    return a & b;
  }
  /// Per-top-level-invocation execution budget: one executeLine, everything
  /// it spawns via alias/exec, and every suspension it accrues. Totals
  /// survive across suspensions (stored in the SuspendedScript, restored on
  /// resume), so a wait-ping-pong cannot reset its budget by suspending.
  struct ExecCtx {
    int stepsUsed = 0;    ///< commands dispatched so far
    int totalWaited = 0;  ///< ticks suspended so far, across all waits
  };
  enum class ExecResult { Ok, Suspended, Aborted };
  /// A script paused by `wait`: the remaining commands plus everything
  /// needed to resume it later, budgets intact.
  struct SuspendedScript {
    std::vector<std::vector<std::string>> cmds;
    int ticksLeft = 0;
    int aliasDepth = 0;  ///< depth where the wait fired (caps survive suspension)
    int stepsUsed = 0;
    int totalWaited = 0;
    uint32_t callerMask = kCallerFullAuthority;  ///< authority survives suspension too
  };
  /// Transient unwind state while a `wait` propagates out of nested
  /// executeCommandList frames. The innermost frame's remainder is captured
  /// first; each outer frame appends its own remainder after it.
  struct WaitUnwind {
    bool active = false;
    bool aborted = false;  ///< set when a wait trips a budget: fail-closed
    int ticks = 0;         ///< clamped wait length that fired
    int fireDepth = 0;     ///< alias depth at the wait command
    /// Authority in effect at the exact point the wait fired (already the
    /// composed definer/invoker mask when the wait sits inside alias
    /// expansions — P1b). queueSuspended stamps THIS onto the continuation
    /// when set, so alias-frame mask restores during unwinding cannot
    /// widen the resumed script's authority back to the invoker's.
    uint32_t callerMask = kCallerFullAuthority;
    bool hasCallerMask = false;
    std::vector<std::vector<std::string>> pending;
  };
  static bool isValidName(std::string_view name);
  ExecResult executeTokens(const std::vector<std::string>& argv, int aliasDepth, ExecCtx& ctx);
  ExecResult executeCommandList(const std::vector<std::vector<std::string>>& cmds, int aliasDepth,
                                ExecCtx& ctx);
  static bool coerce(CVarType type, std::string_view raw, std::string& canonical);
  /// The `wait` builtin implementation (needs the firing alias depth, so it
  /// is a method rather than a bare lambda).
  void cmdWait(const ConsoleArgs& argv);
  /// Adds `delta` to an int/float cvar (with optional [lo,hi] clamping).
  /// Powers `++`/`--` and `incrementvar`. Returns false + prints nothing
  /// when the cvar is unknown or not numeric (callers report).
  bool bumpCVar(const std::string& name, double delta, bool clamp, double lo, double hi);
  /// `<cvar>++` / `<cvar>--` dispatch (±1; int/float cvars only).
  ExecResult execIncDec(const std::string& cvarName, bool inc, const ConsoleArgs& argv);
  /// Enqueues a suspended continuation; fail-closed when the queue is full.
  void queueSuspended(SuspendedScript s);

  /// A key-bind entry: the command plus the authority it was planted with.
  /// The key is parsed once at setKeyBind (dispatch never re-parses the
  /// name); the binder's caller mask is stamped at setKeyBind and
  /// re-installed at dispatch (anti-laundering, fail-closed).
  struct KeyBind {
    std::string command;
    Key key = Key::Count;
    uint32_t callerMask = kCallerFullAuthority;
  };

  OutputFn output_;
  std::map<std::string, Command> commands_;
  std::map<std::string, CVar> cvars_;
  std::map<std::string, Alias> aliases_;
  std::map<std::string, KeyBind> keyBinds_;
  std::filesystem::path configDir_;
  bool cheatsAllowed_ = false;  ///< per-context cheat authority (Decision 8)
  /// Agent caller mask (Phase 4 seam): kCallerFullAuthority until someone
  /// installs a restricted mask. Cheat authority = cheatsAllowed_ AND an
  /// unrestricted mask — installing any restricted mask revokes cheats.
  uint32_t callerPermissions_ = kCallerFullAuthority;
  /// True when cheat-gated commands/cvars may run: the context switch AND a
  /// caller mask that has not been restricted. The fail-closed direction:
  /// a restricted mask can only take cheat authority away, never grant it.
  bool callerHasCheatAuthority() const {
    return cheatsAllowed_ && callerPermissions_ == kCallerFullAuthority;
  }
  /// Session-tier authority (P1b): session/destructive commands run only
  /// for the unrestricted local caller. Independent of cheatsAllowed() —
  /// quit/save/map are ordinary play for the local player, cheats or not,
  /// and unreachable for any restricted mask, cheats or not.
  bool callerHasSessionAuthority() const {
    return callerPermissions_ == kCallerFullAuthority;
  }
  IWorldQuery* worldQuery_ = nullptr;  ///< game backend for `query` (may be null)
  defrag::DefragService* defragService_ = nullptr;  ///< engine defrag service (may be null)
  int execDepth_ = 0;  ///< current executeFile nesting depth (0 = not inside one)
  ExecCtx* currentCtx_ = nullptr;      ///< innermost live invocation (used by exec)
  int currentAliasDepth_ = 0;          ///< alias depth of the executing frame (used by wait)
  WaitUnwind waitUnwind_;              ///< live only while a wait unwinds
  std::vector<SuspendedScript> suspended_;
  bool ticking_ = false;  ///< re-entrancy guard for tick()
};

}  // namespace litholattice::console
