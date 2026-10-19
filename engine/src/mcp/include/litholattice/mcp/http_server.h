// http_server.h: Phase 2 Streamable HTTP transport for the MCP server,
// extended in Phase 3 for the stateless era (2026-07-28).
//
// One POST /mcp endpoint (default 127.0.0.1:1337, JD-locked 2026-10-11). Dual-era:
// - Handshake era (2025-11-25): JSON-RPC request in -> JSON-RPC response
//   out as application/json; when the client sends `Accept:
//   text/event-stream` the single response is delivered as an SSE `message`
//   event instead. Sessions via Mcp-Session-Id (initialize mints one).
// - Stateless era (2026-07-28): the request carries params._meta with the
//   protocol version; NO Mcp-Session-Id is minted, expected, or required.
//   The mirrored headers MCP-Protocol-Version / Mcp-Method / Mcp-Name are
//   validated against the body (SEP-2243); a missing/mismatched header is
//   JSON-RPC -32020. All stateless requests share ONE McpServer (one agent
//   identity) — there is no session to key per-client permission masks on,
//   so grants target the shared stateless agent via
//   grantStatelessPermissions().
//
// Security posture (research report §6):
//   - Bearer token auth: a 256-bit token is generated per server run from
//     the OS CSPRNG (cryptoRandomBytes). EVERY POST /mcp without a valid
//     `Authorization: Bearer <token>` -> 401. No token = no connection. The
//     token is shown ONCE in the debug console by the game; it is never
//     written to the log. rotateToken() replaces it (tests + future
//     `mcp rotate-token` console command).
//   - Bind: 127.0.0.1 only by default. The bind address is configurable,
//     but any non-loopback bind logs a LOUD warning — fail-open is never
//     silent. No TLS in v1: remote exposure wants a tunnel (documented,
//     not rolled here).
//   - Origin/Host allowlist (F-L9): a present Origin or Host header must
//     name the loopback bind (or the explicitly bound address, port-checked
//     when known) — checked before auth, 403 otherwise. Defence in depth
//     against DNS rebinding; the Bearer token remains the real gate.
//   - Sessions: `initialize` with no Mcp-Session-Id creates a session;
//     every later request must carry it. Unknown session id -> 404,
//     missing session id on a non-initialize request -> 400. Each session
//     owns one McpServer (Phase 1 model: one server per connection) tied to
//     one McpAgent identity; permission grants flow through the same
//     grantPermissions/revokePermissions the stdio console flow uses.
//   - Transport fails closed: malformed HTTP -> 400, oversized body -> 413
//     (16 MiB cap, same as stdio), chunked -> 501, wrong content type ->
//     415, port taken -> start() fails loudly, never picks another port.
//
// Threading (locked): ONE accept/dispatch thread (see sockets.h). All
// handleMessage() calls serialize on it, so the per-session McpServer
// instances never see concurrent dispatch. grantPermissions/revokePermissions
// are safe to call from the console thread (McpServer synchronizes the
// agent mask internally); the session map itself is mutex-guarded.
//
// SSE seam: Phase 1 has no server-initiated notifications, so there is no
// live event push yet. SseStream (http.h) formats spec-correct frames today
// and is self-tested; a later phase wires server->client notifications
// through it (GET /mcp currently answers 405 with Allow: POST).
//
// Clean-room; no code taken from any socket/HTTP implementation.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "litholattice/mcp/http.h"
#include "litholattice/mcp/server.h"
#include "litholattice/mcp/sockets.h"

namespace litholattice::mcp {

/// Default port, JD-locked 2026-10-11: 1337.
inline constexpr std::uint16_t kMcpDefaultPort = 1337;
/// The only path served.
inline constexpr const char* kMcpHttpPath = "/mcp";
/// Default (and recommended) bind address.
inline constexpr const char* kMcpLoopbackAddr = "127.0.0.1";
/// Max sequential requests served on one keep-alive connection.
inline constexpr int kMaxHttpRequestsPerConn = 256;

/// Fill `out[0..n)` with cryptographically random bytes.
/// POSIX: getrandom(2), falling back to /dev/urandom. _WIN32:
/// BCryptGenRandom — UNVERIFIED, the Windows builder must compile-check it.
///
/// (Checked net/makeNonce first per the Phase 2 brief: it is a
/// deterministic ChaCha20 packet-nonce builder from (sessionId, counter),
/// not randomness — deliberately NOT reused for tokens/session ids.)
bool cryptoRandomBytes(std::uint8_t* out, std::size_t n);

/// Constant-time comparison for bearer tokens (no early-out on length
/// mismatch is still observed, but content comparison leaks nothing).
bool constantTimeEqual(std::string_view a, std::string_view b);

/// True for loopback bind addresses ("127.0.0.1", "::1", "localhost").
/// Anything else triggers the loud non-loopback warning in start().
bool isLoopbackBindAddress(std::string_view addr);

class McpHttpServer {
 public:
  /// `backends` are cloned (shared_ptr members) into one McpServer per
  /// session; `agentName` seeds each session's McpAgent identity.
  explicit McpHttpServer(McpBackends backends,
                         std::string agentName = "http-agent");
  ~McpHttpServer();

  McpHttpServer(const McpHttpServer&) = delete;
  McpHttpServer& operator=(const McpHttpServer&) = delete;

