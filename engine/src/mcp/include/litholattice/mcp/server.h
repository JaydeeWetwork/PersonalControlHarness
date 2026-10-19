// server.h: Phase 1 MCP server — stdio transport, handshake-era lifecycle
// (2025-11-25), agent identity + kPerm* gating, and the read-only tool /
// resource surface. Phase 2 added the Streamable HTTP transport. Phase 3
// adds the stateless era (2026-07-28): per-request params._meta with the
// protocol version, server/discover, the stateless result envelope
// (resultType + _meta server identity + SEP-2549 cache hints on lists), and
// prompts/list + prompts/get. Clean-room; no code taken from any MCP
// implementation.
//
// Layering (locked):
//   - The server NEVER touches engine subsystems directly. Every data source
//     is an injectable seam interface declared below (ISimStatus, IWorldReader,
//     ...). The game adapts its real classes (SimThread, ModLoader, ChunkMap,
//     EntityManager, ConsoleRegistry, TextIndexer, DefragService, ...) to
//     these seams; the self-tests use stub seams. Consequence: the mcp library
//     has no link dependencies on any other engine module — it only reads
//     three header-only public headers (kPerm* bits, engine version macros,
//     IWorldQuery). The one exception is console::IWorldQuery, which is taken
//     directly because it is already a pure interface (no link dep) and
//     already the engine's agent-query seam.
//   - Threading: the server does its work on whatever thread calls
//     handleMessage()/runStdio(). Seam implementations must therefore be
//     thread-safe for the server's thread (e.g. copy SimThread stats through
//     its lock-free triple buffer; use SimThread::getBlock for single reads).
//     Concurrent handleMessage() calls are NOT supported; run one server per
//     connection/thread. grantPermissions()/revokePermissions() are internally
//     synchronized so the console thread can approve while the server runs.
//   - Fail policy: malformed protocol input is rejected (fail-fast); a null
//     seam means "not wired by the game" and yields a model-actionable
//     isError tool result, never a crash. Tool CALL failures (permission
//     denials, missing entities, unwired backends) are isError results, never
//     JSON-RPC protocol errors. Only genuinely malformed calls (-32602) and
//     lifecycle violations (-32600) are protocol errors.
//
// JSON-schema subset (locked, documented for small local models): tool
// inputSchema values are hand-rolled and minimal:
//   {"type":"object",
//    "properties":{"name":{"type":"<string|integer|number|boolean>",
//                          "description":"...",
//                          ["minimum":n,"maximum":n]}},
//    "required":["name",...]}
// Only "object" appears at the top level; only the four primitive property
// types exist. Anything else in a schema is not produced by this server.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "litholattice/mcp/json.h"
#include "litholattice/mcp/jsonrpc.h"
#include "litholattice/mcp/rate_limit.h"

