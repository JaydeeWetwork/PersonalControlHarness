# MCP game-side seam (`--mcp-stdio`)

How LithoKraft wires the Phase 1 MCP server (`litholattice::mcp::McpServer`,
`engine/src/mcp/`). The engine provides the server, the transport, and the
seam interfaces; the game provides the flag, the adapters, and the approval
UX. Nothing here is implemented yet — this is the exact seam for the game
side (or the builder) to implement.

## 1. The `--mcp-stdio` flag

In `engine/LithoKraft/src/app/main.cpp`, in the existing `else if` arg chain
(next to `--no-save` etc.):

```cpp
else if (!std::strcmp(argv[i], "--mcp-stdio")) mcpStdio = true;
```

with `bool mcpStdio = false;` declared near the other flag locals. When set,
`main()` should boot the world headless (no window, no render loop — reuse
whatever headless path the harness uses), build the `McpServer` as below,
and call `server.runStdio()` on a dedicated thread (or the main thread; it
blocks until stdin EOF). The game's own stdin/stdout is the game console in
normal play, which is why stdio serving is a separate mode, not a runtime
toggle.

## 2. Building the server (engine entry point)

```cpp
#include "litholattice/mcp/server.h"

litholattice::mcp::McpBackends backends;
// ... fill in adapters (see table) ...
litholattice::mcp::McpServer server(std::move(backends),
                                    litholattice::mcp::McpAgent{"grok-cli", 0});
// Deny-by-default: mask 0 until the console approves (see §4).

std::thread mcpThread([&server] { server.runStdio(); });  // real stdin/stdout
// ... run the game ...
mcpThread.join();  // runStdio returns on stdin EOF
```

`runStdio()` never touches the sim thread for READS (Phase 1 is read-only);
Phase 4 write tools marshal onto the sim thread through the `IWorldWriter`
seam (see §9b) — the MCP thread still never mutates world state itself.
Keep one server per connection.

## 3. Adapter table: seam -> game class

Each adapter is ~20 lines: implement the pure interface, delegate to the
real object. **Threading:** every seam method runs on the MCP server's
thread, so adapters must be thread-safe. Copy out of the sim's lock-free
paths; never take the world lock for a bulk scan.

| Seam (`litholattice::mcp`) | Game adapts | Notes |
|---|---|---|
| `ISimStatus` | `SimThread` | `worldTick()` from `stats().worldTick`; `currentTps()` from `stats().tps`; `targetTps()` from config; `paused()` is an atomic already. `stats()` is documented "render thread only" — the triple-buffer read itself is lock-free; snapshot it in the adapter. |
| `IModListView` | `ModLoader` | From the loaded `ModInfo`s: name, version, `grantedPermissions`; `extensions` from the extension registry (empty vector is fine if unwired). |
| `IPlayerView` | `PlayerManager` | Local player: copy `PlayerState` fields; `stats` from the `IStat` registry (name/value pairs; empty is fine). |
| `IEntityView` | `EntityManager` | `within()` iterates entities within radius (ascending id); `get()` by id. Radius is already validated server-side (1..512). |
| `IWorldReader` | `SimThread::getBlock` | Use the existing thread-safe single-block read. |
| `console::IWorldQuery*` | the game's `IWorldQuery` backend | Passed straight through — no adapter needed. Same object the `query` console command uses. |
| `IDefragView` | `DefragService` | `phaseName()` via `DefragService::phaseName(phase())`; the rest is direct. |
| `IConsoleSurface` | `ConsoleRegistry` | `commandNames()` + help text (empty help is fine); `cvars()` gives name/value/help. |
| `IFragmentCorpus` | `index::TextIndexer` + page store | `search()` delegates to `TextIndexer::query(q, max)`; `text` comes from the game's DocId->page mapping (truncate long pages, e.g. 2 KiB). The indexer is NOT thread-safe — serialize with the owner's mutex in the adapter. |
| `IDimensionView` | `world::DimensionRegistry` | id, name, bound half-extent (0 for the unbounded Void), current flag. |
| `ILoreProvider` | game assets | Load the glossary markdown from the game's assets/packs at boot, return the text. **Decision (locked):** the game owns this file; the engine never embeds a copy. Until the glossary exists, leave the seam null — the resource then reads as "not configured". |
| `IPlayerController` | `InputSystem` | The game adapts this against its input system: each `submitFrame(pilot, frame)` becomes synthetic `InputEvent`s merged at `InputSystem::update()` — the exact point keyboard input enters (see `game/.../mcp_player_control.cpp`). Per-pilot shadow state; v1 wires one pilot (`"player"`). Thread-safe for the MCP thread. |
| `IVisionCapture` | `SoftwareRenderer` + mesh cache | The game keeps a CPU copy of each uploaded `ChunkMesh`, owns a `SoftwareRenderer` for vision, and renders from the pilot's camera (player-tracking by default; `setCameraPose` for free cameras). Downgraded: flat shading, low res (`mcp.vision_width`/`mcp.vision_height` cvars). See `game/.../mcp_senses_adapter.cpp`. |
| `IAudioCapture` | `AudioEngine` tap | Enable `audio_.setTapEnabled(true)` once; `captureAudioChunk` drains the tap, downsamples to the tier rate (`mcp.audio_tier` cvar, 0..3 → 8/16/24/48 kHz), and returns 1024 int16 mono frames. See `game/.../mcp_senses_adapter.cpp`. |

