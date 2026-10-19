// jsonrpc.cpp: JSON-RPC 2.0 framing, written clean-room from the specification.
#include "litholattice/mcp/jsonrpc.h"

namespace litholattice::mcp {

std::string rpcErrorMessage(int code) {
  switch (code) {
    case kRpcParseError:
      return "Parse error";
    case kRpcInvalidRequest:
      return "Invalid Request";
    case kRpcMethodNotFound:
      return "Method not found";
    case kRpcInvalidParams:
      return "Invalid params";
    case kRpcInternalError:
      return "Internal error";
    case kRpcUnsupportedProtocolVersion:
      return "Unsupported protocol version";
    default:
      return "Server error";
  }
}

JsonValue buildSuccessResponse(const JsonValue& id, const JsonValue& result) {
  JsonValue::Object o;
  o.emplace_back("jsonrpc", JsonValue::string(kJsonRpcVersion));
  o.emplace_back("result", result);
  o.emplace_back("id", id);
  return JsonValue::object(std::move(o));
}

JsonValue buildErrorResponse(const JsonValue& id, int code, std::string_view message,
                             const JsonValue& data) {
  JsonValue::Object err;
  err.emplace_back("code", JsonValue::integer(code));
  err.emplace_back("message", JsonValue::string(message));
  if (!data.isNull()) {
    err.emplace_back("data", data);
  }
  JsonValue::Object o;
  o.emplace_back("jsonrpc", JsonValue::string(kJsonRpcVersion));
  o.emplace_back("error", JsonValue::object(std::move(err)));
  o.emplace_back("id", id);
  return JsonValue::object(std::move(o));
}

RpcParseOutcome parseRpcRequest(std::string_view text) {
  RpcParseOutcome out;
  ParseResult pr = parseJson(text);
  if (!pr.ok) {
    out.ok = false;
    out.errorCode = kRpcParseError;
    out.errorMessage = rpcErrorMessage(kRpcParseError);
    out.errorId = JsonValue::null();
    return out;
  }
  const JsonValue& v = pr.value;
  // v1 decision (documented in jsonrpc.h): batch arrays are rejected.
  if (!v.isObject()) {
    out.ok = false;
    out.errorCode = kRpcInvalidRequest;
    out.errorMessage = rpcErrorMessage(kRpcInvalidRequest);
    out.errorId = JsonValue::null();
    return out;
  }
  const JsonValue* version = v.find("jsonrpc");
  if (!version || !version->isString() || version->asString() != kJsonRpcVersion) {
    out.ok = false;
    out.errorCode = kRpcInvalidRequest;
    out.errorMessage = rpcErrorMessage(kRpcInvalidRequest);
    out.errorId = JsonValue::null();
    return out;
  }
  const JsonValue* method = v.find("method");
  if (!method || !method->isString()) {
    out.ok = false;
    out.errorCode = kRpcInvalidRequest;
    out.errorMessage = rpcErrorMessage(kRpcInvalidRequest);
    out.errorId = JsonValue::null();
    return out;
  }
  const JsonValue* id = v.find("id");
  const JsonValue* params = v.find("params");
  if (params && !params->isNull() && !params->isObject() && !params->isArray()) {
    // Per spec, params MUST be Array or Object when present; a wrong type is
    // the handler's call, surfaced as Invalid params.
    out.ok = true;
    out.request.isNotification = (id == nullptr);
    out.request.id = id ? *id : JsonValue::null();
    out.request.method = method->asString();
    out.request.params = *params;
    return out;
  }
  out.ok = true;
  out.request.isNotification = (id == nullptr);
  out.request.id = id ? *id : JsonValue::null();
  out.request.method = method->asString();
  out.request.params = params ? *params : JsonValue::null();
  return out;
}

void RpcRouter::registerHandler(std::string method, RpcHandler handler) {
  for (auto& [name, h] : handlers_) {
    if (name == method) {
      h = std::move(handler);
      return;
    }
  }
  handlers_.emplace_back(std::move(method), std::move(handler));
}

bool RpcRouter::hasHandler(std::string_view method) const {
  for (const auto& [name, h] : handlers_) {
    if (name == method) return true;
  }
  return false;
}

std::optional<JsonValue> RpcRouter::dispatch(const RpcRequest& request) {
  if (request.isNotification) {
    // Notifications still invoke the handler (fire and forget) but never
    // produce a response. Handler errors on a notification are swallowed:
    // there is nowhere to send them.
    for (const auto& [name, h] : handlers_) {
      if (name == request.method) {
        try {
          h(request.params);
        } catch (...) {
        }
        return std::nullopt;
      }
    }
    return std::nullopt;
  }
  for (const auto& [name, h] : handlers_) {
    if (name == request.method) {
      try {
        return buildSuccessResponse(request.id, h(request.params));
      } catch (const RpcError&) {
        throw;
      } catch (...) {
        throw RpcError(kRpcInternalError, rpcErrorMessage(kRpcInternalError));
      }
    }
  }
  throw RpcError(kRpcMethodNotFound, rpcErrorMessage(kRpcMethodNotFound),
                 JsonValue::string(request.method));
}

std::optional<JsonValue> handleParsedRequest(const RpcRequest& req, RpcRouter& router) {
  // Invalid params type (present but neither Array nor Object): -32602.
  if (!req.params.isNull() && !req.params.isObject() && !req.params.isArray()) {
    if (req.isNotification) return std::nullopt;  // notifications: no response, ever
    return buildErrorResponse(req.id, kRpcInvalidParams, rpcErrorMessage(kRpcInvalidParams));
  }
  try {
    return router.dispatch(req);  // nullopt for notifications
  } catch (const RpcError& e) {
    return buildErrorResponse(req.id, e.code(), e.message(), e.data());
  }
}

std::optional<std::string> handleRpcMessage(std::string_view text, RpcRouter& router) {
  RpcParseOutcome outcome = parseRpcRequest(text);
  if (!outcome.ok) {
    return buildErrorResponse(outcome.errorId, outcome.errorCode, outcome.errorMessage)
        .serialize();
  }
  std::optional<JsonValue> resp = handleParsedRequest(outcome.request, router);
  if (!resp) return std::nullopt;  // notification
  return resp->serialize();
}

}  // namespace litholattice::mcp
