// http_server.cpp: Phase 2 Streamable HTTP transport for the MCP server.
// Clean-room; written from the MCP Streamable HTTP transport rules and the
// HTTP/1.1 wire format, not from any existing implementation.
#include "litholattice/mcp/http_server.h"

#include <cctype>
#include <chrono>

#include "litholattice/mcp/json.h"
#include "litholattice/mcp/jsonrpc.h"

#ifdef _WIN32
// UNVERIFIED Windows branch (builder to verify).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <fcntl.h>
#include <sys/random.h>
#include <unistd.h>
#endif

namespace litholattice::mcp {
namespace {

std::string hexEncode(const std::uint8_t* data, std::size_t n) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(kDigits[data[i] >> 4]);
    out.push_back(kDigits[data[i] & 0xf]);
  }
  return out;
}

bool startsWithI(std::string_view s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(s[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i])))
      return false;
  return true;
}

// Handshake-era session bounds: at most kMaxHttpSessions live sessions (LRU
// eviction past that), and a session idle longer than kHttpSessionIdleTimeout
// expires. Localhost-only + bearer auth keeps the finding LOW, but an
// authenticated client could otherwise grow the table forever.
constexpr std::size_t kMaxHttpSessions = 64;
constexpr std::chrono::seconds kHttpSessionIdleTimeout{1800};  // 30 minutes

// --- F-L9: Origin/Host allowlist (defence in depth, checked BEFORE auth) ---
// The 256-bit Bearer token already defeats DNS rebinding in practice (a
// browser cannot set Authorization cross-origin without a CORS preflight
// this server never grants), but a rebound/foreign Origin or Host is still
// refused outright: the only legitimate callers are same-machine clients
// addressing the loopback bind. Absent headers are allowed (non-browser
// clients may omit Origin entirely); a PRESENT header must name the
// loopback bind (or the explicitly bound address) and, when it carries a
// port and the bound port is known, that port.
std::string lowerCopy(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

/// Split a Host header / Origin authority into (hostname, port-or-empty).
/// Handles [ipv6]:port. Returns false on malformed input.
bool splitAuthority(std::string_view auth, std::string& host, std::string& port) {
  host.clear();
  port.clear();
  if (auth.empty()) return false;
  if (auth.front() == '[') {
    const size_t close = auth.find(']');
    if (close == std::string_view::npos) return false;
    host = std::string(auth.substr(1, close - 1));
    if (close + 1 < auth.size()) {
      if (auth[close + 1] != ':') return false;
      port = std::string(auth.substr(close + 2));
    }
    return !host.empty();
  }
  const size_t colon = auth.rfind(':');
  if (colon != std::string_view::npos && auth.find(':') == colon) {
    host = std::string(auth.substr(0, colon));
    port = std::string(auth.substr(colon + 1));
  } else {
    host = std::string(auth);  // bare hostname or unbracketed IPv6 (no port)
  }
  return !host.empty();
}

bool authorityAllowed(std::string_view authority, const std::string& boundAddr,
                      std::uint16_t boundPort) {
  std::string host, port;
  if (!splitAuthority(authority, host, port)) return false;
  const std::string h = lowerCopy(host);
  const bool loopback = h == "127.0.0.1" || h == "::1" || h == "localhost";
  const bool bound = !boundAddr.empty() && h == lowerCopy(boundAddr);
  if (!loopback && !bound) return false;
  if (!port.empty() && boundPort != 0) {
    std::uint32_t p = 0;
    for (char c : port) {
      if (c < '0' || c > '9') return false;
      p = p * 10 + static_cast<std::uint32_t>(c - '0');
      if (p > 65535) return false;
    }
    if (p != boundPort) return false;
  }
  return true;
}

bool originAllowed(std::string_view origin, const std::string& boundAddr,
                   std::uint16_t boundPort) {
  // Origin = scheme "://" authority (a path is never part of an Origin).
  const size_t sep = origin.find("://");
  if (sep == std::string_view::npos) return false;  // includes "null" origins
  const std::string scheme = lowerCopy(origin.substr(0, sep));
  if (scheme != "http" && scheme != "https") return false;
  return authorityAllowed(origin.substr(sep + 3), boundAddr, boundPort);
}

}  // namespace