Any seam left null degrades gracefully: its tool returns an `isError`
result ("not available: the game has not wired this backend"), never a
crash. Useful for bringing the server up before every adapter exists.

## 4. Console approval flow (provisional UX, JD 2026-10-11)

Permission bits come from `mods/mod_interface.h` (`kPermWorldRead`,
`kPermConsole`, …). The server starts every connection at mask 0. The game
exposes these console commands (names provisional, behavior locked):

- `mcp agent [name]` — show/rename the connected agent.
- `mcp perms` — show the agent's current mask as names (`world_read, console`).
- `mcp grant <perm> [perm...]` — e.g. `mcp grant world_read console`. Calls `server.grantPermissions(bits)`.
- `mcp revoke <perm> [perm...]` — calls `server.revokePermissions(bits)`.

`<perm>` names are the canonical manifest names (`blocks`, `world_read`,
`world_write`, `tick`, `events`, `ui`, `input`, `assets`, `net`,
`console`); resolve with `mods::permissionBitForName()`. `grant`/`revoke`
are thread-safe — the console thread may call them while the server thread
runs. Tool calls log to the engine log (`mcp: tool <name> agent=<name>
<ok|denied|error|threw>`), visible in the debug console — that is the audit
trail for Phase 1.

Phase 3 adds the stateless-era (2026-07-28) approval path. Stateless
requests carry no session, so all of them share ONE agent identity on the
HTTP server (deny-by-default, mask 0). The game exposes:

- `mcp grant stateless <perm> [perm...]` — calls
  `http.grantStatelessPermissions(bits)`.
- `mcp revoke stateless <perm> [perm...]` — calls
  `http.revokeStatelessPermissions(bits)`.
- `mcp perms stateless` — show the shared stateless mask
  (`http.statelessPermissionMask()`).

Sharing one identity is the stateless tradeoff: there is no session to key
per-client masks on. A stateless client that needs finer isolation should
use the handshake era (its own session + agent) instead. On stdio there is
only one server per connection anyway, so the plain `mcp grant` commands
cover stateless stdio clients with no extra work.

Recommended default grant for a trusted local agent:
`mcp grant world_read console` (all 9 Phase 1 tools + all 6 resources).
`fragment_search` rides on `world_read` for now; a dedicated fragment bit
is an open question for JD (see §6).

## 5. What the server guarantees (so the game doesn't re-check)

- Dual-era (Phase 3): the handshake era (`2025-11-25`, `initialize` +
  `Mcp-Session-Id` sessions) and the stateless era (`2026-07-28`,
  per-request `params._meta`, `server/discover`) coexist on one server.
  Legacy requests behave exactly as Phase 1/2 built them; stateless
  requests skip the initialized gate and get the stateless result envelope
  (`resultType` + `_meta` server identity + `ttlMs`/`cacheScope` on lists).
- Handshake era: other versions in `initialize` get `-32022`.
- Stateless era: `params._meta` with an unsupported protocol version gets
  `-32022`; malformed `_meta` gets `-32602`.
- Pre-`initialize`, everything but `initialize`/`notifications/initialized`/
  `server/discover` is `-32600`.
- Tool failures (denials, unknown tool, missing entity, unwired backend)
  are `isError` results, never protocol errors. Only malformed calls are
  `-32602`.
- `entity_list` radius and `world_query_blocks` region are validated
  server-side (fail-closed); the game must not widen them.
- stdio: one JSON-RPC message per line, `\r` tolerated, blank lines
  skipped, lines over 16 MiB dropped with a log line. Logs go to stderr —
  stdout is the protocol and must stay clean.

## 7. Phase 2: Streamable HTTP transport (`--mcp-http`, engine-side)

The engine now also ships `litholattice::mcp::McpHttpServer`
(`engine/src/mcp/http_server.h`): a `POST /mcp` endpoint (default
`127.0.0.1:1337`, port JD-locked 2026-10-11) that owns one
`McpServer` per handshake-era session, exactly the Phase 1 ownership model
(one server per connection, deny-by-default mask 0).

