// mcp_selftest.cpp: headless checks for the MCP Phase 0 JSON-RPC core.
#include "litholattice/mcp/mcp_selftest.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "litholattice/mcp/json.h"
#include "litholattice/mcp/jsonrpc.h"
#include "litholattice/mcp/server.h"
#include "litholattice/mcp/mcp_senses.h"
// Cross-module headers the server itself reads (header-only usage).
#include "litholattice/console/iworldquery.h"
#include "litholattice/mods/mod_interface.h"

namespace litholattice::mcp {
namespace {

class Checker {
 public:
  void operator()(bool ok, const std::string& what) {
    std::printf("  [%s] mcp: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++fails_;
  }
  int fails() const { return fails_; }

 private:
  int fails_ = 0;
};

bool parsesOk(std::string_view text, JsonValue* out = nullptr) {
  ParseResult r = parseJson(text);
  if (r.ok && out) *out = r.value;
  return r.ok;
}

bool parsesFail(std::string_view text) { return !parseJson(text).ok; }

// Runs one RPC message through a router and returns the parsed response
// (null JsonValue when no response was produced).
JsonValue rpcRoundTrip(RpcRouter& router, std::string_view text, bool* responded) {
  std::optional<std::string> resp = handleRpcMessage(text, router);
  *responded = resp.has_value();
  if (!resp) return JsonValue::null();
  ParseResult r = parseJson(*resp);
  return r.ok ? r.value : JsonValue::null();
}

std::string repeat(char c, std::size_t n) { return std::string(n, c); }

}  // namespace

int runMcpSelfTest() {
  Checker check;

  // ---- 1. valid documents ----
  {
    JsonValue v;
    check(parsesOk(R"({"jsonrpc":"2.0","method":"ping","id":1})", &v) && v.isObject(),
          "parse: simple request object");
    check(parsesOk("[1, 2.5, -3e4, true, false, null, \"s\", [], {}]"),
          "parse: array of all value kinds");
    check(parsesOk("{\"a\":{\"b\":[{\"c\":{}}]}}"), "parse: nested objects/arrays");
    check(parsesOk("  \n\t 42 \r\n ", &v) && v.isInt() && v.asInt() == 42,
          "parse: leading/trailing whitespace accepted");
    check(parsesOk("9223372036854775807", &v) && v.isInt() &&
              v.asInt() == 9223372036854775807LL,
          "parse: int64 max stays int64");
    check(parsesOk("-9223372036854775808", &v) && v.isInt() &&
              v.asInt() == (-9223372036854775807LL - 1),
          "parse: int64 min stays int64");
    check(parsesOk("1.5", &v) && v.isDouble() && v.asDouble() == 1.5, "parse: fraction is double");
    check(parsesOk("2E10", &v) && v.isDouble() && v.asDouble() == 2e10,
          "parse: exponent is double");
    check(parsesOk("-0.25e-3", &v) && v.isDouble(), "parse: signed exponent");
    check(parsesOk("0", &v) && v.asInt() == 0, "parse: zero");
    check(parsesOk("-0", &v) && v.isInt(), "parse: negative zero is int");
  }

  // ---- 2. escapes and unicode ----
  {
    JsonValue v;
    check(parsesOk(R"("a\"b\\c\/d\b\f\n\r\t")", &v) &&
              v.asString() == "a\"b\\c/d\b\f\n\r\t",
          "parse: all short escapes");
    check(parsesOk(R"("\u0041\u00e9")", &v) && v.asString() == std::string("A\xc3\xa9"),
          "parse: \\uXXXX basic + latin-1");
    check(parsesOk(R"("\uD834\uDD1E")", &v) && v.asString() == std::string("\xf0\x9d\x84\x9e"),
          "parse: surrogate pair -> UTF-8 (U+1D11E)");
    check(parsesOk("\"caf\xc3\xa9\"", &v) && v.asString() == std::string("caf\xc3\xa9"),
          "parse: raw UTF-8 bytes pass through");
    check(parsesFail(R"("\uD834")"), "reject: lone high surrogate");
    check(parsesFail(R"("\uDD1E")"), "reject: lone low surrogate");
    check(parsesFail(R"("\uD834x")"), "reject: high surrogate not followed by \\u");
    check(parsesFail(R"("\uD834\u0041")"), "reject: high surrogate + non-low");
    check(parsesFail(R"("\u12")"), "reject: truncated \\u escape");
    check(parsesFail(R"("\u12qx")"), "reject: non-hex in \\u escape");
    check(parsesFail(R"("\x41")"), "reject: invalid escape \\x");
    check(parsesFail(R"("a\)"), "reject: truncated escape at end");
    check(parsesFail(std::string("\"a") + char(0x01) + "b\""), "reject: raw control char in string");
    check(parsesFail(std::string("\"\xff\"")), "reject: invalid UTF-8 byte in string");
    check(parsesFail(std::string("\"\xc3\"")), "reject: truncated UTF-8 sequence");
    check(parsesFail("\"\\ud800\""), "reject: lowercase lone high surrogate");
  }

  // ---- 3. malformed numbers ----
  {
    check(parsesFail("01"), "reject: leading zero");
    check(parsesFail("-01"), "reject: leading zero after minus");
    check(parsesFail("1."), "reject: missing fraction digits");
    check(parsesFail(".5"), "reject: missing integer part");
    check(parsesFail("+1"), "reject: leading plus");
    check(parsesFail("--1"), "reject: double minus");
    check(parsesFail("1e"), "reject: missing exponent digits");
    check(parsesFail("1e+"), "reject: truncated exponent sign");
    check(parsesFail("NaN"), "reject: NaN literal");
    check(parsesFail("Infinity"), "reject: Infinity literal");
    check(parsesFail("0x10"), "reject: hex literal");
    JsonValue v;
    check(parsesFail("1e400"), "reject: 1e400 overflows double (out of range)");
    check(parsesOk("99999999999999999999999", &v) && v.isDouble(),
          "parse: huge integer-form demotes to double, never wraps");
  }

  // ---- 4. structural malformation ----
  {
    check(parsesFail(""), "reject: empty input");
    check(parsesFail("   "), "reject: whitespace-only input");
    check(parsesFail("{"), "reject: truncated object");
    check(parsesFail("{\"a\":1"), "reject: truncated object 2");
    check(parsesFail("[1,2"), "reject: truncated array");
    check(parsesFail("\"abc"), "reject: unterminated string");
    check(parsesFail("{\"a\":1,}"), "reject: trailing comma in object");
    check(parsesFail("[1,]"), "reject: trailing comma in array");
    check(parsesFail("{\"a\" 1}"), "reject: missing colon");
    check(parsesFail("{a:1}"), "reject: unquoted key");
    check(parsesFail("[1 2]"), "reject: missing comma");
    check(parsesFail("{\"a\":1} garbage"), "reject: trailing garbage");
    check(parsesFail("null null"), "reject: two top-level values");
    check(parsesFail("{,}"), "reject: comma-only object");
    check(parsesFail("[,]"), "reject: comma-only array");
    // depth cap: 64 nested arrays OK, 65 rejected
    check(parsesOk(repeat('[', 64) + repeat(']', 64)), "parse: depth 64 accepted");
    check(parsesFail(repeat('[', 65) + repeat(']', 65)), "reject: depth 65 exceeds cap");
    check(parsesFail(repeat('{', 65) + repeat('}', 65)), "reject: object depth 65 exceeds cap");
  }

  // ---- 5. serializer round-trip ----
  {
    // canonical docs: parse -> serialize is byte-identical
    const char* canonicalDocs[] = {
        "null",     "true",  "false", "0", "-42", "9223372036854775807",
        "1.5",      "\"\"",  "\"hello \\\"world\\\"\"", "\"tab\\there\"",
        "[]",       "{}",    "[1,\"two\",[3.5],{\"four\":null}]",
        "{\"b\":1,\"a\":2}",
    };
    bool all = true;
    for (const char* d : canonicalDocs) {
      ParseResult r = parseJson(d);
      if (!r.ok || r.value.serialize() != d) {
        all = false;
        break;
      }
    }
    check(all, "serialize: exact round-trip on canonical documents");
    // non-canonical numeric spellings: value round-trips (reparse compares equal)
    const char* valueDocs[] = {"2E10", "-2.5e-3", "0.30000000000000004", "1e2"};
    bool vals = true;
    for (const char* d : valueDocs) {
      ParseResult r1 = parseJson(d);
      ParseResult r2 = parseJson(r1.ok ? r1.value.serialize() : "");
      if (!r1.ok || !r2.ok || !(r1.value == r2.value)) {
        vals = false;
        break;
      }
    }
    check(vals, "serialize: non-canonical numbers reparse to the equal value");
    // object key order preserved
    JsonValue v;
    parsesOk("{\"z\":1,\"m\":2,\"a\":3}", &v);
    check(v.serialize() == "{\"z\":1,\"m\":2,\"a\":3}", "serialize: object order preserved");
    // unicode round-trip: parser produces UTF-8, serializer emits raw UTF-8
    parsesOk(R"("\u00e9\uD834\uDD1E")", &v);
    check(v.serialize() == std::string("\"\xc3\xa9\xf0\x9d\x84\x9e\""),
          "serialize: unicode round-trip as raw UTF-8");
    // escapes: control chars become \u00XX
    v = JsonValue::string(std::string("a") + char(0x01) + "b");
    check(v.serialize() == "\"a\\u0001b\"", "serialize: control char -> \\u00XX");
    // double round-trip through shortest representation
    parsesOk("0.30000000000000004", &v);
    check(v.serialize() == "0.30000000000000004", "serialize: double shortest round-trip");
    // find / getOrInsert helpers
    parsesOk("{\"k\":\"v\"}", &v);
    check(v.find("k") && v.find("k")->asString() == "v" && v.find("nope") == nullptr,
          "json: find() hits and misses");
    v.getOrInsert("n") = JsonValue::integer(7);
    check(v.find("n")->asInt() == 7, "json: getOrInsert() adds entry");
  }

  // ---- 6. RPC framing: requests, notifications, ids ----
  {
    RpcRouter router;
    router.registerHandler("ping", [](const JsonValue&) { return JsonValue::string("pong"); });
    router.registerHandler("echo",
                           [](const JsonValue& p) { return p; });  // params echo
    router.registerHandler("add", [](const JsonValue& p) {
      const JsonValue* a = p.find("a");
      const JsonValue* b = p.find("b");
      if (!a || !b || !a->isInt() || !b->isInt())
        throw RpcError(kRpcInvalidParams, rpcErrorMessage(kRpcInvalidParams));
      return JsonValue::integer(a->asInt() + b->asInt());
    });

    bool responded = false;
    JsonValue r = rpcRoundTrip(router, R"({"jsonrpc":"2.0","method":"ping","id":1})", &responded);
    check(responded && r.find("result") && r.find("result")->asString() == "pong" &&
              r.find("id")->asInt() == 1 && r.find("jsonrpc")->asString() == "2.0" &&
              !r.find("error"),
          "rpc: request with numeric id -> success response");
    r = rpcRoundTrip(router, R"({"jsonrpc":"2.0","method":"ping","id":"abc"})", &responded);
    check(responded && r.find("id")->asString() == "abc", "rpc: string id round-trips");
    r = rpcRoundTrip(router, R"({"jsonrpc":"2.0","method":"ping","id":null})", &responded);
    check(responded && r.find("id")->isNull(), "rpc: null id is a request, not notification");
    r = rpcRoundTrip(router, R"({"jsonrpc":"2.0","method":"ping"})", &responded);
    check(!responded, "rpc: notification (no id) gets no response");
    r = rpcRoundTrip(router, R"({"jsonrpc":"2.0","method":"echo","params":{"x":[1,2]},"id":9})",
                     &responded);
    check(responded && r.find("result")->find("x")->asArray().size() == 2,
          "rpc: object params passed to handler");
    r = rpcRoundTrip(router, R"({"jsonrpc":"2.0","method":"echo","params":[1,2],"id":9})",
                     &responded);
    check(responded && r.find("result")->asArray().size() == 2, "rpc: array params passed");
    r = rpcRoundTrip(router, R"({"jsonrpc":"2.0","method":"add","params":{"a":2,"b":3},"id":4})",
                     &responded);
    check(responded && r.find("result")->asInt() == 5, "rpc: handler computes from params");
  }

  // ---- 7. RPC error codes ----
  {
    RpcRouter router;
    router.registerHandler("add", [](const JsonValue& p) {
      const JsonValue* a = p.find("a");
      if (!a || !a->isInt()) throw RpcError(kRpcInvalidParams, "need int param 'a'");
      return JsonValue::integer(a->asInt());
    });
    router.registerHandler("boom", [](const JsonValue&) -> JsonValue {
      throw std::runtime_error("unexpected");
    });
    router.registerHandler("coded", [](const JsonValue&) -> JsonValue {
      throw RpcError(-32022, "nope", JsonValue::string("detail"));
    });

    bool responded = false;
    auto errCode = [&](std::string_view text) {
      JsonValue r = rpcRoundTrip(router, text, &responded);
      const JsonValue* e = r.find("error");
      return responded && e ? e->find("code")->asInt() : 0;
    };
    check(errCode("{bad json") == kRpcParseError, "rpc: -32700 parse error");
    check(errCode(R"({"jsonrpc":"2.0","method":"add"})") == 0,
          "rpc: invalid notification gets no response (not even an error)");
    check(errCode(R"({"jsonrpc":"1.0","method":"add","id":1})") == kRpcInvalidRequest,
          "rpc: -32600 wrong jsonrpc version");
    check(errCode(R"({"method":"add","id":1})") == kRpcInvalidRequest,
          "rpc: -32600 missing jsonrpc member");
    check(errCode(R"({"jsonrpc":"2.0","id":1})") == kRpcInvalidRequest,
          "rpc: -32600 missing method");
    check(errCode(R"({"jsonrpc":"2.0","method":42,"id":1})") == kRpcInvalidRequest,
          "rpc: -32600 non-string method");
    check(errCode(R"([{"jsonrpc":"2.0","method":"add","id":1}])") == kRpcInvalidRequest,
          "rpc: -32600 batch array rejected (v1 single-request decision)");
    check(errCode(R"({"jsonrpc":"2.0","method":"nope","id":1})") == kRpcMethodNotFound,
          "rpc: -32601 unknown method");
    check(errCode(R"({"jsonrpc":"2.0","method":"add","params":{"a":"x"},"id":1})") ==
              kRpcInvalidParams,
          "rpc: -32602 handler-thrown invalid params");
    check(errCode(R"({"jsonrpc":"2.0","method":"add","params":"nope","id":1})") ==
              kRpcInvalidParams,
          "rpc: -32602 params wrong type (string)");
    check(errCode(R"({"jsonrpc":"2.0","method":"boom","id":1})") == kRpcInternalError,
          "rpc: -32603 unexpected exception becomes internal error");
    check(errCode(R"({"jsonrpc":"2.0","method":"coded","id":1})") ==
              kRpcUnsupportedProtocolVersion,
          "rpc: -32022 custom code (incl. MCP unsupported-version) passes through");
    check(kRpcUnsupportedProtocolVersion == -32022, "rpc: kRpcUnsupportedProtocolVersion == -32022");

    // error response shape: jsonrpc/id/error{code,message,data}
    JsonValue r = rpcRoundTrip(router, R"({"jsonrpc":"2.0","method":"coded","id":7})", &responded);
    const JsonValue* e = r.find("error");
    check(responded && e && e->find("message")->asString() == "nope" &&
              e->find("data")->asString() == "detail" && r.find("id")->asInt() == 7 &&
              !r.find("result"),
          "rpc: error response shape (no result member, data preserved)");
    // success response omits the error member entirely
    r = rpcRoundTrip(router, R"({"jsonrpc":"2.0","method":"add","params":{"a":1},"id":7})",
                     &responded);
    check(responded && !r.find("error") && r.find("result")->asInt() == 1,
          "rpc: success response has no error member");
    // parse error responses carry id null
    r = rpcRoundTrip(router, "[[[", &responded);
    check(responded && r.find("id")->isNull(), "rpc: parse-error response id is null");
    // handler registration: re-register replaces, hasHandler reflects it
    router.registerHandler("add", [](const JsonValue&) { return JsonValue::integer(99); });
    check(router.hasHandler("add") && !router.hasHandler("missing"), "rpc: hasHandler");
    r = rpcRoundTrip(router, R"({"jsonrpc":"2.0","method":"add","id":1})", &responded);
    check(responded && r.find("result")->asInt() == 99, "rpc: re-register replaces handler");
  }

  // ---- 8. fail-fast documentation checks ----
  {
    // The parser never partially accepts: every rejection leaves a null value
    // and a non-empty error with a byte offset.
    ParseResult r = parseJson("{\"a\": [1, 2,");
    check(!r.ok && r.value.isNull() && !r.error.empty(), "fail-fast: rejection has error text");
    // Deeply hostile input is bounded by the depth cap, not the call stack.
    check(parsesFail(repeat('[', 100000)), "fail-fast: 100k-deep input rejected by cap");
    check(kMaxJsonDepth == 64, "fail-fast: depth cap is 64");
  }

  // ---- 9. Phase 1: server, handshake, tools, resources, stdio ----
  {
    // Stub seams with canned data (the game adapts the real classes).
    struct StubSim : ISimStatus {
      uint64_t worldTick() override { return 12345; }
      double currentTps() override { return 32.0; }
      int targetTps() override { return 32; }
      bool paused() override { return false; }
    };
    struct StubMods : IModListView {
      std::vector<ModEntry> mods() override {
        return {{"testmod", "1.2.3",
                 static_cast<uint32_t>(kPermWorldRead | kPermBlocks), {"ext/a"}}};
      }
    };
    struct StubPlayer : IPlayerView {
      Snapshot localPlayer() override {
        Snapshot s;
        s.x = 1.5;
        s.y = 64.0;
        s.z = -2.5;
        s.health = 64;
        s.stats = {{"kharge", 42.0}};
        return s;
      }
    };
    struct StubEntities : IEntityView {
      std::vector<Ent> within(double, double, double, double) override {
        return {{7, 3, 10.0, 64.0, 10.0}};
      }
      bool get(uint64_t id, Ent& out) override {
        if (id != 7) return false;
        out = {7, 3, 10.0, 64.0, 10.0};
        return true;
      }
    };
    struct StubWorld : IWorldReader {
      uint16_t getBlock(int x, int y, int z) override {
        return (x == 1 && y == 2 && z == 3) ? 5 : 0;
      }
    };
    struct StubQuery : console::IWorldQuery {
      console::WorldQueryResult queryBlocks(uint32_t, int, int, int, int, int,
                                            int, std::string*) override {
        console::WorldQueryResult r;
        r.cellsScanned = 8;
        r.totalMatches = 1;
        r.blocks.push_back({1, 2, 3, 5});
        return r;
      }
      console::WorldQueryResult queryCount(uint32_t, int, int, int, int, int,
                                           int, std::string*) override {
        return {};
      }
      console::WorldQueryResult queryEntities(const std::string&, double, double,
                                              double, double,
                                              std::string*) override {
        return {};
      }
    };
    struct StubDefrag : IDefragView {
      bool running() override { return true; }
      std::string phaseName() override { return "Scan"; }
      double progress() override { return 0.5; }
      std::string statusLine() override { return "scanning"; }
      std::vector<std::string> storeNames() override { return {"fragment"}; }
    };
    struct StubConsole : IConsoleSurface {
      std::vector<Command> commands() override {
        return {{"help", "show help"}, {"query", "query the world"}};
      }
      std::vector<CVar> cvars() override {
        return {{"sv_tps", "32", "target tps"}};
      }
    };
    struct StubCorpus : IFragmentCorpus {
      std::vector<Hit> search(std::string_view q, size_t) override {
        if (q == "zap") return {{1, 2.5, "the zap struck"}};
        return {};
      }
    };
    struct StubDims : IDimensionView {
      std::vector<Dim> dimensions() override {
        return {{"wafer", "Wafer", 196608, true}, {"void", "Void", 0, false}};
      }
    };
    struct StubLore : ILoreProvider {
      std::string text() override { return "# Glossary\nKharge is charge.\n"; }
    };

    auto makeBackends = []() {
      McpBackends b;
      b.sim = std::make_shared<StubSim>();
      b.mods = std::make_shared<StubMods>();
      b.player = std::make_shared<StubPlayer>();
      b.entities = std::make_shared<StubEntities>();
      b.world = std::make_shared<StubWorld>();
      b.worldQuery = std::make_shared<StubQuery>();
      b.defrag = std::make_shared<StubDefrag>();
      b.console = std::make_shared<StubConsole>();
      b.corpus = std::make_shared<StubCorpus>();
      b.dimensions = std::make_shared<StubDims>();
      b.lore = std::make_shared<StubLore>();
      return b;
    };

    auto roundTrip = [](McpServer& srv, std::string_view text, bool* responded) {
      std::optional<std::string> resp = srv.handleMessage(text);
      *responded = resp.has_value();
      if (!resp) return JsonValue::null();
      ParseResult r = parseJson(*resp);
      return r.ok ? r.value : JsonValue::null();
    };
    auto errCode = [](const JsonValue& resp) -> int {
      const JsonValue* e = resp.find("error");
      const JsonValue* c = e ? e->find("code") : nullptr;
      return (c && c->isInt()) ? static_cast<int>(c->asInt()) : 0;
    };
    // tools/call result helpers.
    auto callTool = [&](McpServer& srv, const std::string& body) {
      bool responded = false;
      JsonValue r = roundTrip(srv, body, &responded);
      return std::make_pair(responded, r);
    };
    auto toolText = [](const JsonValue& resp, bool* isError) -> std::string {
      const JsonValue* res = resp.find("result");
      const JsonValue* err = res ? res->find("isError") : nullptr;
      *isError = err && err->isBool() && err->asBool();
      const JsonValue* content = res ? res->find("content") : nullptr;
      const JsonValue* t0 =
          (content && content->isArray() && !content->asArray().empty())
              ? content->asArray()[0].find("text")
              : nullptr;
      return (t0 && t0->isString()) ? t0->asString() : std::string();
    };

    const std::string kInit =
        R"({"jsonrpc":"2.0","method":"initialize","id":1,"params":{"protocolVersion":"2025-11-25","capabilities":{}}})";

    // -- handshake: pre-init gate --
    {
      McpServer srv(makeBackends());
      srv.setLogSink(nullptr);
      bool responded = false;
      JsonValue r = roundTrip(
          srv, R"({"jsonrpc":"2.0","method":"tools/list","id":1})", &responded);
      check(responded && errCode(r) == kRpcInvalidRequest, "mcp: tools/list pre-init -> -32600");
      r = roundTrip(srv, R"({"jsonrpc":"2.0","method":"tools/call","id":2,"params":{"name":"sim_status"}})",
                    &responded);
      check(responded && errCode(r) == kRpcInvalidRequest, "mcp: tools/call pre-init -> -32600");
      r = roundTrip(srv, R"({"jsonrpc":"2.0","method":"resources/list","id":3})", &responded);
      check(responded && errCode(r) == kRpcInvalidRequest, "mcp: resources/list pre-init -> -32600");
      // Notifications never get a response, even pre-init.
      roundTrip(srv, R"({"jsonrpc":"2.0","method":"bogus/thing"})", &responded);
      check(!responded, "mcp: unknown notification pre-init -> no response");
      // notifications/initialized is accepted as a no-op pre-init.
      roundTrip(srv, R"({"jsonrpc":"2.0","method":"notifications/initialized"})", &responded);
      check(!responded && !srv.initialized(), "mcp: notifications/initialized pre-init is a no-op");
      // initialize as a notification must not flip state.
      roundTrip(srv, R"({"jsonrpc":"2.0","method":"initialize","params":{"protocolVersion":"2025-11-25"}})",
                &responded);
      check(!responded && !srv.initialized(), "mcp: initialize as notification does not init");
      // Bad protocol version -> -32022, still uninitialized.
      r = roundTrip(
          srv,
          R"({"jsonrpc":"2.0","method":"initialize","id":4,"params":{"protocolVersion":"2024-11-05"}})",
          &responded);
      check(responded && errCode(r) == kRpcUnsupportedProtocolVersion && !srv.initialized(),
            "mcp: bad protocol version -> -32022");
      const JsonValue* data = r.find("error") ? r.find("error")->find("data") : nullptr;
      const JsonValue* sup = data ? data->find("supported") : nullptr;
      check(sup && sup->isArray() && !sup->asArray().empty() &&
                sup->asArray()[0].asString() == kMcpProtocolVersion,
            "mcp: -32022 names supported versions");
      // Missing protocolVersion is also unsupported (fail-closed).
      r = roundTrip(srv, R"({"jsonrpc":"2.0","method":"initialize","id":5,"params":{}})",
                    &responded);
      check(responded && errCode(r) == kRpcUnsupportedProtocolVersion,
            "mcp: missing protocolVersion -> -32022");
    }

    // -- handshake: good initialize --
    {
      McpServer srv(makeBackends());
      srv.setLogSink(nullptr);
      bool responded = false;
      JsonValue r = roundTrip(srv, kInit, &responded);
      const JsonValue* res = r.find("result");
      check(responded && res && res->find("protocolVersion") &&
                res->find("protocolVersion")->asString() == "2025-11-25",
            "mcp: initialize answers 2025-11-25");
      const JsonValue* info = res ? res->find("serverInfo") : nullptr;
      check(info && info->find("name")->asString() == "litholattice",
            "mcp: serverInfo.name is litholattice");
      check(info && info->find("version")->asString() == mcpEngineVersionString() &&
                mcpEngineVersionString() == "0.1.0",
            "mcp: serverInfo.version tracks engine version macros");
      const JsonValue* caps = res ? res->find("capabilities") : nullptr;
      check(caps && caps->find("tools") && caps->find("resources"),
            "mcp: capabilities declare tools and resources");
      check(srv.initialized(), "mcp: initialized() true after initialize");
      // Post-init traffic flows.
      r = roundTrip(srv, R"({"jsonrpc":"2.0","method":"tools/list","id":6})", &responded);
      check(responded && r.find("result"), "mcp: tools/list works post-init");
      // notifications/initialized post-init: no-op, no response.
      roundTrip(srv, R"({"jsonrpc":"2.0","method":"notifications/initialized"})", &responded);
      check(!responded && srv.initialized(), "mcp: notifications/initialized post-init no-op");
      // Re-initialize is idempotent, not an error.
      r = roundTrip(srv, kInit, &responded);
      check(responded && r.find("result") && srv.initialized(),
            "mcp: re-initialize is idempotent");
    }

    // -- tools/list shape --
    {
      McpServer srv(makeBackends());
      srv.setLogSink(nullptr);
      bool responded = false;
      roundTrip(srv, kInit, &responded);
      JsonValue r = roundTrip(srv, R"({"jsonrpc":"2.0","method":"tools/list","id":1})",
                              &responded);
      const JsonValue* list = r.find("result") ? r.find("result")->find("tools") : nullptr;
      check(list && list->isArray() && list->asArray().size() == 20,
            "mcp: tools/list has 9 Phase 1 + 2 Phase 4 + 5 control + 4 senses tools");
      bool shapeOk = true, foundQuery = false, readOnlyHints = true, writeHintsOk = true;
      if (list && list->isArray()) {
        for (const auto& t : list->asArray()) {
          const JsonValue* schema = t.find("inputSchema");
          const JsonValue* ann = t.find("annotations");
          if (!t.find("name") || !t.find("description") || !schema ||
              !schema->find("type") || schema->find("type")->asString() != "object" ||
              !schema->find("properties") || !schema->find("required")) {
            shapeOk = false;
          }
          const JsonValue* ro = ann ? ann->find("readOnlyHint") : nullptr;
          const JsonValue* dh = ann ? ann->find("destructiveHint") : nullptr;
          const std::string tname =
              (t.find("name") && t.find("name")->isString()) ? t.find("name")->asString() : "";
          const bool isWriteTool = (tname == "world_set_block" || tname == "lithoscript_exec");
          // Control tools actuate the player (kPermPlayerControl); player_get_state
          // is read-only (kPermWorldRead) despite the name prefix.
          uint32_t reqPerms = 0;
          for (const auto& td : srv.tools())
            if (td.name == tname) { reqPerms = td.requiredPerms; break; }
          const bool isControlTool =
              (reqPerms & static_cast<uint32_t>(kPermPlayerControl)) != 0;
          const bool isCameraTool =
              tname == "vision.set_camera" || tname == "vision.track_player";
          if (isWriteTool) {
            // Phase 4 write tools advertise mutability so hosts can prompt.
            if (!ro || !ro->isBool() || ro->asBool()) writeHintsOk = false;
            if (!dh || !dh->isBool() || !dh->asBool()) writeHintsOk = false;
          } else if (isControlTool || isCameraTool) {
            // Control tools actuate the player; camera tools move the camera:
            // correctly NOT read-only.
            if (!ro || !ro->isBool() || ro->asBool()) {
              readOnlyHints = false;
            }
          } else {
            if (!ro || !ro->isBool() || !ro->asBool()) {
              readOnlyHints = false;
            }
          }
          if (tname == "world_query_blocks") foundQuery = true;
        }
      } else {
        shapeOk = false;
      }
      check(shapeOk, "mcp: every tool has name/description/inputSchema{object}");
      check(readOnlyHints,
            "mcp: read-only tools annotate readOnlyHint=true; control/write tools false");
      check(writeHintsOk, "mcp4: write tools are readOnlyHint=false, destructiveHint=true");
      check(foundQuery, "mcp: world_query_blocks is listed");
    }

    // -- permission gating --
    {
      McpServer srv(makeBackends(), McpAgent{"tester", 0});
      srv.setLogSink(nullptr);
      bool responded = false;
      roundTrip(srv, kInit, &responded);
      check(srv.permissionMask() == 0, "mcp: new connections start deny-by-default (mask 0)");
      auto [resp1, r1] =
          callTool(srv, R"({"jsonrpc":"2.0","method":"tools/call","id":1,"params":{"name":"sim_status"}})");
      bool isErr = false;
      std::string text = toolText(r1, &isErr);
      check(resp1 && isErr && text.find("world_read") != std::string::npos,
            "mcp: denied tool call -> isError naming the missing bit");
      // mod_list needs no bits: open at mask 0.
      auto [resp2, r2] =
          callTool(srv, R"({"jsonrpc":"2.0","method":"tools/call","id":2,"params":{"name":"mod_list"}})");
      text = toolText(r2, &isErr);
      check(resp2 && !isErr && text.find("testmod") != std::string::npos,
            "mcp: mod_list works at mask 0");
      // Console-side approval grants the bit; the tool then works.
      srv.grantPermissions(kPermWorldRead);
      check(srv.permissionMask() == static_cast<uint32_t>(kPermWorldRead),
            "mcp: grantPermissions sets the bit");
      auto [resp3, r3] =
          callTool(srv, R"({"jsonrpc":"2.0","method":"tools/call","id":3,"params":{"name":"sim_status"}})");
      text = toolText(r3, &isErr);
      check(resp3 && !isErr && text.find("12345") != std::string::npos,
            "mcp: granted tool call succeeds");
      srv.revokePermissions(kPermWorldRead);
      auto [resp4, r4] =
          callTool(srv, R"({"jsonrpc":"2.0","method":"tools/call","id":4,"params":{"name":"sim_status"}})");
      text = toolText(r4, &isErr);
      check(srv.permissionMask() == 0 && resp4 && isErr,
            "mcp: revokePermissions removes the bit");
    }

    // -- each read tool --
    {
      McpServer srv(makeBackends(), McpAgent{"tester", 0});
      srv.setLogSink(nullptr);
      bool responded = false;
      roundTrip(srv, kInit, &responded);
      srv.grantPermissions(static_cast<uint32_t>(kPermWorldRead | kPermConsole));
      bool isErr = false;
      std::string text;
      JsonValue v;

      auto [s1, r1] =
          callTool(srv, R"({"jsonrpc":"2.0","method":"tools/call","id":1,"params":{"name":"sim_status"}})");
      text = toolText(r1, &isErr);
      check(s1 && !isErr && parsesOk(text, &v) && v.find("tick")->asInt() == 12345 &&
                v.find("paused")->asBool() == false,
            "mcp: sim_status returns tick/tps/paused");

      auto [s2, r2] = callTool(
          srv, R"({"jsonrpc":"2.0","method":"tools/call","id":2,"params":{"name":"player_get_state"}})");
      text = toolText(r2, &isErr);
      check(s2 && !isErr && parsesOk(text, &v) && v.find("x")->asDouble() == 1.5 &&
                v.find("health")->asInt() == 64,
            "mcp: player_get_state returns position/health");

      auto [s3, r3] = callTool(
          srv,
          R"({"jsonrpc":"2.0","method":"tools/call","id":3,"params":{"name":"entity_list","arguments":{"cx":0,"cy":64,"cz":0,"radius":100}}})");
      text = toolText(r3, &isErr);
      check(s3 && !isErr && parsesOk(text, &v) && v.isArray() &&
                v.asArray().size() == 1 && v.asArray()[0].find("id")->asInt() == 7,
            "mcp: entity_list returns entities in radius");
      // Missing radius is malformed (fail-closed), not a silent default.
      auto [s3b, r3b] = callTool(
          srv,
          R"({"jsonrpc":"2.0","method":"tools/call","id":31,"params":{"name":"entity_list","arguments":{"cx":0,"cy":64,"cz":0}}})");
      check(s3b && errCode(r3b) == kRpcInvalidParams, "mcp: entity_list without radius -> -32602");
      // Over-cap radius is rejected, never clamped.
      auto [s3c, r3c] = callTool(
          srv,
          R"({"jsonrpc":"2.0","method":"tools/call","id":32,"params":{"name":"entity_list","arguments":{"cx":0,"cy":64,"cz":0,"radius":1000}}})");
      check(s3c && errCode(r3c) == kRpcInvalidParams, "mcp: entity_list over-cap radius -> -32602");

      auto [s4, r4] = callTool(
          srv,
          R"({"jsonrpc":"2.0","method":"tools/call","id":4,"params":{"name":"entity_get","arguments":{"id":7}}})");
      text = toolText(r4, &isErr);
      check(s4 && !isErr && parsesOk(text, &v) && v.find("type")->asInt() == 3,
            "mcp: entity_get returns the entity");
      auto [s4b, r4b] = callTool(
          srv,
          R"({"jsonrpc":"2.0","method":"tools/call","id":41,"params":{"name":"entity_get","arguments":{"id":999}}})");
      text = toolText(r4b, &isErr);
      check(s4b && isErr && text.find("999") != std::string::npos,
            "mcp: entity_get unknown id -> isError, not a protocol error");

      auto [s5, r5] = callTool(
          srv,
          R"({"jsonrpc":"2.0","method":"tools/call","id":5,"params":{"name":"world_get_block","arguments":{"x":1,"y":2,"z":3}}})");
      text = toolText(r5, &isErr);
      check(s5 && !isErr && parsesOk(text, &v) && v.find("blockId")->asInt() == 5,
            "mcp: world_get_block reads one cell");
      auto [s5b, r5b] = callTool(
          srv,
          R"({"jsonrpc":"2.0","method":"tools/call","id":51,"params":{"name":"world_get_block","arguments":{"x":1,"y":2}}})");
      check(s5b && errCode(r5b) == kRpcInvalidParams, "mcp: world_get_block missing arg -> -32602");

      auto [s6, r6] = callTool(
          srv,
          R"({"jsonrpc":"2.0","method":"tools/call","id":6,"params":{"name":"world_query_blocks","arguments":{"blockId":5,"x1":0,"y1":0,"z1":0,"x2":1,"y2":1,"z2":1}}})");
      text = toolText(r6, &isErr);
      check(s6 && !isErr && parsesOk(text, &v) && v.find("totalMatches")->asInt() == 1 &&
                v.find("blocks")->asArray()[0].find("x")->asInt() == 1,
            "mcp: world_query_blocks scans the box deterministically");
      // Unbounded region: fail-closed.
      auto [s6b, r6b] = callTool(
          srv,
          R"({"jsonrpc":"2.0","method":"tools/call","id":61,"params":{"name":"world_query_blocks","arguments":{"blockId":5,"x1":-100000,"y1":0,"z1":0,"x2":100000,"y2":256,"z2":100000}}})");
      check(s6b && errCode(r6b) == kRpcInvalidParams,
            "mcp: world_query_blocks unbounded region -> -32602");

      auto [s7, r7] = callTool(
          srv,
          R"({"jsonrpc":"2.0","method":"tools/call","id":7,"params":{"name":"fragment_search","arguments":{"query":"zap"}}})");
      text = toolText(r7, &isErr);
      check(s7 && !isErr && parsesOk(text, &v) && v.isArray() &&
                v.asArray().size() == 1 && v.asArray()[0].find("doc")->asInt() == 1,
            "mcp: fragment_search hits the corpus");
      auto [s7b, r7b] = callTool(
          srv,
          R"({"jsonrpc":"2.0","method":"tools/call","id":71,"params":{"name":"fragment_search","arguments":{"query":""}}})");
      check(s7b && errCode(r7b) == kRpcInvalidParams, "mcp: fragment_search empty query -> -32602");

      auto [s8, r8] = callTool(
          srv, R"({"jsonrpc":"2.0","method":"tools/call","id":8,"params":{"name":"defrag_status"}})");
      text = toolText(r8, &isErr);
      check(s8 && !isErr && parsesOk(text, &v) && v.find("running")->asBool() &&
                v.find("phase")->asString() == "Scan",
            "mcp: defrag_status reports the service");

      // Unknown tool: isError result, never a protocol error.
      auto [s9, r9] = callTool(
          srv, R"({"jsonrpc":"2.0","method":"tools/call","id":9,"params":{"name":"nope"}})");
      text = toolText(r9, &isErr);
      check(s9 && isErr && errCode(r9) == 0 && text.find("unknown tool") != std::string::npos,
            "mcp: unknown tool -> isError result");
      // Malformed call: missing name.
      auto [s10, r10] = callTool(
          srv, R"({"jsonrpc":"2.0","method":"tools/call","id":10,"params":{}})");
      check(s10 && errCode(r10) == kRpcInvalidParams, "mcp: tools/call without name -> -32602");
      // Malformed call: arguments not an object.
      auto [s11, r11] = callTool(
          srv, R"({"jsonrpc":"2.0","method":"tools/call","id":11,"params":{"name":"sim_status","arguments":[1]}})");
      check(s11 && errCode(r11) == kRpcInvalidParams,
            "mcp: tools/call with array arguments -> -32602");
    }

    // -- unwired backend degrades to isError --
    {
      McpServer srv(McpBackends{}, McpAgent{"tester", 0});
      srv.setLogSink(nullptr);
      bool responded = false;
      roundTrip(srv, kInit, &responded);
      srv.grantPermissions(0xFFFFFFFFu);
      auto [s1, r1] =
          callTool(srv, R"({"jsonrpc":"2.0","method":"tools/call","id":1,"params":{"name":"sim_status"}})");
      bool isErr = false;
      std::string text = toolText(r1, &isErr);
      check(s1 && isErr && text.find("not wired") != std::string::npos,
            "mcp: null seam -> isError, never a crash");
    }

    // -- resources --
    {
      McpServer srv(makeBackends(), McpAgent{"tester", 0});
      srv.setLogSink(nullptr);
      bool responded = false;
      roundTrip(srv, kInit, &responded);
      JsonValue r = roundTrip(srv, R"({"jsonrpc":"2.0","method":"resources/list","id":1})",
                              &responded);
      const JsonValue* list =
          r.find("result") ? r.find("result")->find("resources") : nullptr;
      check(list && list->isArray() && list->asArray().size() == 6,
            "mcp: resources/list has the 6 Phase 1 resources");

      auto readRes = [&](int id, const std::string& uri) {
        return roundTrip(
            srv,
            "{\"jsonrpc\":\"2.0\",\"method\":\"resources/read\",\"id\":" +
                std::to_string(id) + ",\"params\":{\"uri\":\"" + uri + "\"}}",
            &responded);
      };
      auto resText = [](const JsonValue& resp, bool* ok) -> std::string {
        const JsonValue* res = resp.find("result");
        const JsonValue* contents = res ? res->find("contents") : nullptr;
        const JsonValue* t =
            (contents && contents->isArray() && !contents->asArray().empty())
                ? contents->asArray()[0].find("text")
                : nullptr;
        *ok = t && t->isString();
        return *ok ? t->asString() : std::string();
      };

      // Console surface needs kPermConsole: denied at mask 0.
      r = readRes(2, "lithoscript://commands");
      check(errCode(r) == kMcpPermissionDenied, "mcp: commands resource denied at mask 0");
      srv.grantPermissions(static_cast<uint32_t>(kPermConsole | kPermWorldRead));
      bool ok = false;
      r = readRes(3, "lithoscript://commands");
      std::string text = resText(r, &ok);
      check(ok && text.find("help - show help") != std::string::npos,
            "mcp: lithoscript://commands lists commands");
      r = readRes(4, "lithoscript://cvars");
      text = resText(r, &ok);
      check(ok && text.find("sv_tps = 32") != std::string::npos,
            "mcp: lithoscript://cvars lists cvars");
      r = readRes(5, "world://dimensions");
      text = resText(r, &ok);
      check(ok && text.find("\"wafer\"") != std::string::npos,
            "mcp: world://dimensions lists dimensions");
      r = readRes(6, "mods://list");
      text = resText(r, &ok);
      check(ok && text.find("testmod") != std::string::npos,
            "mcp: mods://list lists mods");
      r = readRes(7, "game://lore-glossary");
      text = resText(r, &ok);
      check(ok && text.find("Kharge") != std::string::npos,
            "mcp: game://lore-glossary serves the injected text");
      r = readRes(8, "net://version");
      text = resText(r, &ok);
      JsonValue v;
      check(ok && parsesOk(text, &v) && v.find("engine")->asString() == "0.1.0" &&
                v.find("mcp")->asString() == "2025-11-25" &&
                v.find("protocol")->asInt() == 3,  // F-L3: kProtocolVersion, not V1
            "mcp: net://version reports engine/sdk/protocol/mcp versions");
      r = readRes(9, "bogus://nope");
      check(errCode(r) == kRpcInvalidParams, "mcp: unknown resource uri -> -32602");
    }

    // -- stdio framing through a scripted transport --
    {
      struct ScriptTransport : StdioTransport {
        explicit ScriptTransport(std::vector<std::string> in) : in_(std::move(in)) {}
        bool readLine(std::string& out) override {
          if (pos_ >= in_.size()) return false;
          out = in_[pos_++];
          return true;
        }
        void writeLine(std::string_view line) override {
          out_.emplace_back(line);
        }
        std::vector<std::string> in_, out_;
        size_t pos_ = 0;
      };

      McpServer srv(makeBackends(), McpAgent{"tester", 0});
      srv.setLogSink(nullptr);
      srv.grantPermissions(static_cast<uint32_t>(kPermWorldRead));
      ScriptTransport t({
          kInit,
          "",  // blank lines are tolerated
          "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\r",  // CR tolerated
          R"({"jsonrpc":"2.0","method":"tools/call","id":7,"params":{"name":"sim_status"}})",
      });
      srv.runStdio(t);
      check(t.out_.size() == 2, "mcp: stdio emits one line per request (blank/CR tolerated)");
      bool linesOk = t.out_.size() == 2;
      JsonValue r0, r1;
      if (linesOk) {
        linesOk = parsesOk(t.out_[0], &r0) && parsesOk(t.out_[1], &r1);
      }
      check(linesOk, "mcp: stdio response lines are valid JSON-RPC");
      if (linesOk) {
        bool isErr = false;
        const JsonValue* res = r1.find("result");
        const JsonValue* content = res ? res->find("content") : nullptr;
        const JsonValue* txt =
            (content && content->isArray() && !content->asArray().empty())
                ? content->asArray()[0].find("text")
                : nullptr;
        isErr = res && res->find("isError") && res->find("isError")->asBool();
        JsonValue v;
        check(!isErr && txt && txt->isString() && parsesOk(txt->asString(), &v) &&
                  v.find("tick")->asInt() == 12345,
              "mcp: stdio round-trips a tool call end to end");
      }
    }
  }

  // ---- 10. Phase 3: stateless era (2026-07-28) + prompts ----
  {
    struct StubSim : ISimStatus {
      uint64_t worldTick() override { return 777; }
      double currentTps() override { return 32.0; }
      int targetTps() override { return 32; }
      bool paused() override { return false; }
    };
    struct StubMods : IModListView {
      std::vector<ModEntry> mods() override { return {{"m", "1", 0, {}}}; }
    };
    McpBackends backends;
    backends.sim = std::make_shared<StubSim>();
    backends.mods = std::make_shared<StubMods>();

    auto roundTrip = [](McpServer& srv, std::string_view text, bool* responded) {
      std::optional<std::string> resp = srv.handleMessage(text);
      *responded = resp.has_value();
      if (!resp) return JsonValue::null();
      ParseResult r = parseJson(*resp);
      return r.ok ? r.value : JsonValue::null();
    };
    auto errCode = [](const JsonValue& resp) -> int {
      const JsonValue* e = resp.find("error");
      const JsonValue* c = e ? e->find("code") : nullptr;
      return (c && c->isInt()) ? static_cast<int>(c->asInt()) : 0;
    };
    // Builds a stateless-era request body: params carry _meta with the
    // spec-namespaced protocol version + client capabilities.
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
    auto resultOf = [](const JsonValue& resp) -> const JsonValue* {
      return resp.find("result");
    };

    // -- server/discover: era-agnostic, answers pre-handshake --
    {
      McpServer srv(backends);
      srv.setLogSink(nullptr);
      bool responded = false;
      JsonValue r = roundTrip(
          srv, R"({"jsonrpc":"2.0","id":1,"method":"server/discover"})",
          &responded);
      const JsonValue* res = resultOf(r);
      const JsonValue* vers = res ? res->find("supportedVersions") : nullptr;
      bool both = vers && vers->isArray() && vers->asArray().size() == 2 &&
                  vers->asArray()[0].asString() == "2025-11-25" &&
                  vers->asArray()[1].asString() == "2026-07-28";
      check(responded && res && both,
            "mcp3: server/discover advertises both protocol versions");
      const JsonValue* info = res ? res->find("serverInfo") : nullptr;
      check(info && info->find("name")->asString() == "litholattice" &&
                info->find("version")->asString() == mcpEngineVersionString(),
            "mcp3: server/discover carries server identity");
      const JsonValue* caps = res ? res->find("capabilities") : nullptr;
      check(caps && caps->find("tools") && caps->find("resources") &&
                caps->find("prompts"),
            "mcp3: server/discover capabilities declare tools/resources/prompts");
      const JsonValue* instr = res ? res->find("instructions") : nullptr;
      check(instr && instr->isString() && !instr->asString().empty(),
            "mcp3: server/discover has a non-empty instructions string");
      const JsonValue* ttl = res ? res->find("ttlMs") : nullptr;
      const JsonValue* scope = res ? res->find("cacheScope") : nullptr;
      check(ttl && ttl->asInt() == kMcpListTtlMs && scope &&
                scope->asString() == "public",
            "mcp3: server/discover carries ttlMs/cacheScope hints");
      check(!srv.initialized(),
            "mcp3: server/discover does not flip initialized state");
    }

    // -- stateless tools/list without any handshake --
    {
      McpServer srv(backends);
      srv.setLogSink(nullptr);
      bool responded = false;
      JsonValue r = roundTrip(srv, statelessBody("tools/list", 1, ""), &responded);
      const JsonValue* res = resultOf(r);
      const JsonValue* list = res ? res->find("tools") : nullptr;
      check(responded && res && list && list->isArray() &&
                list->asArray().size() == 20 && !srv.initialized(),
            "mcp3: stateless tools/list works with no initialize");
      const JsonValue* rt = res ? res->find("resultType") : nullptr;
      check(rt && rt->asString() == "complete",
            "mcp3: stateless result carries resultType \"complete\"");
      const JsonValue* meta = res ? res->find("_meta") : nullptr;
      const JsonValue* si =
          meta ? meta->find("io.modelcontextprotocol/serverInfo") : nullptr;
      check(si && si->find("name")->asString() == "litholattice",
            "mcp3: stateless result _meta carries server identity");
      const JsonValue* pv =
          meta ? meta->find("io.modelcontextprotocol/protocolVersion") : nullptr;
      check(pv && pv->asString() == "2026-07-28",
            "mcp3: stateless result _meta carries the protocol version");
      const JsonValue* ttl = res ? res->find("ttlMs") : nullptr;
      check(ttl && ttl->asInt() == kMcpListTtlMs,
            "mcp3: stateless tools/list carries ttlMs/cacheScope");
    }

    // -- lenient plain "protocolVersion" key also works --
    {
      McpServer srv(backends);
      srv.setLogSink(nullptr);
      bool responded = false;
      JsonValue r = roundTrip(
          srv,
          R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":{"protocolVersion":"2026-07-28"}}})",
          &responded);
      check(responded && resultOf(r),
            "mcp3: plain (non-namespaced) _meta protocolVersion is accepted");
    }

    // -- unsupported version in _meta -> -32022 --
    {
      McpServer srv(backends);
      srv.setLogSink(nullptr);
      bool responded = false;
      JsonValue r =
          roundTrip(srv, statelessBody("tools/list", 1, "", "2099-01-01"),
                    &responded);
      check(responded && errCode(r) == kRpcUnsupportedProtocolVersion,
            "mcp3: fictional protocol version -> -32022");
      const JsonValue* data =
          r.find("error") ? r.find("error")->find("data") : nullptr;
      const JsonValue* sup = data ? data->find("supported") : nullptr;
      check(sup && sup->isArray() && sup->asArray().size() == 2,
            "mcp3: -32022 names both supported versions");
      // ... and the strict probe shape: discover with a fictional version.
      r = roundTrip(
          srv,
          R"({"jsonrpc":"2.0","id":2,"method":"server/discover","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2099-01-01"}}})",
          &responded);
      check(responded && errCode(r) == kRpcUnsupportedProtocolVersion,
            "mcp3: server/discover with fictional version -> -32022");
    }

    // -- malformed _meta shapes --
    {
      McpServer srv(backends);
      srv.setLogSink(nullptr);
      bool responded = false;
      JsonValue r = roundTrip(
          srv,
          R"({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{"_meta":"nope"}})",
          &responded);
      check(responded && errCode(r) == kRpcInvalidParams,
            "mcp3: non-object _meta -> -32602");
      r = roundTrip(
          srv,
          R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"_meta":{}}})",
          &responded);
      check(responded && errCode(r) == kRpcInvalidParams,
            "mcp3: _meta without protocol version -> -32602");
      // A stateless notification gets no response, even a malformed one.
      roundTrip(
          srv,
          R"({"jsonrpc":"2.0","method":"tools/list","params":{"_meta":"nope"}})",
          &responded);
      check(!responded, "mcp3: malformed stateless notification -> no response");
      // Declaring the handshake era in _meta still requires the handshake.
      r = roundTrip(
          srv,
          R"({"jsonrpc":"2.0","id":3,"method":"tools/list","params":{"_meta":{"io.modelcontextprotocol/protocolVersion":"2025-11-25"}}})",
          &responded);
      check(responded && errCode(r) == kRpcInvalidRequest,
            "mcp3: _meta 2025-11-25 pre-init still needs initialize (-32600)");
    }