bool cryptoRandomBytes(std::uint8_t* out, std::size_t n) {
#ifdef _WIN32
  // UNVERIFIED: the Windows builder must compile-check this path.
  return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
  std::size_t done = 0;
  while (done < n) {
    // getrandom(2): blocks only until the CSPRNG is seeded, then never.
    ssize_t r = ::getrandom(out + done, n - done, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      break;  // ENOSYS or other: fall through to /dev/urandom
    }
    done += static_cast<std::size_t>(r);
  }
  if (done == n) return true;
  // Fallback for kernels/libcs without getrandom: /dev/urandom.
  int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  while (done < n) {
    ssize_t r = ::read(fd, out + done, n - done);
    if (r < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return false;
    }
    if (r == 0) {
      ::close(fd);
      return false;
    }
    done += static_cast<std::size_t>(r);
  }
  ::close(fd);
  return true;
#endif
}

bool constantTimeEqual(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  unsigned diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i)
    diff |= static_cast<unsigned>(a[i] ^ b[i]);
  return diff == 0;
}

bool isLoopbackBindAddress(std::string_view addr) {
  return addr == "127.0.0.1" || addr == "::1" || addr == "localhost";
}

// ---------------------------------------------------------------------------
// McpHttpServer
// ---------------------------------------------------------------------------

McpHttpServer::McpHttpServer(McpBackends backends, std::string agentName)
    : backends_(std::move(backends)), agentName_(std::move(agentName)) {
  std::uint8_t raw[32];
  if (!cryptoRandomBytes(raw, sizeof(raw))) {
    // Fail-fast: a token we cannot make random is worse than no server.
    // Leave the token empty; start() refuses to run without one.
    log("mcp: http FATAL: OS CSPRNG unavailable, bearer token not generated");
    return;
  }
  bearerToken_ = hexEncode(raw, sizeof(raw));
  // Phase 3: the shared stateless-era server. One agent identity for every
  // stateless client (no session to key per-client masks on); deny-by-
  // default like every other connection. Its log sink forwards through
  // McpHttpServer::log so it always tracks the current game sink without
  // a data race on reassignment.
  statelessServer_ =
      std::make_unique<McpServer>(backends_, McpAgent{agentName_ + "/stateless", 0});
  statelessServer_->setLogSink([this](std::string_view line) { log(line); });
}

McpHttpServer::~McpHttpServer() { stop(); }

bool McpHttpServer::start(std::uint16_t port, const std::string& bindAddr) {
  if (running_.load()) {
    log("mcp: http start: already running");
    return false;
  }
  if (bearerToken_.empty()) {
    log("mcp: http start: refused, no bearer token (CSPRNG failed)");
    return false;
  }
  if (!isLoopbackBindAddress(bindAddr)) {
    // LOUD on purpose: fail-open is never silent. No TLS in v1 — remote
    // exposure wants a tunnel (research report §6).
    log("mcp: http WARNING: binding non-loopback address '" + bindAddr +
        "'; the bearer token is the ONLY auth and there is no TLS in v1. "
        "Prefer 127.0.0.1 or a tunnel.");
  }
  TcpListener listener;
  std::string err;
  if (!listener.bind(bindAddr, port, err)) {
    // Fail-fast: a taken port is an error, never a silent re-pick.
    log("mcp: http start failed: " + err);
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = std::move(listener);
    boundAddress_ = listener_.boundAddress();
    boundPort_ = listener_.boundPort();
  }
  running_.store(true);
  thread_ = std::thread(&McpHttpServer::acceptLoop, this);
  return true;
}