Game-side wiring (the game owns the flag, the lifecycle, and the approval
UX — same split as §1–§4):

```cpp
#include "litholattice/mcp/http_server.h"

litholattice::mcp::McpHttpServer http(backends, "grok-http");
http.setLogSink([](std::string_view l) { engineLog(l); });  // audit trail
if (!http.start(litholattice::mcp::kMcpDefaultPort)) {
  // fail-fast: port taken -> error, never a silent re-pick
}
// Show ONCE in the debug console (§6 posture); never log it:
console->print("MCP bearer token: " + http.token());
```

- Every `POST /mcp` needs `Authorization: Bearer <token>` (else 401) and,
  after `initialize`, `Mcp-Session-Id` (missing → 400, unknown → 404).
- The console approval commands from §4 grow a session argument, e.g.
  `mcp grant <session-id-prefix> world_read console`, calling
  `http.grantPermissions(sessionId, bits)` / `revokePermissions`.
  `http.sessionIds()` lists live sessions. `http.rotateToken()` replaces
  the bearer token (a future `mcp rotate-token` command).
- `http.stop()` joins the accept/dispatch thread. Threading: ONE
  accept/dispatch thread serves all sessions sequentially (documented in
  `sockets.h`); seam adapters keep the Phase 1 thread-safety rules.
- `GET /mcp` answers 405 today — the SSE stream endpoint is a later phase.
  The SSE framing seam (`SseStream`) is implemented and self-tested; live
  server→client notifications are not wired yet.

### 7b. Phase 3: the stateless era over HTTP (no game-side changes needed)

Stateless requests (`params._meta` declaring `2026-07-28`) need no
`Mcp-Session-Id`: they are answered by the `McpHttpServer`'s dedicated
shared stateless server, and no session id is minted or expected. Bearer
auth still applies (401 without it). The mirrored headers
`MCP-Protocol-Version` / `Mcp-Method` / `Mcp-Name` are validated against the
body (missing/mismatched → JSON-RPC `-32020`, HTTP 400); unknown methods →
`-32601` with HTTP 404; other JSON-RPC errors → HTTP 400. The game side
only gains the stateless approval commands from §4
(`http.grantStatelessPermissions()` / `revokeStatelessPermissions()` /
`statelessPermissionMask()`); the flag, lifecycle, and approval UX are
otherwise unchanged. Audit lines for stateless traffic read
`session=stateless`.

## 8. Open questions for JD / the builder

1. Fragment privacy: `fragment_search` currently requires `world_read`.
   The journal is personal — does it want its own `kPermFragment` bit (new
   bit in `mod_interface.h`, Phase 2)?
2. The lore glossary asset doesn't exist yet — where should it live
   (`assets/lore/glossary.md`?) and who authors it?
3. `--mcp-stdio` headless boot: reuse the headless harness path or a
   lighter dedicated boot?
4. Multi-agent: one server per connection is the Phase 1 model; connection
   multiplexing / agent naming across reconnects is Phase 2+.

## 9. Phase 4: write tools (engine-side, 2026-10-11)

The smallest safe v1 write set: `world_set_block` and `lithoscript_exec`.
Both are deny-by-default (a fresh connection can do NOTHING mutating until
granted), approval-gated through the console commands below, rate-limited,
audit-logged, and announced to hosts as mutable via MCP tool annotations
(`readOnlyHint: false`, `destructiveHint: true` — `world_set_block` is
additionally `idempotentHint: true`). NOT in v1: `sim_pause`/`sim_resume`
and `angelscript_eval` (research report §5: v2, high-privilege).

### 9a. Approval shapes (provisional UX, JD 2026-10-11)

The game already exposes `mcp grant <perm...>`, `mcp revoke <perm...>`,
`mcp perms` (stdio), the `<session-id-prefix>` variants (HTTP handshake
sessions), and the `stateless` variants (HTTP stateless) from §4/§7. Phase 4
gives them teeth — the exact shapes for the write tools:

- stdio: `mcp grant world_write` → `server.grantPermissions(kPermWorldWrite)`
  unlocks `world_set_block`; `mcp grant console` → unlocks
  `lithoscript_exec`. `mcp revoke world_write` locks it again (revoke is
  immediate and thread-safe).
- HTTP handshake sessions: `mcp grant <session-id-prefix> world_write`
  (per-session agent, same as §7).
- HTTP stateless: `mcp grant stateless world_write` /
  `mcp grant stateless console` (the one shared stateless identity).