namespace litholattice {
// Forward declaration: the server takes console::IWorldQuery directly (it is
// a pure interface, so no link dependency), but the full header is only
// included by server.cpp. Game code implementing the seam includes the real
// header itself.
namespace console {
class IWorldQuery;
}  // namespace console
namespace mcp {

/// MCP protocol version served in the handshake era (Phase 1; the stateless
/// era is Phase 3). Clients asking for anything else get -32022.
inline constexpr const char* kMcpProtocolVersion = "2025-11-25";
/// MCP protocol version served in the stateless era (Phase 3, 2026-07-28).
/// Declared per request in params._meta; no initialize handshake, no
/// session. Both eras coexist on one server (dual-era).
inline constexpr const char* kMcpProtocolVersionStateless = "2026-07-28";

/// params._meta keys in the stateless era. The spec namespaces them under
/// io.modelcontextprotocol/; the plain "protocolVersion" key is ALSO
/// accepted (lenient) for clients written from prose descriptions of the
/// era. The namespaced key wins when both are present.
inline constexpr const char* kMetaProtocolVersion = "io.modelcontextprotocol/protocolVersion";
inline constexpr const char* kMetaClientCapabilities = "io.modelcontextprotocol/clientCapabilities";
inline constexpr const char* kMetaServerInfo = "io.modelcontextprotocol/serverInfo";

/// SEP-2549 cache hints on list/discover results. Our registries are fixed
/// at boot and identical for every agent, so 1h / public is honest.
/// resources/read (live data, permission-gated) gets ttlMs 0 / private.
inline constexpr int64_t kMcpListTtlMs = 3600000;
inline constexpr const char* kMcpCacheScopePublic = "public";
inline constexpr const char* kMcpCacheScopePrivate = "private";
/// serverInfo.name returned by initialize.
inline constexpr const char* kMcpServerName = "litholattice";

/// JSON-RPC server-error code for permission denials on resources/read.
/// (Tool-call denials are isError results, not protocol errors.)
inline constexpr int kMcpPermissionDenied = -32001;

/// Max radius (blocks) accepted by entity_list. Fail-closed: larger radii are
/// -32602, never silently clamped.
inline constexpr double kMaxEntityRadius = 512.0;
/// Max line length accepted on stdio (16 MiB). Longer lines are dropped with
/// a log line; the connection continues.
inline constexpr std::size_t kMaxStdioLineBytes = 16u * 1024u * 1024u;

// ---------------------------------------------------------------------------
// Protocol eras (Phase 3: dual-era dispatch).
// ---------------------------------------------------------------------------

/// Which protocol era one request belongs to, decided from params._meta
/// (the 2026-07-28 stateless marker). Shared by the stdio and HTTP
/// transports — the era logic lives in the dispatch layer, not per
/// transport.
enum class McpEra { kLegacy, kStateless };

/// Classifies one request's era from its params value:
/// - no _meta (or null _meta): kLegacy (handshake-era lifecycle applies).
/// - _meta.protocolVersion == "2026-07-28": kStateless (no initialize, no
///   session; the dispatch layer applies the stateless result envelope).
/// - _meta.protocolVersion == "2025-11-25": kLegacy (the handshake era
///   never used per-request _meta; the lifecycle gate still applies).
/// - anything else in protocolVersion: throws RpcError
///   (kRpcUnsupportedProtocolVersion, -32022) with data.supported naming
///   both served versions.
/// - _meta present but not an object, or protocolVersion missing/non-string:
///   throws RpcError(kRpcInvalidParams, -32602) — the client attempted the
///   stateless shape but malformed it.
/// Never throws for absent _meta. `params` is the request's params value
/// (null when the member was absent).
McpEra classifyRequestEra(const JsonValue& params);

// Stateless agent identity (Phase 3): a stateless request carries no
// session, so there is no per-client handshake to hang an identity on. It
// is served with the McpServer's own McpAgent identity — deny-by-default
// (mask 0), exactly like a fresh handshake-era connection, and the Phase 1
// permission gating (isError denials on tools, -32001 on gated resources)
// applies unchanged. Permissions arrive through the same approval flow:
// - stdio: the game calls server.grantPermissions(bits) (console
//   `mcp grant <perm...>`), which flips the one server's agent.
// - HTTP: all stateless clients share the McpHttpServer's dedicated
//   stateless McpServer; the game calls
//   McpHttpServer::grantStatelessPermissions(bits) (console
//   `mcp grant stateless <perm...>`). Sharing one identity is the
//   stateless tradeoff: there is no session to key per-client masks on.

// ---------------------------------------------------------------------------
// Agent identity + permission gating.
// ---------------------------------------------------------------------------

/// One MCP connection's identity. Deny-by-default: a fresh connection starts
/// at permissionMask 0; the console side grants bits (see grantPermissions).
/// Bits are the kPerm* values from mods/mod_interface.h.
struct McpAgent {
  std::string name = "agent";
  uint32_t permissionMask = 0;
};

// ---------------------------------------------------------------------------
// Injectable data seams. The game implements these against the real headers;
// the server only ever talks to the seams. All calls happen on the server's
// thread and must be thread-safe (see the layering note above).
// ---------------------------------------------------------------------------

/// sim/sim_thread.h + sim/clock_domain.h: tick, TPS, paused.
class ISimStatus {
 public:
  virtual ~ISimStatus() = default;
  virtual uint64_t worldTick() = 0;
  virtual double currentTps() = 0;
  virtual int targetTps() = 0;
  virtual bool paused() = 0;
};

/// mods/mod_loader.h (+ extension_registry.h): installed mods, versions,
/// granted permission masks, and extension trees.
class IModListView {
 public:
  struct ModEntry {
    std::string name;
    std::string version;
    uint32_t permissions = 0;  ///< granted kPerm* mask
    std::vector<std::string> extensions;  ///< extension tree paths, may be empty
  };
  virtual ~IModListView() = default;
  virtual std::vector<ModEntry> mods() = 0;
};

/// player/: the local player's state + named stats.
class IPlayerView {
 public:
  struct Snapshot {
    double x = 0, y = 0, z = 0;
    double vx = 0, vy = 0, vz = 0;
    float yaw = 0, pitch = 0;
    uint8_t dimension = 1;  ///< 1 = Wafer
    uint8_t health = 64;    ///< 0-64
    std::vector<std::pair<std::string, double>> stats;  ///< named stats, may be empty
    std::vector<uint16_t> hotbar;  ///< hotbar block ids, slot order
    int selectedSlot = 0;          ///< index into hotbar
  };
  virtual ~IPlayerView() = default;
  virtual Snapshot localPlayer() = 0;
};

class IVisionCapture;
class IAudioCapture;

/// player control: lets an approved agent pilot the local player through the
/// REAL input path. The game adapts this seam against its InputSystem: each
/// submitted frame becomes synthetic InputEvents merged with the backend's
/// events at the next InputSystem::update() — the exact point keyboard input
/// enters. The model plays by the same rules as a keyboard player: the same
/// physics in Player::update, the same reach/interact checks in
/// handleWorldActions, the same sim-thread marshaling for world edits.
///
/// MULTI-PILOT (locked 2026-10-11): the surface is per-pilot from day one.
/// Every control tool takes a `pilot` id; the game wires the pilot set
/// (v1: exactly one, "player" = the local player). Adding pilot #2 is
/// configuration — the game registers another id with its own shadow input
/// state — never surgery on this interface, the tools, or the rate limits.
/// Rate buckets and audit logging are keyed by pilot id.
///
/// NPC FUTURE (not built): a companion NPC is a pilot slot driven by game AI
/// instead of an MCP client — same addressing, same input path, no MCP
/// permission involved (internal pilots bypass the agent grant flow).
///
/// ANTI-CHEAT NOTE (locked): a model-driven player is still a player. This
/// seam grants NO new capabilities: movement goes through Player::update
/// (speed caps, no-faster-diagonals), and break/place/use go through the
/// game's normal action handling into sim_->edit() at tick boundaries (reach
/// checks, interactable checks, creative rules — all enforced). What it does
/// NOT stop: the model playing 24/7 with inhuman reaction time and perfect
/// recall. That is the point of the feature (agent playground), not a bug.
/// Server-authoritative multiplayer is out of scope for v1: the MCP server
/// is localhost-only by default, and a remote server would never grant
/// kPermPlayerControl to an untrusted network peer.
///
/// Threading: submitFrame()/releaseAll() run on the MCP server's thread and
/// must be thread-safe for it. Frames are consumed on the game's main thread
/// at the frame pump; each pump consumes at most one staged batch, so the
/// effective input rate can never exceed the frame rate no matter how fast
/// the model calls.
class IPlayerController {
 public:
  /// One frame of control input. All fields optional; absent fields leave
  /// the corresponding state unchanged (sticky held-buttons, one-shot
  /// presses and look deltas).
  struct ControlFrame {
    float moveForward = 0.f;  ///< -1..1 (clamped); held until changed
    float moveStrafe = 0.f;    ///< -1..1 (clamped); held until changed
    float lookDx = 0.f;        ///< mouse-motion pixels right, applied once
    float lookDy = 0.f;        ///< mouse-motion pixels down, applied once
    bool jump = false;         ///< held until changed
    bool sneak = false;        ///< held until changed
    bool sprint = false;       ///< held until changed
    bool usePressed = false;   ///< edge: interact, this frame only
    bool attackPressed = false;///< edge: break, this frame only
    bool placePressed = false; ///< edge: place, this frame only
    bool dropPressed = false;  ///< edge: drop selected, this frame only
    int hotbarSlot = -1;       ///< 0..7: select slot this frame; -1 = no change
  };
  virtual ~IPlayerController() = default;
  /// Ids of currently wired pilots (v1: {"player"}). The game owns this set.
  virtual std::vector<std::string> pilotIds() const = 0;
  /// Queue one control frame for `pilot`, applied at the next frame's input
  /// pump. Returns false when that pilot has no controllable player right
  /// now (menu, paused, inventory open, ...) — unknown pilot ids are
  /// rejected by the tool layer before this is called.
  virtual bool submitFrame(std::string_view pilot, const ControlFrame& f) = 0;
  /// Release every held button for `pilot` (safety: a disconnecting model
  /// must never leave its player running into a wall forever).
  virtual void releaseAll(std::string_view pilot) = 0;
};

/// entity/entity.h: radius-bounded listing + lookup by id.
class IEntityView {
 public:
  struct Ent {
    uint64_t id = 0;
    uint32_t type = 0;
    double x = 0, y = 0, z = 0;
  };
  virtual ~IEntityView() = default;
  /// Entities within `radius` of (cx, cy, cz), ascending id order.
  virtual std::vector<Ent> within(double cx, double cy, double cz, double radius) = 0;
  /// True + fills `out` when the entity exists.
  virtual bool get(uint64_t id, Ent& out) = 0;
};

/// world/block.h: single block read (game: SimThread::getBlock or ChunkMap).
class IWorldReader {
 public:
  virtual ~IWorldReader() = default;
  virtual uint16_t getBlock(int x, int y, int z) = 0;
};

/// world/block.h: single-block WRITE, marshaled onto the sim thread
/// (Phase 4).
///
/// DETERMINISM GUARD (research §6): the write MUST be applied on the sim
/// thread — never on the MCP/HTTP thread. The game adapts this seam against
/// SimThread by pushing an EditCmd::Command (a game-defined SimCommand
/// carrying x/y/z/block id) through SimThread::edit(); the sim thread
/// drains it in applyEdits() at a deterministic point of the tick, applies
/// the write there, then signals the completion this call is blocked on.
/// Edit-drain order is tick-deterministic, so an agent's writes land in the
/// same place in the tick as player edits: the sim stays bit-identical.
///
/// The call blocks up to `timeoutMs`; false means "not applied" (timeout or
/// backend failure) with `why` filled in. Never a hang, and never a partial
/// apply reported as success. Runs on the MCP server's thread and must be
/// thread-safe for it.
class IWorldWriter {
 public:
  virtual ~IWorldWriter() = default;
  virtual bool setBlock(int x, int y, int z, uint16_t id, int timeoutMs, std::string& why) = 0;
};

/// console/concmd.h ConsoleRegistry::executeLine, agent-scoped (Phase 4).
///
/// THE critical rule (research §6 + the console's own SCRIPT TRUST MODEL):
/// the line runs at the AGENT's permission mask, NEVER the player's. The
/// game adapter enforces it by installing `callerMask` on the registry via
/// ConsoleRegistry::setCallerPermissions() for the duration of the call —
/// cheat-gated commands/cvars are refused for any restricted mask even when
/// the player's context allows cheats — capturing the OutputFn sink into
/// `out`, and restoring both afterwards.
///
/// Runs on the MCP server's thread: the adapter must serialize against the
/// game's own console use (e.g. hold the game's console mutex for the whole
/// call; the registry itself is not thread-safe — see concmd.h).
class IConsoleExec {
 public:
  virtual ~IConsoleExec() = default;
  virtual void executeLine(std::string_view line, uint32_t callerMask,
                           std::vector<std::string>& out) = 0;
};

/// defrag/defrag.h: forever-world maintenance visibility (status only).
class IDefragView {
 public:
  virtual ~IDefragView() = default;
  virtual bool running() = 0;
  virtual std::string phaseName() = 0;  ///< e.g. "Idle", "Scan", ...
  virtual double progress() = 0;        ///< 0..1
  virtual std::string statusLine() = 0;
  virtual std::vector<std::string> storeNames() = 0;
};

/// console/concmd.h ConsoleRegistry: the live console surface for the
/// lithoscript://commands and lithoscript://cvars resources.
class IConsoleSurface {
 public:
  struct Command {
    std::string name;
    std::string help;
  };
  struct CVar {
    std::string name;
    std::string value;  ///< current value, canonical form
    std::string help;
  };
  virtual ~IConsoleSurface() = default;
  virtual std::vector<Command> commands() = 0;
  virtual std::vector<CVar> cvars() = 0;
};

/// index/text_indexer.h: the Fragment corpus. The game wires its TextIndexer
/// plus the DocId -> page-text mapping; the server never interprets DocIds.
class IFragmentCorpus {
 public:
  struct Hit {
    uint64_t doc = 0;
    double score = 0.0;
    std::string text;  ///< page text (may be truncated by the game)
  };
  virtual ~IFragmentCorpus() = default;
  /// Deterministic search (score desc, doc asc on ties, like TextIndexer).
  virtual std::vector<Hit> search(std::string_view query, size_t maxResults) = 0;
};

/// world/dimension.h: dimension list, bounds, current dimension.
class IDimensionView {
 public:
  struct Dim {
    std::string id;    ///< registry key chosen by the game
    std::string name;  ///< display name
    int64_t bound = 0;  ///< half-extent in blocks (0 = unbounded, e.g. the Void)
    bool current = false;
  };
  virtual ~IDimensionView() = default;
  virtual std::vector<Dim> dimensions() = 0;
};

/// game://lore-glossary text. DECISION (locked): the game owns the glossary
/// file — it lives with the game's assets/packs and is loaded at runtime by
/// the game, which injects the text here. The engine never embeds a copy
/// (embedding would drift from the shipped packs). Empty text = not
/// configured; the resource read then fails with a clear message.
class ILoreProvider {
 public:
  virtual ~ILoreProvider() = default;
  virtual std::string text() = 0;
};

/// All seams in one bundle. Any field may be null (= not wired by the game);
/// tools backed by a null seam return an isError result naming the seam.
struct McpBackends {
  std::shared_ptr<ISimStatus> sim;
  std::shared_ptr<IModListView> mods;
  std::shared_ptr<IPlayerView> player;
  std::shared_ptr<IEntityView> entities;
  std::shared_ptr<IWorldReader> world;
  /// Taken directly (pure interface, no link dep): console/iworldquery.h
  /// IWorldQuery::queryBlocks. Forward-declared above; server.cpp includes
  /// the real header.
  std::shared_ptr<console::IWorldQuery> worldQuery;
  std::shared_ptr<IDefragView> defrag;
  std::shared_ptr<IConsoleSurface> console;
  std::shared_ptr<IFragmentCorpus> corpus;
  std::shared_ptr<IDimensionView> dimensions;
  std::shared_ptr<ILoreProvider> lore;
  /// Phase 4: single-block write, marshaled onto the sim thread (null = the
  /// world_set_block tool reports "not wired"; the game adapts this against
  /// SimThread::edit — see IWorldWriter).
  std::shared_ptr<IWorldWriter> writer;
  /// Phase 4: agent-scoped lithoscript execution (null = the lithoscript_exec
  /// tool reports "not wired"; runs at the AGENT's mask, never the player's).
  std::shared_ptr<IConsoleExec> consoleExec;
  /// Player control: model pilots the local player through the real input
  /// path (null = the player_* tools report "not wired"; the game adapts
  /// this against its InputSystem — see IPlayerController).
  std::shared_ptr<IPlayerController> controller;
  /// Pilot senses: agent vision (software-renderer frames from the pilot's
  /// camera, downgraded) and agent audio (mixed-output tap at a downgraded
  /// tier). Null = the vision.capture_frame / audio.capture_chunk tools
  /// report "not wired"; the game adapts these against its renderer and
  /// AudioEngine — see IVisionCapture / IAudioCapture in mcp_senses.h.
  std::shared_ptr<IVisionCapture> vision;
  std::shared_ptr<IAudioCapture> audio;
};

// ---------------------------------------------------------------------------
// Tool / resource descriptors.
// ---------------------------------------------------------------------------

/// Model-actionable tool result. Rendered as
/// {"content":[{"type":"text","text":...}],"isError":bool}.
struct ToolResult {
  bool isError = false;
  std::string text;
  static ToolResult ok(std::string t) { return ToolResult{false, std::move(t)}; }
  static ToolResult error(std::string t) { return ToolResult{true, std::move(t)}; }
};

/// Tool handler: validated arguments object -> result. May throw RpcError
/// (-32602) for malformed arguments; any other exception becomes an isError
/// result (never a protocol error).
using ToolHandler = std::function<ToolResult(const JsonValue& args, const McpAgent& agent)>;

/// One input-schema property (see the schema-subset note at the top).
struct ToolParam {
  std::string name;
  std::string type;  ///< "string" | "integer" | "number" | "boolean"
  std::string description;
  bool required = false;
  bool hasMinimum = false;
  double minimum = 0;
  bool hasMaximum = false;
  double maximum = 0;
};

struct ToolDescriptor {
  std::string name;
  std::string description;  ///< tight: small local models choke on huge schemas
  std::vector<ToolParam> params;
  uint32_t requiredPerms = 0;  ///< kPerm* bits; denied -> isError naming the bit
  ToolHandler handler;
  /// MCP tool annotations (spec): hosts use these to decide whether to
  /// prompt the human before calling. Phase 1 tools are all read-only; the
  /// Phase 4 write tools set readOnlyHint=false (+ destructiveHint).
  struct Annotations {
    bool readOnlyHint = true;
    bool destructiveHint = false;
    bool idempotentHint = false;
    bool openWorldHint = false;
  };
  Annotations annotations;
};

struct ResourceDescriptor {
  std::string uri;
  std::string name;
  std::string description;
  std::string mimeType;
  uint32_t requiredPerms = 0;  ///< denied -> kMcpPermissionDenied protocol error
  /// Returns the resource text. Throws RpcError on failure.
  std::function<std::string(const McpAgent& agent)> read;
};

/// One prompt argument (spec shape: name/description/required — prompts do
/// NOT use JSON Schema; arguments arrive as strings).
struct PromptArgument {
  std::string name;
  std::string description;
  bool required = false;
};

/// A reusable prompt template. Rendered by prompts/get into user-role
/// messages with the arguments substituted.
struct PromptDescriptor {
  std::string name;
  std::string description;
  std::vector<PromptArgument> arguments;
  /// kPerm* bits; denied -> kMcpPermissionDenied protocol error (mirrors
  /// resources/read — prompts/get has no isError channel). All v1 prompts
  /// are mask 0 (open): they are static templates that reveal no more than
  /// tools/list already does.
  uint32_t requiredPerms = 0;
  /// Renders the prompt text with validated arguments substituted. Arguments
  /// are pre-validated (present/typed/known) before render is called.
  std::function<std::string(const std::map<std::string, std::string>& args)> render;
};

// ---------------------------------------------------------------------------
// stdio transport.
// ---------------------------------------------------------------------------

/// Newline-delimited JSON-RPC framing. Injectable so tests can drive the
/// server with scripted lines instead of real stdin/stdout.
class StdioTransport {
 public:
  virtual ~StdioTransport() = default;
  /// Reads one line (without the newline); false on EOF. Implementations
  /// should tolerate a trailing '\r'.
  virtual bool readLine(std::string& out) = 0;
  /// Writes one line + '\n', flushed.
  virtual void writeLine(std::string_view line) = 0;
};

/// The real stdin/stdout transport.
class FdStdioTransport : public StdioTransport {
 public:
  bool readLine(std::string& out) override;
  void writeLine(std::string_view line) override;