void McpHttpServer::stop() {
  bool was = running_.exchange(false);
  // Closing the listener wakes the poll (best effort); the 50 ms poll tick
  // in accept() guarantees the thread notices stop() promptly regardless.
  listener_.close();
  if (was && thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.clear();
}

std::string McpHttpServer::token() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return bearerToken_;
}

std::string McpHttpServer::rotateToken() {
  std::uint8_t raw[32];
  std::string next;
  if (cryptoRandomBytes(raw, sizeof(raw))) next = hexEncode(raw, sizeof(raw));
  {
    // Scoped: log() takes the same mutex, so it must not be held here.
    std::lock_guard<std::mutex> lock(mutex_);
    if (!next.empty()) bearerToken_ = next;
  }
  // else: keep the old token; a failed rotation must not lock everyone out.
  log("mcp: http bearer token rotated");
  std::lock_guard<std::mutex> lock(mutex_);
  return bearerToken_;
}

bool McpHttpServer::grantPermissions(const std::string& sessionId,
                                     std::uint32_t bits) {
  if (McpServer* s = findSession(sessionId)) {
    s->grantPermissions(bits);
    return true;
  }
  return false;
}

bool McpHttpServer::revokePermissions(const std::string& sessionId,
                                      std::uint32_t bits) {
  if (McpServer* s = findSession(sessionId)) {
    s->revokePermissions(bits);
    return true;
  }
  return false;
}

bool McpHttpServer::grantStatelessPermissions(std::uint32_t bits) {
  // Copy the pointer under the lock, then grant without holding it:
  // McpServer::grantPermissions is internally synchronized, and holding
  // the HTTP mutex here would deadlock against the server's log sink
  // (which forwards through McpHttpServer::log, i.e. this same mutex).
  McpServer* s = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    s = statelessServer_.get();
  }
  if (!s) return false;
  s->grantPermissions(bits);
  return true;
}

bool McpHttpServer::revokeStatelessPermissions(std::uint32_t bits) {
  McpServer* s = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    s = statelessServer_.get();
  }
  if (!s) return false;
  s->revokePermissions(bits);
  return true;
}

std::uint32_t McpHttpServer::statelessPermissionMask() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return statelessServer_ ? statelessServer_->permissionMask() : 0;
}

std::vector<std::string> McpHttpServer::sessionIds() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> ids;
  ids.reserve(sessions_.size());
  for (const auto& kv : sessions_) ids.push_back(kv.first);
  return ids;
}

std::string McpHttpServer::boundAddress() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return boundAddress_;
}

std::uint16_t McpHttpServer::boundPort() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return boundPort_;
}

void McpHttpServer::log(std::string_view line) {
  LogSink sink;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sink = logSink_;
  }
  if (sink) sink(line);
}

// --- sessions --------------------------------------------------------------

std::string McpHttpServer::createSession() {
  std::uint8_t raw[16];
  std::string id;
  if (cryptoRandomBytes(raw, sizeof(raw))) {
    id = hexEncode(raw, sizeof(raw));
  } else {
    // Should not happen (token generation already proved the CSPRNG), but
    // fail closed: no session without real randomness.
    log("mcp: http FATAL: CSPRNG failed during session creation");
    return {};
  }
  auto server =
      std::make_unique<McpServer>(backends_, McpAgent{agentName_, 0});
  std::vector<std::string> expiredIds;
  std::string evictedId;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logSink_) server->setLogSink(logSink_);
    const auto now = std::chrono::steady_clock::now();
    sessions_.emplace(id, Session{std::move(server), now});
    // Idle expiry first: session churn reaps dead sessions.
    for (auto it = sessions_.begin(); it != sessions_.end();) {
      if (now - it->second.lastActivity > kHttpSessionIdleTimeout) {
        expiredIds.push_back(it->first);
        it = sessions_.erase(it);
      } else {
        ++it;
      }
    }
    // Then the LRU cap: the brand-new session carries the newest stamp, so
    // it can never be the victim.
    while (sessions_.size() > kMaxHttpSessions) {
      auto lru = sessions_.begin();
      for (auto it = std::next(sessions_.begin()); it != sessions_.end(); ++it) {
        if (it->second.lastActivity < lru->second.lastActivity) lru = it;
      }
      evictedId = lru->first;
      sessions_.erase(lru);
    }
  }
  // log() takes the mutex: never under the lock (see rotateToken).
  for (const auto& eid : expiredIds) log("mcp: http session expired (idle): " + eid);
  if (!evictedId.empty()) log("mcp: http session evicted (LRU cap): " + evictedId);
  log("mcp: http session created: " + id);
  return id;
}