Before any grant, both tools answer `isError` naming the missing bit
(`world_write` / `console`) — never a protocol error, and the denied write
never reaches the sim thread. Recommended default grant for a trusted
local agent is unchanged: `mcp grant world_read console` (read surface +
lithoscript). Add `world_write` only when the agent should build.

### 9b. `world_set_block`: the sim-thread marshaling contract

Tool args: `x`, `y`, `z` (integer, int32 range), `block` (integer
0..65535), optional `timeout_ms` (1..2000, default 2000). The engine side is
the `IWorldWriter` seam (`mcp/server.h`); the game adapts it against
`SimThread` with this recipe (the determinism guard, research §6):

1. Build a game-defined `SimCommand` (`sim/world_simulation.h`):
   `{code = <game's kCmdMcpSetBlock>, x, y, z, a = blockId}`.
2. Push `EditCmd{Kind::Command, x, y, z, command}` via
   `SimThread::edit()`. The sim thread drains the edit queue in
   `applyEdits()` at a deterministic point of the tick — an agent's write
   lands exactly where a player edit would, so the sim stays bit-identical.
3. Block the MCP thread on a per-call completion (mutex + condition
   variable, or an atomic flag the caller spins on with
   `SimThread::waitLoops`) until the sim thread's
   `IWorldSimulation::command` handler applies the write and signals it.
4. On timeout (`timeout_ms`, default `kMcpSimWriteTimeoutMs` = 2000):
   return false with `why` = "timed out after Nms waiting for the sim
   thread". The tool turns that into an `isError` result — never a hang,
   and never a partial apply reported as success.

The tool additionally consumes one token per call from the per-connection
write bucket: 32 block writes/sec sustained, burst 64 (powers of two;
`kMcpBlockWriteLimitPerSec` / `kMcpBlockWriteBurst` in
`mcp/rate_limit.h`). Exceeded → `isError` naming the limit. The bucket is
per `McpServer`: per connection on stdio, per session on HTTP, one shared
for all stateless traffic.

### 9c. `lithoscript_exec`: the agent's mask, never the player's

Tool args: `line` (string, non-empty, max 64 KiB — longer is `-32602`).
The engine side is the `IConsoleExec` seam (`mcp/server.h`); the game
adapter does exactly this (the critical rule from research §6 and the
console's own trust model):

```cpp
void MyConsoleExec::executeLine(std::string_view line, uint32_t callerMask,
                                std::vector<std::string>& out) {
  std::lock_guard l(gameConsoleMutex_);          // the registry is not thread-safe
  auto* reg = gameConsoleRegistry();
  auto savedSink = /* current OutputFn */;
  const uint32_t savedMask = reg->callerPermissions();
  reg->setOutput([&out](const std::string& s) { out.push_back(s); });
  reg->setCallerPermissions(callerMask);        // THE rule: the agent's mask
  reg->executeLine(line);
  reg->setCallerPermissions(savedMask);
  reg->setOutput(savedSink);
}
```

`ConsoleRegistry::setCallerPermissions` (new in Phase 4, `console/concmd.h`)
is the minimal seam: default `kCallerFullAuthority` preserves the old
behavior for every existing caller; installing any restricted mask revokes
cheat authority for cheat-gated commands/cvars — fail-closed, a restricted
mask can never widen authority. The mask is stamped onto `wait`-suspended
scripts and restored on resume, so a suspended agent line cannot outlive
its authority. The console selftest pins this (`console: caller: ...`).

Console output lines are captured into the tool result's text (joined with
`\n`; `(no output)` when the line prints nothing).

### 9d. Audit

Every write/exec call appends `mcp:`-prefixed lines to the engine log
(who/what/result; "when" is the log's own `HH:MM:SS.mmm` stamp), on top of
the Phase 1 `tool <name> agent=<name> <ok|denied|error|threw>` line:

- `mcp: write world_set_block agent=<name> (x,y,z)=id -> ok`
- `mcp: write world_set_block agent=<name> -> rate_limited (32/sec, burst 64)`
- `mcp: write world_set_block agent=<name> -> not applied: <why>`
- `mcp: exec lithoscript_exec agent=<name> mask=<names> bytes=<n> -> ok (k output lines)`

### 9e. Per-connection tool-call rate cap

Every `tools/call` on a server consumes one token from a second bucket:
256 calls/sec sustained, burst 512 (`kMcpToolCallLimitPerSec` /
`kMcpToolCallBurst`). Exceeded → `isError` naming the limit (never a
protocol error). The bucket is per `McpServer`, i.e. per connection /
session / shared stateless identity. Both buckets take a fakeable monotonic
clock (`McpServer::setRateLimitClock`, used by the selftests); production
uses the steady clock. Rate limiting is an I/O-edge policy — it never
touches sim state, so determinism is unaffected.