 private:
  /// Bytes already read past a line's newline (a chunk rarely ends exactly
  /// on one): drained first by the next readLine call.
  std::string pending_;
};

using LogSink = std::function<void(std::string_view line)>;

// ---------------------------------------------------------------------------
// The server.
// ---------------------------------------------------------------------------

class McpServer {
 public:
  explicit McpServer(McpBackends backends = {}, McpAgent agent = {});
  McpServer(const McpServer&) = delete;
  McpServer& operator=(const McpServer&) = delete;

  // -- agent identity -------------------------------------------------------
  /// Console-side approval flow (provisional UX, JD 2026-10-11): the game
  /// exposes console commands `mcp grant <perm...>` / `mcp revoke <perm...>`
  /// / `mcp agent` which call these. Thread-safe.
  void grantPermissions(uint32_t bits);
  void revokePermissions(uint32_t bits);
  uint32_t permissionMask() const;
  void setAgentName(std::string name);
  McpAgent agent() const;

  // -- dispatch --------------------------------------------------------------
  /// One JSON-RPC message in, optional serialized response out. Dual-era
  /// (Phase 3):
  /// - Handshake era (no params._meta): pre-initialization, only
  ///   `initialize`, `notifications/initialized`, and `server/discover` are
  ///   accepted; anything else is -32600 (notifications get no response,
  ///   ever). Post-initialization, everything dispatches normally.
  /// - Stateless era (params._meta declares 2026-07-28): the initialized
  ///   gate is skipped entirely — no session needed. Success results get
  ///   the stateless envelope (resultType "complete" + _meta server
  ///   identity; SEP-2549 cache hints on lists/discover/read). `initialize`
  ///   is always the handshake-era method: _meta never changes that.
  /// - params._meta with an unsupported version: -32022.
  /// Not thread-safe for concurrent calls; run one server per connection.
  std::optional<std::string> handleMessage(std::string_view text);
  bool initialized() const { return initialized_; }

