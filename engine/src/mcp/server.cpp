// server.cpp: Phase 1 MCP server implementation. Clean-room; every line
// written for this file. See server.h for the locked layering, threading,
// and schema-subset decisions.
#include "litholattice/mcp/server.h"
#include "litholattice/mcp/mcp_senses.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>

// kPerm* bits (header-only C macros) and the engine/SDK version macros.
#include "litholattice/mods/mod_interface.h"
// Engine version macros + protocol version macro (header-only).
#include "litholattice/net/version.h"
// kProtocolVersion (the current wire protocol, F-L3).
#include "litholattice/net/protocol.h"
// IWorldQuery: pure interface + kMaxQueryCells/kMaxQueryResults (no link dep).
#include "litholattice/console/iworldquery.h"

namespace litholattice::mcp {
namespace {

// ---------------------------------------------------------------------------
// Small JSON builders.
// ---------------------------------------------------------------------------

JsonValue strVec(const std::vector<std::string>& v) {
  JsonValue::Array a;
  a.reserve(v.size());
  for (const auto& s : v) a.emplace_back(JsonValue::string(s));
  return JsonValue::array(std::move(a));
}

// ---------------------------------------------------------------------------
// Argument reader: strict, fail-fast validation of tool arguments.
// Violations throw RpcError(-32602) -> a JSON-RPC protocol error (the call
// itself was malformed). Anything else (denials, missing data, backend
// failures) becomes an isError tool result.
// ---------------------------------------------------------------------------

class ArgReader {
 public:
  explicit ArgReader(const JsonValue& args) : args_(args) {}

  std::string reqString(const char* name) {
    const JsonValue* v = req(name);
    if (!v->isString()) fail(name, "string");
    return v->asString();
  }
  std::string reqNonEmptyString(const char* name) {
    std::string s = reqString(name);
    if (s.empty()) fail(name, "non-empty string");
    return s;
  }
  int64_t reqInt(const char* name) {
    const JsonValue* v = req(name);
    if (!v->isInt()) fail(name, "integer");
    return v->asInt();
  }
  double reqNumber(const char* name) {
    const JsonValue* v = req(name);
    if (!v->isNumber()) fail(name, "number");
    return v->isInt() ? static_cast<double>(v->asInt()) : v->asDouble();
  }
  int64_t optInt(const char* name, int64_t def, int64_t lo, int64_t hi) {
    const JsonValue* v = args_.find(name);
    if (!v || v->isNull()) return def;
    if (!v->isInt() || v->asInt() < lo || v->asInt() > hi) fail(name, "integer in range");
    return v->asInt();
  }
  double optNumber(const char* name, double def, double lo, double hi) {
    const JsonValue* v = args_.find(name);
    if (!v || v->isNull()) return def;
    double d = v->isInt() ? static_cast<double>(v->asInt())
                          : (v->isDouble() ? v->asDouble() : NAN);
    if (std::isnan(d) || d < lo || d > hi) fail(name, "number in range");
    return d;
  }
  bool optBool(const char* name, bool def) {
    const JsonValue* v = args_.find(name);
    if (!v || v->isNull()) return def;
    if (!v->isBool()) fail(name, "boolean");
    return v->asBool();
  }