  /// Start listening and spawn the accept/dispatch thread. Fail-fast:
  /// returns false (and logs) if the port is taken — the server NEVER
  /// silently picks another port. A non-loopback bindAddr logs a LOUD
  /// warning before binding.
  bool start(std::uint16_t port = kMcpDefaultPort,
             const std::string& bindAddr = kMcpLoopbackAddr);
  /// Stop the accept thread and close the listener. Idempotent.
  void stop();
  bool running() const { return running_.load(); }

  /// Current bearer token (hex). The game shows it ONCE in the debug
  /// console; it is never written to the log.
  std::string token() const;
  /// Replace the token; returns the new one. In-flight sessions keep
  /// working — only new HTTP requests must present the new token.
  std::string rotateToken();

  // -- console approval flow (one McpAgent identity per session) ------------
  /// Grant/revoke kPerm* bits on a session's agent (cf.
  /// McpServer::grantPermissions). Thread-safe; false when the session id
  /// is unknown.
  bool grantPermissions(const std::string& sessionId, std::uint32_t bits);
  bool revokePermissions(const std::string& sessionId, std::uint32_t bits);
  std::vector<std::string> sessionIds() const;

  // -- stateless-era agent (Phase 3) -----------------------------------------
  /// Grant/revoke kPerm* bits on the SHARED stateless agent (all stateless
  /// requests are served by one McpServer — there is no session to key
  /// per-client masks on). Thread-safe. The console command is
  /// `mcp grant stateless <perm...>` (see engine/docs/MCP-GAME-SEAM.md).
  bool grantStatelessPermissions(std::uint32_t bits);
  bool revokeStatelessPermissions(std::uint32_t bits);
  std::uint32_t statelessPermissionMask() const;

  void setLogSink(LogSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    logSink_ = std::move(sink);
  }

  // -- unit-testable core (no sockets) --------------------------------------
  /// Full HTTP semantics for one parsed request: routing, auth, session
  /// lifecycle, JSON-RPC dispatch, SSE negotiation. Used by the accept
  /// thread and directly by self-tests.
  HttpResponse handleRequest(const HttpRequest& req);

  /// What start() actually bound (asserted by tests: loopback by default).
  std::string boundAddress() const;
  std::uint16_t boundPort() const;

 private:
  void acceptLoop();
  /// Serve sequential requests on one connection until the peer closes,
  /// keep-alive ends, kMaxHttpRequestsPerConn is hit, or a fatal framing
  /// error occurs (413/431 close the connection after responding).
  void serveConnection(TcpConn& conn);
  /// Read one full request (headers + Content-Length body). False when the
  /// peer closed, timed out, or sent framing this server refuses; `resp`
  /// carries the error response to send (if any) and whether to close.
  struct ReadOutcome {
    bool ok = false;
    bool closeAfter = false;
    HttpRequest request;
    HttpResponse errorResp;
    bool hasErrorResp = false;
  };
  ReadOutcome readRequest(TcpConn& conn, std::string& buf);

  /// Session map helpers (caller need not hold the mutex; taken inside).
  /// findSession touches the session's last-activity stamp (every authed
  /// request keeps it alive) and expires it fail-closed past the idle
  /// timeout; createSession sweeps expired sessions and evicts LRU past the
  /// session cap.
  std::string createSession();
  McpServer* findSession(const std::string& id);
  void eraseSession(const std::string& id);

  bool checkAuth(const HttpRequest& req) const;
  /// Extract the JSON-RPC method name from a body, or empty when the body
  /// is not a JSON-RPC request object (fail-fast at the transport layer).
  static std::string rpcMethodOf(std::string_view body);
  /// Stateless-era dispatch (2026-07-28): header validation (SEP-2243),
  /// then McpServer::handleMessage on the shared stateless server. No
  /// session is created, expected, or minted.
  HttpResponse handleStatelessRequest(const HttpRequest& req,
                                      const JsonValue& body,
                                      const std::string& method);
  /// JSON-RPC error body for the stateless path: -32601 -> HTTP 404 (so a
  /// dual-era client can tell "no such method" from "bad request"), other
  /// protocol errors -> HTTP 400.
  static HttpResponse statelessRpcError(const JsonValue& id, int code,
                                        const std::string& message,
                                        const JsonValue& data = JsonValue::null());
  void log(std::string_view line);

  McpBackends backends_;
  std::string agentName_;
  TcpListener listener_;
  std::thread thread_;
  std::atomic<bool> running_{false};

  /// One handshake-era session: the per-agent server plus its last-activity
  /// stamp. Every authed request touches the stamp (findSession); a session
  /// idle past the timeout expires fail-closed (the client re-handshakes),
  /// and the table is LRU-capped (see createSession).
  struct Session {
    std::unique_ptr<McpServer> server;
    std::chrono::steady_clock::time_point lastActivity{};
  };

  mutable std::mutex mutex_;
  std::map<std::string, Session> sessions_;
  /// Phase 3: the shared stateless-era server (one agent identity for every
  /// stateless client). Used only on the accept/dispatch thread; grants are
  /// internally synchronized like every McpServer.
  std::unique_ptr<McpServer> statelessServer_;
  std::string bearerToken_;
  LogSink logSink_;
  std::string boundAddress_;
  std::uint16_t boundPort_ = 0;
};

}  // namespace litholattice::mcp