  /// Blocking stdio loop on the CALLING thread: read a line, dispatch, write
  /// the response. Returns on EOF. The game should call this on a dedicated
  /// thread (or a companion process); it never returns while the pipe is open.
  void runStdio(StdioTransport& transport);
  /// runStdio() on the real stdin/stdout.
  void runStdio();

  void setLogSink(LogSink sink);

  /// Replaces the monotonic clock behind both rate limiters (tool-call cap
  /// and block-write cap). Production default is the steady clock; tests
  /// inject a fake. Documented for tests; the game has no reason to call it.
  void setRateLimitClock(McpTokenBucket::ClockFn fn);

  // -- registries (for tests / introspection) --------------------------------
  const std::vector<ToolDescriptor>& tools() const { return tools_; }
  const std::vector<ResourceDescriptor>& resources() const { return resources_; }
  const std::vector<PromptDescriptor>& prompts() const { return prompts_; }

 private:
  JsonValue onInitialize(const JsonValue& params);
  JsonValue onDiscover(const JsonValue& params);
  JsonValue onToolsList(const JsonValue& params);
  JsonValue onToolsCall(const JsonValue& params);
  JsonValue onResourcesList(const JsonValue& params);
  JsonValue onResourcesRead(const JsonValue& params);
  JsonValue onPromptsList(const JsonValue& params);
  JsonValue onPromptsGet(const JsonValue& params);

