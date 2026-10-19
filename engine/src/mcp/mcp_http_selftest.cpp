// mcp_http_selftest.cpp: Phase 2 self-tests — strict HTTP/1.1 parser,
// SSE framing, bearer-token auth, handshake-era session lifecycle, and a
// live-loopback integration test of the accept/dispatch thread.
//
// The parser/session/auth unit tests never touch a real socket: they drive
// parseHttpHeaders() and McpHttpServer::handleRequest() directly. The live
// tests at the end use real loopback sockets (POSIX only).
#include "litholattice/mcp/mcp_selftest.h"

#include <cstdio>
#include <string>
#include <vector>

#include "litholattice/mcp/http.h"
#include "litholattice/mcp/http_server.h"
#include "litholattice/mcp/json.h"
#include "litholattice/mcp/server.h"
#include "litholattice/mcp/sockets.h"
// Cross-module header the server reads (header-only kPerm* bits).
#include "litholattice/mods/mod_interface.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace litholattice::mcp {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] mcp-http: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

const char* kInitBody =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
    R"({"protocolVersion":"2025-11-25","capabilities":{},)"
    R"("clientInfo":{"name":"t","version":"1"}}})";
const char* kToolsListBody =
    R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})";
const char* kNotifyBody =
    R"({"jsonrpc":"2.0","method":"notifications/initialized","params":{}})";

HttpRequest makePost(const std::string& body, const std::string& token,
                     const std::string& sessionId = "") {
  HttpRequest r;
  r.method = "POST";
  r.target = "/mcp";
  r.version = "HTTP/1.1";
  r.headers.push_back({"content-type", "application/json"});
  r.headers.push_back({"content-length", std::to_string(body.size())});
  if (!token.empty())
    r.headers.push_back({"authorization", "Bearer " + token});
  if (!sessionId.empty())
    r.headers.push_back({"mcp-session-id", sessionId});
  r.body = body;
  return r;
}

std::string respSessionId(const HttpResponse& r) {
  return std::string(r.headerValue("mcp-session-id"));
}

bool bodyHas(const HttpResponse& r, const std::string& needle) {
  return r.body.find(needle) != std::string::npos;
}

bool bodyIsJsonRpcResult(const HttpResponse& r, std::string* protocolOut = nullptr) {
  ParseResult pr = parseJson(r.body);
  if (!pr.ok || !pr.value.isObject()) return false;
  const JsonValue* res = pr.value.find("result");
  if (!res || !res->isObject()) return false;
  if (protocolOut) {
    const JsonValue* pv = res->find("protocolVersion");
    *protocolOut = (pv && pv->isString()) ? pv->asString() : "";
  }
  return true;
}

#ifndef _WIN32
// Minimal raw HTTP client for the live-loopback tests (POSIX only).
std::string httpRaw(const std::string& request, std::uint16_t port) {
  int s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return {};
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (::connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
    ::close(s);
    return {};
  }
  const char* p = request.data();
  std::size_t left = request.size();
  while (left > 0) {
    ssize_t n = ::send(s, p, left, MSG_NOSIGNAL);
    if (n <= 0) {
      ::close(s);
      return {};
    }
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  std::string out;
  char buf[4096];
  ssize_t n;
  while ((n = ::recv(s, buf, sizeof(buf), 0)) > 0) out.append(buf, n);
  ::close(s);
  return out;
}
#endif

}  // namespace