McpServer* McpHttpServer::findSession(const std::string& id) {
  std::string expiredId;
  McpServer* out = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sessions_.find(id);
    if (it != sessions_.end()) {
      const auto now = std::chrono::steady_clock::now();
      if (now - it->second.lastActivity > kHttpSessionIdleTimeout) {
        expiredId = it->first;  // fail-closed: the client re-handshakes
        sessions_.erase(it);
      } else {
        it->second.lastActivity = now;  // every authed use keeps it alive
        out = it->second.server.get();
      }
    }
  }
  if (!expiredId.empty()) log("mcp: http session expired (idle): " + expiredId);
  return out;
}

void McpHttpServer::eraseSession(const std::string& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.erase(id);
}

bool McpHttpServer::checkAuth(const HttpRequest& req) const {
  std::string_view auth = req.headerValue("authorization");
  if (!startsWithI(auth, "Bearer ")) return false;
  std::string_view presented = auth.substr(7);
  std::lock_guard<std::mutex> lock(mutex_);
  return constantTimeEqual(presented, bearerToken_) && !bearerToken_.empty();
}

std::string McpHttpServer::rpcMethodOf(std::string_view body) {
  ParseResult pr = parseJson(body);
  if (!pr.ok || !pr.value.isObject()) return {};
  const JsonValue* m = pr.value.find("method");
  if (!m || !m->isString()) return {};
  return m->asString();
}

// --- request handling -------------------------------------------------------

HttpResponse McpHttpServer::statelessRpcError(const JsonValue& id, int code,
                                             const std::string& message,
                                             const JsonValue& data) {
  HttpResponse r;
  // -32601 -> 404: a dual-era client tells "no such method" (modern server,
  // unknown method) apart from "bad request". Every other protocol-level
  // error -> 400. (Handshake-era sessions keep their Phase 2 behavior:
  // JSON-RPC errors ride inside a 200.)
  r.status = (code == kRpcMethodNotFound) ? 404 : 400;
  r.setHeader("Content-Type", "application/json");
  r.body = buildErrorResponse(id, code, message, data).serialize();
  return r;
}