  /// Stateless-era dispatch: no lifecycle gate; the success result gets the
  /// stateless envelope (see applyStatelessEnvelope), merged into the
  /// response value before serializing (no parse/serialize round trip).
  std::optional<std::string> dispatchStateless(const RpcRequest& req);
  /// Merges the 2026-07-28 result envelope into a result object: resultType
  /// "complete" (SEP-2322; this server never needs client input), _meta with
  /// the protocol version + server identity, and SEP-2549 ttlMs/cacheScope
  /// on list/discover/read results. Existing keys are never overwritten.
  void applyStatelessEnvelope(JsonValue& result, const std::string& method);

  void registerPhase1Tools();
  void registerPhase1Resources();
  void registerPhase3Prompts();
  /// Phase 4: the gated write tools (world_set_block, lithoscript_exec).
  void registerPhase4WriteTools();
  /// Player control tools (player_move/look/action/hotbar/release): the
  /// model pilots the local player through the real input path. Gated on
  /// kPermPlayerControl, rate-limited by the control bucket.
  void registerPlayerControlTools();
  /// Pilot senses tools (vision.capture_frame, audio.capture_chunk): the
  /// model sees/hears through the piloted player. Gated on kPermPilotSenses,
  /// rate-limited per pilot (vision 4/sec burst 8, audio 8/sec burst 16).
  void registerPilotSensesTools();
  void log(std::string_view line);