int runMcpHttpSelfTest() {
  Checker check;

  // ---- 1. HTTP parser: valid request ----
  {
    std::string raw =
        "POST /mcp HTTP/1.1\r\n"
        "Host: 127.0.0.1:1337\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 42\r\n"
        "Authorization: Bearer abc\r\n"
        "\r\n";
    HttpParseOutcome o = parseHttpHeaders(raw);
    check(o.status == HttpParseStatus::kOk, "http: valid POST /mcp parses");
    check(o.request.method == "POST" && o.request.target == "/mcp" &&
              o.request.version == "HTTP/1.1",
          "http: request line fields");
    check(o.consumed == raw.size(), "http: consumed == full header block");
    check(o.request.headerValue("content-length") == "42",
          "http: header lookup is case-insensitive");
    check(o.request.headerValue("AUTHORIZATION") == "Bearer abc",
          "http: mixed-case header name lookup");
    std::uint64_t len = 0;
    check(parseContentLength(o.request, len) && len == 42,
          "http: Content-Length parses");
    check(o.request.keepAlive(), "http: HTTP/1.1 defaults to keep-alive");
  }

  // ---- 2. parser: incomplete / malformed ----
  {
    HttpParseOutcome o = parseHttpHeaders("POST /mcp HTTP/1.1\r\nHost: x\r\n");
    check(o.status == HttpParseStatus::kNeedMore, "http: partial headers need more");
    o = parseHttpHeaders("GOT /mcp HTTP/1.1\r\n\r\n");
    // "GOT" is a valid token; method validity is the router's job, not the
    // parser's — the parser only rejects malformed framing.
    check(o.status == HttpParseStatus::kOk, "http: unknown method token still parses");
    o = parseHttpHeaders("POST  HTTP/1.1\r\n\r\n");
    check(o.status == HttpParseStatus::kError, "http: empty target rejected");
    o = parseHttpHeaders("POST /mcp HTTP/2.0\r\n\r\n");
    check(o.status == HttpParseStatus::kError, "http: HTTP/2 version rejected");
    o = parseHttpHeaders("POST /mcp HTTP/1.1\r\nBadHeader\r\n\r\n");
    check(o.status == HttpParseStatus::kError, "http: header without colon rejected");
    o = parseHttpHeaders("POST /mcp HTTP/1.1\r\nX-Fold: a\r\n b\r\n\r\n");
    check(o.status == HttpParseStatus::kError, "http: obs-fold rejected");
    o = parseHttpHeaders(
        "POST /mcp HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\n");
    check(o.status == HttpParseStatus::kError, "http: duplicate Content-Length rejected");
    o = parseHttpHeaders("POST /mcp HTTP/1.1\r\nContent-Length: zz\r\n\r\n");
    check(o.status == HttpParseStatus::kOk, "http: non-numeric CL parses (validated later)");
    std::uint64_t len = 0;
    check(!parseContentLength(o.request, len), "http: non-numeric Content-Length rejected");
    // > 64 KiB header block without terminator.
    std::string big(70 * 1024, 'x');
    o = parseHttpHeaders("POST /mcp HTTP/1.1\r\n" + big);
    check(o.status == HttpParseStatus::kError, "http: oversize header block rejected");
  }

  // ---- 3. response serialization ----
  {
    HttpResponse r;
    r.status = 200;
    r.setHeader("Content-Type", "application/json");
    r.body = "{}";
    std::string s = r.serialize();
    check(s == "HTTP/1.1 200 OK\r\ncontent-type: application/json\r\n"
                "content-length: 2\r\n\r\n{}",
          "http: response serializes byte-exact");
    HttpResponse e = makeErrorResponse(404, "nope");
    check(e.status == 404 && bodyHas(e, "nope"), "http: error response shape");
  }

  // ---- 4. SSE framing ----
  {
    std::string f = SseStream::formatEvent("message", R"({"a":1})");
    check(f == "event: message\ndata: {\"a\":1}\n\n", "http: SSE single-line event bytes");
    f = SseStream::formatEvent("message", "line1\nline2\n");
    check(f == "event: message\ndata: line1\ndata: line2\n\n",
          "http: SSE multi-line data + trailing newline");
    SseStream s;
    s.pushEvent("message", "{}");
    s.pushEvent("ping", "");
    check(s.bytes() == "event: message\ndata: {}\n\nevent: ping\ndata:\n\n",
          "http: SseStream accumulates frames");
    HttpResponse sr = SseStream::streamResponse("event: x\ndata: y\n\n");
    check(sr.status == 200 &&
              sr.headerValue("content-type") == "text/event-stream",
          "http: SSE stream response headers");
  }

  // ---- 5. auth + session lifecycle (no sockets) ----
  {
    McpHttpServer server(McpBackends{}, "test-agent");
    std::string logs;
    server.setLogSink([&](std::string_view l) { logs += std::string(l) + "\n"; });
    std::string token = server.token();
    check(!token.empty() && token.size() == 64, "http: bearer token generated (256-bit hex)");

    HttpResponse r = server.handleRequest(makePost(kInitBody, ""));
    check(r.status == 401, "http: missing bearer -> 401");
    r = server.handleRequest(makePost(kInitBody, "wrong"));
    check(r.status == 401, "http: wrong bearer -> 401");

    r = server.handleRequest(makePost("{not json", token));
    check(r.status == 400, "http: malformed JSON body -> 400");

    // F-L9: Origin/Host allowlist runs before auth — a foreign Origin or a
    // rebound Host is 403 even with a valid token; loopback values pass on
    // to the normal auth/framing checks (400 here = past the F-L9 gate).
    {
      HttpRequest evilOrigin = makePost(kInitBody, token);
      evilOrigin.headers.push_back({"origin", "http://evil.example.com"});
      r = server.handleRequest(evilOrigin);
      check(r.status == 403, "http: foreign Origin -> 403 (F-L9)");
      HttpRequest evilHost = makePost(kInitBody, token);
      evilHost.headers.push_back({"host", "rebound.example.com"});
      r = server.handleRequest(evilHost);
      check(r.status == 403, "http: foreign Host -> 403 (F-L9)");
      HttpRequest goodOrigin = makePost("{not json", token);
      goodOrigin.headers.push_back({"origin", "http://127.0.0.1"});
      goodOrigin.headers.push_back({"host", "localhost"});
      r = server.handleRequest(goodOrigin);
      check(r.status == 400, "http: loopback Origin/Host pass the F-L9 gate");
    }

    r = server.handleRequest(makePost(kToolsListBody, token));
    check(r.status == 400, "http: non-initialize without session -> 400");

    r = server.handleRequest(makePost(kToolsListBody, token, "nope"));
    check(r.status == 404, "http: unknown session id -> 404");

    // initialize -> session
    r = server.handleRequest(makePost(kInitBody, token));
    check(r.status == 200, "http: initialize -> 200");
    std::string sid = respSessionId(r);
    check(!sid.empty(), "http: initialize returns Mcp-Session-Id");
    std::string proto;
    check(bodyIsJsonRpcResult(r, &proto) && proto == "2025-11-25",
          "http: initialize body is the handshake-era result");
    check(server.sessionIds().size() == 1, "http: one session tracked");

    // authed call on the session
    r = server.handleRequest(makePost(kToolsListBody, token, sid));
    check(r.status == 200 && bodyIsJsonRpcResult(r),
          "http: tools/list on session -> 200 JSON-RPC");
    check(std::string(r.headerValue("content-type")) == "application/json",
          "http: JSON-RPC response content type");
    check(respSessionId(r) == sid, "http: session id echoed on responses");

    // permission gating end to end: mask 0 denies, grant opens
    std::string callSim =
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call",)"
        R"("params":{"name":"sim_status","arguments":{}}})";
    r = server.handleRequest(makePost(callSim, token, sid));
    check(r.status == 200 && bodyHas(r, "permission denied"),
          "http: tool call denied at mask 0 (isError, still 200)");
    check(server.grantPermissions(sid, kPermWorldRead),
          "http: grantPermissions on session");
    r = server.handleRequest(makePost(callSim, token, sid));
    check(r.status == 200 && !bodyHas(r, "permission denied"),
          "http: tool call passes gating after grant");
    check(!server.grantPermissions("bogus", kPermWorldRead),
          "http: grant on unknown session fails");

    // notification -> 202, no body
    r = server.handleRequest(makePost(kNotifyBody, token, sid));
    check(r.status == 202, "http: JSON-RPC notification -> 202");

    // SSE negotiation: same logical response as an SSE message event
    HttpRequest sseReq = makePost(kToolsListBody, token, sid);
    sseReq.headers.push_back({"accept", "text/event-stream"});
    HttpRequest plainReq = makePost(kToolsListBody, token, sid);
    HttpResponse sseResp = server.handleRequest(sseReq);
    HttpResponse plainResp = server.handleRequest(plainReq);
    check(sseResp.status == 200 &&
              sseResp.headerValue("content-type") == "text/event-stream",
          "http: Accept: text/event-stream -> SSE response");
    check(sseResp.body == SseStream::formatEvent("message", plainResp.body),
          "http: SSE body wraps the JSON-RPC response exactly");

    // token rotation
    std::string newToken = server.rotateToken();
    check(newToken != token && newToken.size() == 64, "http: token rotates");
    r = server.handleRequest(makePost(kToolsListBody, token, sid));
    check(r.status == 401, "http: old token rejected after rotation");
    r = server.handleRequest(makePost(kToolsListBody, newToken, sid));
    check(r.status == 200, "http: new token accepted after rotation");

    // routing / framing unit checks
    HttpRequest get = makePost(kToolsListBody, newToken, sid);
    get.method = "GET";
    r = server.handleRequest(get);
    check(r.status == 405, "http: GET /mcp -> 405 (SSE endpoint reserved)");
    HttpRequest bad = makePost(kToolsListBody, newToken, sid);
    bad.target = "/nope";
    r = server.handleRequest(bad);
    check(r.status == 404, "http: unknown path -> 404");
    HttpRequest noCL = makePost(kToolsListBody, newToken, sid);
    noCL.headers.erase(noCL.headers.begin() + 1);  // drop content-length
    r = server.handleRequest(noCL);
    check(r.status == 411, "http: missing Content-Length -> 411");
    HttpRequest badCT = makePost(kToolsListBody, newToken, sid);
    badCT.headers[0].value = "text/plain";
    r = server.handleRequest(badCT);
    check(r.status == 415, "http: wrong Content-Type -> 415");
    HttpRequest chunked = makePost(kToolsListBody, newToken, sid);
    chunked.headers.push_back({"transfer-encoding", "chunked"});
    r = server.handleRequest(chunked);
    check(r.status == 501, "http: chunked -> 501");
    HttpRequest huge = makePost(kToolsListBody, newToken, sid);
    huge.headers[1].value = "99999999";
    r = server.handleRequest(huge);
    check(r.status == 413, "http: oversize body -> 413");

    check(logs.find("mcp: http POST /mcp") != std::string::npos,
          "http: every request is audit-logged with the mcp: prefix");
    check(logs.find(token) == std::string::npos,
          "http: bearer token never appears in logs");
  }

  // ---- 5b. session expiry: LRU cap + DELETE close (eraseSession wired) ----
  {
    McpHttpServer server(McpBackends{}, "test-agent");
    server.setLogSink(nullptr);
    std::string token = server.token();

    auto initSession = [&]() {
      HttpResponse r = server.handleRequest(makePost(kInitBody, token));
      return respSessionId(r);
    };

    // DELETE /mcp closes a session explicitly (this wires up eraseSession,
    // which previously had zero callers).
    std::string sid = initSession();
    check(!sid.empty() && server.sessionIds().size() == 1,
          "http: session minted for the DELETE test");
    HttpRequest del = makePost("", token, sid);
    del.method = "DELETE";
    HttpResponse r = server.handleRequest(del);
    check(r.status == 204, "http: DELETE /mcp closes the session -> 204");
    check(server.sessionIds().empty(), "http: closed session is gone from the table");
    r = server.handleRequest(makePost(kToolsListBody, token, sid));
    check(r.status == 404, "http: closed session id -> 404");
    HttpRequest delBogus = makePost("", token, "bogus");
    delBogus.method = "DELETE";
    r = server.handleRequest(delBogus);
    check(r.status == 404, "http: DELETE unknown session -> 404");
    HttpRequest delNoAuth = makePost("", "", sid);
    delNoAuth.method = "DELETE";
    r = server.handleRequest(delNoAuth);
    check(r.status == 401, "http: DELETE without bearer -> 401");

    // LRU cap: the table never grows past 64; the oldest-idle go first.
    std::vector<std::string> ids;
    for (int i = 0; i < 70; ++i) ids.push_back(initSession());
    check(server.sessionIds().size() == 64, "http: session table capped at 64 (LRU eviction)");
    r = server.handleRequest(makePost(kToolsListBody, token, ids.front()));
    check(r.status == 404, "http: LRU-evicted session id -> 404");
    r = server.handleRequest(makePost(kToolsListBody, token, ids.back()));
    check(r.status == 200, "http: newest session survives the LRU cap");
  }

  // ---- 6. crypto helpers ----
  {
    std::uint8_t a[32], b[32];
    check(cryptoRandomBytes(a, sizeof(a)) && cryptoRandomBytes(b, sizeof(b)),
          "http: OS CSPRNG produces bytes");
    check(!constantTimeEqual(
              std::string_view(reinterpret_cast<char*>(a), 32),
              std::string_view(reinterpret_cast<char*>(b), 32)),
          "http: two CSPRNG outputs differ");
    check(constantTimeEqual("abc", "abc") && !constantTimeEqual("abc", "abd") &&
              !constantTimeEqual("abc", "abcd"),
          "http: constant-time compare semantics");
    check(isLoopbackBindAddress("127.0.0.1") &&
              !isLoopbackBindAddress("0.0.0.0"),
          "http: loopback bind detection");
  }

#ifndef _WIN32
  // ---- 7. live loopback: socket layer + accept thread ----
  {
    McpHttpServer server(McpBackends{}, "live-agent");
    std::string logs;
    server.setLogSink([&](std::string_view l) { logs += std::string(l) + "\n"; });
    check(server.start(0), "http: live server starts on ephemeral port");
    check(server.boundAddress() == "127.0.0.1",
          "http: default bind is loopback-only");
    std::uint16_t port = server.boundPort();
    check(port != 0, "http: ephemeral port reported");

    std::string token = server.token();
    std::string initReq =
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer " + token + "\r\n"
        "Content-Length: " + std::to_string(std::string(kInitBody).size()) +
        "\r\nConnection: close\r\n\r\n" + kInitBody;
    std::string raw = httpRaw(initReq, port);
    check(raw.find("HTTP/1.1 200") == 0, "http: live initialize -> 200");
    std::string sid;
    {
      std::size_t p = raw.find("mcp-session-id: ");
      if (p != std::string::npos) {
        std::size_t e = raw.find("\r\n", p);
        sid = raw.substr(p + 16, e - p - 16);
      }
    }
    check(!sid.empty(), "http: live initialize returns session id");
    check(raw.find("\"protocolVersion\":\"2025-11-25\"") != std::string::npos,
          "http: live initialize body is the handshake result");

    // second connection: authed tools/list on the session
    std::string listBody(kToolsListBody);
    std::string listReq =
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer " + token + "\r\n"
        "Mcp-Session-Id: " + sid + "\r\n"
        "Content-Length: " + std::to_string(listBody.size()) +
        "\r\nConnection: close\r\n\r\n" + listBody;
    raw = httpRaw(listReq, port);
    check(raw.find("HTTP/1.1 200") == 0 &&
              raw.find("\"tools\"") != std::string::npos,
          "http: live tools/list on session -> 200 with tool list");

    // bad token over the wire
    std::string badReq =
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer wrong\r\n"
        "Content-Length: " + std::to_string(listBody.size()) +
        "\r\nConnection: close\r\n\r\n" + listBody;
    raw = httpRaw(badReq, port);
    check(raw.find("HTTP/1.1 401") == 0, "http: live bad token -> 401");

    // port-in-use: fail fast, never silently re-pick
    McpHttpServer squatter(McpBackends{});
    check(!squatter.start(port), "http: port-in-use fails fast");

    server.stop();
    check(!server.running(), "http: server stops cleanly");
  }

  // ---- 8. non-loopback bind warns loudly ----
  {
    McpHttpServer server(McpBackends{});
    std::string logs;
    server.setLogSink([&](std::string_view l) { logs += std::string(l) + "\n"; });
    bool ok = server.start(0, "0.0.0.0");
    check(ok, "http: non-loopback bind allowed when explicit");
    check(logs.find("WARNING") != std::string::npos &&
              logs.find("non-loopback") != std::string::npos,
          "http: non-loopback bind logs a loud warning");
    server.stop();
  }
#endif

  // ---- 9. Phase 3: stateless era over Streamable HTTP ----
  {
    struct StubSim : ISimStatus {
      uint64_t worldTick() override { return 4242; }
      double currentTps() override { return 32.0; }
      int targetTps() override { return 32; }
      bool paused() override { return false; }
    };
    McpBackends p3backends;
    p3backends.sim = std::make_shared<StubSim>();
    auto statelessBody = [](const std::string& method, int id,
                            const std::string& paramsInner,
                            const std::string& version = "2026-07-28") {
      const std::string p = paramsInner.empty() ? "" : paramsInner + ",";
      return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(id) +
             ",\"method\":\"" + method + "\",\"params\":{" + p +
             "\"_meta\":{\"io.modelcontextprotocol/protocolVersion\":\"" +
             version +
             "\",\"io.modelcontextprotocol/clientCapabilities\":{}}}}";
    };
    auto makeStatelessPost = [](const std::string& body,
                                const std::string& token,
                                const std::string& method,
                                const std::string& name = "") {
      HttpRequest r = makePost(body, token);
      r.headers.push_back({"MCP-Protocol-Version", "2026-07-28"});
      r.headers.push_back({"Mcp-Method", method});
      if (!name.empty()) r.headers.push_back({"Mcp-Name", name});
      return r;
    };
    auto rpcErrCode = [](const HttpResponse& r) {
      ParseResult pr = parseJson(r.body);
      const JsonValue* e = pr.ok ? pr.value.find("error") : nullptr;
      const JsonValue* c = e ? e->find("code") : nullptr;
      return (c && c->isInt()) ? static_cast<int>(c->asInt()) : 0;
    };

    McpHttpServer server(p3backends, "http-phase3");
    std::string logs;
    server.setLogSink([&](std::string_view l) { logs += std::string(l) + "\n"; });
    std::string token = server.token();
    check(server.statelessPermissionMask() == 0,
          "http3: stateless agent starts deny-by-default");

    // stateless tools/list: 200, no session minted, envelope present.
    std::string listBody = statelessBody("tools/list", 1, "");
    HttpResponse r =
        server.handleRequest(makeStatelessPost(listBody, token, "tools/list"));
    check(r.status == 200 && respSessionId(r).empty() &&
              bodyHas(r, "\"resultType\":\"complete\"") &&
              bodyHas(r, "\"name\":\"litholattice\""),
          "http3: stateless tools/list -> 200, no session, envelope present");

    // Bearer <redacted> still required on the stateless path.
    r = server.handleRequest(makeStatelessPost(listBody, "", "tools/list"));
    check(r.status == 401, "http3: stateless without Bearer <redacted> -> 401");

    // Header validation (SEP-2243): missing/mismatched -> -32020.
    // Header order from makePost/makeStatelessPost: 0 content-type,
    // 1 content-length, 2 authorization, 3 MCP-Protocol-Version, 4 Mcp-Method.
    HttpRequest noPV = makeStatelessPost(listBody, token, "tools/list");
    noPV.headers.erase(noPV.headers.begin() + 3);  // drop MCP-Protocol-Version
    r = server.handleRequest(noPV);
    check(r.status == 400 && rpcErrCode(r) == kRpcHeaderMismatch,
          "http3: missing MCP-Protocol-Version -> 400/-32020");
    HttpRequest badPV = makeStatelessPost(listBody, token, "tools/list");
    badPV.headers[3].value = "1999-01-01";
    r = server.handleRequest(badPV);
    check(r.status == 400 && rpcErrCode(r) == kRpcHeaderMismatch,
          "http3: wrong MCP-Protocol-Version -> 400/-32020");
    HttpRequest noMM = makeStatelessPost(listBody, token, "tools/list");
    noMM.headers.erase(noMM.headers.begin() + 4);  // drop Mcp-Method
    r = server.handleRequest(noMM);
    check(r.status == 400 && rpcErrCode(r) == kRpcHeaderMismatch,
          "http3: missing Mcp-Method -> 400/-32020");
    HttpRequest badMM = makeStatelessPost(listBody, token, "tools/list");
    badMM.headers[4].value = "tools/call";
    r = server.handleRequest(badMM);
    check(r.status == 400 && rpcErrCode(r) == kRpcHeaderMismatch,
          "http3: mismatched Mcp-Method -> 400/-32020");

    // Mcp-Name on named methods.
    std::string callBody =
        statelessBody("tools/call", 2, R"("name":"sim_status")");
    r = server.handleRequest(makeStatelessPost(callBody, token, "tools/call"));
    check(r.status == 400 && rpcErrCode(r) == kRpcHeaderMismatch,
          "http3: tools/call without Mcp-Name -> 400/-32020");
    r = server.handleRequest(
        makeStatelessPost(callBody, token, "tools/call", "wrong_name"));
    check(r.status == 400 && rpcErrCode(r) == kRpcHeaderMismatch,
          "http3: tools/call with wrong Mcp-Name -> 400/-32020");
    r = server.handleRequest(
        makeStatelessPost(callBody, token, "tools/call", "sim_status"));
    check(r.status == 200 && rpcErrCode(r) == 0 &&
              bodyHas(r, "\"isError\":true"),
          "http3: stateless tools/call denied at mask 0 (isError, HTTP 200)");

    // A Mcp-Session-Id on a stateless request is ignored, never minted.
    HttpRequest withSid =
        makeStatelessPost(listBody, token, "tools/list");
    withSid.headers.push_back({"Mcp-Session-Id", "whatever"});
    r = server.handleRequest(withSid);
    check(r.status == 200 && respSessionId(r).empty(),
          "http3: Mcp-Session-Id on stateless request is ignored");

    // Fictional version -> -32022 (HTTP 400); unknown method -> 404.
    std::string badVerBody = statelessBody("tools/list", 3, "", "2099-01-01");
    r = server.handleRequest(makeStatelessPost(badVerBody, token, "tools/list"));
    check(r.status == 400 && rpcErrCode(r) == kRpcUnsupportedProtocolVersion,
          "http3: fictional version -> 400/-32022");
    std::string bogusBody = statelessBody("bogus/method", 4, "");
    r = server.handleRequest(makeStatelessPost(bogusBody, token, "bogus/method"));
    check(r.status == 404 && rpcErrCode(r) == kRpcMethodNotFound,
          "http3: unknown stateless method -> 404/-32601");

    // server/discover over HTTP, stateless.
    std::string discBody = statelessBody("server/discover", 5, "");
    r = server.handleRequest(
        makeStatelessPost(discBody, token, "server/discover"));
    check(r.status == 200 && bodyHas(r, "\"supportedVersions\""),
          "http3: stateless server/discover -> 200");

    // prompts/get over HTTP, stateless.
    std::string promptBody = statelessBody(
        "prompts/get", 6, R"("name":"diagnose","arguments":{"ticks":"100"})");
    r = server.handleRequest(
        makeStatelessPost(promptBody, token, "prompts/get", "diagnose"));
    check(r.status == 200 && bodyHas(r, "100-tick"),
          "http3: stateless prompts/get renders");
    std::string badPromptBody =
        statelessBody("prompts/get", 7, R"("name":"nope")");
    r = server.handleRequest(
        makeStatelessPost(badPromptBody, token, "prompts/get", "nope"));
    check(r.status == 400 && rpcErrCode(r) == kRpcInvalidParams,
          "http3: stateless prompts/get unknown prompt -> 400/-32602");

    // The shared stateless agent takes grants like every other identity.
    check(server.grantStatelessPermissions(
                  static_cast<std::uint32_t>(kPermWorldRead)) &&
              server.statelessPermissionMask() ==
                  static_cast<std::uint32_t>(kPermWorldRead),
          "http3: grantStatelessPermissions flips the shared agent mask");
    r = server.handleRequest(
        makeStatelessPost(callBody, token, "tools/call", "sim_status"));
    check(r.status == 200 && bodyHas(r, "\"isError\":false") &&
              bodyHas(r, "4242"),
          "http3: stateless tools/call works after grant");
    check(server.revokeStatelessPermissions(
                  static_cast<std::uint32_t>(kPermWorldRead)) &&
              server.statelessPermissionMask() == 0,
          "http3: revokeStatelessPermissions clears the mask");
  }

  return check.fails();
}

}  // namespace litholattice::mcp