HttpResponse McpHttpServer::handleStatelessRequest(const HttpRequest& req,
                                                   const JsonValue& body,
                                                   const std::string& method) {
  std::string agentLog = "unauthenticated";
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (statelessServer_) agentLog = statelessServer_->agent().name;
  }
  auto audit = [&](int status) {
    log("mcp: http " + req.method + " " + req.target +
        " session=stateless agent=" + agentLog + " method=" + method +
        " -> " + std::to_string(status));
  };
  const JsonValue* idv = body.find("id");
  const JsonValue id = idv ? *idv : JsonValue::null();
  const JsonValue* params = body.find("params");

  // SEP-2243: Streamable HTTP requests mirror the era in headers so
  // infrastructure can route without parsing the body. Missing or
  // mismatched -> -32020 (fail-closed: a mismatch means a proxy could route
  // on one value while the server acts on another).
  if (std::string(req.headerValue("mcp-protocol-version")) !=
      kMcpProtocolVersionStateless) {
    HttpResponse r = statelessRpcError(
        id, kRpcHeaderMismatch,
        "MCP-Protocol-Version must be '2026-07-28' and match params._meta");
    audit(r.status);
    return r;
  }
  if (std::string(req.headerValue("mcp-method")) != method) {
    HttpResponse r = statelessRpcError(
        id, kRpcHeaderMismatch, "Mcp-Method must match the JSON-RPC method");
    audit(r.status);
    return r;
  }
  if (method == "tools/call" || method == "prompts/get" ||
      method == "resources/read") {
    const char* field = (method == "resources/read") ? "uri" : "name";
    const JsonValue* nv =
        (params && params->isObject()) ? params->find(field) : nullptr;
    if (!nv || !nv->isString() ||
        std::string(req.headerValue("mcp-name")) != nv->asString()) {
      HttpResponse r = statelessRpcError(
          id, kRpcHeaderMismatch,
          std::string("Mcp-Name must match params.") + field);
      audit(r.status);
      return r;
    }
  }

  // Dispatch on the shared stateless server. McpServer::handleMessage owns
  // the stateless envelope (resultType + _meta identity), so stdio and HTTP
  // stay one code path — the transport only adds headers and status codes.
  std::optional<std::string> rpcResp;
  try {
    McpServer* s = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      s = statelessServer_.get();
    }
    if (!s) {
      HttpResponse r = makeErrorResponse(500, "stateless server unavailable");
      audit(500);
      return r;
    }
    rpcResp = s->handleMessage(req.body);
  } catch (const std::exception& e) {
    // handleMessage is not supposed to throw std::exception (the router
    // converts), so this is a backend bug: log it, 500, no session to keep.
    log(std::string("mcp: http stateless handler exception: ") + e.what());
    HttpResponse r =
        statelessRpcError(id, kRpcInternalError, "internal error");
    r.status = 500;
    audit(500);
    return r;
  }

  HttpResponse r;
  if (!rpcResp) {
    r.status = 202;  // JSON-RPC notification: nothing to send back.
    r.setHeader("Content-Type", "application/json");
  } else if (req.wantsEventStream()) {
    // Streamable HTTP: the single response as an SSE `message` event.
    SseStream sse;
    sse.pushEvent("message", *rpcResp);
    r = SseStream::streamResponse(sse.bytes());
  } else {
    int rpcCode = 0;
    ParseResult pr = parseJson(*rpcResp);
    if (pr.ok && pr.value.isObject()) {
      const JsonValue* err = pr.value.find("error");
      const JsonValue* code = err ? err->find("code") : nullptr;
      if (code && code->isInt()) rpcCode = static_cast<int>(code->asInt());
    }
    r.status = (rpcCode == kRpcMethodNotFound)
                   ? 404
                   : (rpcCode != 0 ? 400 : 200);
    r.setHeader("Content-Type", "application/json");
    r.body = *rpcResp;
  }
  // Never mint Mcp-Session-Id on the stateless path (SEP-2567).
  audit(r.status);
  return r;
}