  McpBackends backends_;
  McpAgent agent_;
  mutable std::mutex agentMutex_;
  RpcRouter router_;
  std::vector<ToolDescriptor> tools_;
  /// Name -> index into tools_: the per-call linear scan was fine at ~20
  /// tools, but the count grows with every phase. Rebuilt lazily (tools_
  /// only grows during construction, so a size check detects staleness).
  std::unordered_map<std::string, size_t> toolIndex_;
  size_t toolIndexBuiltFor_ = 0;
  /// Indexed tool lookup (nullptr when unknown). See toolIndex_.
  const ToolDescriptor* findTool(const std::string& name);
  std::vector<ResourceDescriptor> resources_;
  std::vector<PromptDescriptor> prompts_;
  bool initialized_ = false;
  LogSink logSink_;
  /// Per-connection rate limiters (Phase 4): one bucket per McpServer, and
  /// there is one McpServer per connection (stdio), per HTTP session, and
  /// one shared for all stateless traffic — so the caps are per
  /// session/stateless identity, exactly as the research plan requires.
  McpTokenBucket toolCalls_{kMcpToolCallLimitPerSec, kMcpToolCallBurst};
  McpTokenBucket blockWrites_{kMcpBlockWriteLimitPerSec, kMcpBlockWriteBurst};
  /// Player-control frames, keyed by pilot id (multi-pilot, locked
  /// 2026-10-11): each pilot gets its own 120/sec sustained, burst 240.
  /// The frame pump is the natural limiter (one staged batch per frame per
  /// pilot), but this bucket stops a model vibrating a player with
  /// press/release spam at wire speed. Lazily created; adding a pilot is
  /// configuration, not surgery.
  std::map<std::string, McpTokenBucket> controlBuckets_;
  /// Bucket for `pilot`, creating it (full) on first use.
  McpTokenBucket& controlBucket(const std::string& pilot);
  /// Pilot-sense buckets, keyed by pilot id (multi-pilot, locked
  /// 2026-10-11): vision 4/sec burst 8, audio 8/sec burst 16 per pilot.
  /// Frames are the most expensive thing an agent can request; the vision
  /// cap keeps a curious model from melting the machine.
  std::map<std::string, McpTokenBucket> visionBuckets_;
  std::map<std::string, McpTokenBucket> audioBuckets_;
  McpTokenBucket& visionBucket(const std::string& pilot);
  McpTokenBucket& audioBucket(const std::string& pilot);
};

/// Canonical manifest name for one kPerm* bit (mirrors
/// mods/mod_permissions.h's canonical names; unknown bits render as hex).
/// Used in permission-denied messages so the agent (and the approving human)
/// sees WHICH bit is missing.
std::string permBitName(uint32_t bit);
/// Comma-joined names for a mask, e.g. "world_read, console".
std::string permMaskNames(uint32_t mask);

/// Compile-time engine version "major.minor.patch" from net/version.h.
std::string mcpEngineVersionString();

}  // namespace mcp
}  // namespace litholattice