 private:
  const JsonValue* req(const char* name) {
    const JsonValue* v = args_.find(name);
    if (!v || v->isNull()) fail(name, "present");
    return v;
  }
  [[noreturn]] void fail(const char* name, const char* want) {
    throw RpcError(kRpcInvalidParams,
                   std::string("invalid argument '") + name + "': expected " + want);
  }
  const JsonValue& args_;
};

JsonValue buildInputSchema(const std::vector<ToolParam>& params) {
  JsonValue::Object props;
  JsonValue::Array required;
  for (const auto& p : params) {
    JsonValue::Object pd;
    pd.emplace_back("type", JsonValue::string(p.type));
    pd.emplace_back("description", JsonValue::string(p.description));
    if (p.hasMinimum) pd.emplace_back("minimum", JsonValue::number(p.minimum));
    if (p.hasMaximum) pd.emplace_back("maximum", JsonValue::number(p.maximum));
    props.emplace_back(p.name, JsonValue::object(std::move(pd)));
    if (p.required) required.emplace_back(JsonValue::string(p.name));
  }
  JsonValue::Object schema;
  schema.emplace_back("type", JsonValue::string("object"));
  schema.emplace_back("properties", JsonValue::object(std::move(props)));
  schema.emplace_back("required", JsonValue::array(std::move(required)));
  return JsonValue::object(std::move(schema));
}

JsonValue toolResultJson(const ToolResult& r) {
  JsonValue::Object text;
  text.emplace_back("type", JsonValue::string("text"));
  text.emplace_back("text", JsonValue::string(r.text));
  JsonValue::Object out;
  out.emplace_back("content",
                   JsonValue::array({JsonValue::object(std::move(text))}));
  out.emplace_back("isError", JsonValue::boolean(r.isError));
  return JsonValue::object(std::move(out));
}

std::vector<std::string> permBitNameList(uint32_t mask) {
  std::vector<std::string> names;
  for (uint32_t bit = 1; bit != 0; bit <<= 1) {
    if (mask & bit) names.emplace_back(permBitName(bit));
  }
  return names;
}

// Serializes one parsed-request dispatch; nullopt when no response is owed
// (notification). Lets handleMessage parse the body once and reuse the DOM
// instead of re-parsing inside handleRpcMessage.
std::optional<std::string> dispatchParsed(const RpcRequest& req, RpcRouter& router) {
  std::optional<JsonValue> resp = handleParsedRequest(req, router);
  if (!resp) return std::nullopt;
  return resp->serialize();
}

}  // namespace

// ---------------------------------------------------------------------------
// Permission bit names (mirror the canonical manifest names in
// mods/mod_permissions.h).
// ---------------------------------------------------------------------------

std::string permBitName(uint32_t bit) {
  switch (bit) {
    case kPermBlocks: return "blocks";
    case kPermWorldRead: return "world_read";
    case kPermWorldWrite: return "world_write";
    case kPermTick: return "tick";
    case kPermEvents: return "events";
    case kPermUI: return "ui";
    case kPermInput: return "input";
    case kPermAssets: return "assets";
    case kPermNet: return "net";
    case kPermConsole: return "console";
    case kPermPlayerControl: return "player_control";
    case kPermPilotSenses: return "pilot_senses";
    default: break;
  }
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%x", bit);
  return std::string("unknown(") + buf + ")";
}

std::string permMaskNames(uint32_t mask) {
  if (mask == 0) return "(none)";
  std::string out;
  for (const auto& n : permBitNameList(mask)) {
    if (!out.empty()) out += ", ";
    out += n;
  }
  return out;
}

std::string mcpEngineVersionString() {
  return std::to_string(LITHOLATTICE_ENGINE_VERSION_MAJOR) + "." +
         std::to_string(LITHOLATTICE_ENGINE_VERSION_MINOR) + "." +
         std::to_string(LITHOLATTICE_ENGINE_VERSION_PATCH);
}

// ---------------------------------------------------------------------------
// Phase 3: protocol-era classification (dual-era dispatch).
// ---------------------------------------------------------------------------

namespace {

/// data.supported payload shared by every -32022 we emit.
JsonValue unsupportedVersionData() {
  JsonValue::Array supported;
  supported.emplace_back(JsonValue::string(kMcpProtocolVersion));
  supported.emplace_back(JsonValue::string(kMcpProtocolVersionStateless));
  JsonValue::Object data;
  data.emplace_back("supported", JsonValue::array(std::move(supported)));
  return JsonValue::object(std::move(data));
}

}  // namespace

McpEra classifyRequestEra(const JsonValue& params) {
  if (!params.isObject()) return McpEra::kLegacy;  // absent params: legacy
  const JsonValue* meta = params.find("_meta");
  if (!meta || meta->isNull()) return McpEra::kLegacy;
  if (!meta->isObject()) {
    throw RpcError(kRpcInvalidParams, "_meta must be an object");
  }
  // Spec-namespaced key first, plain key as the lenient fallback.
  const JsonValue* v = meta->find(kMetaProtocolVersion);
  if (!v || v->isNull()) v = meta->find("protocolVersion");
  if (!v || v->isNull()) {
    throw RpcError(kRpcInvalidParams, "_meta: missing protocol version");
  }
  if (!v->isString()) {
    throw RpcError(kRpcInvalidParams, "_meta: protocol version must be a string");
  }
  const std::string& ver = v->asString();
  if (ver == kMcpProtocolVersionStateless) return McpEra::kStateless;
  if (ver == kMcpProtocolVersion) return McpEra::kLegacy;
  throw RpcError(kRpcUnsupportedProtocolVersion,
                 "unsupported protocol version '" + ver + "'",
                 unsupportedVersionData());
}

// ---------------------------------------------------------------------------
// McpServer.
// ---------------------------------------------------------------------------

McpServer::McpServer(McpBackends backends, McpAgent agent)
    : backends_(std::move(backends)), agent_(std::move(agent)) {
  logSink_ = [](std::string_view line) {
    std::fprintf(stderr, "mcp: %.*s\n", static_cast<int>(line.size()), line.data());
  };
  router_.registerHandler("initialize",
                          [this](const JsonValue& p) { return onInitialize(p); });
  router_.registerHandler("server/discover",
                          [this](const JsonValue& p) { return onDiscover(p); });
  router_.registerHandler("notifications/initialized",
                          [](const JsonValue&) { return JsonValue::null(); });
  router_.registerHandler("tools/list",
                          [this](const JsonValue& p) { return onToolsList(p); });
  router_.registerHandler("tools/call",
                          [this](const JsonValue& p) { return onToolsCall(p); });
  router_.registerHandler("resources/list",
                          [this](const JsonValue& p) { return onResourcesList(p); });
  router_.registerHandler("resources/read",
                          [this](const JsonValue& p) { return onResourcesRead(p); });
  router_.registerHandler("prompts/list",
                          [this](const JsonValue& p) { return onPromptsList(p); });
  router_.registerHandler("prompts/get",
                          [this](const JsonValue& p) { return onPromptsGet(p); });
  registerPhase1Tools();
  registerPhase1Resources();
  registerPhase3Prompts();
  registerPhase4WriteTools();
  registerPlayerControlTools();
  registerPilotSensesTools();
}

void McpServer::setLogSink(LogSink sink) {
  logSink_ = sink ? std::move(sink) : LogSink();
}

void McpServer::setRateLimitClock(McpTokenBucket::ClockFn fn) {
  toolCalls_.setClock(fn);
  blockWrites_.setClock(fn);
}

void McpServer::log(std::string_view line) {
  if (logSink_) logSink_(line);
}

void McpServer::grantPermissions(uint32_t bits) {
  {
    std::lock_guard<std::mutex> lock(agentMutex_);
    agent_.permissionMask |= bits;
  }
  log("grant: +" + permMaskNames(bits));
}

void McpServer::revokePermissions(uint32_t bits) {
  {
    std::lock_guard<std::mutex> lock(agentMutex_);
    agent_.permissionMask &= ~bits;
  }
  log("revoke: -" + permMaskNames(bits));
}

uint32_t McpServer::permissionMask() const {
  std::lock_guard<std::mutex> lock(agentMutex_);
  return agent_.permissionMask;
}

void McpServer::setAgentName(std::string name) {
  std::lock_guard<std::mutex> lock(agentMutex_);
  agent_.name = std::move(name);
}

McpAgent McpServer::agent() const {
  std::lock_guard<std::mutex> lock(agentMutex_);
  return agent_;
}

std::optional<std::string> McpServer::handleMessage(std::string_view text) {
  RpcParseOutcome outcome = parseRpcRequest(text);
  if (!outcome.ok) {
    return buildErrorResponse(outcome.errorId, outcome.errorCode,
                              outcome.errorMessage)
        .serialize();
  }
  const RpcRequest& req = outcome.request;

  // Phase 3: stateless-era classification from params._meta. Malformed _meta
  // or an unsupported version is a protocol error (never a notification
  // response — notifications get no response, ever).
  McpEra era = McpEra::kLegacy;
  try {
    era = classifyRequestEra(req.params);
  } catch (const RpcError& e) {
    if (req.isNotification) return std::nullopt;
    return buildErrorResponse(req.id, e.code(), e.message(), e.data())
        .serialize();
  }

  // Stateless era: skip the initialized gate entirely. `initialize` stays
  // the handshake-era method no matter what _meta says — a stateless-era
  // client has no reason to send it, and a confused one gets the legacy
  // answer (protocolVersion 2025-11-25) rather than a rejection.
  if (era == McpEra::kStateless && req.method != "initialize") {
    return dispatchStateless(req);
  }

  if (!initialized_) {
    if (req.method == "initialize") {
      if (req.isNotification) return std::nullopt;  // must be a request; ignore
      return dispatchParsed(req, router_);  // handler flips initialized_
    }
    if (req.method == "notifications/initialized") {
      if (!req.isNotification) {
        return buildErrorResponse(req.id, kRpcInvalidRequest,
                                  "notifications/initialized must be a notification")
            .serialize();
      }
      return std::nullopt;  // accepted, no-op
    }
    if (req.method == "server/discover") {
      // Discovery is the era-agnostic entry point (2026-07-28 MUST): it
      // answers before any handshake so a client can learn the eras.
      if (req.isNotification) return std::nullopt;
      return dispatchParsed(req, router_);
    }
    if (req.isNotification) return std::nullopt;  // no response, ever
    return buildErrorResponse(req.id, kRpcInvalidRequest,
                              "server not initialized: send 'initialize' first")
        .serialize();
  }
  return dispatchParsed(req, router_);
}

std::optional<std::string> McpServer::dispatchStateless(const RpcRequest& req) {
  // The envelope merges into the response VALUE before serializing: no
  // serialize-then-re-parse round trip on the stateless hot path.
  std::optional<JsonValue> resp = handleParsedRequest(req, router_);
  if (!resp) return std::nullopt;  // notification: no response, ever
  // Merge the stateless result envelope into the result object. Error
  // responses keep their JSON-RPC shape (-32022 already names the server's
  // supported versions in data.supported).
  JsonValue& doc = *resp;
  const JsonValue* rc = doc.find("result");
  if (!rc || !rc->isObject()) return doc.serialize();  // defensive; unreachable
  // Present (checked above), so getOrInsert cannot insert: it returns the
  // existing entry, mutably.
  JsonValue& result = doc.getOrInsert("result");
  applyStatelessEnvelope(result, req.method);
  return doc.serialize();
}

void McpServer::applyStatelessEnvelope(JsonValue& result,
                                       const std::string& method) {
  // SEP-2322: every 2026-07-28 result carries resultType. This server never
  // needs client input (no MRTR/elicitation in v1), so it always completes.
  if (!result.find("resultType")) {
    result.getOrInsert("resultType") = JsonValue::string("complete");
  }
  // Server identity: with the handshake gone, result _meta is the only
  // place a stateless client learns who answered.
  JsonValue& meta = result.getOrInsert("_meta");
  if (!meta.isObject()) meta = JsonValue::object({});
  JsonValue::Object serverInfo;
  serverInfo.emplace_back("name", JsonValue::string(kMcpServerName));
  serverInfo.emplace_back("version", JsonValue::string(mcpEngineVersionString()));
  JsonValue::Object& mo = meta.asObject();
  bool hasVer = false, hasInfo = false;
  for (const auto& [k, v] : mo) {
    if (k == kMetaProtocolVersion) hasVer = true;
    if (k == kMetaServerInfo) hasInfo = true;
  }
  if (!hasVer) mo.emplace_back(kMetaProtocolVersion, JsonValue::string(kMcpProtocolVersionStateless));
  if (!hasInfo) mo.emplace_back(kMetaServerInfo, JsonValue::object(std::move(serverInfo)));
  // SEP-2549 cache hints: lists and discover are static per boot (public,
  // 1h); resources/read is live, permission-gated data (private, no cache).
  const bool isList = method == "tools/list" || method == "resources/list" ||
                      method == "prompts/list" || method == "server/discover";
  if (isList) {
    if (!result.find("ttlMs")) {
      result.getOrInsert("ttlMs") = JsonValue::integer(kMcpListTtlMs);
    }
    if (!result.find("cacheScope")) {
      result.getOrInsert("cacheScope") = JsonValue::string(kMcpCacheScopePublic);
    }
  } else if (method == "resources/read") {
    if (!result.find("ttlMs")) {
      result.getOrInsert("ttlMs") = JsonValue::integer(0);
    }
    if (!result.find("cacheScope")) {
      result.getOrInsert("cacheScope") = JsonValue::string(kMcpCacheScopePrivate);
    }
  }
}

void McpServer::runStdio(StdioTransport& transport) {
  std::string line;
  while (transport.readLine(line)) {
    if (line.size() > kMaxStdioLineBytes) {
      log("stdio: dropped overlong line (" + std::to_string(line.size()) + " bytes)");
      continue;
    }
    if (line.empty()) continue;  // tolerate blank lines
    std::optional<std::string> resp;
    try {
      resp = handleMessage(line);
    } catch (const std::exception& e) {
      // A backend threw something the tool path did not convert: log it and
      // keep the connection alive. (Non-std exceptions propagate: fail-fast.)
      log(std::string("stdio: handler exception: ") + e.what());
      continue;
    }
    if (resp) transport.writeLine(*resp);
  }
  log("stdio: EOF, connection closed");
}

void McpServer::runStdio() {
  FdStdioTransport transport;
  runStdio(transport);
}

// ---------------------------------------------------------------------------
// Lifecycle handlers.
// ---------------------------------------------------------------------------

JsonValue McpServer::onInitialize(const JsonValue& params) {
  const JsonValue* v = params.find("protocolVersion");
  const std::string want = (v && v->isString()) ? v->asString() : "";
  if (want != kMcpProtocolVersion) {
    throw RpcError(kRpcUnsupportedProtocolVersion,
                   "unsupported protocol version '" + want + "'",
                   unsupportedVersionData());
  }
  initialized_ = true;
  log("initialize: protocol " + want);
  JsonValue::Object serverInfo;
  serverInfo.emplace_back("name", JsonValue::string(kMcpServerName));
  serverInfo.emplace_back("version", JsonValue::string(mcpEngineVersionString()));
  JsonValue::Object capabilities;
  capabilities.emplace_back("tools", JsonValue::object({}));
  capabilities.emplace_back("resources", JsonValue::object({}));
  capabilities.emplace_back("prompts", JsonValue::object({}));
  JsonValue::Object result;
  result.emplace_back("protocolVersion", JsonValue::string(kMcpProtocolVersion));
  result.emplace_back("capabilities", JsonValue::object(std::move(capabilities)));
  result.emplace_back("serverInfo", JsonValue::object(std::move(serverInfo)));
  return JsonValue::object(std::move(result));
}

/// Short instructions string for server/discover: how an LLM client should
/// use this server. Tight (small local models read it verbatim).
inline constexpr const char* kMcpDiscoverInstructions =
    "LithoKraft MCP server (LithoLattice engine). Prefer prompts/get "
    "'agent_onboarding' for a full briefing. All v1 tools are read-only; "
    "permission denials come back as isError results, never protocol errors. "
    "Handshake-era clients: send initialize with protocolVersion "
    "\"2025-11-25\". Stateless clients: put \"2026-07-28\" in "
    "params._meta on every request; no handshake, no session.";

JsonValue McpServer::onDiscover(const JsonValue&) {
  JsonValue::Array versions;
  versions.emplace_back(JsonValue::string(kMcpProtocolVersion));
  versions.emplace_back(JsonValue::string(kMcpProtocolVersionStateless));
  JsonValue::Object serverInfo;
  serverInfo.emplace_back("name", JsonValue::string(kMcpServerName));
  serverInfo.emplace_back("version", JsonValue::string(mcpEngineVersionString()));
  JsonValue::Object capabilities;
  capabilities.emplace_back("tools", JsonValue::object({}));
  capabilities.emplace_back("resources", JsonValue::object({}));
  capabilities.emplace_back("prompts", JsonValue::object({}));
  JsonValue::Object result;
  result.emplace_back("supportedVersions",
                      JsonValue::array(std::move(versions)));
  result.emplace_back("capabilities", JsonValue::object(std::move(capabilities)));
  result.emplace_back("serverInfo", JsonValue::object(std::move(serverInfo)));
  result.emplace_back("instructions", JsonValue::string(kMcpDiscoverInstructions));
  // server/discover is inherently modern: its result shape always carries
  // the SEP-2549 cache hints (the stateless envelope adds resultType/_meta
  // on top when the request itself was stateless).
  result.emplace_back("ttlMs", JsonValue::integer(kMcpListTtlMs));
  result.emplace_back("cacheScope", JsonValue::string(kMcpCacheScopePublic));
  return JsonValue::object(std::move(result));
}

JsonValue McpServer::onPromptsList(const JsonValue&) {
  JsonValue::Array list;
  for (const auto& p : prompts_) {
    JsonValue::Array args;
    for (const auto& a : p.arguments) {
      JsonValue::Object ao;
      ao.emplace_back("name", JsonValue::string(a.name));
      ao.emplace_back("description", JsonValue::string(a.description));
      ao.emplace_back("required", JsonValue::boolean(a.required));
      args.emplace_back(JsonValue::object(std::move(ao)));
    }
    JsonValue::Object entry;
    entry.emplace_back("name", JsonValue::string(p.name));
    entry.emplace_back("description", JsonValue::string(p.description));
    entry.emplace_back("arguments", JsonValue::array(std::move(args)));
    list.emplace_back(JsonValue::object(std::move(entry)));
  }
  JsonValue::Object result;
  result.emplace_back("prompts", JsonValue::array(std::move(list)));
  return JsonValue::object(std::move(result));
}

JsonValue McpServer::onPromptsGet(const JsonValue& params) {
  if (!params.isObject()) {
    throw RpcError(kRpcInvalidParams, "prompts/get: params must be an object");
  }
  const JsonValue* nv = params.find("name");
  if (!nv || !nv->isString() || nv->asString().empty()) {
    throw RpcError(kRpcInvalidParams,
                   "prompts/get: 'name' must be a non-empty string");
  }
  const std::string name = nv->asString();
  const PromptDescriptor* prompt = nullptr;
  for (const auto& p : prompts_) {
    if (p.name == name) {
      prompt = &p;
      break;
    }
  }
  // The prompt result type has no isError channel, so an unknown name is a
  // protocol error (-32602), not a model-actionable result. (Contrast with
  // tools/call, where the locked Phase 1 decision keeps unknown tools as
  // isError results.)
  if (!prompt) {
    throw RpcError(kRpcInvalidParams, "unknown prompt: '" + name + "'");
  }
  McpAgent agentCopy;
  uint32_t missing;
  {
    std::lock_guard<std::mutex> lock(agentMutex_);
    missing = prompt->requiredPerms & ~agent_.permissionMask;
    agentCopy = agent_;
  }
  if (missing != 0) {
    throw RpcError(kMcpPermissionDenied,
                   "permission denied: agent '" + agentCopy.name +
                       "' is missing " + permMaskNames(missing) +
                       " for prompt '" + name + "'");
  }
  // arguments: object of strings, validated strictly against the descriptor.
  const JsonValue* av = params.find("arguments");
  if (av && !av->isObject() && !av->isNull()) {
    throw RpcError(kRpcInvalidParams, "prompts/get: 'arguments' must be an object");
  }
  std::map<std::string, std::string> args;
  if (av && av->isObject()) {
    for (const auto& [k, v] : av->asObject()) {
      if (!v.isString()) {
        throw RpcError(kRpcInvalidParams,
                       "prompts/get: argument '" + k + "' must be a string");
      }
      args[k] = v.asString();
    }
  }
  for (const auto& a : prompt->arguments) {
    if (a.required && args.find(a.name) == args.end()) {
      throw RpcError(kRpcInvalidParams, "prompts/get: missing required argument '" +
                                            a.name + "' for prompt '" + name + "'");
    }
  }
  for (const auto& [k, v] : args) {
    (void)v;
    bool known = false;
    for (const auto& a : prompt->arguments) {
      if (a.name == k) {
        known = true;
        break;
      }
    }
    if (!known) {
      throw RpcError(kRpcInvalidParams, "prompts/get: prompt '" + name +
                                            "' takes no argument '" + k + "'");
    }
  }
  const std::string text = prompt->render(args);
  JsonValue::Object content;
  content.emplace_back("type", JsonValue::string("text"));
  content.emplace_back("text", JsonValue::string(text));
  JsonValue::Object message;
  message.emplace_back("role", JsonValue::string("user"));
  message.emplace_back(
      "content", JsonValue::array({JsonValue::object(std::move(content))}));
  JsonValue::Object result;
  result.emplace_back("description", JsonValue::string(prompt->description));
  result.emplace_back("messages",
                      JsonValue::array({JsonValue::object(std::move(message))}));
  return JsonValue::object(std::move(result));
}

JsonValue McpServer::onToolsList(const JsonValue&) {
  JsonValue::Array list;
  for (const auto& t : tools_) {
    JsonValue::Object annotations;
    annotations.emplace_back("readOnlyHint", JsonValue::boolean(t.annotations.readOnlyHint));
    annotations.emplace_back("destructiveHint", JsonValue::boolean(t.annotations.destructiveHint));
    annotations.emplace_back("idempotentHint", JsonValue::boolean(t.annotations.idempotentHint));
    annotations.emplace_back("openWorldHint", JsonValue::boolean(t.annotations.openWorldHint));
    JsonValue::Object entry;
    entry.emplace_back("name", JsonValue::string(t.name));
    entry.emplace_back("description", JsonValue::string(t.description));
    entry.emplace_back("inputSchema", buildInputSchema(t.params));
    entry.emplace_back("annotations", JsonValue::object(std::move(annotations)));
    list.emplace_back(JsonValue::object(std::move(entry)));
  }
  JsonValue::Object result;
  result.emplace_back("tools", JsonValue::array(std::move(list)));
  return JsonValue::object(std::move(result));
}

const ToolDescriptor* McpServer::findTool(const std::string& name) {
  // tools_ only grows (all registration happens in the constructor's
  // register*() family), so a size check is enough to detect a stale index.
  if (toolIndexBuiltFor_ != tools_.size()) {
    toolIndex_.clear();
    for (size_t i = 0; i < tools_.size(); ++i) toolIndex_.emplace(tools_[i].name, i);
    toolIndexBuiltFor_ = tools_.size();
  }
  const auto it = toolIndex_.find(name);
  return it == toolIndex_.end() ? nullptr : &tools_[it->second];
}

JsonValue McpServer::onToolsCall(const JsonValue& params) {
  if (!params.isNull() && !params.isObject()) {
    throw RpcError(kRpcInvalidParams, "tools/call: params must be an object");
  }
  const JsonValue emptyArgs = JsonValue::object({});
  ArgReader args(params.isObject() ? params : emptyArgs);
  const std::string name = args.reqNonEmptyString("name");
  const JsonValue* a = params.find("arguments");
  if (a && !a->isObject() && !a->isNull()) {
    throw RpcError(kRpcInvalidParams, "tools/call: 'arguments' must be an object");
  }
  const JsonValue callArgs =
      (a && a->isObject()) ? *a : JsonValue::object({});

  // Phase 4: per-connection tool-call rate cap. Consumed before the tool
  // lookup so even unknown-tool probes cost budget (a scanner burns its
  // allowance like everyone else). Exceeded -> isError naming the limit,
  // never a protocol error.
  if (!toolCalls_.take(1)) {
    std::string agentName;
    {
      std::lock_guard<std::mutex> lock(agentMutex_);
      agentName = agent_.name;
    }
    log("tool " + name + " agent=" + agentName + " rate_limited (" +
        std::to_string(kMcpToolCallLimitPerSec) + " calls/sec, burst " +
        std::to_string(kMcpToolCallBurst) + ")");
    return toolResultJson(ToolResult::error(
        "rate limit exceeded: at most " + std::to_string(kMcpToolCallLimitPerSec) +
        " tool calls/sec (burst " + std::to_string(kMcpToolCallBurst) +
        ") per connection"));
  }

  const ToolDescriptor* tool = findTool(name);
  ToolResult r = ToolResult::error("unknown tool: '" + name + "'");
  std::string outcome = "unknown";
  if (tool) {
    uint32_t missing;
    {
      std::lock_guard<std::mutex> lock(agentMutex_);
      missing = tool->requiredPerms & ~agent_.permissionMask;
    }
    if (missing != 0) {
      std::string agentName;
      {
        std::lock_guard<std::mutex> lock(agentMutex_);
        agentName = agent_.name;
      }
      r = ToolResult::error("permission denied: agent '" + agentName +
                            "' is missing " + permMaskNames(missing) +
                            " for tool '" + name + "'");
      outcome = "denied";
    } else {
      try {
        McpAgent agentCopy;
        {
          std::lock_guard<std::mutex> lock(agentMutex_);
          agentCopy = agent_;
        }
        r = tool->handler(callArgs, agentCopy);
        outcome = r.isError ? "error" : "ok";
      } catch (const RpcError&) {
        throw;  // malformed arguments: protocol error, not a tool failure
      } catch (const std::exception& e) {
        // Backend blew up: model-actionable failure, never a protocol error.
        r = ToolResult::error(std::string("tool '") + name + "' failed: " + e.what());
        outcome = "threw";
      }
    }
  }
  {
    std::lock_guard<std::mutex> lock(agentMutex_);
    log("tool " + name + " agent=" + agent_.name + " " + outcome);
  }
  return toolResultJson(r);
}

JsonValue McpServer::onResourcesList(const JsonValue&) {
  JsonValue::Array list;
  for (const auto& r : resources_) {
    JsonValue::Object entry;
    entry.emplace_back("uri", JsonValue::string(r.uri));
    entry.emplace_back("name", JsonValue::string(r.name));
    entry.emplace_back("description", JsonValue::string(r.description));
    entry.emplace_back("mimeType", JsonValue::string(r.mimeType));
    list.emplace_back(JsonValue::object(std::move(entry)));
  }
  JsonValue::Object result;
  result.emplace_back("resources", JsonValue::array(std::move(list)));
  return JsonValue::object(std::move(result));
}

JsonValue McpServer::onResourcesRead(const JsonValue& params) {
  if (!params.isObject()) {
    throw RpcError(kRpcInvalidParams, "resources/read: params must be an object");
  }
  ArgReader args(params);
  const std::string uri = args.reqNonEmptyString("uri");
  const ResourceDescriptor* res = nullptr;
  for (const auto& r : resources_) {
    if (r.uri == uri) {
      res = &r;
      break;
    }
  }
  if (!res) {
    throw RpcError(kRpcInvalidParams, "unknown resource uri: '" + uri + "'",
                   JsonValue::string(uri));
  }
  uint32_t missing;
  McpAgent agentCopy;
  {
    std::lock_guard<std::mutex> lock(agentMutex_);
    missing = res->requiredPerms & ~agent_.permissionMask;
    agentCopy = agent_;
  }
  if (missing != 0) {
    throw RpcError(kMcpPermissionDenied,
                   "permission denied: agent '" + agentCopy.name + "' is missing " +
                       permMaskNames(missing) + " for resource '" + uri + "'");
  }
  std::string text = res->read(agentCopy);  // throws RpcError on failure
  JsonValue::Object content;
  content.emplace_back("uri", JsonValue::string(uri));
  content.emplace_back("mimeType", JsonValue::string(res->mimeType));
  content.emplace_back("text", JsonValue::string(text));
  JsonValue::Object result;
  result.emplace_back("contents",
                      JsonValue::array({JsonValue::object(std::move(content))}));
  return JsonValue::object(std::move(result));
}

// ---------------------------------------------------------------------------
// Phase 1 tools.
// ---------------------------------------------------------------------------

namespace {

ToolParam p(std::string name, std::string type, std::string desc, bool required = false) {
  ToolParam tp;
  tp.name = std::move(name);
  tp.type = std::move(type);
  tp.description = std::move(desc);
  tp.required = required;
  return tp;
}

const char* kNotWired = "not available: the game has not wired this backend";

}  // namespace

void McpServer::registerPhase1Tools() {
  McpBackends& b = backends_;

  // -- sim_status ------------------------------------------------------------
  tools_.push_back(ToolDescriptor{
      "sim_status",
      "Simulation clock: world tick, measured TPS, target TPS, paused flag.",
      {},
      kPermWorldRead,
      [&b](const JsonValue&, const McpAgent&) {
        if (!b.sim) return ToolResult::error(kNotWired);
        JsonValue::Object o;
        o.emplace_back("tick", JsonValue::integer(static_cast<int64_t>(b.sim->worldTick())));
        o.emplace_back("tps", JsonValue::number(b.sim->currentTps()));
        o.emplace_back("targetTps", JsonValue::integer(b.sim->targetTps()));
        o.emplace_back("paused", JsonValue::boolean(b.sim->paused()));
        return ToolResult::ok(JsonValue::object(std::move(o)).serialize());
      },
  });

  // -- mod_list ---------------------------------------------------------------
  tools_.push_back(ToolDescriptor{
      "mod_list",
      "Installed mods: name, version, granted permission mask and names.",
      {},
      0,
      [&b](const JsonValue&, const McpAgent&) {
        if (!b.mods) return ToolResult::error(kNotWired);
        JsonValue::Array arr;
        for (const auto& m : b.mods->mods()) {
          JsonValue::Object o;
          o.emplace_back("name", JsonValue::string(m.name));
          o.emplace_back("version", JsonValue::string(m.version));
          o.emplace_back("permissions",
                         JsonValue::integer(static_cast<int64_t>(m.permissions)));
          o.emplace_back("permissionNames", strVec(permBitNameList(m.permissions)));
          o.emplace_back("extensions", strVec(m.extensions));
          arr.emplace_back(JsonValue::object(std::move(o)));
        }
        return ToolResult::ok(JsonValue::array(std::move(arr)).serialize());
      },
  });

  // -- player_get_state --------------------------------------------------------
  tools_.push_back(ToolDescriptor{
      "player_get_state",
      "Local player: position, velocity, look direction, dimension, health, stats, hotbar, selected slot.",
      {},
      kPermWorldRead,
      [&b](const JsonValue&, const McpAgent&) {
        if (!b.player) return ToolResult::error(kNotWired);
        IPlayerView::Snapshot s = b.player->localPlayer();
        JsonValue::Object o;
        o.emplace_back("x", JsonValue::number(s.x));
        o.emplace_back("y", JsonValue::number(s.y));
        o.emplace_back("z", JsonValue::number(s.z));
        o.emplace_back("vx", JsonValue::number(s.vx));
        o.emplace_back("vy", JsonValue::number(s.vy));
        o.emplace_back("vz", JsonValue::number(s.vz));
        o.emplace_back("yaw", JsonValue::number(s.yaw));
        o.emplace_back("pitch", JsonValue::number(s.pitch));
        o.emplace_back("dimension", JsonValue::integer(s.dimension));
        o.emplace_back("health", JsonValue::integer(s.health));
        JsonValue::Array stats;
        for (const auto& [name, value] : s.stats) {
          JsonValue::Object e;
          e.emplace_back("name", JsonValue::string(name));
          e.emplace_back("value", JsonValue::number(value));
          stats.emplace_back(JsonValue::object(std::move(e)));
        }
        o.emplace_back("stats", JsonValue::array(std::move(stats)));
        JsonValue::Array hotbar;
        for (uint16_t id : s.hotbar) hotbar.emplace_back(JsonValue::integer(id));
        o.emplace_back("hotbar", JsonValue::array(std::move(hotbar)));
        o.emplace_back("selectedSlot", JsonValue::integer(s.selectedSlot));
        return ToolResult::ok(JsonValue::object(std::move(o)).serialize());
      },
  });

  // -- entity_list --------------------------------------------------------------
  {
    std::vector<ToolParam> params = {
        p("cx", "number", "Center x.", true),
        p("cy", "number", "Center y.", true),
        p("cz", "number", "Center z.", true),
    };
    ToolParam radius = p("radius", "number",
                         "Search radius in blocks. Required, 1..512 (fail-closed).",
                         true);
    radius.hasMinimum = true;
    radius.minimum = 1;
    radius.hasMaximum = true;
    radius.maximum = kMaxEntityRadius;
    params.push_back(std::move(radius));
    tools_.push_back(ToolDescriptor{
        "entity_list",
        "Entities within radius of a point, ascending id. Radius mandatory and capped.",
        std::move(params),
        kPermWorldRead,
        [&b](const JsonValue& a, const McpAgent&) {
          if (!b.entities) return ToolResult::error(kNotWired);
          ArgReader r(a);
          const double cx = r.reqNumber("cx"), cy = r.reqNumber("cy"),
                       cz = r.reqNumber("cz");
          const double radius = r.reqNumber("radius");
          if (!(radius >= 1.0 && radius <= kMaxEntityRadius)) {
            throw RpcError(kRpcInvalidParams,
                           "entity_list: 'radius' must be in [1, 512]");
          }
          JsonValue::Array arr;
          for (const auto& e : b.entities->within(cx, cy, cz, radius)) {
            JsonValue::Object o;
            o.emplace_back("id", JsonValue::integer(static_cast<int64_t>(e.id)));
            o.emplace_back("type", JsonValue::integer(e.type));
            o.emplace_back("x", JsonValue::number(e.x));
            o.emplace_back("y", JsonValue::number(e.y));
            o.emplace_back("z", JsonValue::number(e.z));
            arr.emplace_back(JsonValue::object(std::move(o)));
          }
          return ToolResult::ok(JsonValue::array(std::move(arr)).serialize());
        },
    });
  }

  // -- entity_get -----------------------------------------------------------------
  tools_.push_back(ToolDescriptor{
      "entity_get",
      "One entity by id. Unknown id is an isError result, not a protocol error.",
      {p("id", "integer", "Entity id.", true)},
      kPermWorldRead,
      [&b](const JsonValue& a, const McpAgent&) {
        if (!b.entities) return ToolResult::error(kNotWired);
        ArgReader r(a);
        const int64_t id = r.reqInt("id");
        if (id < 0) throw RpcError(kRpcInvalidParams, "entity_get: 'id' must be >= 0");
        IEntityView::Ent e;
        if (!b.entities->get(static_cast<uint64_t>(id), e)) {
          return ToolResult::error("no entity with id " + std::to_string(id));
        }
        JsonValue::Object o;
        o.emplace_back("id", JsonValue::integer(static_cast<int64_t>(e.id)));
        o.emplace_back("type", JsonValue::integer(e.type));
        o.emplace_back("x", JsonValue::number(e.x));
        o.emplace_back("y", JsonValue::number(e.y));
        o.emplace_back("z", JsonValue::number(e.z));
        return ToolResult::ok(JsonValue::object(std::move(o)).serialize());
      },
  });

  // -- world_get_block ---------------------------------------------------------------
  tools_.push_back(ToolDescriptor{
      "world_get_block",
      "Block id at one cell. Unloaded cells read as air (0).",
      {p("x", "integer", "Cell x.", true), p("y", "integer", "Cell y.", true),
       p("z", "integer", "Cell z.", true)},
      kPermWorldRead,
      [&b](const JsonValue& a, const McpAgent&) {
        if (!b.world) return ToolResult::error(kNotWired);
        ArgReader r(a);
        const int64_t x = r.reqInt("x"), y = r.reqInt("y"), z = r.reqInt("z");
        // int64 -> int is safe: JSON ints are int64 and cell coords fit int32
        // in every dimension; out-of-int32 is rejected rather than truncated.
        for (int64_t v : {x, y, z}) {
          if (v < INT32_MIN || v > INT32_MAX) {
            throw RpcError(kRpcInvalidParams, "world_get_block: coordinate out of range");
          }
        }
        const uint16_t id =
            b.world->getBlock(static_cast<int>(x), static_cast<int>(y), static_cast<int>(z));
        JsonValue::Object o;
        o.emplace_back("x", JsonValue::integer(x));
        o.emplace_back("y", JsonValue::integer(y));
        o.emplace_back("z", JsonValue::integer(z));
        o.emplace_back("blockId", JsonValue::integer(id));
        return ToolResult::ok(JsonValue::object(std::move(o)).serialize());
      },
  });

  // -- world_query_blocks -----------------------------------------------------------------
  tools_.push_back(ToolDescriptor{
      "world_query_blocks",
      "Scan an inclusive box for a block id. Bounds mandatory, cells capped, "
      "deterministic order (x, then y, then z).",
      {p("blockId", "integer", "Block id to find.", true),
       p("x1", "integer", "Box corner x.", true), p("y1", "integer", "Box corner y.", true),
       p("z1", "integer", "Box corner z.", true), p("x2", "integer", "Box corner x.", true),
       p("y2", "integer", "Box corner y.", true), p("z2", "integer", "Box corner z.", true)},
      kPermWorldRead,
      [&b](const JsonValue& a, const McpAgent&) {
        if (!b.worldQuery) return ToolResult::error(kNotWired);
        ArgReader r(a);
        const int64_t blockId = r.reqInt("blockId");
        const int64_t x1 = r.reqInt("x1"), y1 = r.reqInt("y1"), z1 = r.reqInt("z1");
        const int64_t x2 = r.reqInt("x2"), y2 = r.reqInt("y2"), z2 = r.reqInt("z2");
        if (blockId < 0 || blockId > UINT32_MAX) {
          throw RpcError(kRpcInvalidParams, "world_query_blocks: 'blockId' out of range");
        }
        for (int64_t v : {x1, y1, z1, x2, y2, z2}) {
          if (v < INT32_MIN || v > INT32_MAX) {
            throw RpcError(kRpcInvalidParams, "world_query_blocks: coordinate out of range");
          }
        }
        // Fail-closed on unbounded scans: mirror console::kMaxQueryCells here
        // so the cap holds even if a backend forgot it.
        const auto span = [](int64_t lo, int64_t hi) -> uint64_t {
          return static_cast<uint64_t>(hi >= lo ? hi - lo + 1 : lo - hi + 1);
        };
        const uint64_t cells = span(x1, x2) * span(y1, y2) * span(z1, z2);
        if (cells > console::kMaxQueryCells) {
          throw RpcError(kRpcInvalidParams,
                         "world_query_blocks: region too large (" +
                             std::to_string(cells) + " cells > " +
                             std::to_string(console::kMaxQueryCells) + ")");
        }
        std::string err;
        console::WorldQueryResult res = b.worldQuery->queryBlocks(
            static_cast<uint32_t>(blockId), static_cast<int>(x1), static_cast<int>(y1),
            static_cast<int>(z1), static_cast<int>(x2), static_cast<int>(y2),
            static_cast<int>(z2), &err);
        if (!err.empty()) return ToolResult::error("world_query_blocks: " + err);
        JsonValue::Array hits;
        for (const auto& h : res.blocks) {
          JsonValue::Object o;
          o.emplace_back("x", JsonValue::integer(h.x));
          o.emplace_back("y", JsonValue::integer(h.y));
          o.emplace_back("z", JsonValue::integer(h.z));
          o.emplace_back("blockId", JsonValue::integer(h.blockId));
          hits.emplace_back(JsonValue::object(std::move(o)));
        }
        JsonValue::Object o;
        o.emplace_back("cellsScanned", JsonValue::integer(static_cast<int64_t>(res.cellsScanned)));
        o.emplace_back("totalMatches", JsonValue::integer(static_cast<int64_t>(res.totalMatches)));
        o.emplace_back("truncated", JsonValue::boolean(res.truncated));
        o.emplace_back("blocks", JsonValue::array(std::move(hits)));
        return ToolResult::ok(JsonValue::object(std::move(o)).serialize());
      },
  });

  // -- fragment_search -----------------------------------------------------------------
  {
    ToolParam maxResults =
        p("maxResults", "integer", "Max hits (1..64).", false);
    maxResults.hasMinimum = true;
    maxResults.minimum = 1;
    maxResults.hasMaximum = true;
    maxResults.maximum = 64;
    tools_.push_back(ToolDescriptor{
        "fragment_search",
        "Full-text search over the player's Fragment journal. Returns doc ids, "
        "scores, and page text.",
        {p("query", "string", "Search text.", true), std::move(maxResults)},
        kPermWorldRead,
        [&b](const JsonValue& a, const McpAgent&) {
          if (!b.corpus) return ToolResult::error(kNotWired);
          ArgReader r(a);
          const std::string q = r.reqNonEmptyString("query");
          const int64_t maxResults = r.optInt("maxResults", 10, 1, 64);
          JsonValue::Array arr;
          for (const auto& h :
               b.corpus->search(q, static_cast<size_t>(maxResults))) {
            JsonValue::Object o;
            o.emplace_back("doc", JsonValue::integer(static_cast<int64_t>(h.doc)));
            o.emplace_back("score", JsonValue::number(h.score));
            o.emplace_back("text", JsonValue::string(h.text));
            arr.emplace_back(JsonValue::object(std::move(o)));
          }
          return ToolResult::ok(JsonValue::array(std::move(arr)).serialize());
        },
    });
  }

  // -- defrag_status -----------------------------------------------------------------
  tools_.push_back(ToolDescriptor{
      "defrag_status",
      "Forever-world defrag service: running, phase, progress, stores. Status only.",
      {},
      0,
      [&b](const JsonValue&, const McpAgent&) {
        if (!b.defrag) return ToolResult::error(kNotWired);
        JsonValue::Object o;
        o.emplace_back("running", JsonValue::boolean(b.defrag->running()));
        o.emplace_back("phase", JsonValue::string(b.defrag->phaseName()));
        o.emplace_back("progress", JsonValue::number(b.defrag->progress()));
        o.emplace_back("statusLine", JsonValue::string(b.defrag->statusLine()));
        o.emplace_back("stores", strVec(b.defrag->storeNames()));
        return ToolResult::ok(JsonValue::object(std::move(o)).serialize());
      },
  });
}

// ---------------------------------------------------------------------------
// Phase 4 write tools (2026-10-11): the smallest safe v1 set.
//
// world_set_block: one block, kPermWorldWrite-gated, approval-gated
// (deny-by-default: the grant flow must explicitly grant the bit), and
// rate-limited by the per-connection write bucket. The write is applied on
// the SIM THREAD via the IWorldWriter seam — the MCP/HTTP thread never
// touches world state; the call blocks until the sim thread applies it (or
// the timeout expires -> isError, never a hang). See IWorldWriter in
// server.h for the marshaling contract and MCP-GAME-SEAM.md §9 for the
// game's SimThread::edit adapter recipe.
//
// lithoscript_exec: one lithoscript line via ConsoleRegistry::executeLine,
// output captured into the result. Runs at the AGENT's permission mask,
// NEVER the player's (the IConsoleExec adapter installs the mask via the
// ConsoleRegistry::setCallerPermissions seam — cheat-gated commands are
// refused for any restricted mask).
//
// NOT in v1 (research §5, v2): sim_pause/sim_resume, angelscript_eval.
// ---------------------------------------------------------------------------

/// Max lithoscript line the exec tool accepts (64 KiB). Longer lines are
/// malformed (-32602): a line this long is a batch, not a line — split it.
inline constexpr std::size_t kMcpMaxExecLineBytes = 64u * 1024u;

void McpServer::registerPhase4WriteTools() {
  McpBackends& b = backends_;

  // -- world_set_block -------------------------------------------------------
  ToolDescriptor setBlock;
  setBlock.name = "world_set_block";
  setBlock.description =
      "Set one block at absolute coordinates. Applied on the sim thread; "
      "the call blocks until applied or the timeout expires. Requires the "
      "world_write grant (deny-by-default) and is rate-limited.";
  setBlock.params = {p("x", "integer", "block X coordinate", true),
                     p("y", "integer", "block Y coordinate", true),
                     p("z", "integer", "block Z coordinate", true),
                     p("block", "integer", "block id (0..65535)", true),
                     p("timeout_ms", "integer",
                       "max ms to wait for the sim thread (1..2000, default 2000)")};
  setBlock.requiredPerms = kPermWorldWrite;
  setBlock.annotations.readOnlyHint = false;
  setBlock.annotations.destructiveHint = true;
  setBlock.annotations.idempotentHint = true;  // setting the same block twice is a no-op
  setBlock.handler = [&b, this](const JsonValue& args, const McpAgent& agent) {
    ArgReader r(args);
    const int64_t xi = r.reqInt("x");
    const int64_t yi = r.reqInt("y");
    const int64_t zi = r.reqInt("z");
    const int64_t bi = r.reqInt("block");
    if (xi < INT32_MIN || xi > INT32_MAX || yi < INT32_MIN || yi > INT32_MAX ||
        zi < INT32_MIN || zi > INT32_MAX) {
      throw RpcError(kRpcInvalidParams, "world_set_block: coordinates out of int32 range");
    }
    if (bi < 0 || bi > 65535) {
      throw RpcError(kRpcInvalidParams, "world_set_block: 'block' must be 0..65535");
    }
    const int timeoutMs =
        static_cast<int>(r.optInt("timeout_ms", kMcpSimWriteTimeoutMs, 1, kMcpSimWriteTimeoutMs));
    if (!b.writer) {
      log("mcp: write world_set_block agent=" + agent.name + " -> not wired");
      return ToolResult::error(kNotWired);
    }
    if (!blockWrites_.take(1)) {
      log("mcp: write world_set_block agent=" + agent.name + " -> rate_limited (" +
          std::to_string(kMcpBlockWriteLimitPerSec) + "/sec, burst " +
          std::to_string(kMcpBlockWriteBurst) + ")");
      return ToolResult::error(
          "write rate limit exceeded: at most " + std::to_string(kMcpBlockWriteLimitPerSec) +
          " block writes/sec (burst " + std::to_string(kMcpBlockWriteBurst) + ") per connection");
    }
    const int x = static_cast<int>(xi), y = static_cast<int>(yi), z = static_cast<int>(zi);
    std::string why;
    const bool ok =
        b.writer->setBlock(x, y, z, static_cast<uint16_t>(bi), timeoutMs, why);
    log("mcp: write world_set_block agent=" + agent.name + " (" + std::to_string(x) + "," +
        std::to_string(y) + "," + std::to_string(z) + ")=" + std::to_string(bi) + " -> " +
        (ok ? "ok" : "not applied: " + why));
    if (!ok) return ToolResult::error("world_set_block not applied: " + why);
    JsonValue::Object o;
    o.emplace_back("x", JsonValue::integer(x));
    o.emplace_back("y", JsonValue::integer(y));
    o.emplace_back("z", JsonValue::integer(z));
    o.emplace_back("block", JsonValue::integer(bi));
    o.emplace_back("applied", JsonValue::boolean(true));
    return ToolResult::ok(JsonValue::object(std::move(o)).serialize());
  };
  tools_.push_back(std::move(setBlock));

  // -- lithoscript_exec ------------------------------------------------------
  ToolDescriptor exec;
  exec.name = "lithoscript_exec";
  exec.description =
      "Execute one lithoscript line and return its console output. Runs at "
      "the AGENT's permission mask, never the player's: cheat-gated commands "
      "are refused unless the agent's context allows them. Requires the "
      "console grant (deny-by-default).";
  exec.params = {p("line", "string", "one lithoscript line (max 64 KiB)", true)};
  exec.requiredPerms = kPermConsole;
  exec.annotations.readOnlyHint = false;
  exec.annotations.destructiveHint = true;  // an arbitrary line can mutate the world
  exec.handler = [&b, this](const JsonValue& args, const McpAgent& agent) {
    ArgReader r(args);
    const std::string line = r.reqNonEmptyString("line");
    if (line.size() > kMcpMaxExecLineBytes) {
      throw RpcError(kRpcInvalidParams, "lithoscript_exec: line exceeds 64 KiB");
    }
    if (!b.consoleExec) {
      log("mcp: exec lithoscript_exec agent=" + agent.name + " -> not wired");
      return ToolResult::error(kNotWired);
    }
    std::vector<std::string> out;
    // THE critical rule: the agent's mask, never the player's. The adapter
    // installs it on the registry for the duration of the call.
    b.consoleExec->executeLine(line, agent.permissionMask, out);
    std::string text;
    for (size_t i = 0; i < out.size(); ++i) {
      if (i) text += '\n';
      text += out[i];
    }
    if (text.empty()) text = "(no output)";
    log("mcp: exec lithoscript_exec agent=" + agent.name + " mask=" +
        permMaskNames(agent.permissionMask) + " bytes=" + std::to_string(line.size()) +
        " -> ok (" + std::to_string(out.size()) + " output lines)");
    return ToolResult::ok(text);
  };
  tools_.push_back(std::move(exec));
}

// ---------------------------------------------------------------------------
// Player control tools: the model pilots the local player through the REAL
// input path (synthetic InputEvents merged where the keyboard enters).
// Gated on kPermPlayerControl (deny-by-default), rate-limited by the
// per-connection control bucket (120/sec, burst 240). Every frame pump
// consumes at most one staged batch, so the effective input rate can never
// exceed the frame rate. See IPlayerController in server.h for the
// anti-cheat note: a model-driven player is still a player.
// ---------------------------------------------------------------------------

McpTokenBucket& McpServer::controlBucket(const std::string& pilot) {
  auto it = controlBuckets_.find(pilot);
  if (it == controlBuckets_.end())
    it = controlBuckets_
             .emplace(pilot, McpTokenBucket(kMcpControlLimitPerSec, kMcpControlBurst))
             .first;
  return it->second;
}

McpTokenBucket& McpServer::visionBucket(const std::string& pilot) {
  auto it = visionBuckets_.find(pilot);
  if (it == visionBuckets_.end())
    it = visionBuckets_
             .emplace(pilot, McpTokenBucket(kMcpVisionLimitPerSec, kMcpVisionBurst))
             .first;
  return it->second;
}

McpTokenBucket& McpServer::audioBucket(const std::string& pilot) {
  auto it = audioBuckets_.find(pilot);
  if (it == audioBuckets_.end())
    it = audioBuckets_
             .emplace(pilot, McpTokenBucket(kMcpAudioLimitPerSec, kMcpAudioBurst))
             .first;
  return it->second;
}

void McpServer::registerPlayerControlTools() {
  McpBackends& b = backends_;

  // Per-pilot helpers (multi-pilot, locked 2026-10-11).
  auto takePilotBudget = [this](const std::string& pilot, const McpAgent& agent,
                                const char* tool) -> bool {
    if (controlBucket(pilot).take(1)) return true;
    log("mcp: control " + std::string(tool) + " agent=" + agent.name + " pilot=" + pilot +
        " -> rate_limited (" + std::to_string(kMcpControlLimitPerSec) + "/sec, burst " +
        std::to_string(kMcpControlBurst) + " per pilot)");
    return false;
  };
  auto needPilot = [&b, this](const std::string& pilot, const McpAgent& agent,
                              const char* tool) -> IPlayerController* {
    if (!b.controller) {
      log("mcp: control " + std::string(tool) + " agent=" + agent.name + " pilot=" + pilot +
          " -> not wired");
      return nullptr;
    }
    const std::vector<std::string> ids = b.controller->pilotIds();
    if (std::find(ids.begin(), ids.end(), pilot) == ids.end()) {
      std::string known;
      for (size_t i = 0; i < ids.size(); ++i) {
        if (i) known += ", ";
        known += ids[i];
      }
      log("mcp: control " + std::string(tool) + " agent=" + agent.name + " -> unknown pilot '" +
          pilot + "'");
      return nullptr;
    }
    return b.controller.get();
  };
  auto reqPilot = [](ArgReader& r) {
    const std::string pilot = r.reqNonEmptyString("pilot");
    if (pilot.size() > 64)
      throw RpcError(kRpcInvalidParams, "pilot id exceeds 64 chars");
    return pilot;
  };
  auto rateErr = []() {
    return ToolResult::error(
        "control rate limit exceeded: at most " + std::to_string(kMcpControlLimitPerSec) +
        " control calls/sec (burst " + std::to_string(kMcpControlBurst) + ") per pilot");
  };
  const char* kNoPilot = "no controllable player for this pilot (menu, paused, or inventory open)";

  // -- player_move -----------------------------------------------------------
  {
    ToolDescriptor t;
    t.name = "player_move";
    t.description =
        "Set a pilot's held movement state (sticky until changed): "
        "forward/strafe in -1..1, jump/sneak/sprint held. 0,0 stops. "
        "Same physics as keyboard input. Requires the player_control grant.";
    t.params = {p("pilot", "string", "pilot id (v1: \"player\")", true),
                p("forward", "number", "forward/back held: -1..1", true),
                p("strafe", "number", "strafe held: -1..1 (right positive)", true),
                p("jump", "boolean", "hold jump (fly up)"),
                p("sneak", "boolean", "hold sneak (fly down)"),
                p("sprint", "boolean", "hold sprint")};
    t.params[1].hasMinimum = t.params[1].hasMaximum = true;
    t.params[1].minimum = -1.0; t.params[1].maximum = 1.0;
    t.params[2].hasMinimum = t.params[2].hasMaximum = true;
    t.params[2].minimum = -1.0; t.params[2].maximum = 1.0;
    t.requiredPerms = kPermPlayerControl;
    t.annotations.readOnlyHint = false;
    t.handler = [&b, this, takePilotBudget, needPilot, reqPilot, rateErr, kNoPilot](
                    const JsonValue& args, const McpAgent& agent) {
      ArgReader r(args);
      const std::string pilot = reqPilot(r);
      const double fwd = r.optNumber("forward", 0.0, -1.0, 1.0);
      const double str = r.optNumber("strafe", 0.0, -1.0, 1.0);
      if (!takePilotBudget(pilot, agent, "player_move")) return rateErr();
      IPlayerController* c = needPilot(pilot, agent, "player_move");
      if (!c) return ToolResult::error(kNotWired);
      IPlayerController::ControlFrame f;
      f.moveForward = static_cast<float>(fwd);
      f.moveStrafe = static_cast<float>(str);
      f.jump = r.optBool("jump", false);
      f.sneak = r.optBool("sneak", false);
      f.sprint = r.optBool("sprint", false);
      if (!c->submitFrame(pilot, f)) {
        log("mcp: control player_move agent=" + agent.name + " pilot=" + pilot +
            " -> no controllable player");
        return ToolResult::error(kNoPilot);
      }
      log("mcp: control player_move agent=" + agent.name + " pilot=" + pilot + " -> ok");
      return ToolResult::ok("{\"applied\":true}");
    };
    tools_.push_back(std::move(t));
  }

  // -- player_look -----------------------------------------------------------
  {
    ToolDescriptor t;
    t.name = "player_look";
    t.description =
        "Turn a pilot's view by relative mouse-motion pixels (one-shot). "
        "Same path as mouse look; clamped to +-2048 px per call. Requires the "
        "player_control grant.";
    t.params = {p("pilot", "string", "pilot id (v1: \"player\")", true),
                p("dx", "number", "pixels right (positive) / left", true),
                p("dy", "number", "pixels down (positive) / up", true)};
    t.requiredPerms = kPermPlayerControl;
    t.annotations.readOnlyHint = false;
    t.handler = [&b, this, takePilotBudget, needPilot, reqPilot, rateErr, kNoPilot](
                    const JsonValue& args, const McpAgent& agent) {
      ArgReader r(args);
      const std::string pilot = reqPilot(r);
      const double dx = r.optNumber("dx", 0.0, -2048.0, 2048.0);
      const double dy = r.optNumber("dy", 0.0, -2048.0, 2048.0);
      if (!takePilotBudget(pilot, agent, "player_look")) return rateErr();
      IPlayerController* c = needPilot(pilot, agent, "player_look");
      if (!c) return ToolResult::error(kNotWired);
      IPlayerController::ControlFrame f;
      f.lookDx = static_cast<float>(dx);
      f.lookDy = static_cast<float>(dy);
      if (!c->submitFrame(pilot, f)) {
        log("mcp: control player_look agent=" + agent.name + " pilot=" + pilot +
            " -> no controllable player");
        return ToolResult::error(kNoPilot);
      }
      log("mcp: control player_look agent=" + agent.name + " pilot=" + pilot + " -> ok");
      return ToolResult::ok("{\"applied\":true}");
    };
    tools_.push_back(std::move(t));
  }

  // -- player_action ---------------------------------------------------------
  {
    ToolDescriptor t;
    t.name = "player_action";
    t.description =
        "Press one action button for a pilot for exactly one frame: use "
        "(interact), attack (break), place (selected hotbar block), drop "
        "(clear selected slot). Goes through the game's normal action "
        "handling and sim-thread marshaling — same reach and interact checks "
        "as a human click. Requires the player_control grant.";
    t.params = {p("pilot", "string", "pilot id (v1: \"player\")", true),
                p("action", "string", "one of: use, attack, place, drop", true)};
    t.requiredPerms = kPermPlayerControl;
    t.annotations.readOnlyHint = false;
    t.annotations.destructiveHint = true;  // attack/place mutate the world
    t.handler = [&b, this, takePilotBudget, needPilot, reqPilot, rateErr, kNoPilot](
                    const JsonValue& args, const McpAgent& agent) {
      ArgReader r(args);
      const std::string pilot = reqPilot(r);
      const std::string a = r.reqString("action");
      IPlayerController::ControlFrame f;
      if (a == "use") f.usePressed = true;
      else if (a == "attack") f.attackPressed = true;
      else if (a == "place") f.placePressed = true;
      else if (a == "drop") f.dropPressed = true;
      else
        throw RpcError(kRpcInvalidParams,
                       "player_action: 'action' must be one of: use, attack, place, drop");
      if (!takePilotBudget(pilot, agent, "player_action")) return rateErr();
      IPlayerController* c = needPilot(pilot, agent, "player_action");
      if (!c) return ToolResult::error(kNotWired);
      if (!c->submitFrame(pilot, f)) {
        log("mcp: control player_action agent=" + agent.name + " pilot=" + pilot +
            " -> no controllable player");
        return ToolResult::error(kNoPilot);
      }
      log("mcp: control player_action agent=" + agent.name + " pilot=" + pilot + " action=" + a +
          " -> ok");
      return ToolResult::ok("{\"applied\":true}");
    };
    tools_.push_back(std::move(t));
  }

  // -- player_hotbar ---------------------------------------------------------
  {
    ToolDescriptor t;
    t.name = "player_hotbar";
    t.description =
        "Select a pilot's hotbar slot (0-based). Same as the hotbar number "
        "keys. Requires the player_control grant.";
    t.params = {p("pilot", "string", "pilot id (v1: \"player\")", true),
                p("slot", "integer", "hotbar slot 0..7", true)};
    t.requiredPerms = kPermPlayerControl;
    t.annotations.readOnlyHint = false;
    t.handler = [&b, this, takePilotBudget, needPilot, reqPilot, rateErr, kNoPilot](
                    const JsonValue& args, const McpAgent& agent) {
      ArgReader r(args);
      const std::string pilot = reqPilot(r);
      const int64_t slot = r.optInt("slot", -1, 0, 7);
      if (slot < 0)
        throw RpcError(kRpcInvalidParams, "player_hotbar: 'slot' must be 0..7");
      if (!takePilotBudget(pilot, agent, "player_hotbar")) return rateErr();
      IPlayerController* c = needPilot(pilot, agent, "player_hotbar");
      if (!c) return ToolResult::error(kNotWired);
      IPlayerController::ControlFrame f;
      f.hotbarSlot = static_cast<int>(slot);
      if (!c->submitFrame(pilot, f)) {
        log("mcp: control player_hotbar agent=" + agent.name + " pilot=" + pilot +
            " -> no controllable player");
        return ToolResult::error(kNoPilot);
      }
      log("mcp: control player_hotbar agent=" + agent.name + " pilot=" + pilot + " -> ok");
      return ToolResult::ok("{\"applied\":true}");
    };
    tools_.push_back(std::move(t));
  }

  // -- player_release --------------------------------------------------------
  {
    ToolDescriptor t;
    t.name = "player_release";
    t.description =
        "Release every held movement button for a pilot (safety). Call when "
        "done driving, or the player keeps walking. Requires the "
        "player_control grant.";
    t.params = {p("pilot", "string", "pilot id (v1: \"player\")", true)};
    t.requiredPerms = kPermPlayerControl;
    t.annotations.readOnlyHint = false;
    t.handler = [&b, this, takePilotBudget, needPilot, reqPilot, rateErr](
                    const JsonValue& args, const McpAgent& agent) {
      ArgReader r(args);
      const std::string pilot = reqPilot(r);
      if (!takePilotBudget(pilot, agent, "player_release")) return rateErr();
      IPlayerController* c = needPilot(pilot, agent, "player_release");
      if (!c) return ToolResult::error(kNotWired);
      c->releaseAll(pilot);
      log("mcp: control player_release agent=" + agent.name + " pilot=" + pilot + " -> ok");
      return ToolResult::ok("{\"applied\":true}");
    };
    tools_.push_back(std::move(t));
  }
}

// ---------------------------------------------------------------------------
// Pilot senses: vision.capture_frame + audio.capture_chunk. The model sees
// and hears through the piloted player, closing the see -> think -> act
// loop with query (world state) and the player_* control tools.
//
// Gated on kPermPilotSenses (deny-by-default) — observation only, NO
// actuation. Rate-limited per pilot: vision 4/sec (burst 8), audio 8/sec
// (burst 16). Downgraded by design: vision is the software renderer at the
// lowest settings and low resolution; audio starts at tier 0 (8 kHz mono)
// and ramps via the game's mcp.audio_tier cvar. See mcp_senses.h.
// ---------------------------------------------------------------------------

void McpServer::registerPilotSensesTools() {
  McpBackends& b = backends_;

  auto reqPilot = [](ArgReader& r) {
    const std::string pilot = r.reqNonEmptyString("pilot");
    if (pilot.size() > 64)
      throw RpcError(kRpcInvalidParams, "pilot id exceeds 64 chars");
    return pilot;
  };
  // Validates the pilot against a senses backend's own pilot list (each
  // seam is self-describing; the controller need not be wired to see).
  auto needSensePilot = [this](const std::vector<std::string>& ids,
                               const std::string& pilot, const McpAgent& agent,
                               const char* tool) -> bool {
    if (std::find(ids.begin(), ids.end(), pilot) != ids.end()) return true;
    std::string known;
    for (size_t i = 0; i < ids.size(); ++i) {
      if (i) known += ", ";
      known += ids[i];
    }
    log("mcp: senses " + std::string(tool) + " agent=" + agent.name +
        " -> unknown pilot '" + pilot + "' (known: " + known + ")");
    return false;
  };

  // -- vision.capture_frame --------------------------------------------------
  {
    ToolDescriptor t;
    t.name = "vision.capture_frame";
    t.description =
        "Capture one downgraded frame from a pilot's viewpoint: software "
        "renderer, lowest settings, low resolution (game cvars "
        "mcp.vision_width/height, default 160x90). Returns PNG bytes "
        "(base64) plus the camera. Rate-limited: 4/sec, burst 8 per pilot. "
        "Requires the pilot_senses grant.";
    t.params = {p("pilot", "string", "pilot id (v1: \"player\")", true),
                p("width", "integer",
                  "capture width, 16..512 (default: the game's cvar)"),
                p("height", "integer",
                  "capture height, 16..512 (default: the game's cvar)")};
    t.params[1].hasMinimum = t.params[1].hasMaximum = true;
    t.params[1].minimum = 16; t.params[1].maximum = 512;
    t.params[2].hasMinimum = t.params[2].hasMaximum = true;
    t.params[2].minimum = 16; t.params[2].maximum = 512;
    t.requiredPerms = kPermPilotSenses;
    t.annotations.readOnlyHint = true;  // observation only: never mutates
    t.handler = [&b, this, reqPilot, needSensePilot](const JsonValue& args,
                                                    const McpAgent& agent) {
      ArgReader r(args);
      const std::string pilot = reqPilot(r);
      if (!visionBucket(pilot).take(1)) {
        log("mcp: senses vision.capture_frame agent=" + agent.name + " pilot=" + pilot +
            " -> rate_limited (" + std::to_string(kMcpVisionLimitPerSec) + "/sec, burst " +
            std::to_string(kMcpVisionBurst) + " per pilot)");
        return ToolResult::error(
            "vision rate limit exceeded: at most " + std::to_string(kMcpVisionLimitPerSec) +
            " frames/sec (burst " + std::to_string(kMcpVisionBurst) + ") per pilot");
      }
      if (!b.vision) return ToolResult::error(kNotWired);
      if (!needSensePilot(b.vision->pilotIds(), pilot, agent, "vision.capture_frame"))
        return ToolResult::error("unknown pilot '" + pilot + "'");
      const int w = static_cast<int>(r.optInt("width", 0, 16, 512));
      const int h = static_cast<int>(r.optInt("height", 0, 16, 512));
      VisionFrame f;
      if (!b.vision->captureVisionFrame(pilot, w, h, f) || f.png.empty())
        return ToolResult::error("vision capture failed for pilot '" + pilot + "'");
      JsonValue::Object o;
      o.emplace_back("width", JsonValue::integer(f.width));
      o.emplace_back("height", JsonValue::integer(f.height));
      o.emplace_back("format", JsonValue::string("png"));
      o.emplace_back("data",
                     JsonValue::string(base64Encode(f.png.data(), f.png.size())));
      o.emplace_back("tick", JsonValue::integer(int64_t(f.tick)));
      JsonValue::Object cam;
      cam.emplace_back("x", JsonValue::number(f.camX));
      cam.emplace_back("y", JsonValue::number(f.camY));
      cam.emplace_back("z", JsonValue::number(f.camZ));
      cam.emplace_back("yaw", JsonValue::number(f.yaw));
      cam.emplace_back("pitch", JsonValue::number(f.pitch));
      o.emplace_back("camera", JsonValue::object(std::move(cam)));
      log("mcp: senses vision.capture_frame agent=" + agent.name + " pilot=" + pilot +
          " -> ok " + std::to_string(f.width) + "x" + std::to_string(f.height));
      return ToolResult::ok(JsonValue::object(std::move(o)).serialize());
    };
    tools_.push_back(std::move(t));
  }

  // -- audio.capture_chunk ---------------------------------------------------
  {
    ToolDescriptor t;
    t.name = "audio.capture_chunk";
    t.description =
        "Capture one 1024-frame mono audio chunk of what a pilot hears: the "
        "engine's positional mix tapped post-shed at a downgraded sample "
        "rate. tier 0=8kHz (default, cheapest) .. 3=48kHz; the game's "
        "mcp.audio_tier cvar ramps the default without code changes. "
        "Rate-limited: 8/sec, burst 16 per pilot. Requires pilot_senses.";
    t.params = {p("pilot", "string", "pilot id (v1: \"player\")", true),
                p("tier", "integer",
                  "downgrade tier 0..3 (default 0 = 8 kHz mono)")};
    t.params[1].hasMinimum = t.params[1].hasMaximum = true;
    t.params[1].minimum = 0; t.params[1].maximum = 3;
    t.requiredPerms = kPermPilotSenses;
    t.annotations.readOnlyHint = true;  // observation only: never mutates
    t.handler = [&b, this, reqPilot, needSensePilot](const JsonValue& args,
                                                    const McpAgent& agent) {
      ArgReader r(args);
      const std::string pilot = reqPilot(r);
      if (!audioBucket(pilot).take(1)) {
        log("mcp: senses audio.capture_chunk agent=" + agent.name + " pilot=" + pilot +
            " -> rate_limited (" + std::to_string(kMcpAudioLimitPerSec) + "/sec, burst " +
            std::to_string(kMcpAudioBurst) + " per pilot)");
        return ToolResult::error(
            "audio rate limit exceeded: at most " + std::to_string(kMcpAudioLimitPerSec) +
            " chunks/sec (burst " + std::to_string(kMcpAudioBurst) + ") per pilot");
      }
      if (!b.audio) return ToolResult::error(kNotWired);
      if (!needSensePilot(b.audio->pilotIds(), pilot, agent, "audio.capture_chunk"))
        return ToolResult::error("unknown pilot '" + pilot + "'");
      const int tier = static_cast<int>(r.optInt("tier", 0, 0, 3));
      AudioChunk c;
      if (!b.audio->captureAudioChunk(pilot, tier, c) || c.samples.empty())
        return ToolResult::error("audio capture failed for pilot '" + pilot + "'");
      JsonValue::Object o;
      o.emplace_back("sampleRate", JsonValue::integer(c.sampleRate));
      o.emplace_back("channels", JsonValue::integer(1));
      o.emplace_back("format", JsonValue::string("pcm_s16le"));
      o.emplace_back("frames", JsonValue::integer(int64_t(c.samples.size())));
      o.emplace_back("data", JsonValue::string(base64Encode(
                                   reinterpret_cast<const uint8_t*>(c.samples.data()),
                                   c.samples.size() * sizeof(int16_t))));
      o.emplace_back("tick", JsonValue::integer(int64_t(c.tick)));
      log("mcp: senses audio.capture_chunk agent=" + agent.name + " pilot=" + pilot +
          " -> ok tier " + std::to_string(tier));
      return ToolResult::ok(JsonValue::object(std::move(o)).serialize());
    };
    tools_.push_back(std::move(t));
  }

  // -- vision.set_camera -----------------------------------------------------
  {
    ToolDescriptor t;
    t.name = "vision.set_camera";
    t.description =
        "Place a pilot's vision camera at an arbitrary world pose (free "
        "camera, independent of any player): position + yaw/pitch + fov. "
        "The camera persists until vision.track_player or the next "
        "set_camera. Unloaded territory renders as sky. Requires pilot_senses.";
    t.params = {p("pilot", "string", "pilot id (v1: \"player\")", true),
                p("x", "number", "camera world x", true),
                p("y", "number", "camera world y", true),
                p("z", "number", "camera world z", true),
                p("yaw", "number", "degrees, 0 = -Z, positive clockwise", true),
                p("pitch", "number", "degrees, positive up, -90..90", true),
                p("fov", "number", "degrees, 10..120 (default 70)")};
    t.params[5].hasMinimum = t.params[5].hasMaximum = true;
    t.params[5].minimum = -90.0; t.params[5].maximum = 90.0;
    t.params[6].hasMinimum = t.params[6].hasMaximum = true;
    t.params[6].minimum = 10.0; t.params[6].maximum = 120.0;
    t.requiredPerms = kPermPilotSenses;
    t.annotations.readOnlyHint = false;  // moves the camera (observer state)
    t.handler = [&b, this, reqPilot, needSensePilot](const JsonValue& args,
                                                    const McpAgent& agent) {
      ArgReader r(args);
      const std::string pilot = reqPilot(r);
      if (!visionBucket(pilot).take(1)) {
        log("mcp: senses vision.set_camera agent=" + agent.name + " pilot=" + pilot +
            " -> rate_limited");
        return ToolResult::error("vision rate limit exceeded for this pilot");
      }
      if (!b.vision) return ToolResult::error(kNotWired);
      if (!needSensePilot(b.vision->pilotIds(), pilot, agent, "vision.set_camera"))
        return ToolResult::error("unknown pilot '" + pilot + "'");
      CameraPose pose;
      pose.x = r.reqNumber("x");
      pose.y = r.reqNumber("y");
      pose.z = r.reqNumber("z");
      pose.yaw = float(r.reqNumber("yaw"));
      pose.pitch = float(r.reqNumber("pitch"));
      pose.fov = float(r.optNumber("fov", 70.0, 10.0, 120.0));
      pose.trackPlayer = false;
      if (!b.vision->setCameraPose(pilot, pose))
        return ToolResult::error("vision.set_camera rejected for pilot '" + pilot + "'");
      log("mcp: senses vision.set_camera agent=" + agent.name + " pilot=" + pilot +
          " -> ok (" + std::to_string(pose.x) + "," + std::to_string(pose.y) + "," +
          std::to_string(pose.z) + ")");
      return ToolResult::ok("{\"applied\":true}");
    };
    tools_.push_back(std::move(t));
  }

  // -- vision.track_player ---------------------------------------------------
  {
    ToolDescriptor t;
    t.name = "vision.track_player";
    t.description =
        "Return a pilot's vision camera to player-tracking (the default): "
        "each capture follows the piloted player. Requires pilot_senses.";
    t.params = {p("pilot", "string", "pilot id (v1: \"player\")", true)};
    t.requiredPerms = kPermPilotSenses;
    t.annotations.readOnlyHint = false;
    t.handler = [&b, this, reqPilot, needSensePilot](const JsonValue& args,
                                                    const McpAgent& agent) {
      ArgReader r(args);
      const std::string pilot = reqPilot(r);
      if (!visionBucket(pilot).take(1)) {
        log("mcp: senses vision.track_player agent=" + agent.name + " pilot=" + pilot +
            " -> rate_limited");
        return ToolResult::error("vision rate limit exceeded for this pilot");
      }
      if (!b.vision) return ToolResult::error(kNotWired);
      if (!needSensePilot(b.vision->pilotIds(), pilot, agent, "vision.track_player"))
        return ToolResult::error("unknown pilot '" + pilot + "'");
      if (!b.vision->trackPlayer(pilot))
        return ToolResult::error("vision.track_player rejected for pilot '" + pilot + "'");
      log("mcp: senses vision.track_player agent=" + agent.name + " pilot=" + pilot +
          " -> ok");
      return ToolResult::ok("{\"applied\":true}");
    };
    tools_.push_back(std::move(t));
  }
}

// ---------------------------------------------------------------------------
// Phase 1 resources.
// ---------------------------------------------------------------------------

void McpServer::registerPhase1Resources() {
  McpBackends& b = backends_;

  resources_.push_back(ResourceDescriptor{
      "lithoscript://commands",
      "Lithoscript commands",
      "Live console command surface: name and help, one per line.",
      "text/plain",
      kPermConsole,
      [&b](const McpAgent&) {
        if (!b.console) throw RpcError(kRpcInternalError, "console surface not wired");
        auto cmds = b.console->commands();
        std::sort(cmds.begin(), cmds.end(),
                  [](const auto& x, const auto& y) { return x.name < y.name; });
        std::ostringstream out;
        for (const auto& c : cmds) {
          out << c.name;
          if (!c.help.empty()) out << " - " << c.help;
          out << "\n";
        }
        return out.str();
      },
  });

  resources_.push_back(ResourceDescriptor{
      "lithoscript://cvars",
      "Lithoscript cvars",
      "Live console variables: name, current value, help. One per line.",
      "text/plain",
      kPermConsole,
      [&b](const McpAgent&) {
        if (!b.console) throw RpcError(kRpcInternalError, "console surface not wired");
        auto cvars = b.console->cvars();
        std::sort(cvars.begin(), cvars.end(),
                  [](const auto& x, const auto& y) { return x.name < y.name; });
        std::ostringstream out;
        for (const auto& c : cvars) {
          out << c.name << " = " << c.value;
          if (!c.help.empty()) out << "  # " << c.help;
          out << "\n";
        }
        return out.str();
      },
  });

  resources_.push_back(ResourceDescriptor{
      "world://dimensions",
      "Dimensions",
      "Dimension list: id, display name, bound half-extent, current flag.",
      "application/json",
      kPermWorldRead,
      [&b](const McpAgent&) {
        if (!b.dimensions) throw RpcError(kRpcInternalError, "dimension view not wired");
        JsonValue::Array arr;
        for (const auto& d : b.dimensions->dimensions()) {
          JsonValue::Object o;
          o.emplace_back("id", JsonValue::string(d.id));
          o.emplace_back("name", JsonValue::string(d.name));
          o.emplace_back("bound", JsonValue::integer(d.bound));
          o.emplace_back("current", JsonValue::boolean(d.current));
          arr.emplace_back(JsonValue::object(std::move(o)));
        }
        return JsonValue::array(std::move(arr)).serialize();
      },
  });

  resources_.push_back(ResourceDescriptor{
      "mods://list",
      "Installed mods",
      "Installed mods: name, version, granted permission mask, extension trees.",
      "application/json",
      0,
      [&b](const McpAgent&) {
        if (!b.mods) throw RpcError(kRpcInternalError, "mod list not wired");
        JsonValue::Array arr;
        for (const auto& m : b.mods->mods()) {
          JsonValue::Object o;
          o.emplace_back("name", JsonValue::string(m.name));
          o.emplace_back("version", JsonValue::string(m.version));
          o.emplace_back("permissions",
                         JsonValue::integer(static_cast<int64_t>(m.permissions)));
          o.emplace_back("permissionNames", strVec(permBitNameList(m.permissions)));
          o.emplace_back("extensions", strVec(m.extensions));
          arr.emplace_back(JsonValue::object(std::move(o)));
        }
        return JsonValue::array(std::move(arr)).serialize();
      },
  });

  resources_.push_back(ResourceDescriptor{
      "game://lore-glossary",
      "Lore glossary",
      "The LithoKraft lore glossary (Kharge, the Fragment, the Daemon...), "
      "as markdown text supplied by the game.",
      "text/markdown",
      0,
      [&b](const McpAgent&) {
        if (!b.lore) throw RpcError(kRpcInternalError, "lore glossary not wired");
        std::string text = b.lore->text();
        if (text.empty()) {
          throw RpcError(kRpcInternalError,
                         "lore glossary not configured by the game");
        }
        return text;
      },
  });

  resources_.push_back(ResourceDescriptor{
      "net://version",
      "Version info",
      "Engine, SDK, network protocol, and MCP protocol versions.",
      "application/json",
      0,
      [](const McpAgent&) {
        JsonValue::Object o;
        o.emplace_back("engine", JsonValue::string(mcpEngineVersionString()));
        o.emplace_back("sdk", JsonValue::integer(LITHOLATTICE_SDK_VERSION));
        // F-L3: advertise the CURRENT protocol version (kProtocolVersion,
        // v3) — this resource previously reported LITHOLATTICE_PROTOCOL_V1.
        o.emplace_back("protocol", JsonValue::integer(net::kProtocolVersion));
        o.emplace_back("mcp", JsonValue::string(kMcpProtocolVersion));
        return JsonValue::object(std::move(o)).serialize();
      },
  });
}

// ---------------------------------------------------------------------------
// stdio transport on real stdin/stdout.
// ---------------------------------------------------------------------------

bool FdStdioTransport::readLine(std::string& out) {
  // Chunked read with an early cap: the old std::getline buffered the ENTIRE
  // line before runStdio's kMaxStdioLineBytes check could fire, so one giant
  // line OOMed the server first. Now the cap trips DURING the read — at most
  // one chunk past it is ever buffered — the rest of the line is discarded,
  // and bytes past its newline are stashed in pending_ for the next call.
  //
  // Contract note: when the cap trips, `out` is set to a sentinel of size
  // kMaxStdioLineBytes + 1 so runStdio's existing overlong-line check still
  // fires (drop + log). The byte count in that log line is therefore
  // "cap+1", not the true length — the true length is unknowable without
  // buffering it, which is exactly what this avoids.
  out.clear();
  out.reserve(4096);
  bool overlong = false;   // cap tripped: discarding through end of line
  bool haveBytes = false;  // anything consumed (a trailing unterminated line counts)
  bool lineDone = false;

  // Feeds one buffer of fresh bytes into the line being built.
  const auto feed = [&](const char* p, size_t n) {
    if (lineDone || n == 0) return;
    haveBytes = true;
    const char* nl = static_cast<const char*>(std::memchr(p, '\n', n));
    const size_t take = nl ? static_cast<size_t>(nl - p) : n;
    if (!overlong) {
      if (out.size() + take > kMaxStdioLineBytes) {
        overlong = true;  // trip: stop buffering, discard through the newline
      } else {
        out.append(p, take);
      }
    }
    if (nl) {
      const size_t tail = n - (take + 1);
      if (tail > 0) pending_.assign(p + take + 1, tail);
      lineDone = true;
    }
  };

  if (!pending_.empty()) {
    const std::string pend = std::move(pending_);
    pending_.clear();
    feed(pend.data(), pend.size());
  }
  char chunk[8192];
  while (!lineDone && std::cin) {
    std::cin.read(chunk, sizeof chunk);
    const std::streamsize n = std::cin.gcount();
    if (n <= 0) break;  // EOF or read error
    feed(chunk, static_cast<size_t>(n));
  }

  if (!haveBytes) return false;  // EOF with no bytes at all: matches std::getline
  if (overlong) {
    out.assign(kMaxStdioLineBytes + 1, '\0');  // drop sentinel (see contract note)
  } else if (!out.empty() && out.back() == '\r') {
    out.pop_back();  // tolerate a trailing CR, as before
  }
  return true;
}

void FdStdioTransport::writeLine(std::string_view line) {
  std::cout << line << '\n';
  std::cout.flush();
}

}  // namespace litholattice::mcp