HttpResponse McpHttpServer::handleRequest(const HttpRequest& req) {
  std::string sessionLog = "-";
  std::string agentLog = "unauthenticated";
  std::string methodLog;

  auto audit = [&](int status) {
    std::string line = "mcp: http " + req.method + " " + req.target +
                       " session=" + sessionLog + " agent=" + agentLog +
                       " -> " + std::to_string(status);
    if (!methodLog.empty()) line += " method=" + methodLog;
    log(line);
  };

  // 1. Routing: POST /mcp is the RPC endpoint; DELETE /mcp terminates a
  //    handshake-era session (MCP Streamable HTTP session lifecycle).
  if (req.target != kMcpHttpPath) {
    HttpResponse r = makeErrorResponse(404, "not found: only POST and DELETE /mcp exist");
    audit(404);
    return r;
  }
  // 1b. F-L9 Origin/Host allowlist, BEFORE auth (defence in depth against
  //     DNS rebinding; see the helpers' comment). Absent headers pass.
  {
    const std::string bAddr = boundAddress();
    const std::uint16_t bPort = boundPort();
    const std::string_view origin = req.headerValue("origin");
    const std::string_view host = req.headerValue("host");
    if ((!origin.empty() && !originAllowed(origin, bAddr, bPort)) ||
        (!host.empty() && !authorityAllowed(host, bAddr, bPort))) {
      HttpResponse r = makeErrorResponse(403, "Origin/Host not allowed for this server");
      log("mcp: http Origin/Host check failed (possible DNS-rebinding attempt)");
      audit(403);
      return r;
    }
  }
  if (req.method == "DELETE") {
    // Explicit session close: this wires up eraseSession (it had zero
    // callers). Authenticated only — an unauthenticated client must not be
    // able to kill sessions. Unknown id -> 404, fail-closed.
    if (!checkAuth(req)) {
      HttpResponse r = makeErrorResponse(401, "missing or invalid Bearer <redacted>");
      r.setHeader("WWW-Authenticate", "Bearer <redacted>\"litholattice-mcp\"");
      log("mcp: http auth failure on DELETE /mcp (bad or missing Bearer <redacted>)");
      audit(401);
      return r;
    }
    const std::string sid(req.headerValue("mcp-session-id"));
    sessionLog = sid.empty() ? "-" : sid;
    if (sid.empty() || !findSession(sid)) {
      HttpResponse r = makeErrorResponse(404, "unknown Mcp-Session-Id");
      audit(404);
      return r;
    }
    eraseSession(sid);
    log("mcp: http session closed by client: " + sid);
    HttpResponse r;
    r.status = 204;
    audit(204);
    return r;
  }
  if (req.method != "POST") {
    HttpResponse r;
    if (req.method == "GET") {
      // Reserved: the Streamable HTTP SSE stream endpoint is a later phase.
      r = makeErrorResponse(
          405, "GET /mcp is reserved for a future SSE stream endpoint");
    } else {
      r = makeErrorResponse(405, "method not allowed: use POST /mcp");
    }
    r.setHeader("Allow", "POST, DELETE");
    audit(405);
    return r;
  }

  // 2. Framing: no chunked in v1, Content-Length required and capped.
  if (!req.headerValue("transfer-encoding").empty()) {
    HttpResponse r =
        makeErrorResponse(501, "Transfer-Encoding is not supported in v1");
    audit(501);
    return r;
  }
  std::uint64_t contentLength = 0;
  if (!parseContentLength(req, contentLength)) {
    HttpResponse r =
        makeErrorResponse(411, "Content-Length is required on POST /mcp");
    audit(411);
    return r;
  }
  if (contentLength > kMaxHttpBodyBytes) {
    HttpResponse r = makeErrorResponse(
        413, "body exceeds 16 MiB cap (same posture as the stdio transport)");
    audit(413);
    return r;
  }
  if (req.body.size() != contentLength) {
    // Defensive: the connection reader guarantees this; a mismatch means a
    // bug or a smuggling attempt — never trust it.
    HttpResponse r = makeErrorResponse(400, "body length mismatch");
    audit(400);
    return r;
  }

  // 3. Content type.
  if (!startsWithI(req.headerValue("content-type"), "application/json")) {
    HttpResponse r =
        makeErrorResponse(415, "Content-Type must be application/json");
    audit(415);
    return r;
  }

  // 4. Bearer auth: no token = no connection.
  if (!checkAuth(req)) {
    HttpResponse r = makeErrorResponse(401, "missing or invalid bearer token");
    r.setHeader("WWW-Authenticate", "Bearer realm=\"litholattice-mcp\"");
    log("mcp: http auth failure on POST /mcp (bad or missing bearer token)");
    audit(401);
    return r;
  }

  // 5. Transport-level JSON sanity: the body must be a JSON object with a
  //    string "method". Anything finer (valid JSON-RPC shape, params, ids)
  //    is the protocol layer's job and comes back as a JSON-RPC error
  //    inside a 200. Malformed-at-the-transport -> 400, never trusted.
  methodLog = rpcMethodOf(req.body);
  if (methodLog.empty()) {
    HttpResponse r = makeErrorResponse(400, "body is not a JSON-RPC message");
    audit(400);
    return r;
  }

  // 5b. Stateless era (2026-07-28): the request carries params._meta with
  // the protocol version. No session is created, expected, or minted; the
  // shared stateless server answers. Malformed _meta / unsupported version
  // is a JSON-RPC error (400), never a silent fallback to the legacy path.
  {
    ParseResult pr = parseJson(req.body);
    if (pr.ok && pr.value.isObject()) {
      const JsonValue& body = pr.value;
      const JsonValue* params = body.find("params");
      McpEra era = McpEra::kLegacy;
      bool eraOk = true;
      int eraCode = 0;
      std::string eraMsg;
      JsonValue eraData = JsonValue::null();
      try {
        era = classifyRequestEra(params ? *params : JsonValue::null());
      } catch (const RpcError& e) {
        eraOk = false;
        eraCode = e.code();
        eraMsg = e.message();
        eraData = e.data();
      }
      if (!eraOk) {
        const JsonValue* idv = body.find("id");
        HttpResponse r = statelessRpcError(
            idv ? *idv : JsonValue::null(), eraCode, eraMsg, eraData);
        audit(r.status);
        return r;
      }
      if (era == McpEra::kStateless) {
        return handleStatelessRequest(req, body, methodLog);
      }
    }
  }

  // 6. Session lifecycle (handshake era).
  std::string sid(req.headerValue("mcp-session-id"));
  McpServer* session = nullptr;
  if (sid.empty()) {
    // Only initialize may open a session.
    if (methodLog != "initialize") {
      HttpResponse r = makeErrorResponse(
          400, "missing Mcp-Session-Id: send 'initialize' first");
      audit(400);
      return r;
    }
    sid = createSession();
    if (sid.empty()) {
      HttpResponse r = makeErrorResponse(500, "could not create session");
      audit(500);
      return r;
    }
    session = findSession(sid);
  } else {
    session = findSession(sid);
    if (!session) {
      HttpResponse r =
          makeErrorResponse(404, "unknown Mcp-Session-Id");
      sessionLog = sid;
      audit(404);
      return r;
    }
  }
  sessionLog = sid;
  agentLog = session->agent().name;

  // 7. Dispatch to the session's McpServer (Phase 1 logic unchanged).
  std::optional<std::string> rpcResp;
  try {
    rpcResp = session->handleMessage(req.body);
  } catch (const std::exception& e) {
    // handleMessage is not supposed to throw std::exception (the router
    // converts), so this is a backend bug: log it, 500, keep the session.
    log(std::string("mcp: http handler exception: ") + e.what());
    JsonValue errVal =
        buildErrorResponse(JsonValue::null(), kRpcInternalError,
                           "internal error");
    HttpResponse r;
    r.status = 500;
    r.setHeader("Content-Type", "application/json");
    r.setHeader("Mcp-Session-Id", sid);
    r.body = errVal.serialize();
    audit(500);
    return r;
  }

  // 8. Shape the HTTP response.
  HttpResponse r;
  if (!rpcResp) {
    // JSON-RPC notification: nothing to send back.
    r.status = 202;
    r.setHeader("Content-Type", "application/json");
  } else if (req.wantsEventStream()) {
    // Streamable HTTP: the single response as an SSE `message` event.
    SseStream sse;
    sse.pushEvent("message", *rpcResp);
    r = SseStream::streamResponse(sse.bytes());
  } else {
    r.status = 200;
    r.setHeader("Content-Type", "application/json");
    r.body = *rpcResp;
  }
  r.setHeader("Mcp-Session-Id", sid);
  audit(r.status);
  return r;
}