    // -- stateless tools/call: deny-by-default identity still applies --
    {
      McpServer srv(backends, McpAgent{"stateless-tester", 0});
      srv.setLogSink(nullptr);
      bool responded = false;
      auto callSim = [&](int id) {
        return roundTrip(
            srv, statelessBody("tools/call", id, R"("name":"sim_status")"),
            &responded);
      };
      JsonValue r = callSim(1);
      const JsonValue* res = resultOf(r);
      const JsonValue* isErr = res ? res->find("isError") : nullptr;
      const JsonValue* content = res ? res->find("content") : nullptr;
      const JsonValue* t0 = (content && content->isArray() &&
                             !content->asArray().empty())
                                ? content->asArray()[0].find("text")
                                : nullptr;
      check(responded && isErr && isErr->asBool() && t0 &&
                t0->asString().find("world_read") != std::string::npos,
            "mcp3: stateless tool call denied at mask 0 (isError, names bit)");
      // Same approval flow as the handshake era: console grants the bit.
      srv.grantPermissions(static_cast<uint32_t>(kPermWorldRead));
      r = callSim(2);
      res = resultOf(r);
      isErr = res ? res->find("isError") : nullptr;
      content = res ? res->find("content") : nullptr;
      t0 = (content && content->isArray() && !content->asArray().empty())
               ? content->asArray()[0].find("text")
               : nullptr;
      check(responded && isErr && !isErr->asBool() && t0 &&
                t0->asString().find("777") != std::string::npos,
            "mcp3: stateless tool call works after grant");
      // Stateless initialize stays the handshake-era method.
      r = roundTrip(
          srv,
          R"({"jsonrpc":"2.0","id":3,"method":"initialize","params":{"protocolVersion":"2025-11-25","_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})",
          &responded);
      res = resultOf(r);
      check(responded && res &&
                res->find("protocolVersion")->asString() == "2025-11-25" &&
                !res->find("resultType") && srv.initialized(),
            "mcp3: initialize with stateless _meta answers the legacy era");
    }

    // -- handshake-era regression: byte-identical legacy shapes --
    {
      McpServer srv(backends);
      srv.setLogSink(nullptr);
      bool responded = false;
      roundTrip(
          srv,
          R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{}}})",
          &responded);
      JsonValue r = roundTrip(
          srv, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})", &responded);
      const JsonValue* res = resultOf(r);
      check(responded && res && !res->find("resultType") && !res->find("_meta") &&
                !res->find("ttlMs"),
            "mcp3: legacy tools/list has no stateless envelope fields");
      r = roundTrip(srv, R"({"jsonrpc":"2.0","id":3,"method":"prompts/list"})",
                    &responded);
      res = resultOf(r);
      const JsonValue* list = res ? res->find("prompts") : nullptr;
      check(responded && list && list->isArray() &&
                list->asArray().size() == 3 && !res->find("ttlMs"),
            "mcp3: legacy prompts/list works, no envelope fields");
      // Legacy initialize now advertises prompts capability (additive).
      r = roundTrip(
          srv,
          R"({"jsonrpc":"2.0","id":4,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{}}})",
          &responded);
      res = resultOf(r);
      const JsonValue* caps = res ? res->find("capabilities") : nullptr;
      check(caps && caps->find("prompts"),
            "mcp3: initialize capabilities now include prompts");
    }

    // -- prompts/list: the 3 prompts --
    {
      McpServer srv(backends);
      srv.setLogSink(nullptr);
      bool responded = false;
      JsonValue r = roundTrip(srv, statelessBody("prompts/list", 1, ""),
                              &responded);
      const JsonValue* res = resultOf(r);
      const JsonValue* list = res ? res->find("prompts") : nullptr;
      bool namesOk = false, argsOk = false;
      if (list && list->isArray() && list->asArray().size() == 3) {
        namesOk = list->asArray()[0].find("name")->asString() ==
                      "agent_onboarding" &&
                  list->asArray()[1].find("name")->asString() ==
                      "build_contraption" &&
                  list->asArray()[2].find("name")->asString() == "diagnose";
        const JsonValue* a0 = list->asArray()[0].find("arguments");
        const JsonValue* a2 = list->asArray()[2].find("arguments");
        argsOk =
            a0 && a0->isArray() && a0->asArray().size() == 1 &&
            a0->asArray()[0].find("name")->asString() == "name" &&
            a0->asArray()[0].find("required")->asBool() &&
            a2 && a2->isArray() && a2->asArray().size() == 1 &&
            a2->asArray()[0].find("name")->asString() == "ticks" &&
            !a2->asArray()[0].find("required")->asBool();
      }
      check(namesOk, "mcp3: prompts/list has the 3 Phase 3 prompts");
      check(argsOk, "mcp3: prompt arguments declared (name required, ticks optional)");
      check(res && res->find("ttlMs") && res->find("cacheScope"),
            "mcp3: stateless prompts/list carries ttlMs/cacheScope");
    }

    // -- prompts/get: rendering + validation --
    {
      McpServer srv(backends, McpAgent{"p", 0});
      srv.setLogSink(nullptr);
      bool responded = false;
      auto getPrompt = [&](int id, const std::string& paramsInner) {
        return roundTrip(
            srv, statelessBody("prompts/get", id, paramsInner), &responded);
      };
      auto msgText = [](const JsonValue& resp, std::string* roleOut) {
        const JsonValue* res = resp.find("result");
        const JsonValue* msgs = res ? res->find("messages") : nullptr;
        const JsonValue* m0 =
            (msgs && msgs->isArray() && !msgs->asArray().empty())
                ? &msgs->asArray()[0]
                : nullptr;
        if (roleOut && m0 && m0->find("role"))
          *roleOut = m0->find("role")->asString();
        const JsonValue* c = m0 ? m0->find("content") : nullptr;
        const JsonValue* t0 =
            (c && c->isArray() && !c->asArray().empty())
                ? c->asArray()[0].find("text")
                : nullptr;
        return (t0 && t0->isString()) ? t0->asString() : std::string();
      };

      std::string role;
      JsonValue r =
          getPrompt(1, R"("name":"agent_onboarding","arguments":{"name":"Testy"})");
      std::string text = msgText(r, &role);
      check(responded && role == "user" && text.find("Testy") != std::string::npos &&
                text.find("world_query_blocks") != std::string::npos &&
                text.find("lithoscript://commands") != std::string::npos,
            "mcp3: agent_onboarding renders with the agent name, grounded in real surface names");

      r = getPrompt(2, R"("name":"build_contraption","arguments":{"what":"a 5x5 piston door"})");
      text = msgText(r, &role);
      check(responded && text.find("a 5x5 piston door") != std::string::npos &&
                text.find("no placement tool yet") != std::string::npos,
            "mcp3: build_contraption is honest about v1 read-only limits");

      r = getPrompt(3, R"("name":"diagnose")");
      text = msgText(r, &role);
      check(responded && text.find("600-tick") != std::string::npos &&
                text.find("not yet exposed over MCP") != std::string::npos,
            "mcp3: diagnose defaults to a 600-tick window, honest about the log");
      r = getPrompt(4, R"("name":"diagnose","arguments":{"ticks":"1200"})");
      text = msgText(r, &role);
      check(responded && text.find("1200-tick") != std::string::npos,
            "mcp3: diagnose honors the ticks argument");

      // Invalid argument shapes: -32602, never isError (the prompt result
      // type has no isError channel).
      r = getPrompt(5, R"("name":"agent_onboarding")");
      check(responded && errCode(r) == kRpcInvalidParams,
            "mcp3: prompts/get missing required arg -> -32602");
      r = getPrompt(6, R"("name":"agent_onboarding","arguments":{"name":42})");
      check(responded && errCode(r) == kRpcInvalidParams,
            "mcp3: prompts/get non-string arg -> -32602");
      r = getPrompt(7,
                    R"("name":"agent_onboarding","arguments":{"name":"x","bogus":"y"})");
      check(responded && errCode(r) == kRpcInvalidParams,
            "mcp3: prompts/get unknown arg -> -32602");
      r = getPrompt(8, R"("name":"diagnose","arguments":{"ticks":"fast"})");
      check(responded && errCode(r) == kRpcInvalidParams,
            "mcp3: prompts/get malformed ticks -> -32602");
      r = getPrompt(9, R"("name":"nope","arguments":{})");
      check(responded && errCode(r) == kRpcInvalidParams,
            "mcp3: prompts/get unknown prompt -> -32602 (spec: no isError channel)");
      r = getPrompt(10, R"("name":42)");
      check(responded && errCode(r) == kRpcInvalidParams,
            "mcp3: prompts/get non-string name -> -32602");
    }

    // -- stateless resources/read gets the private/no-cache envelope --
    {
      McpServer srv(backends, McpAgent{"p", 0});
      srv.setLogSink(nullptr);
      bool responded = false;
      JsonValue r = roundTrip(
          srv,
          statelessBody("resources/list", 1, ""),
          &responded);
      const JsonValue* res = resultOf(r);
      check(responded && res && res->find("ttlMs") &&
                res->find("cacheScope")->asString() == "public",
            "mcp3: stateless resources/list is cacheScope public");
      r = roundTrip(
          srv, statelessBody("resources/read", 2, R"("uri":"mods://list")"),
          &responded);
      res = resultOf(r);
      const JsonValue* contents = res ? res->find("contents") : nullptr;
      check(responded && res && contents && contents->isArray() &&
                !contents->asArray().empty() &&
                res->find("ttlMs")->asInt() == 0 &&
                res->find("cacheScope")->asString() == "private",
            "mcp3: stateless resources/read is cacheScope private, ttlMs 0");
    }
  }

  // ---- 11. Phase 4: gated write tools + hardening ----
  {
    // Sim-thread-marshaling fake: setBlock enqueues a job; a worker thread
    // (standing in for the sim thread) applies it after a short delay and
    // signals completion. hang=true makes jobs never complete (timeout test).
    struct FakeWriter : IWorldWriter {
      struct CallState {
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
      };
      struct Job {
        int x = 0, y = 0, z = 0;
        uint16_t id = 0;
        std::shared_ptr<CallState> st;
      };
      std::mutex m_;
      std::condition_variable cv_;
      std::queue<Job> q_;
      std::atomic<bool> stop_{false};
      std::atomic<bool> hang_{false};
      std::thread worker_;
      std::mutex am_;
      int ax_ = 0, ay_ = 0, az_ = 0;
      uint16_t aid_ = 0;
      std::thread::id appliedBy_;
      int applied_ = 0;

      FakeWriter() : worker_([this] { run(); }) {}
      ~FakeWriter() {
        stop_ = true;
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
      }
      void run() {
        for (;;) {
          Job j;
          {
            std::unique_lock l(m_);
            cv_.wait(l, [&] { return stop_ || !q_.empty(); });
            if (stop_) return;
            j = std::move(q_.front());
            q_.pop();
          }
          if (hang_) continue;  // the sim thread never gets to it: caller must time out
          std::this_thread::sleep_for(std::chrono::milliseconds(5));  // sim-tick latency
          {
            std::lock_guard l(am_);
            ax_ = j.x;
            ay_ = j.y;
            az_ = j.z;
            aid_ = j.id;
            appliedBy_ = std::this_thread::get_id();
            ++applied_;
          }
          {
            std::lock_guard l(j.st->m);
            j.st->done = true;
          }
          j.st->cv.notify_one();
        }
      }
      bool setBlock(int x, int y, int z, uint16_t id, int timeoutMs,
                    std::string& why) override {
        auto st = std::make_shared<CallState>();
        {
          std::lock_guard l(m_);
          q_.push(Job{x, y, z, id, st});
        }
        cv_.notify_one();
        std::unique_lock l(st->m);
        if (!st->cv.wait_for(l, std::chrono::milliseconds(timeoutMs),
                             [&] { return st->done; })) {
          why = "timed out after " + std::to_string(timeoutMs) + "ms waiting for the sim thread";
          return false;
        }
        return true;
      }
    };

    // Agent-scoped console fake. It mirrors the real adapter + registry
    // contract faithfully (the registry side itself is proven in the
    // console selftest): a restricted caller mask revokes cheat authority
    // even when the player context allows cheats. What THIS test proves is
    // the MCP layer's half of the rule — the handler passes the AGENT's
    // permission mask (recorded in lastMask), never the player's.
    struct FakeExec : IConsoleExec {
      std::string lastLine;
      uint32_t lastMask = 0;
      void executeLine(std::string_view line, uint32_t callerMask,
                       std::vector<std::string>& out) override {
        lastLine = std::string(line);
        lastMask = callerMask;
        // 0xFFFFFFFF == console::kCallerFullAuthority (the unrestricted mask).
        if (callerMask != 0xFFFFFFFFu && line == "mcp_test_cheat") {
          out.push_back("command 'mcp_test_cheat' requires cheats (not allowed in this context)");
          return;
        }
        if (line == "mcp_test_echo hello") {
          out.push_back("hello");
          return;
        }
        if (line == "mcp_test_echo") return;  // no output
        out.push_back(std::string("ran: ") + std::string(line));
      }
    };

    auto roundTrip = [](McpServer& srv, std::string_view text, bool* responded) {
      std::optional<std::string> resp = srv.handleMessage(text);
      *responded = resp.has_value();
      if (!resp) return JsonValue::null();
      ParseResult r = parseJson(*resp);
      return r.ok ? r.value : JsonValue::null();
    };
    auto errCode = [](const JsonValue& resp) -> int {
      const JsonValue* e = resp.find("error");
      const JsonValue* c = e ? e->find("code") : nullptr;
      return (c && c->isInt()) ? static_cast<int>(c->asInt()) : 0;
    };
    auto toolText = [](const JsonValue& resp, bool* isError) -> std::string {
      const JsonValue* res = resp.find("result");
      const JsonValue* err = res ? res->find("isError") : nullptr;
      *isError = err && err->isBool() && err->asBool();
      const JsonValue* content = res ? res->find("content") : nullptr;
      const JsonValue* t0 =
          (content && content->isArray() && !content->asArray().empty())
              ? content->asArray()[0].find("text")
              : nullptr;
      return (t0 && t0->isString()) ? t0->asString() : std::string();
    };
    auto callTool = [&](McpServer& srv, int id, const std::string& name,
                        const std::string& args) {
      bool responded = false;
      JsonValue r = roundTrip(srv,
                              R"({"jsonrpc":"2.0","method":"tools/call","id":)" +
                                  std::to_string(id) + R"(,"params":{"name":")" + name +
                                  R"(","arguments":)" + args + "}}",
                              &responded);
      return std::make_pair(responded, r);
    };
    const std::string kInit =
        R"({"jsonrpc":"2.0","method":"initialize","id":1,"params":{"protocolVersion":"2025-11-25","capabilities":{}}})";
    const std::thread::id mainThread = std::this_thread::get_id();

    // -- write tools denied on a fresh connection (deny-by-default) --
    {
      auto writer = std::make_shared<FakeWriter>();
      auto exec = std::make_shared<FakeExec>();
      McpBackends b;
      b.writer = writer;
      b.consoleExec = exec;
      McpServer srv(std::move(b), McpAgent{"phase4", 0});
      srv.setLogSink(nullptr);
      bool responded = false;
      roundTrip(srv, kInit, &responded);
      bool isErr = false;
      auto [r1, j1] = callTool(srv, 2, "world_set_block", R"({"x":1,"y":2,"z":3,"block":5})");
      std::string t1 = toolText(j1, &isErr);
      check(r1 && isErr && t1.find("world_write") != std::string::npos,
            "mcp4: world_set_block denied without grant names world_write");
      auto [r2, j2] = callTool(srv, 3, "lithoscript_exec", R"({"line":"mcp_test_echo hi"})");
      std::string t2 = toolText(j2, &isErr);
      check(r2 && isErr && t2.find("console") != std::string::npos,
            "mcp4: lithoscript_exec denied without grant names console");
      check(writer->applied_ == 0, "mcp4: denied write never reached the sim thread");
    }

    // -- grant -> applied on the sim thread; revoke -> denied again --
    {
      auto writer = std::make_shared<FakeWriter>();
      McpBackends b;
      b.writer = writer;
      McpServer srv(std::move(b), McpAgent{"phase4", 0});
      srv.setLogSink(nullptr);
      bool responded = false;
      roundTrip(srv, kInit, &responded);
      srv.grantPermissions(static_cast<uint32_t>(kPermWorldWrite));
      bool isErr = false;
      auto [r1, j1] = callTool(srv, 2, "world_set_block", R"({"x":7,"y":64,"z":-9,"block":12})");
      std::string t1 = toolText(j1, &isErr);
      check(r1 && !isErr && t1.find("\"applied\":true") != std::string::npos,
            "mcp4: granted world_set_block applies");
      {
        std::lock_guard l(writer->am_);
        check(writer->applied_ == 1 && writer->ax_ == 7 && writer->ay_ == 64 &&
                  writer->az_ == -9 && writer->aid_ == 12,
              "mcp4: sim thread applied the right coords/block");
        check(writer->appliedBy_ != mainThread,
              "mcp4: write was applied off the calling thread (sim-thread marshal)");
      }
      // double-grant is idempotent: the mask does not change.
      const uint32_t m1 = srv.permissionMask();
      srv.grantPermissions(static_cast<uint32_t>(kPermWorldWrite));
      check(srv.permissionMask() == m1, "mcp4: double grant is idempotent");
      srv.revokePermissions(static_cast<uint32_t>(kPermWorldWrite));
      auto [r2, j2] = callTool(srv, 3, "world_set_block", R"({"x":1,"y":2,"z":3,"block":5})");
      std::string t2 = toolText(j2, &isErr);
      check(r2 && isErr && t2.find("world_write") != std::string::npos,
            "mcp4: revoke -> world_set_block denied again");
    }

    // -- malformed write args are protocol errors (-32602), not isError --
    {
      auto writer = std::make_shared<FakeWriter>();
      McpBackends b;
      b.writer = writer;
      McpServer srv(std::move(b), McpAgent{"phase4", 0});
      srv.setLogSink(nullptr);
      bool responded = false;
      roundTrip(srv, kInit, &responded);
      srv.grantPermissions(static_cast<uint32_t>(kPermWorldWrite));
      auto [r1, j1] = callTool(srv, 2, "world_set_block", R"({"x":1,"y":2,"z":3,"block":70000})");
      check(r1 && errCode(j1) == kRpcInvalidParams, "mcp4: block id >65535 -> -32602");
      auto [r2, j2] = callTool(srv, 3, "world_set_block", R"({"x":"one","y":2,"z":3,"block":5})");
      check(r2 && errCode(j2) == kRpcInvalidParams, "mcp4: non-integer coord -> -32602");
    }

    // -- write timeout -> isError, never a hang --
    {
      auto writer = std::make_shared<FakeWriter>();
      writer->hang_ = true;  // the sim thread never completes the job
      McpBackends b;
      b.writer = writer;
      McpServer srv(std::move(b), McpAgent{"phase4", 0});
      srv.setLogSink(nullptr);
      bool responded = false;
      roundTrip(srv, kInit, &responded);
      srv.grantPermissions(static_cast<uint32_t>(kPermWorldWrite));
      bool isErr = false;
      auto [r1, j1] =
          callTool(srv, 2, "world_set_block", R"({"x":1,"y":2,"z":3,"block":5,"timeout_ms":50})");
      std::string t1 = toolText(j1, &isErr);
      check(r1 && isErr && t1.find("timed out") != std::string::npos,
            "mcp4: hung sim thread -> isError timeout, no hang");
      writer->hang_ = false;
    }

    // -- unwired writer backend -> isError naming the seam --
    {
      McpBackends b;  // no writer wired
      McpServer srv(std::move(b), McpAgent{"phase4", 0});
      srv.setLogSink(nullptr);
      bool responded = false;
      roundTrip(srv, kInit, &responded);
      srv.grantPermissions(static_cast<uint32_t>(kPermWorldWrite));
      bool isErr = false;
      auto [r1, j1] = callTool(srv, 2, "world_set_block", R"({"x":1,"y":2,"z":3,"block":5})");
      std::string t1 = toolText(j1, &isErr);
      check(r1 && isErr && t1.find("not available") != std::string::npos,
            "mcp4: null writer seam -> isError, never a crash");
    }

    // -- write rate limit trips -> isError naming the limit --
    {
      auto writer = std::make_shared<FakeWriter>();
      McpBackends b;
      b.writer = writer;
      McpServer srv(std::move(b), McpAgent{"phase4", 0});
      int64_t now = 1000000;
      srv.setRateLimitClock([&]() { return now; });  // frozen clock: no refills
      srv.setLogSink(nullptr);
      bool responded = false;
      roundTrip(srv, kInit, &responded);
      srv.grantPermissions(static_cast<uint32_t>(kPermWorldWrite));
      bool ok = true, isErr = false;
      for (int i = 0; i < 64; ++i) {
        auto [rr, jj] = callTool(srv, 100 + i, "world_set_block", R"({"x":1,"y":2,"z":3,"block":5})");
        std::string tt = toolText(jj, &isErr);
        if (!rr || isErr) ok = false;
      }
      check(ok, "mcp4: 64 block writes fit the burst");
      auto [r1, j1] = callTool(srv, 200, "world_set_block", R"({"x":1,"y":2,"z":3,"block":5})");
      std::string t1 = toolText(j1, &isErr);
      check(r1 && isErr && t1.find("32") != std::string::npos && t1.find("64") != std::string::npos,
            "mcp4: 65th write -> isError naming the 32/sec burst-64 limit");
      now += 2000;  // 2 s at 32/sec refills the 64-burst bucket
      auto [r2, j2] = callTool(srv, 201, "world_set_block", R"({"x":1,"y":2,"z":3,"block":5})");
      std::string t2 = toolText(j2, &isErr);
      check(r2 && !isErr, "mcp4: write bucket refills over time");
    }

    // -- per-connection tool-call rate cap --
    {
      struct StubSim : ISimStatus {
        uint64_t worldTick() override { return 1; }
        double currentTps() override { return 32.0; }
        int targetTps() override { return 32; }
        bool paused() override { return false; }
      };
      McpBackends b;
      b.sim = std::make_shared<StubSim>();
      McpServer srv(std::move(b), McpAgent{"phase4", 0});
      int64_t now = 5000000;
      srv.setRateLimitClock([&]() { return now; });  // frozen clock: no refills
      srv.setLogSink(nullptr);
      bool responded = false;
      roundTrip(srv, kInit, &responded);
      srv.grantPermissions(static_cast<uint32_t>(kPermWorldRead));
      bool ok = true;
      for (int i = 0; i < 512; ++i) {
        bool rr = false;
        roundTrip(srv,
                  R"({"jsonrpc":"2.0","method":"tools/call","id":)" + std::to_string(1000 + i) +
                      R"(,"params":{"name":"sim_status"}})",
                  &rr);
        if (!rr) ok = false;
      }
      check(ok, "mcp4: 512 tool calls fit the burst");
      bool isErr = false;
      auto [r1, j1] = callTool(srv, 2000, "sim_status", "{}");
      std::string t1 = toolText(j1, &isErr);
      check(r1 && isErr && t1.find("256") != std::string::npos,
            "mcp4: 513th tool call -> isError naming the 256/sec cap");
      now += 1000;  // 1 s at 256/sec refills 256 tokens
      auto [r2, j2] = callTool(srv, 2001, "sim_status", "{}");
      std::string t2 = toolText(j2, &isErr);
      check(r2 && !isErr, "mcp4: tool-call bucket refills over time");
    }

    // -- token bucket unit checks (fake clock) --
    {
      int64_t now = 0;
      McpTokenBucket bkt(32, 64, [&]() { return now; });
      check(bkt.take(64), "mcp4: bucket starts full (burst 64)");
      check(!bkt.take(1), "mcp4: empty bucket refuses");
      now += 1000;  // 32 tokens/sec
      check(bkt.balance() == 32, "mcp4: 1 s refills 32 tokens");
      check(bkt.take(32) && !bkt.take(1), "mcp4: partial take then empty");
      now += 100000;  // huge jump caps at burst, never above
      check(bkt.balance() == 64, "mcp4: refill caps at burst");
      check(bkt.take(0), "mcp4: take(0) always succeeds");
    }

    // -- lithoscript_exec runs at the AGENT's mask, never the player's --
    {
      auto exec = std::make_shared<FakeExec>();
      McpBackends b;
      b.consoleExec = exec;
      McpServer srv(std::move(b), McpAgent{"phase4", 0});
      srv.setLogSink(nullptr);
      bool responded = false;
      roundTrip(srv, kInit, &responded);
      srv.grantPermissions(static_cast<uint32_t>(kPermConsole));
      bool isErr = false;
      // The handler must pass the AGENT's mask (console only) to the seam —
      // never the player's full authority. The fake refuses the cheat-gated
      // line exactly as the real registry does for a restricted mask.
      auto [r1, j1] = callTool(srv, 2, "lithoscript_exec", R"({"line":"mcp_test_cheat"})");
      std::string t1 = toolText(j1, &isErr);
      check(r1 && !isErr && t1.find("requires cheats") != std::string::npos &&
                exec->lastMask == static_cast<uint32_t>(kPermConsole),
            "mcp4: exec runs at the agent's mask (cheat refused, mask recorded)");
      auto [r2, j2] = callTool(srv, 3, "lithoscript_exec", R"({"line":"mcp_test_echo hello"})");
      std::string t2 = toolText(j2, &isErr);
      check(r2 && !isErr && t2 == "hello", "mcp4: plain command runs, output captured");
      // No output -> the friendly marker, still ok.
      auto [r3, j3] =
          callTool(srv, 4, "lithoscript_exec", R"({"line":"mcp_test_echo"})");
      std::string t3 = toolText(j3, &isErr);
      check(r3 && !isErr, "mcp4: empty output is ok");
      // Oversized line -> -32602 (malformed), not isError.
      std::string big(70000, 'x');
      auto [r4, j4] =
          callTool(srv, 5, "lithoscript_exec", "{\"line\":\"" + big + "\"}");
      check(r4 && errCode(j4) == kRpcInvalidParams, "mcp4: >64 KiB line -> -32602");
      // Unwired exec backend -> isError.
      McpBackends b2;
      McpServer srv2(std::move(b2), McpAgent{"phase4", 0});
      srv2.setLogSink(nullptr);
      roundTrip(srv2, kInit, &responded);
      srv2.grantPermissions(static_cast<uint32_t>(kPermConsole));
      auto [r5, j5] = callTool(srv2, 6, "lithoscript_exec", R"({"line":"mcp_test_echo hi"})");
      std::string t5 = toolText(j5, &isErr);
      check(r5 && isErr && t5.find("not available") != std::string::npos,
            "mcp4: null consoleExec seam -> isError, never a crash");
    }

    // -- audit: every write/exec call is logged with the mcp: prefix --
    {
      auto writer = std::make_shared<FakeWriter>();
      auto exec = std::make_shared<FakeExec>();
      McpBackends b;
      b.writer = writer;
      b.consoleExec = exec;
      std::vector<std::string> logLines;
      McpServer srv(std::move(b), McpAgent{"audit", 0});
      srv.setLogSink([&logLines](std::string_view l) { logLines.emplace_back(l); });
      bool responded = false;
      roundTrip(srv, kInit, &responded);
      srv.grantPermissions(static_cast<uint32_t>(kPermWorldWrite | kPermConsole));
      bool isErr = false;
      auto [r1, j1] = callTool(srv, 2, "world_set_block", R"({"x":1,"y":2,"z":3,"block":5})");
      toolText(j1, &isErr);
      auto [r2, j2] = callTool(srv, 3, "lithoscript_exec", R"({"line":"mcp_test_echo hi"})");
      toolText(j2, &isErr);
      auto has = [&](const std::string& needle) {
        for (const auto& l : logLines)
          if (l.rfind("mcp:", 0) == 0 && l.find(needle) != std::string::npos) return true;
        return false;
      };
      auto hasAny = [&](const std::string& needle) {
        for (const auto& l : logLines)
          if (l.find(needle) != std::string::npos) return true;
        return false;
      };
      check(has("write world_set_block") && has("agent=audit") && has("(1,2,3)=5") && has("-> ok"),
            "mcp4: audit logs the write (who/what/result) with the mcp: prefix");
      check(has("exec lithoscript_exec") && has("agent=audit") && has("mask=") && has("console"),
            "mcp4: audit logs the exec with the agent's mask");
      // The Phase 1/2 tool audit line (no mcp: prefix, unchanged) still fires.
      check(hasAny("tool world_set_block agent=audit ok"),
            "mcp4: the standard tool audit line still fires for writes");
    }
    // Test-only base64 decoder (validates tool output; production only encodes).
    auto testB64Decode = [](const std::string& in) {
      auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
      };
      std::vector<uint8_t> out;
      for (size_t i = 0; i + 4 <= in.size(); i += 4) {
        int a = val(in[i]), b = val(in[i + 1]);
        int c = in[i + 2] == '=' ? 0 : val(in[i + 2]);
        int d = in[i + 3] == '=' ? 0 : val(in[i + 3]);
        if (a < 0 || b < 0) break;
        out.push_back((uint8_t)((a << 2) | (b >> 4)));
        if (in[i + 2] != '=') out.push_back((uint8_t)((b << 4) | (c >> 2)));
        if (in[i + 3] != '=') out.push_back((uint8_t)((c << 6) | d));
      }
      return out;
    };

    // -- pilot senses: base64 + PNG --
    {
      check(base64Encode(nullptr, 0).empty(), "senses: base64 of empty is empty");
      const uint8_t foobar[] = {'f', 'o', 'o', 'b', 'a', 'r'};
      check(base64Encode(foobar, 6) == "Zm9vYmFy", "senses: base64 known vector (foobar)");
      const uint8_t f[] = {'f'};
      check(base64Encode(f, 1) == "Zg==", "senses: base64 padding (1 byte)");
      const uint8_t fo[] = {'f', 'o'};
      check(base64Encode(fo, 2) == "Zm8=", "senses: base64 padding (2 bytes)");

      // 1x1 red pixel: structural check of our clean-room PNG.
      const uint8_t red[3] = {255, 0, 0};
      std::vector<uint8_t> png;
      check(encodePngRgb(1, 1, red, png) && png.size() > 8 &&
                png[0] == 137 && png[1] == 80 && png[2] == 78 && png[3] == 71 &&
                png[4] == 13 && png[5] == 10 && png[6] == 26 && png[7] == 10,
            "senses: PNG signature correct");
      // IHDR chunk: length 13, type IHDR, width/height = 1.
      const bool ihdr = png.size() > 29 && png[8] == 0 && png[9] == 0 && png[10] == 0 &&
                        png[11] == 13 && png[12] == 'I' && png[13] == 'H' &&
                        png[14] == 'D' && png[15] == 'R' && png[16] == 0 && png[17] == 0 &&
                        png[18] == 0 && png[19] == 1 && png[24] == 8 && png[25] == 2;
      check(ihdr, "senses: PNG IHDR carries width=1 height=1 depth=8 RGB");
      // IEND present at the tail.
      const size_t n = png.size();
      const bool iend = n > 12 && png[n - 12] == 0 && png[n - 11] == 0 && png[n - 10] == 0 &&
                        png[n - 9] == 0 && png[n - 8] == 'I' && png[n - 7] == 'E' &&
                        png[n - 6] == 'N' && png[n - 5] == 'D';
      check(iend, "senses: PNG ends with IEND");
      // Deterministic: same pixels -> byte-identical output (viewpoint tests rely on it).
      std::vector<uint8_t> png2;
      check(encodePngRgb(1, 1, red, png2) && png == png2, "senses: PNG encoding is deterministic");
      // Different pixels -> different bytes (a moved camera changes the frame).
      const uint8_t blue[3] = {0, 0, 255};
      std::vector<uint8_t> png3;
      check(encodePngRgb(1, 1, blue, png3) && png3 != png, "senses: different pixels -> different PNG");
      // Rejects bad dimensions.
      std::vector<uint8_t> bad;
      check(!encodePngRgb(0, 16, red, bad) && !encodePngRgb(16, 0, red, bad) &&
                !encodePngRgb(5000, 16, red, bad) && !encodePngRgb(16, 16, nullptr, bad),
            "senses: PNG rejects empty/oversize/null input");
    }

    // -- pilot senses: permission bit + tiers --
    {
      check(static_cast<uint32_t>(kPermPilotSenses) == (1u << 11),
            "senses: kPermPilotSenses is bit 11");
      check(kPermPilotSenses != kPermPlayerControl,
            "senses: pilot_senses is a NEW bit, not player_control (observe vs actuate)");
      check(permBitName(kPermPilotSenses) == "pilot_senses",
            "senses: permBitName(kPermPilotSenses) = \"pilot_senses\"");
      check(kAudioTierCount == 4 && kAudioTierRates[0] == 8000 && kAudioTierRates[1] == 16000 &&
                kAudioTierRates[2] == 24000 && kAudioTierRates[3] == 48000,
            "senses: audio tier ladder is 8/16/24/48 kHz (tier 0 = lowest)");
      check(kAudioChunkFrames == 1024, "senses: audio chunk is 1024 frames");
    }

    // -- pilot senses: tool surface --
    {
      struct StubVision : IVisionCapture {
        std::vector<std::string> pilotIds() const override { return {"player"}; }
        bool captureVisionFrame(std::string_view pilot, int w, int h,
                                VisionFrame& out) override {
          if (pilot != "player") return false;
          out.width = w > 0 ? w : 160;
          out.height = h > 0 ? h : 90;
          std::vector<uint8_t> rgb(size_t(out.width) * size_t(out.height) * 3, 128);
          rgb[0] = 255; rgb[1] = 0; rgb[2] = 0;
          if (!encodePngRgb(out.width, out.height, rgb.data(), out.png)) return false;
          out.tick = 777;
          // The reported camera follows the pose: free camera when set.
          out.camX = cam.x; out.camY = cam.y; out.camZ = cam.z;
          out.yaw = cam.yaw; out.pitch = cam.pitch;
          return true;
        }
        bool setCameraPose(std::string_view pilot, const CameraPose& pose) override {
          if (pilot != "player") return false;
          if (pose.pitch < -90 || pose.pitch > 90 || pose.fov < 10 || pose.fov > 120)
            return false;
          cam = pose;
          cam.trackPlayer = false;
          return true;
        }
        bool trackPlayer(std::string_view pilot) override {
          if (pilot != "player") return false;
          cam = CameraPose{};
          return true;
        }
        bool cameraPose(std::string_view pilot, CameraPose& out) const override {
          if (pilot != "player") return false;
          out = cam;
          return true;
        }
        CameraPose cam;  // default = trackPlayer at origin
      };
      struct StubAudio : IAudioCapture {
        std::vector<std::string> pilotIds() const override { return {"player"}; }
        bool captureAudioChunk(std::string_view pilot, int tier,
                               AudioChunk& out) override {
          if (pilot != "player" || tier < 0 || tier > 3) return false;
          out.sampleRate = kAudioTierRates[tier];
          out.samples.assign(kAudioChunkFrames, int16_t(1000));
          out.tick = 778;
          return true;
        }
      };
      auto senseCall = [&](McpServer& srv, const std::string& body) {
        bool responded = false;
        JsonValue r = roundTrip(srv, body, &responded);
        return std::make_pair(responded, r);
      };
      auto makeSensesBackends = [&]() {
        McpBackends b;
        b.vision = std::make_shared<StubVision>();
        b.audio = std::make_shared<StubAudio>();
        return b;
      };
      const std::string kVisionCall =
          R"({"jsonrpc":"2.0","method":"tools/call","id":1,"params":{"name":"vision.capture_frame","arguments":{"pilot":"player"}}})";
      const std::string kAudioCall =
          R"({"jsonrpc":"2.0","method":"tools/call","id":1,"params":{"name":"audio.capture_chunk","arguments":{"pilot":"player"}}})";

      // Deny-by-default: mask 0 names the missing bit.
      {
        McpServer srv(makeSensesBackends(), McpAgent{"tester", 0});
        srv.setLogSink(nullptr);
        bool responded = false;
        roundTrip(srv, kInit, &responded);
        bool isErr = false;
        auto [r1, j1] = senseCall(srv, kVisionCall);
        std::string text = toolText(j1, &isErr);
        check(r1 && isErr && text.find("pilot_senses") != std::string::npos,
              "senses: vision denied at mask 0, names the pilot_senses bit");
        auto [r2, j2] = senseCall(srv, kAudioCall);
        text = toolText(j2, &isErr);
        check(r2 && isErr && text.find("pilot_senses") != std::string::npos,
              "senses: audio denied at mask 0, names the pilot_senses bit");
      }
      // Granted: vision returns a real PNG through the whole tool path.
      {
        McpServer srv(makeSensesBackends(), McpAgent{"tester", 0});
        srv.setLogSink(nullptr);
        bool responded = false;
        roundTrip(srv, kInit, &responded);
        srv.grantPermissions(static_cast<uint32_t>(kPermPilotSenses));
        bool isErr = false;
        JsonValue v;
        auto [r1, j1] = senseCall(srv, kVisionCall);
        std::string text = toolText(j1, &isErr);
        const bool ok = r1 && !isErr && parsesOk(text, &v) && v.find("width")->asInt() == 160 &&
                        v.find("height")->asInt() == 90 &&
                        v.find("format")->asString() == "png" &&
                        v.find("tick")->asInt() == 777 &&
                        v.find("camera")->find("x")->asDouble() == 0.0;
        // The base64 payload starts with the PNG signature ("iVBORw0KGgo=").
        const std::string data = v.find("data")->asString();
        // End-to-end PNG validity: base64-decode (test-only decoder) and
        // check the signature + IHDR of what the tool actually returned.
        const std::vector<uint8_t> raw = testB64Decode(data);
        const bool pngOk =
            raw.size() > 29 && raw[0] == 137 && raw[1] == 80 && raw[2] == 78 &&
            raw[3] == 71 && raw[4] == 13 && raw[5] == 10 && raw[6] == 26 &&
            raw[7] == 10 && raw[12] == 'I' && raw[13] == 'H' && raw[14] == 'D' &&
            raw[15] == 'R' && raw[16] == 0 && raw[17] == 0 && raw[18] == 0 &&
            raw[19] == 160 && raw[23] == 90;
        check(ok && pngOk,
              "senses: vision.capture_frame returns valid PNG + camera (default 160x90)");
        // Width/height override honored.
        auto [r2, j2] = senseCall(
            srv, R"({"jsonrpc":"2.0","method":"tools/call","id":2,"params":{"name":"vision.capture_frame","arguments":{"pilot":"player","width":320,"height":180}}})");
        text = toolText(j2, &isErr);
        check(r2 && !isErr && parsesOk(text, &v) && v.find("width")->asInt() == 320 &&
                  v.find("height")->asInt() == 180,
              "senses: vision width/height override honored");
        // Independent camera: set_camera moves it, captures report it, track_player resets.
        auto [rc1, jc1] = senseCall(
            srv, R"({"jsonrpc":"2.0","method":"tools/call","id":10,"params":{"name":"vision.set_camera","arguments":{"pilot":"player","x":100,"y":64,"z":-200,"yaw":90,"pitch":-15,"fov":60}}})");
        text = toolText(jc1, &isErr);
        check(rc1 && !isErr && text.find("applied") != std::string::npos,
              "senses: vision.set_camera places the pilot camera");
        auto [rc2, jc2] = senseCall(srv, kVisionCall);
        text = toolText(jc2, &isErr);
        check(rc2 && !isErr && parsesOk(text, &v) &&
                  v.find("camera")->find("x")->asDouble() == 100.0 &&
                  v.find("camera")->find("yaw")->asDouble() == 90.0,
              "senses: capture reports the free camera pose");
        auto [rc3, jc3] = senseCall(
            srv, R"({"jsonrpc":"2.0","method":"tools/call","id":11,"params":{"name":"vision.track_player","arguments":{"pilot":"player"}}})");
        text = toolText(jc3, &isErr);
        check(rc3 && !isErr && text.find("applied") != std::string::npos,
              "senses: vision.track_player returns to player tracking");
        // set_camera is deny-by-default too.
        {
          McpServer srv2(makeSensesBackends(), McpAgent{"tester", 0});
          srv2.setLogSink(nullptr);
          bool resp = false;
          roundTrip(srv2, kInit, &resp);
          bool e2 = false;
          auto [rr, jj] = senseCall(
              srv2, R"({"jsonrpc":"2.0","method":"tools/call","id":1,"params":{"name":"vision.set_camera","arguments":{"pilot":"player","x":1,"y":2,"z":3,"yaw":0,"pitch":0}}})");
          std::string t2 = toolText(jj, &e2);
          check(rr && e2 && t2.find("pilot_senses") != std::string::npos,
                "senses: vision.set_camera denied at mask 0");
        }
        // Unknown pilot rejected.
        auto [r3, j3] = senseCall(
            srv, R"({"jsonrpc":"2.0","method":"tools/call","id":3,"params":{"name":"vision.capture_frame","arguments":{"pilot":"ghost"}}})");
        text = toolText(j3, &isErr);
        check(r3 && isErr && text.find("unknown pilot") != std::string::npos,
              "senses: vision rejects an unknown pilot");
      }
      // Granted: audio returns tiered PCM.
      {
        McpServer srv(makeSensesBackends(), McpAgent{"tester", 0});
        srv.setLogSink(nullptr);
        bool responded = false;
        roundTrip(srv, kInit, &responded);
        srv.grantPermissions(static_cast<uint32_t>(kPermPilotSenses));
        bool isErr = false;
        JsonValue v;
        auto [r1, j1] = senseCall(srv, kAudioCall);
        std::string text = toolText(j1, &isErr);
        check(r1 && !isErr && parsesOk(text, &v) && v.find("sampleRate")->asInt() == 8000 &&
                  v.find("channels")->asInt() == 1 &&
                  v.find("format")->asString() == "pcm_s16le" &&
                  v.find("frames")->asInt() == 1024 && v.find("tick")->asInt() == 778 &&
                  !v.find("data")->asString().empty(),
              "senses: audio.capture_chunk defaults to tier 0 (8 kHz, 1024 frames)");
        auto [r2, j2] = senseCall(
            srv, R"({"jsonrpc":"2.0","method":"tools/call","id":2,"params":{"name":"audio.capture_chunk","arguments":{"pilot":"player","tier":2}}})");
        text = toolText(j2, &isErr);
        check(r2 && !isErr && parsesOk(text, &v) && v.find("sampleRate")->asInt() == 24000,
              "senses: audio tier 2 -> 24 kHz (cvar ramp changes the output)");
        auto [r3, j3] = senseCall(
            srv, R"({"jsonrpc":"2.0","method":"tools/call","id":3,"params":{"name":"audio.capture_chunk","arguments":{"pilot":"ghost"}}})");
        text = toolText(j3, &isErr);
        check(r3 && isErr && text.find("unknown pilot") != std::string::npos,
              "senses: audio rejects an unknown pilot");
      }
      // Not wired: null backends report it.
      {
        McpServer srv(McpBackends{}, McpAgent{"tester", 0});
        srv.setLogSink(nullptr);
        bool responded = false;
        roundTrip(srv, kInit, &responded);
        srv.grantPermissions(static_cast<uint32_t>(kPermPilotSenses));
        bool isErr = false;
        auto [r1, j1] = senseCall(srv, kVisionCall);
        std::string text = toolText(j1, &isErr);
        check(r1 && isErr && text.find("not wired") != std::string::npos,
              "senses: vision reports not wired when the game has no backend");
        auto [r2, j2] = senseCall(srv, kAudioCall);
        text = toolText(j2, &isErr);
        check(r2 && isErr && text.find("not wired") != std::string::npos,
              "senses: audio reports not wired when the game has no backend");
      }
      // Rate limits: per-pilot buckets (vision 4/sec burst 8, audio 8/sec burst 16).
      {
        McpServer srv(makeSensesBackends(), McpAgent{"tester", 0});
        srv.setLogSink(nullptr);
        bool responded = false;
        roundTrip(srv, kInit, &responded);
        srv.grantPermissions(static_cast<uint32_t>(kPermPilotSenses));
        bool isErr = false;
        int visionOk = 0, visionLimited = 0;
        for (int i = 0; i < 10; ++i) {
          auto [r, j] = senseCall(srv, kVisionCall);
          toolText(j, &isErr);
          if (!isErr) ++visionOk; else ++visionLimited;
          (void)r;
        }
        check(visionOk == 8 && visionLimited == 2,
              "senses: vision burst 8 then rate-limited (per pilot)");
        int audioOk = 0, audioLimited = 0;
        for (int i = 0; i < 18; ++i) {
          auto [r, j] = senseCall(srv, kAudioCall);
          toolText(j, &isErr);
          if (!isErr) ++audioOk; else ++audioLimited;
          (void)r;
        }
        check(audioOk == 16 && audioLimited == 2,
              "senses: audio burst 16 then rate-limited (per pilot)");
      }
    }

  }

  int fails = check.fails();
  // Phase 2: HTTP transport checks (mcp_http_selftest.cpp). Same binary,
  // same `ctest -R engine_mcp` run.
  fails += runMcpHttpSelfTest();
  return fails;
}

}  // namespace litholattice::mcp