// --- connection serving ----------------------------------------------------

McpHttpServer::ReadOutcome McpHttpServer::readRequest(TcpConn& conn,
                                                     std::string& buf) {
  ReadOutcome out;
  // Phase A: headers.
  while (true) {
    HttpParseOutcome pr = parseHttpHeaders(buf);
    if (pr.status == HttpParseStatus::kNeedMore) {
      if (buf.size() > kMaxHttpHeaderBytes + 4) {
        out.errorResp = makeErrorResponse(431, "header block too large");
        out.hasErrorResp = true;
        out.closeAfter = true;
        return out;
      }
      std::string chunk = conn.readSome(8192);
      if (chunk.empty()) return out;  // peer closed or timed out
      buf += std::move(chunk);
      continue;
    }
    if (pr.status == HttpParseStatus::kError) {
      out.errorResp = makeErrorResponse(400, "malformed HTTP request");
      out.hasErrorResp = true;
      out.closeAfter = true;
      return out;
    }
    out.request = std::move(pr.request);
    buf.erase(0, pr.consumed);
    break;
  }
  // Phase B: body via Content-Length.
  std::uint64_t contentLength = 0;
  if (!parseContentLength(out.request, contentLength)) {
    out.errorResp =
        makeErrorResponse(411, "Content-Length is required on POST /mcp");
    out.hasErrorResp = true;
    out.closeAfter = true;
    return out;
  }
  if (contentLength > kMaxHttpBodyBytes) {
    // Fail closed: respond and drop the connection without reading the
    // (huge) body off the wire.
    out.errorResp = makeErrorResponse(413, "body exceeds 16 MiB cap");
    out.hasErrorResp = true;
    out.closeAfter = true;
    return out;
  }
  while (buf.size() < contentLength) {
    std::string chunk = conn.readSome(65536);
    if (chunk.empty()) return out;  // peer closed or timed out mid-body
    buf += std::move(chunk);
    if (buf.size() > kMaxHttpBodyBytes + 1) {
      out.errorResp = makeErrorResponse(413, "body exceeds 16 MiB cap");
      out.hasErrorResp = true;
      out.closeAfter = true;
      return out;
    }
  }
  out.request.body.assign(buf.data(), static_cast<std::size_t>(contentLength));
  buf.erase(0, static_cast<std::size_t>(contentLength));
  out.ok = true;
  return out;
}

void McpHttpServer::serveConnection(TcpConn& conn) {
  std::string buf;  // carry-over for pipelined requests
  for (int i = 0; i < kMaxHttpRequestsPerConn; ++i) {
    ReadOutcome ro = readRequest(conn, buf);
    if (!ro.ok) {
      if (ro.hasErrorResp) conn.sendAll(ro.errorResp.serialize());
      return;  // peer gone, timeout, or framing refusal (closeAfter)
    }
    HttpResponse resp = handleRequest(ro.request);
    // Framing refusals at the handleRequest layer are per-request; the
    // connection reader already closed on the dangerous ones (413/431).
    if (!conn.sendAll(resp.serialize())) return;
    if (!ro.request.keepAlive()) return;
  }
}

void McpHttpServer::acceptLoop() {
  log("mcp: http listening on " + boundAddress() + ":" +
      std::to_string(boundPort()));
  while (running_.load()) {
    TcpConn conn;
    std::string err;
    if (!listener_.accept(conn, 50, err)) {
      if (!err.empty()) log("mcp: http accept: " + err);
      continue;  // timeout tick (or transient failure): re-check running_
    }
    serveConnection(conn);
  }
  log("mcp: http accept loop stopped");
}

}  // namespace litholattice::mcp
